#include <catch2/catch_test_macros.hpp>

#include "lua/lua_bytes.hpp"
#include "lua/lua_state.hpp"
#include "sim/command_codec.hpp"

#include <limits>
#include <string>
#include <vector>

extern "C" {
#include <lua.h>
}

using namespace osc::lua;

namespace {

/// The bytes of the value `expr` evaluates to in `state`.
std::optional<std::string> bytes_of(LuaState& state, const std::string& expr) {
    REQUIRE(state.do_string("__value = " + expr).ok());
    lua_State* L = state.raw();
    lua_getglobal(L, "__value");
    auto bytes = lua_to_bytes(L, -1);
    lua_pop(L, 1);
    return bytes;
}

/// Push the bytes' value as global `name`; false if they hold none.
bool set_global(LuaState& state, const char* name, const std::string& bytes) {
    lua_State* L = state.raw();
    const int top = lua_gettop(L);
    if (!push_lua_bytes(L, bytes)) {
        CHECK(lua_gettop(L) == top); // nothing left behind
        return false;
    }
    CHECK(lua_gettop(L) == top + 1);
    lua_setglobal(L, name);
    return true;
}

bool lua_true(LuaState& state, const std::string& expr) {
    REQUIRE(state.do_string("__check = " + expr).ok());
    lua_getglobal(state.raw(), "__check");
    const bool value = lua_toboolean(state.raw(), -1) != 0;
    lua_pop(state.raw(), 1);
    return value;
}

std::string as_string(const std::vector<osc::u8>& v) {
    return {v.begin(), v.end()};
}

} // namespace

TEST_CASE("lua_bytes: a mod list crosses Lua states whole", "[lua][lua_bytes]") {
    LuaState from;
    const auto bytes = bytes_of(from, R"({
        { uid = '74A9EAB2-E851-11DB-A1F1-F2C755D89593', name = 'Resource Rich',
          location = '/mods/resourcerich', hookdir = '/hook', version = 1,
          exclusive = false, ui_only = false, selectable = true,
          requires = { 'a', 'b' }, requiresNames = { a = 'Mod A' } },
        { uid = 'osc-test', location = '/mods/osc', version = -2.5 },
    })");
    REQUIRE(bytes);

    LuaState to;
    REQUIRE(set_global(to, "mods", *bytes));
    CHECK(lua_true(to, "table.getn(mods) == 2"));
    CHECK(lua_true(to, "mods[1].uid == '74A9EAB2-E851-11DB-A1F1-F2C755D89593'"));
    CHECK(lua_true(to, "mods[1].location == '/mods/resourcerich' and mods[1].version == 1"));
    CHECK(lua_true(to, "mods[1].exclusive == false and mods[1].selectable == true"));
    CHECK(lua_true(to, "mods[1].requires[2] == 'b' and mods[1].requiresNames.a == 'Mod A'"));
    CHECK(lua_true(to, "mods[2].version == -2.5 and mods[2].name == nil"));
    // ...and writes back as the same bytes
    lua_getglobal(to.raw(), "mods");
    CHECK(lua_to_bytes(to.raw(), -1) == bytes);
    lua_pop(to.raw(), 1);
}

TEST_CASE("lua_bytes: the same data gives the same bytes, whatever order it was built in",
          "[lua][lua_bytes]") {
    LuaState state;
    const auto a = bytes_of(state, "{ z = 1, a = 2, [3] = 'c', [1] = 'a', [true] = 1, "
                                   "[false] = 0, [-1.5] = 'n' }");
    const auto built = state.do_string("t = {}; t[false] = 0; t[1] = 'a'; t.a = 2; t[true] = 1; "
                                       "t[-1.5] = 'n'; t[3] = 'c'; t.z = 1");
    REQUIRE(built.ok());
    const auto b = bytes_of(state, "t");
    REQUIRE(a);
    CHECK(a == b);
}

TEST_CASE("lua_bytes: scalars, strings with any bytes, and what is left out", "[lua][lua_bytes]") {
    LuaState from;
    LuaState to;
    for (const char* expr :
         {"nil", "true", "false", "0", "-0.25", "1e300", "'plain'", "'nul\\0in\\255side'", "{}"}) {
        const auto bytes = bytes_of(from, expr);
        REQUIRE(bytes);
        REQUIRE(set_global(to, "v", *bytes));
        lua_getglobal(to.raw(), "v");
        CHECK(lua_to_bytes(to.raw(), -1) == bytes); // same value back
        lua_pop(to.raw(), 1);
    }
    // Functions, userdata and coroutines hold no data: entries holding or
    // keyed by them go, and one on its own is nil.
    const auto bytes = bytes_of(from, "{ keep = 1, f = print, [print] = 2, "
                                      "co = coroutine.create(function() end), [{}] = 3 }");
    REQUIRE(bytes);
    REQUIRE(set_global(to, "t", *bytes));
    CHECK(lua_true(to, "t.keep == 1 and next(t, 'keep') == nil and next(t) == 'keep'"));
    const auto function = bytes_of(from, "print");
    REQUIRE(function);
    REQUIRE(set_global(to, "fn", *function));
    CHECK(lua_true(to, "fn == nil"));
}

