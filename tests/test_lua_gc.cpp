// The sim's Lua collections (M224g): a lazy sweep, and frozen tables. What a
// program can see must not change -- values stay, garbage goes, weak tables
// clear as before -- whatever the collector skips or leaves for later.

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <string>

namespace {

struct Lua {
    lua_State* L = lua_open();
    Lua() {
        luaopen_base(L);
        luaopen_table(L);
        luaopen_string(L);
        lua_settop(L, 0);
    }
    ~Lua() { lua_close(L); }
    Lua(const Lua&) = delete;
    Lua& operator=(const Lua&) = delete;
    Lua(Lua&&) = delete;
    Lua& operator=(Lua&&) = delete;

    /// Run `code`; its error, or "" if none.
    std::string run(const std::string& code) const {
        if (luaL_loadbuffer(L, code.c_str(), code.size(), "test") != 0 ||
            lua_pcall(L, 0, 0, 0) != 0) {
            std::string err = lua_tostring(L, -1);
            lua_pop(L, 1);
            return err;
        }
        return "";
    }

    void collect() const { lua_setgcthreshold(L, 0); }
    [[nodiscard]] int kb() const { return lua_getgccount(L); }

    /// Sweep to the end, in slices of `work`: how many slices it took.
    int sweep_all(int work) const {
        int slices = 0;
        while (lua_sweepstep(L, work) == 0) ++slices;
        return slices;
    }

    /// Freeze the global `name`.
    void freeze(const char* name) const {
        lua_pushstring(L, name);
        lua_rawget(L, LUA_GLOBALSINDEX);
        REQUIRE(lua_istable(L, -1));
        lua_freeze(L, -1);
        lua_pop(L, 1);
    }
};

/// Garbage: `n` tables with a few fields, dropped at once.
const char* kGarbage = R"(
    for i = 1, 20000 do local t = { i, i + 1, name = 'g' .. i, sub = { i } } end
)";

} // namespace

TEST_CASE("A lazy sweep frees garbage over its slices, and keeps what lives", "[lua_gc]") {
    Lua lua;
    lua_setlazysweep(lua.L, 1);
    REQUIRE(lua.run(R"(
        live = {}
        for i = 1, 5000 do live[i] = { value = i, name = 'live' .. i } end
    )")
                .empty());
    REQUIRE(lua.run(kGarbage).empty());
    const int before = lua.kb();
    lua.collect(); // marks; the sweep is left for the slices
    // Nothing is freed yet but userdata and strings: the tables' garbage is
    // still counted
    CHECK(lua.kb() > before / 2);
    // Objects made while it sweeps are the new list's: never swept by it
    REQUIRE(lua.run("fresh = {}; for i = 1, 3000 do fresh[i] = { i } end").empty());
    const int slices = lua.sweep_all(1000);
    CHECK(slices > 5); // it took many slices
    CHECK(lua.kb() < before / 2);
    REQUIRE(lua.run(R"(
        for i = 1, 5000 do
            assert(live[i].value == i and live[i].name == 'live' .. i)
        end
        for i = 1, 3000 do assert(fresh[i][1] == i) end
    )")
                .empty());
    // And the next collections go on as usual
    lua.collect();
    lua.sweep_all(1 << 20);
    lua.collect();
    REQUIRE(lua.run("for i = 1, 5000 do assert(live[i].value == i) end").empty());
}

TEST_CASE("A collection during a lazy sweep ends the sweep first", "[lua_gc]") {
    Lua lua;
    lua_setlazysweep(lua.L, 1);
    REQUIRE(lua.run("keep = {}; for i = 1, 2000 do keep[i] = { i } end").empty());
    REQUIRE(lua.run(kGarbage).empty());
    lua.collect();
    CHECK(lua_sweepstep(lua.L, 10) == 0); // a sweep under way
    REQUIRE(lua.run(kGarbage).empty());
    lua.collect(); // ends it, then collects again
    lua.sweep_all(1 << 20);
    REQUIRE(lua.run("for i = 1, 2000 do assert(keep[i][1] == i) end").empty());
    // Turning it off ends the sweep too
    REQUIRE(lua.run(kGarbage).empty());
    lua.collect();
    lua_setlazysweep(lua.L, 0);
    CHECK(lua_sweepstep(lua.L, 1) == 1); // none under way
}

