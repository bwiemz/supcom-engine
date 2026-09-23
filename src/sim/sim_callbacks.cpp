// SimCallbacks: a UI script's request for the sim to do something. Moho runs
// them as commands, inside a tick; here too (SimState::dispatch_due_commands),
// so in multiplayer every peer runs each one on the same tick.

#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <string>
#include <type_traits>
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

template <typename T> const T* arg(const SimCallbackEntry& cb, const char* key) {
    auto it = cb.args.find(key);
    return it == cb.args.end() ? nullptr : std::get_if<T>(&it->second);
}

/// The live units a callback names, in its order.
template <typename Fn> void for_each_unit(SimState& sim, const SimCallbackEntry& cb, Fn fn) {
    for (u32 eid : cb.unit_ids) {
        auto* e = sim.entity_registry().find(eid);
        if (!e || !e->is_unit() || e->destroyed()) continue;
        fn(*static_cast<Unit*>(e));
    }
}

/// self:OnScriptBitSet(bit) / OnScriptBitClear(bit).
void script_bit_hook(lua_State* L, const Unit& u, i32 bit, bool set) {
    if (u.lua_table_ref() < 0) return;
    const char* name = set ? "OnScriptBitSet" : "OnScriptBitClear";
    lua_rawgeti(L, LUA_REGISTRYINDEX, u.lua_table_ref());
    const int self = lua_gettop(L);
    lua_pushstring(L, name);
    lua_gettable(L, self);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, self);
        lua_pushnumber(L, bit);
        if (lua_pcall(L, 2, 0, 0) != 0) spdlog::warn("{} error: {}", name, lua_tostring(L, -1));
    }
    lua_settop(L, self - 1);
}

// A setting applies to each unit, and a unit whose setting changes gets the
// script hook Moho calls for it.

void set_paused(lua_State* L, Unit& u, bool v) {
    if (u.is_paused() == v) return;
    u.pause(v);
    u.call_lua_method(L, v ? "OnPaused" : "OnUnpaused");
}

void set_auto_mode(lua_State* L, Unit& u, bool v) {
    if (u.auto_mode() == v) return;
    u.set_auto_mode(v);
    u.call_lua_method(L, v ? "OnAutoModeOn" : "OnAutoModeOff");
}

void set_script_bit(lua_State* L, Unit& u, i32 bit, bool v) {
    if (u.get_script_bit(bit) == v) return;
    u.set_script_bit(bit, v);
    script_bit_hook(L, u, bit, v);
}

/// The UI's unit settings (kUnitSettingCallback).
void unit_setting(SimState& sim, lua_State* L, const SimCallbackEntry& cb) {
    const auto* setting = arg<std::string>(cb, "Setting");
    const auto* flag = arg<bool>(cb, "Value");
    const auto* number = arg<f64>(cb, "Value");
    const auto* bit = arg<f64>(cb, "Bit");
    const std::string name = setting ? *setting : std::string();

    if (name == "Paused" && flag) {
        for_each_unit(sim, cb, [&](Unit& u) { set_paused(L, u, *flag); });
    } else if (name == "AutoMode" && flag) {
        for_each_unit(sim, cb, [&](Unit& u) { set_auto_mode(L, u, *flag); });
    } else if (name == "AutoSurfaceMode" && flag) {
        for_each_unit(sim, cb, [&](Unit& u) { u.set_auto_surface_mode(*flag); });
    } else if (name == "FireState" && number && (*number == 0 || *number == 1 || *number == 2)) {
        const auto state = static_cast<i32>(*number);
        for_each_unit(sim, cb, [&](Unit& u) { u.set_fire_state(state); });
    } else if (name == "ScriptBit" && flag && bit && *bit >= 0 && *bit <= 8 &&
               *bit == static_cast<f64>(static_cast<i32>(*bit))) {
        const auto b = static_cast<i32>(*bit);
        for_each_unit(sim, cb, [&](Unit& u) { set_script_bit(L, u, b, *flag); });
    } else {
        spdlog::warn("unit setting: unsupported '{}'", name);
    }
}

/// ProcessInfo(action, value): UserUnit's request for one of the settings
/// the UI sends this way (retail: auto mode, repeat build; FAF's
/// construction panel also pauses factories). The value arrives as text.
void process_info(SimState& sim, lua_State* L, const SimCallbackEntry& cb) {
    const auto* action = arg<std::string>(cb, "Action");
    const auto* text = arg<std::string>(cb, "Value");
    const bool value = text && *text == "true";
    const std::string name = action ? *action : std::string();
    if (name == "SetPaused") {
        for_each_unit(sim, cb, [&](Unit& u) { set_paused(L, u, value); });
    } else if (name == "SetAutoMode") {
        for_each_unit(sim, cb, [&](Unit& u) { set_auto_mode(L, u, value); });
    } else if (name == "SetRepeatQueue") {
        for_each_unit(sim, cb, [&](Unit& u) { u.set_repeat_queue(value); });
    } else {
        spdlog::warn("ProcessInfo: unsupported action '{}'", name);
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

/// DecreaseBuildCountInQueue: fewer of one of a factory's queued blueprints.
void decrease_build_count(SimState& sim, lua_State* L, const SimCallbackEntry& cb) {
    const auto* index = arg<f64>(cb, "Index");
    const auto* count = arg<f64>(cb, "Count");
    if (!index || !count || *index < 1 || *index > 1e6 || *count < 1 || *count > 1e6) return;
    for_each_unit(sim, cb, [&](Unit& u) {
        u.decrease_build_count(static_cast<int>(*index), static_cast<int>(*count),
                               sim.entity_registry(), L);
    });
}

/// A dropped player's army is defeated.
void defeat_dropped_army(SimState& sim, const SimCallbackEntry& cb) {
    const auto* army = arg<f64>(cb, "Army");
    if (!army || *army < 0 || *army >= static_cast<f64>(sim.army_count())) return;
    sim.defeat_army(static_cast<i32>(*army));
}

/// FA's /lua/SimCallbacks.lua DoCallback(func name, args, units).
void do_callback(SimState& sim, lua_State* L, const SimCallbackEntry& cb) {
    if (!push_callbacks_module(L)) return;
    lua_pushstring(L, "DoCallback");
    lua_rawget(L, -2);
    if (!lua_isfunction(L, -1)) return;

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
        for_each_unit(sim, cb, [&](const Unit& u) {
            if (u.lua_table_ref() < 0) return;
            lua_rawgeti(L, LUA_REGISTRYINDEX, u.lua_table_ref());
            lua_rawseti(L, units, idx++);
        });
    } else {
        lua_pushnil(L);
    }
    if (lua_pcall(L, 3, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        spdlog::warn("SimCallback '{}' error: {}", cb.func_name, err ? err : "(unknown)");
    }
}

} // namespace

void SimState::run_sim_callback(const SimCallbackEntry& cb) {
    lua_State* L = L_;
    const int top = lua_gettop(L);
    const NotHumanInput not_human(*this);
    if (cb.func_name == kProcessInfoCallback) process_info(*this, L, cb);
    else if (cb.func_name == kUnitSettingCallback) unit_setting(*this, L, cb);
    else if (cb.func_name == kDecreaseBuildCountCallback) decrease_build_count(*this, L, cb);
    else if (cb.func_name == kDefeatArmyCallback) defeat_dropped_army(*this, cb);
    else do_callback(*this, L, cb);
    lua_settop(L, top);
}

} // namespace osc::sim
