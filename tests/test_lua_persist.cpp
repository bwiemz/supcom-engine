// Lua heap persistence (M208c): a state's heap written and read back into
// another state, which must then behave as the first would have.

#include <catch2/catch_test_macros.hpp>

#include <lpersist.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

// Internals: the collector's state, and what it marks.
#include <lgc.h>
#include <lstate.h>

#include <string>

namespace {

/// A bare state with the standard libraries the tests use.
struct State {
    lua_State* L = nullptr;
    State() {
        L = lua_open();
        luaopen_base(L);
        luaopen_table(L);
        luaopen_string(L);
        luaopen_math(L);
        lua_settop(L, 0);
    }
    ~State() { lua_close(L); }
    State(const State&) = delete;
    State& operator=(const State&) = delete;

    void run(const std::string& code) {
        INFO(code);
        REQUIRE(luaL_loadbuffer(L, code.data(), code.size(), "test") == 0);
        if (lua_pcall(L, 0, 0, 0) != 0) FAIL(lua_tostring(L, -1));
        lua_settop(L, 0);
    }
    std::string str(const std::string& expr) {
        run("__r = tostring(" + expr + ")");
        lua_pushstring(L, "__r");
        lua_rawget(L, LUA_GLOBALSINDEX);
        std::string s = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        lua_settop(L, 0);
        return s;
    }
    lu_mem blocks() const { return G(L)->nblocks; }
};

const lua_PersistHooks kNoHooks{};

std::string persist(State& s, const lua_PersistHooks& hooks = kNoHooks) {
    std::string out;
    const std::string err = lua_persist(s.L, out, hooks);
    INFO(err);
    REQUIRE(err.empty());
    return out;
}

void unpersist(State& s, const std::string& bytes, const lua_PersistHooks& hooks = kNoHooks) {
    const std::string err = lua_unpersist(s.L, bytes, hooks);
    INFO(err);
    REQUIRE(err.empty());
}

/// A state restored from `from`'s heap.
void copy(State& from, State& to) {
    unpersist(to, persist(from));
}

/// Freeze the global `name` (lua_freeze).
void freeze(State& s, const char* name) {
    lua_pushstring(s.L, name);
    lua_rawget(s.L, LUA_GLOBALSINDEX);
    REQUIRE(lua_istable(s.L, -1));
    lua_freeze(s.L, -1);
    lua_settop(s.L, 0);
}

/// The table the expression gives.
const Table* table_of(State& s, const std::string& expr) {
    s.run("__t = " + expr);
    lua_pushstring(s.L, "__t");
    lua_rawget(s.L, LUA_GLOBALSINDEX);
    REQUIRE(lua_istable(s.L, -1));
    const auto* t = static_cast<const Table*>(lua_topointer(s.L, -1));
    lua_settop(s.L, 0);
    s.run("__t = nil");
    return t;
}

int frozen_count(State& s) {
    int n = 0;
    for (GCObject* o = G(s.L)->frozengc; o != nullptr; o = o->gch.next) ++n;
    return n;
}

// The order a table iterates in, as a string.
const char* kOrder = R"(
    function order(t)
        local s = ''
        for k, v in pairs(t) do s = s .. tostring(k) .. '=' .. tostring(v) .. ';' end
        return s
    end
)";

int upvalue_one(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

} // namespace

TEST_CASE("A heap comes back with its sharing and cycles", "[lua_persist]") {
    State a;
    a.run(R"(
        shared = {n = 1}
        root = {x = 1.5, s = 'text', yes = true, no = false, p = shared, q = shared, list = {10, 20, 30}}
        root.self = root
        root.inner = {back = root}
    )");
    State b;
    copy(a, b);
    CHECK(b.str("root.self == root and root.inner.back == root") == "true");
    CHECK(b.str("root.p == root.q and root.p == shared") == "true");
    CHECK(b.str("root.x .. root.s .. tostring(root.yes) .. tostring(root.no)") ==
          "1.5texttruefalse");
    CHECK(b.str("root.list[1] + root.list[2] + root.list[3]") == "60");
}