TEST_CASE("lua_bytes: a table inside itself, nested too deep, or too long, has no bytes",
          "[lua][lua_bytes]") {
    LuaState state;
    REQUIRE(state.do_string("cyclic = { inner = {} }; cyclic.inner.back = cyclic").ok());
    CHECK_FALSE(bytes_of(state, "cyclic"));
    // Shared tables are written each place they are found: two to a level
    // double at each, so these few lines ask for 2^30 copies of the last.
    const auto made =
        state.do_string("dag = { 1 }; for i = 1, 30 do dag = { a = dag, b = dag } end");
    REQUIRE(made.ok());
    CHECK_FALSE(bytes_of(state, "dag"));
    CHECK_FALSE(bytes_of(state, "string.rep('x', 17 * 1024 * 1024)"));
    CHECK(bytes_of(state, "string.rep('x', 15 * 1024 * 1024)"));
    // A table reached twice, but not inside itself, is written twice.
    REQUIRE(state.do_string("shared = {1}; twice = { a = shared, b = shared }").ok());
    const auto twice = bytes_of(state, "twice");
    REQUIRE(twice);
    REQUIRE(set_global(state, "copy", *twice));
    CHECK(lua_true(state, "copy.a[1] == 1 and copy.b[1] == 1 and copy.a ~= copy.b"));

    auto nest = [&](int depth) {
        std::string expr = "1";
        for (int i = 0; i < depth; ++i) {
            expr.insert(0, "{");
            expr += '}';
        }
        return bytes_of(state, expr);
    };
    CHECK(nest(kLuaBytesMaxDepth));
    CHECK_FALSE(nest(kLuaBytesMaxDepth + 1));
    const int top = lua_gettop(state.raw());
    CHECK_FALSE(nest(kLuaBytesMaxDepth + 1));
    CHECK(lua_gettop(state.raw()) == top);
}

TEST_CASE("lua_bytes: bytes that aren't exactly one value push nothing", "[lua][lua_bytes]") {
    LuaState state;
    const auto good = bytes_of(state, "{ a = { 'x', true }, [2] = 3.5 }");
    REQUIRE(good);
    REQUIRE(set_global(state, "ok", *good));

    CHECK_FALSE(set_global(state, "v", ""));
    for (size_t n = 1; n < good->size(); ++n)
        CHECK_FALSE(set_global(state, "v", good->substr(0, n)));
    CHECK_FALSE(set_global(state, "v", *good + '\0')); // a byte after it

    using osc::sim::ByteWriter;
    std::vector<osc::u8> b;
    ByteWriter w(b);
    auto reset = [&] { b.clear(); };

    reset();
    w.u8v(9); // no such tag
    CHECK_FALSE(set_global(state, "v", as_string(b)));

    // A table whose key is a table, nil or NaN, or whose value is nil
    for (int bad = 0; bad < 4; ++bad) {
        reset();
        w.u8v(5);
        w.u32v(1);
        if (bad == 0) {
            w.u8v(5);
            w.u32v(0);
        } else if (bad == 1) {
            w.u8v(0);
        } else {
            w.u8v(3);
            w.f64v(bad == 2 ? std::numeric_limits<double>::quiet_NaN() : 1.0);
        }
        w.u8v(bad == 3 ? 0 : 2); // the value: nil, or true
        CHECK_FALSE(set_global(state, "v", as_string(b)));
    }

    // Lengths and counts the bytes can't hold
    reset();
    w.u8v(4);
    w.u32v(0xFFFFFFFFu);
    CHECK_FALSE(set_global(state, "v", as_string(b)));
    reset();
    w.u8v(5);
    w.u32v(0xFFFFFFFFu);
    w.u8v(2);
    w.u8v(2);
    CHECK_FALSE(set_global(state, "v", as_string(b)));

    // Nesting deeper than the limit, which only crafted bytes have
    reset();
    for (int i = 0; i <= kLuaBytesMaxDepth; ++i) {
        w.u8v(5);
        w.u32v(1);
        w.u8v(2);
    }
    w.u8v(2);
    CHECK_FALSE(set_global(state, "v", as_string(b)));
}
