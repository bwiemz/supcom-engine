// Moho's argument-count error, raised from one helper (lua/arg_count.hpp):
// the function's help text, then "expected <n> args, but got <got>"
// (kLuaExpectedArgsWarning), or its range form, as ClearFrame and MATH_Lerp
// raise it (faf-re Sim.cpp, SCR_Math.cpp).

#include <catch2/catch_test_macros.hpp>

#include "lua/arg_count.hpp"
#include "lua/lua_state.hpp"
#include "lua/sim_bindings.hpp"

#include <string>

namespace {

/// The error `code` raises in `state` (after Lua's position, if it adds
/// one), or "" when it ran.
std::string error_of(osc::lua::LuaState& state, const char* code) {
    auto r = state.do_string(code);
    return r.ok() ? std::string() : r.error().message;
}

int takes_two(lua_State* L) {
    if (lua_gettop(L) != 2) return osc::lua::arg_count_error(L, "TakesTwo(a, b)", 2, lua_gettop(L));
    lua_pushstring(L, "answered");
    return 1;
}

int takes_one_to_three(lua_State* L) {
    const int n = lua_gettop(L);
    if (n < 1 || n > 3) return osc::lua::arg_range_error(L, "TakesOneToThree(a[, b, c])", 1, 3, n);
    lua_pushstring(L, "answered");
    return 1;
}

} // namespace

TEST_CASE("The argument-count error reads as Moho's", "[lua][args]") {
    osc::lua::LuaState state;
    state.register_function("TakesTwo", takes_two);
    state.register_function("TakesOneToThree", takes_one_to_three);

    CHECK(error_of(state, "assert(TakesTwo(1, 2) == 'answered')").empty());
    CHECK(error_of(state, "TakesTwo(1)").ends_with("TakesTwo(a, b)\n  expected 2 args, but got 1"));
    CHECK(error_of(state, "TakesTwo(1, 2, 3)")
              .ends_with("TakesTwo(a, b)\n  expected 2 args, but got 3"));

    CHECK(error_of(state, "assert(TakesOneToThree(1, 2) == 'answered')").empty());
    CHECK(error_of(state, "TakesOneToThree()")
              .ends_with("TakesOneToThree(a[, b, c])\n  expected between 1 and 3 args, but got 0"));
    CHECK(error_of(state, "TakesOneToThree(1, 2, 3, 4)")
              .ends_with("TakesOneToThree(a[, b, c])\n  expected between 1 and 3 args, but got 4"));

    // It raises: a pcall gets it, and the binding answers nothing.
    CHECK(error_of(state, R"(
        local ok, err = pcall(TakesTwo)
        assert(not ok, 'answered')
        assert(string.find(err, 'expected 2 args, but got 0', 1, true), err)
    )")
              .empty());
}

TEST_CASE("A binding that counts its arguments raises Moho's words", "[lua][args]") {
    osc::lua::LuaState state;
    osc::lua::register_core_bindings(state);
    // MATH_Lerp's own usage, as Moho registers it (the engine's message had
    // been "MATH_Lerp: expected between 3 and 5 args").
    CHECK(error_of(state, "MATH_Lerp(1, 2)")
              .ends_with("MATH_Lerp(s, a, b) or MATH_Lerp(s, sMin, sMax, a, b) -> number -- linear "
                         "interpolation from a (at s=0 or s=sMin) to b (at s=1 or s=sMax)\n"
                         "  expected between 3 and 5 args, but got 2"));
}
