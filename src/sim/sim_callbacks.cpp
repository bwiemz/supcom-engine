// SimCallbacks: a UI script's request for the sim to do something. Moho runs
// them as commands, inside a tick; here too (SimState::dispatch_due_commands),
// so in multiplayer every peer runs each one on the same tick.

#include "sim/entity.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <string>
#include <type_traits>
#include <unordered_set>
#include <variant>

namespace osc::sim {

namespace {

/// Human input off while a callback runs: it already runs on every peer, so
/// the orders it issues apply directly rather than being broadcast again.
struct NotHumanInput {
    SimState& sim;
    bool was;
    explicit NotHumanInput(SimState& s) : sim(s), was(s.human_input_active()) {
        sim.set_human_input_active(false);
    }
    ~NotHumanInput() { sim.set_human_input_active(was); }
    NotHumanInput(const NotHumanInput&) = delete;
    NotHumanInput& operator=(const NotHumanInput&) = delete;
};

/// ProcessInfo(action, value): the unit's own method of that name, with the
/// value as a boolean -- for the settings the UI sends this way only (retail:
/// auto mode, repeat build; FAF's construction panel also pauses factories),
/// never an arbitrary method a UI script names.
void process_info(SimState& sim, lua_State* L, const SimCallbackEntry& cb) {
    static const std::unordered_set<std::string> kActions = {"SetAutoMode", "SetRepeatQueue",
                                                             "SetPaused"};
    auto get = [&](const char* key) -> std::string {
        auto it = cb.args.find(key);
        if (it == cb.args.end()) return {};
        if (const auto* str = std::get_if<std::string>(&it->second)) return *str;
        return {};
    };
    const std::string action = get("Action");
    const bool value = get("Value") == "true";
    if (!kActions.count(action)) {
        spdlog::warn("ProcessInfo: unsupported action '{}'", action);
        return;
    }
    for (u32 eid : cb.unit_ids) {
        auto* e = sim.entity_registry().find(eid);
        if (!e || !e->is_unit() || e->destroyed() || e->lua_table_ref() < 0) continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->lua_table_ref());
        const int unit = lua_gettop(L);
        lua_pushstring(L, action.c_str());
        lua_gettable(L, unit);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, unit);
            lua_pushboolean(L, value ? 1 : 0);
            if (lua_pcall(L, 2, 0, 0) != 0) {
                spdlog::warn("ProcessInfo {} error: {}", action, lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // unit table
    }
}

/// Push /lua/SimCallbacks.lua's module table, or return false.
bool push_callbacks_module(lua_State* L) {
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    lua_pushstring(L, "/lua/SimCallbacks.lua");
    if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) return true;
    if (lua_isstring(L, -1)) spdlog::warn("SimCallback import error: {}", lua_tostring(L, -1));
    lua_pop(L, 1);
    return false;
}

} // namespace

void SimState::run_sim_callback(const SimCallbackEntry& cb) {
    lua_State* L = L_;
    const int top = lua_gettop(L);
    const NotHumanInput not_human(*this);

    if (cb.func_name == kProcessInfoCallback) {
        process_info(*this, L, cb);
        lua_settop(L, top);
        return;
    }

    if (!push_callbacks_module(L)) {
        lua_settop(L, top);
        return;
    }
    lua_pushstring(L, "DoCallback");
    lua_rawget(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return;
    }

    // DoCallback(func name, args, units)
    lua_pushstring(L, cb.func_name.c_str());
    lua_newtable(L);
    for (const auto& [key, val] : cb.args) {
        lua_pushstring(L, key.c_str());
        std::visit(
            [&](const auto& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::string>) lua_pushstring(L, v.c_str());
                else if constexpr (std::is_same_v<T, f64>) lua_pushnumber(L, v);
                else lua_pushboolean(L, v ? 1 : 0);
            },
            val);
        lua_rawset(L, -3);
    }
    if (!cb.unit_ids.empty()) {
        lua_newtable(L);
        const int units = lua_gettop(L);
        int idx = 1;
        for (u32 eid : cb.unit_ids) {
            auto* e = entity_registry_.find(eid);
            if (e && e->is_unit() && !e->destroyed() && e->lua_table_ref() >= 0) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, e->lua_table_ref());
                lua_rawseti(L, units, idx++);
            }
        }
    } else {
        lua_pushnil(L);
    }
    if (lua_pcall(L, 3, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        spdlog::warn("SimCallback '{}' error: {}", cb.func_name, err ? err : "(unknown)");
    }
    lua_settop(L, top);
}

} // namespace osc::sim