TEST_CASE("A table iterates as it did, and goes on to", "[lua_persist]") {
    const char* build = R"(
        t = {}
        for i = 1, 60 do t['k' .. i] = i end
        for i = 1, 60, 3 do t['k' .. i] = nil end
        t[3.25] = 'f'
        t[-7] = 'n'
        t[1000] = 'far'
        t[true] = 'yes'
    )";
    State a;
    a.run(kOrder);
    a.run(build);
    State b;
    copy(a, b);
    CHECK(b.str("order(t)") == a.str("order(t)"));
    // Both go on: removed keys come back, new ones join
    const char* more = R"(
        t.k1 = 'again'
        for i = 61, 80 do t['k' .. i] = i end
        t.k2 = nil
    )";
    a.run(more);
    b.run(more);
    CHECK(b.str("order(t)") == a.str("order(t)"));
}

TEST_CASE("Closures keep their upvalues, shared as they were", "[lua_persist]") {
    State a;
    a.run(R"(
        function counter()
            local n = 0
            return function() n = n + 1 return n end, function() return n end
        end
        inc, get = counter()
        inc() inc()
        inc2, get2 = counter()
    )");
    State b;
    copy(a, b);
    b.run("inc()");
    CHECK(b.str("get()") == "3");
    CHECK(b.str("get2()") == "0");
}

TEST_CASE("Suspended coroutines resume where they stopped", "[lua_persist]") {
    State a;
    a.run(R"(
        local function deep(x) return coroutine.yield(x) end
        local function middle(x) return deep(x) + 1 end
        co = coroutine.create(function(v)
            while true do v = v + middle(v) end
        end)
        _, first = coroutine.resume(co, 10)
        -- A closure over a local of a suspended coroutine: an open upvalue
        holder = coroutine.create(function()
            local v = 1
            getter = function() return v end
            setter = function(x) v = x end
            while true do v = v * 2 coroutine.yield() end
        end)
        coroutine.resume(holder)
    )");
    REQUIRE(a.str("first") == "10");
    REQUIRE(a.str("getter()") == "2");
    State b;
    copy(a, b);
    // co: v = 10 + (5 + 1) = 16, then it yields 16
    const char* step = "_, got = coroutine.resume(co, 5)";
    a.run(step);
    b.run(step);
    CHECK(b.str("got") == a.str("got"));
    CHECK(b.str("got") == "16");
    // The upvalue is still the coroutine's local
    CHECK(b.str("getter()") == "2");
    b.run("setter(7) coroutine.resume(holder)");
    CHECK(b.str("getter()") == "14");
    CHECK(b.str("coroutine.status(holder)") == "suspended");
}

TEST_CASE("C functions and C closures come back as themselves", "[lua_persist]") {
    State a;
    lua_pushnumber(a.L, 7);
    lua_pushcclosure(a.L, upvalue_one, 1);
    lua_setglobal(a.L, "seven");
    a.run("fmt = string.format keep = {ins = table.insert}");
    State b;
    copy(a, b);
    CHECK(b.str("seven()") == "7");
    CHECK(b.str("fmt('%d-%s', 3, 'x')") == "3-x");
    b.run("local t = {} keep.ins(t, 'v') got = t[1]");
    CHECK(b.str("got") == "v");
}