TEST_CASE("Closing a state in the middle of a lazy sweep frees everything", "[lua_gc]") {
    // (Under ASan and LeakSanitizer, a leak or a double free fails this)
    lua_State* L = lua_open();
    luaopen_base(L);
    lua_setlazysweep(L, 1);
    const char* code = "t = {}; for i = 1, 10000 do t[i] = { i } end; t = nil";
    REQUIRE(luaL_loadbuffer(L, code, std::string(code).size(), "c") == 0);
    REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
    lua_setgcthreshold(L, 0);
    CHECK(lua_sweepstep(L, 10) == 0);
    lua_close(L);
}

TEST_CASE("Weak tables clear at the collection, however the sweep is spread", "[lua_gc]") {
    Lua lua;
    lua_setlazysweep(lua.L, 1);
    REQUIRE(lua.run(R"(
        cache = setmetatable({}, { __mode = 'v' })
        held = {}
        for i = 1, 100 do
            local t = { i }
            cache[i] = t
            if i <= 10 then held[i] = t end
        end
    )")
                .empty());
    lua.collect();
    // Before any slice: the dropped values are gone already
    REQUIRE(lua.run(R"(
        local n = 0
        for k, v in cache do n = n + 1; assert(held[k] == v) end
        assert(n == 10, 'weak entries left: ' .. n)
    )")
                .empty());
    lua.sweep_all(1 << 20);
}

TEST_CASE("Frozen tables keep their data through collections", "[lua_gc]") {
    Lua lua;
    REQUIRE(lua.run(R"(
        bp = {}
        for i = 1, 500 do
            bp['unit' .. i] = {
                Id = 'unit' .. i,
                Economy = { Mass = i, Energy = i * 10 },
                Categories = { 'LAND', 'MOBILE', 'TECH' .. i },
                Weapon = { { Damage = i, Label = 'Gun' }, { Damage = i * 2 } },
            }
        end
        bp.self = bp -- a cycle
    )")
                .empty());
    lua.freeze("bp");
    const int frozen = lua.kb();
    for (int i = 0; i < 3; ++i) {
        REQUIRE(lua.run(kGarbage).empty());
        lua.collect();
    }
    CHECK(lua.kb() <= frozen + 64); // the garbage went; the frozen data stayed
    REQUIRE(lua.run(R"(
        for i = 1, 500 do
            local u = bp['unit' .. i]
            assert(u.Id == 'unit' .. i)
            assert(u.Economy.Mass == i and u.Economy.Energy == i * 10)
            assert(u.Categories[1] == 'LAND' and u.Categories[3] == 'TECH' .. i)
            assert(u.Weapon[2].Damage == i * 2 and u.Weapon[1].Label == 'Gun')
        end
        assert(bp.self == bp)
    )")
                .empty());
}

TEST_CASE("What is written into a frozen table lives (the write barrier)", "[lua_gc]") {
    Lua lua;
    lua_setlazysweep(lua.L, 1);
    REQUIRE(
        lua.run("bp = { Unit = { Economy = { Mass = 1 } }, List = { 1, 2 }, Inserted = { 1, 2 }, "
                "Slots = { 1, 2, 3 } }")
            .empty());
    lua.freeze("bp");
    // New tables, a function and a string stored into frozen tables, by
    // every path, each into a table no other write reached first: a field
    // set (luaH_set), rawset, an integer key, table.insert (luaH_setnum)
    REQUIRE(lua.run(R"(
        bp.Unit.Buffs = { Speed = { Add = 3 } }
        rawset(bp.Unit, 'Hook', function() return 'hooked' end)
        bp.List[3] = { 'three' }
        table.insert(bp.Inserted, { 'four' })
        bp.Unit.Economy.Name = string.rep('x', 3) .. 'mass'
    )")
                .empty());
    // An existing array slot, written from C: luaH_setnum with no resize (a
    // table that grows is re-inserted through luaH_set, whose barrier would
    // cover for a missing one here)
    lua_pushstring(lua.L, "bp");
    lua_rawget(lua.L, LUA_GLOBALSINDEX);
    lua_pushstring(lua.L, "Slots");
    lua_rawget(lua.L, -2);
    REQUIRE(lua_istable(lua.L, -1));
    REQUIRE(luaL_loadbuffer(lua.L, "return { 'slot' }", 17, "s") == 0);
    REQUIRE(lua_pcall(lua.L, 0, 1, 0) == 0);
    lua_rawseti(lua.L, -2, 2);
    lua_settop(lua.L, 0);
    for (int i = 0; i < 4; ++i) {
        REQUIRE(lua.run(kGarbage).empty());
        lua.collect();
        lua.sweep_all(1 << 20);
    }
    REQUIRE(lua.run(R"(
        assert(bp.Unit.Buffs.Speed.Add == 3)
        assert(bp.Unit.Hook() == 'hooked')
        assert(bp.List[3][1] == 'three' and bp.Inserted[3][1] == 'four')
        assert(bp.Slots[2][1] == 'slot' and bp.Slots[3] == 3)
        assert(bp.Unit.Economy.Name == 'xxxmass')
        assert(bp.Unit.Economy.Mass == 1)
    )")
                .empty());
}

