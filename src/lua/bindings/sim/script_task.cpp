// Script orders' tasks: moho.ScriptTask_Methods (M206w). A task object is an
// instance of its script class (retail's ScriptTask and what derives from
// it); the unit runs it (sim/unit_script_task.cpp).

#include "lua/moho_bindings_internal.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lua.h>
}

namespace osc::lua {

namespace {

/// task:GetUnit(): the task's unit (its object, also while the unit is
/// being destroyed, as Moho's task reaches its unit then), or nil.
int task_GetUnit(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, "_c_unit");
    lua_rawget(L, 1);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
    return 1;
}

/// task:SetAIResult(result): the order's AI result (retail's AIRESULT:
/// 0 Unknown, 1 Success, 2 Fail, 3 Ignored), kept on the unit.
int task_SetAIResult(lua_State* L) {
    if (!lua_istable(L, 1) || !lua_isnumber(L, 2)) return 0;
    lua_pushstring(L, "_c_unit");
    lua_rawget(L, 1);
    const int unit = lua_gettop(L);
    if (lua_istable(L, unit)) {
        if (auto* e = check_entity(L, unit); e && e->is_unit())
            static_cast<sim::Unit*>(e)->set_script_task_result(
                static_cast<i32>(lua_tonumber(L, 2)));
    }
    lua_settop(L, unit - 1);
    return 0;
}

} // namespace

// clang-format off
const MethodEntry script_task_methods[] = {
    {"GetUnit",     task_GetUnit},
    {"SetAIResult", task_SetAIResult},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