TEST_CASE("Light userdata go through the hooks", "[lua_persist]") {
    int old_objects[2] = {};
    int new_objects[2] = {};
    struct Map {
        int* from;
        int* to;
    } map{old_objects, new_objects};
    lua_PersistHooks hooks;
    hooks.ud = &map;
    hooks.name = [](void* ud, void* p, std::uint32_t* kind, std::uint64_t* id) {
        auto* m = static_cast<Map*>(ud);
        for (int i = 0; i < 2; ++i)
            if (p == m->from + i) {
                *kind = 9;
                *id = static_cast<std::uint64_t>(i);
                return true;
            }
        return false;
    };
    hooks.find = [](void* ud, std::uint32_t kind, std::uint64_t id, void** p) {
        auto* m = static_cast<Map*>(ud);
        if (kind != 9 || id > 1) return false;
        *p = m->to + id;
        return true;
    };
    State a;
    lua_pushstring(a.L, "obj");
    lua_newtable(a.L);
    lua_pushstring(a.L, "_c_object");
    lua_pushlightuserdata(a.L, old_objects + 1);
    lua_rawset(a.L, -3);
    lua_rawset(a.L, LUA_GLOBALSINDEX);
    lua_pushstring(a.L, "gone");
    lua_pushlightuserdata(a.L, nullptr);
    lua_rawset(a.L, LUA_GLOBALSINDEX);

    State b;
    unpersist(b, persist(a, hooks), hooks);
    b.run("__p = obj._c_object");
    lua_pushstring(b.L, "__p");
    lua_rawget(b.L, LUA_GLOBALSINDEX);
    CHECK(lua_touserdata(b.L, -1) == new_objects + 1);
    lua_settop(b.L, 0);
    CHECK(b.str("gone == nil") == "false");

    // One the hooks cannot name fails the save
    int stranger = 0;
    lua_pushstring(a.L, "stranger");
    lua_pushlightuserdata(a.L, &stranger);
    lua_rawset(a.L, LUA_GLOBALSINDEX);
    std::string out;
    CHECK_FALSE(lua_persist(a.L, out, hooks).empty());
}

TEST_CASE("Registry refs resolve as they did, and the next one is the same", "[lua_persist]") {
    State a;
    int refs[3];
    for (int i = 0; i < 3; ++i) {
        lua_newtable(a.L);
        lua_pushnumber(a.L, i);
        lua_rawseti(a.L, -2, 1);
        refs[i] = luaL_ref(a.L, LUA_REGISTRYINDEX);
    }
    luaL_unref(a.L, LUA_REGISTRYINDEX, refs[1]);
    State b;
    copy(a, b);
    for (int i : {0, 2}) {
        lua_rawgeti(b.L, LUA_REGISTRYINDEX, refs[i]);
        lua_rawgeti(b.L, -1, 1);
        CHECK(lua_tonumber(b.L, -1) == i);
        lua_settop(b.L, 0);
    }
    lua_newtable(a.L);
    lua_newtable(b.L);
    CHECK(luaL_ref(b.L, LUA_REGISTRYINDEX) == luaL_ref(a.L, LUA_REGISTRYINDEX));
}

TEST_CASE("A load keeps the collector's modes, and frees the old heap", "[lua_persist]") {
    State a;
    lua_setlazysweep(a.L, 1);
    lua_setmanualgc(a.L, 1);
    a.run("keep = {} for i = 1, 200 do keep[i] = {i, tostring(i) .. 'x'} end");
    State b;
    b.run("old = {} for i = 1, 20000 do old[i] = {i, 'old' .. i} end");
    const int old_kb = lua_getgccount(b.L);
    copy(a, b);
    CHECK(G(b.L)->lazysweep == 1);
    CHECK(G(b.L)->manualgc == 1);
    CHECK(G(b.L)->GCthreshold == MAX_LUMEM); // collects only when told
    CHECK(lua_getgccount(b.L) < old_kb / 4);
    CHECK(b.str("old == nil and keep[200][2]") == "200x");
}

TEST_CASE("Frozen tables come back frozen, their strings fixed", "[lua_persist]") {
    State a;
    a.run(R"(
        data = {list = {1, 2, 3}, name = 'frozen name', sub = {deep = {x = 1}}}
        roots = {fn = function() return 'kept' end}
    )");
    freeze(a, "data");
    freeze(a, "roots"); // it holds a function: a frozen root
    REQUIRE(G(a.L)->nfrozenroots == 1);
    State b;
    b.run("mine = {a = {1}}");
    freeze(b, "mine"); // the old heap's: thawed and freed
    copy(a, b);
    CHECK(frozen_count(b) == frozen_count(a));
    CHECK(G(b.L)->nfrozenroots == 1);
    CHECK((table_of(b, "data.sub.deep")->marked & (1 << FROZENBIT)) != 0);
    CHECK((table_of(b, "roots")->marked & (1 << FROZENROOTBIT)) != 0);
    // Its strings are fixed: the mark never walks a frozen table
    b.run("__s = data.name");
    lua_pushstring(b.L, "__s");
    lua_rawget(b.L, LUA_GLOBALSINDEX);
    const auto* name = reinterpret_cast<const TString*>(lua_tostring(b.L, -1)) - 1;
    CHECK((name->tsv.marked & (1 << 4)) != 0);
    lua_settop(b.L, 0);
    // Through collections, and a write after (the barrier makes it a root)
    b.run("data.sub.added = {'new'} collectgarbage() collectgarbage()");
    CHECK(b.str("roots.fn() .. data.name .. data.sub.added[1] .. data.sub.deep.x") ==
          "keptfrozen namenew1");
}

