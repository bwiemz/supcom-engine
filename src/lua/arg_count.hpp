#pragma once

/// Moho's argument-count error, which its bindings raise before they read an
/// argument: the function's help text (the usage its binder registers), then
/// the count it takes and the count it got. The only place its words live.

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

/// Raise "<help>\n  expected <expected> args, but got <got>", Moho's
/// kLuaExpectedArgsWarning (faf-re Sim.cpp). Never returns; it is an int so
/// a binding can `return` it, as it would luaL_error.
inline int arg_count_error(lua_State* L, const char* help, int expected, int got) {
    return luaL_error(L, "%s\n  expected %d args, but got %d", help, expected, got);
}

/// Its range form, for a function that takes `min` to `max` arguments:
/// "<help>\n  expected between <min> and <max> args, but got <got>", as
/// Moho's ClearFrame and MATH_Lerp raise it (faf-re Sim.cpp, SCR_Math.cpp).
inline int arg_range_error(lua_State* L, const char* help, int min, int max, int got) {
    return luaL_error(L, "%s\n  expected between %d and %d args, but got %d", help, min, max, got);
}

/// LuaPlus's type error, as Moho's bindings raise it for an argument of the
/// wrong type: "attempt to use as <expected> a <got> value".
inline int arg_type_error(lua_State* L, int idx, const char* expected) {
    return luaL_error(L, "attempt to use as %s a %s value", expected,
                      lua_typename(L, lua_type(L, idx)));
}

/// LuaPlus's GetBoolean of an argument: nil is false, a boolean is itself,
/// anything else raises its type error.
inline bool arg_boolean(lua_State* L, int idx) {
    if (lua_isnil(L, idx)) {
        return false;
    }
    if (!lua_isboolean(L, idx)) {
        arg_type_error(L, idx, "boolean");
    }
    return lua_toboolean(L, idx) != 0;
}

} // namespace osc::lua