TEST_CASE("A frozen table holding what can't freeze keeps it alive", "[lua_gc]") {
    Lua lua;
    REQUIRE(lua.run(R"(
        local mt = { __index = function(t, k) return 'default' end }
        bp = {
            Script = function() return 42 end,
            Instance = setmetatable({ x = 1 }, mt),
            Weak = setmetatable({}, { __mode = 'k' }),
            Nested = { Deep = { Fn = function() return 'deep' end } },
        }
    )")
                .empty());
    lua.freeze("bp");
    for (int i = 0; i < 3; ++i) {
        REQUIRE(lua.run(kGarbage).empty());
        lua.collect();
    }
    REQUIRE(lua.run(R"(
        assert(bp.Script() == 42)
        assert(bp.Instance.x == 1 and bp.Instance.missing == 'default')
        assert(bp.Nested.Deep.Fn() == 'deep')
        assert(type(bp.Weak) == 'table')
    )")
                .empty());
    // A metatable given to a frozen table later lives too
    REQUIRE(lua.run(R"(
        setmetatable(bp.Nested, { __index = function() return 'from mt' end })
    )")
                .empty());
    lua.collect();
    lua.collect();
    REQUIRE(lua.run("assert(bp.Nested.nothing == 'from mt')").empty());
}

TEST_CASE("Weak tables keep frozen keys and values", "[lua_gc]") {
    Lua lua;
    REQUIRE(lua.run(R"(
        bp = { A = { 1 }, B = { 2 } }
        byKey = setmetatable({}, { __mode = 'k' })
        byValue = setmetatable({}, { __mode = 'v' })
    )")
                .empty());
    lua.freeze("bp");
    REQUIRE(lua.run(R"(
        byKey[bp.A] = 'a'
        byValue.b = bp.B
        byValue.gone = {}
    )")
                .empty());
    lua.collect();
    REQUIRE(lua.run(R"(
        assert(byKey[bp.A] == 'a', 'frozen key cleared')
        assert(byValue.b == bp.B, 'frozen value cleared')
        assert(byValue.gone == nil, 'a dead value stayed')
    )")
                .empty());
}

TEST_CASE("Freezing asks nothing of what isn't a plain table", "[lua_gc]") {
    Lua lua;
    REQUIRE(lua.run("obj = setmetatable({ x = 1 }, {}); plain = { y = { 2 } }").empty());
    lua.freeze("obj"); // has a metatable: not frozen, no harm
    lua.freeze("plain");
    lua.freeze("plain"); // twice: no harm
    REQUIRE(lua.run(kGarbage).empty());
    lua.collect();
    REQUIRE(lua.run("assert(obj.x == 1 and plain.y[1] == 2)").empty());
}

TEST_CASE("Closing a state with frozen tables frees them", "[lua_gc]") {
    // (Under ASan and LeakSanitizer, a leak fails this)
    lua_State* L = lua_open();
    luaopen_base(L);
    const char* code =
        "bp = {}; for i = 1, 1000 do bp[i] = { i, name = 'n' .. i } end; bp[1].x = {}";
    REQUIRE(luaL_loadbuffer(L, code, std::string(code).size(), "c") == 0);
    REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
    lua_pushstring(L, "bp");
    lua_rawget(L, LUA_GLOBALSINDEX);
    lua_freeze(L, -1);
    lua_pop(L, 1);
    const char* write = "bp[2].later = { 'x' }"; // a frozen root, in the roots list
    REQUIRE(luaL_loadbuffer(L, write, std::string(write).size(), "w") == 0);
    REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
    lua_close(L);
}