TEST_CASE("A save during a lazy sweep reads only what lives", "[lua_persist]") {
    State a;
    lua_setlazysweep(a.L, 1);
    lua_setmanualgc(a.L, 1);
    a.run(R"(
        live = {}
        for i = 1, 3000 do live[i] = {i} end
        probe = setmetatable({}, {__mode = 'k'})
        do
            -- A coroutine dropped while suspended, and a closure over its
            -- local: an open upvalue on a dead thread
            local co = coroutine.create(function()
                local v = {n = 5}
                get = function() return v.n end
                coroutine.yield()
            end)
            coroutine.resume(co)
            probe[co] = true
        end
        for i = 1, 20000 do local t = {i, {i}} end
    )");
    a.run("collectgarbage()");
    REQUIRE(a.str("next(probe) == nil") == "true"); // the coroutine died
    REQUIRE(lua_sweepstep(a.L, 100) == 0);          // a sweep under way
    State b;
    copy(a, b);
    CHECK(b.str("get()") == "5");
    CHECK(b.str("live[3000][1]") == "3000");
    b.run("collectgarbage()");
    CHECK(b.str("get()") == "5");
    // The saved state goes on too
    while (lua_sweepstep(a.L, 1 << 20) == 0) {}
    CHECK(a.str("get()") == "5");
}

TEST_CASE("Only a main thread at rest saves", "[lua_persist]") {
    State a;
    std::string out;
    lua_pushnumber(a.L, 1); // something of the program's on its stack
    CHECK_FALSE(lua_persist(a.L, out, kNoHooks).empty());
    lua_settop(a.L, 0);
    lua_State* co = lua_newthread(a.L);
    CHECK_FALSE(lua_persist(co, out, kNoHooks).empty());
    lua_settop(a.L, 0);
    CHECK(lua_persist(a.L, out, kNoHooks).empty());
}

TEST_CASE("Weak tables keep their entries until the next collection", "[lua_persist]") {
    State a;
    // No automatic collections (the parser runs one before a chunk when the
    // heap nears the threshold): only the explicit ones below
    lua_setmanualgc(a.L, 1);
    a.run(R"(
        weak = setmetatable({}, {__mode = 'k'})
        do local key = {} weak[key] = 1 end
        function count(t) local n = 0 for _ in pairs(t) do n = n + 1 end return n end
    )");
    REQUIRE(a.str("count(weak)") == "1"); // not collected yet
    State b;
    copy(a, b);
    CHECK(b.str("count(weak)") == "1");
    b.run("collectgarbage()");
    a.run("collectgarbage()");
    CHECK(b.str("count(weak)") == "0");
    CHECK(a.str("count(weak)") == "0");
}

TEST_CASE("Metatables and per-type metatables come back", "[lua_persist]") {
    State a;
    a.run("base = {hello = function() return 'hi' end} obj = setmetatable({}, {__index = base})");
    a.run("number_methods = {twice = function(n) return n * 2 end}");
    // LuaPlus: a metatable for every number
    lua_pushnumber(a.L, 0);
    lua_newtable(a.L);
    lua_pushstring(a.L, "__index");
    lua_pushstring(a.L, "number_methods");
    lua_rawget(a.L, LUA_GLOBALSINDEX);
    lua_rawset(a.L, -3);
    lua_setmetatable(a.L, -2);
    lua_settop(a.L, 0);
    REQUIRE(a.str("(21):twice()") == "42");
    State b;
    copy(a, b);
    CHECK(b.str("obj:hello()") == "hi");
    CHECK(b.str("(21):twice()") == "42");
}

TEST_CASE("A restored heap saves to the same bytes", "[lua_persist]") {
    State a;
    a.run(kOrder);
    a.run(R"(
        data = {n = 1, list = {1, 2, 3}, name = 'x'}
        for i = 1, 30 do data['k' .. i] = i * 1.5 end
        data.k4 = nil
        function make() local c = 0 return function() c = c + 1 return c end end
        tick = make() tick()
        co = coroutine.create(function(v) while true do v = v + coroutine.yield(v) end end)
        coroutine.resume(co, 1)
    )");
    const std::string first = persist(a);
    State b;
    unpersist(b, first);
    CHECK(persist(b) == first);
}

TEST_CASE("A corrupted snapshot fails, and leaves the state as it was", "[lua_persist]") {
    State a;
    a.run("x = {1, 2, 3} s = 'saved'");
    std::string bad = persist(a);
    bad[bad.size() / 2] = static_cast<char>(bad[bad.size() / 2] ^ 0x40);
    State b;
    b.run("mine = 'kept'");
    CHECK_FALSE(lua_unpersist(b.L, bad, kNoHooks).empty());
    CHECK(b.str("mine") == "kept");
    b.run("collectgarbage()"); // what the load made is garbage
    CHECK(b.str("mine .. tostring(x)") == "keptnil");
}

TEST_CASE("Only a main thread at rest loads", "[lua_persist]") {
    State a;
    a.run("x = 1");
    const std::string good = persist(a);
    State b;
    lua_pushnumber(b.L, 1);
    CHECK_FALSE(lua_unpersist(b.L, good, kNoHooks).empty());
    lua_settop(b.L, 0);
    CHECK(lua_unpersist(b.L, good, kNoHooks).empty());
}

// Damaged snapshots, their hash rewritten so the load reads them: each one
// loads or fails, and never crashes (run it under ASan to see more).
TEST_CASE("Damaged snapshots fail or load, never crash", "[lua_persist]") {
    State a;
    a.run(kOrder);
    a.run(R"(
        data = {n = 1, list = {1, 2, 3}, name = 'x', [2.5] = true}
        for i = 1, 20 do data['k' .. i] = {i} end
        keyed = {[data] = 1, [print] = 2}
        function make() local c = 0 return function() c = c + 1 return c end end
        tick = make() tick()
        co = coroutine.create(function(v)
            local w = {v}
            while true do v = v + coroutine.yield(w) end
        end)
        coroutine.resume(co, 1)
    )");
    const std::string good = persist(a);
    std::uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    int loaded = 0;
    for (int round = 0; round < 400; ++round) {
        std::string bad = good;
        const int flips = 1 + static_cast<int>(next() % 3);
        for (int f = 0; f < flips; ++f) {
            const std::size_t at = next() % (bad.size() - sizeof(std::uint64_t));
            bad[at] = static_cast<char>(bad[at] ^ static_cast<char>(1 + next() % 255));
        }
        lua_persist_seal(bad);
        State b;
        if (lua_unpersist(b.L, bad, kNoHooks).empty()) {
            ++loaded;
            lua_setgcthreshold(b.L, 0); // the collector walks what loaded
        }
    }
    INFO(loaded << " of 400 loaded");
    CHECK(loaded < 400);
}

TEST_CASE("A snapshot that is not one fails to load", "[lua_persist]") {
    State a;
    a.run("x = {1, 2, 3}");
    const std::string good = persist(a);
    {
        State b;
        CHECK_FALSE(lua_unpersist(b.L, "not a snapshot", kNoHooks).empty());
    }
    {
        State b;
        CHECK_FALSE(lua_unpersist(b.L, good.substr(0, good.size() / 2), kNoHooks).empty());
    }
}
