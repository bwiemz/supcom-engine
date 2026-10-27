// SimCallbacks: a UI script's request for the sim to do something. Moho runs
// them as commands, in a beat; here too (SimState::dispatch_due_commands),
// so in multiplayer every peer runs each one in the same beat.

#include "blueprints/blueprint_store.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/build_placement.hpp"
#include "sim/lua_bytes.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/work_range.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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

/// A number argument within [lo, hi], else nullptr. The check is a range
/// that NaN fails: callbacks arrive over the network, and a NaN cast to an
/// integer is undefined (and may differ between peers).
const f64* number_in(const SimCallbackEntry& cb, const char* key, f64 lo, f64 hi) {
    const auto* v = arg<f64>(cb, key);
    return v && *v >= lo && *v <= hi ? v : nullptr;
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
        for_each_unit(sim, cb, [&](Unit& u) { u.pause(L, *flag); });
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
        for_each_unit(sim, cb, [&](Unit& u) { u.pause(L, value); });
    } else if (name == "SetAutoMode") {
        for_each_unit(sim, cb, [&](Unit& u) { set_auto_mode(L, u, value); });
    } else if (name == "SetRepeatQueue") {
        for_each_unit(sim, cb, [&](Unit& u) { u.set_repeat_queue(value); });
    } else if (name == "CustomName") {
        // UserUnit:SetCustomName (Moho's ProcessInfoPair "CustomName").
        if (text) for_each_unit(sim, cb, [&](Unit& u) { u.set_custom_name(*text); });
    } else if (name == "PlayNoStagingPlatformsVO") {
        for_each_unit(sim, cb,
                      [&](Unit& u) { sim.brain_hears(u.army(), "OnPlayNoStagingPlatformsVO"); });
    } else if (name == "PlayBusyStagingPlatformsVO") {
        for_each_unit(sim, cb,
                      [&](Unit& u) { sim.brain_hears(u.army(), "OnPlayBusyStagingPlatformsVO"); });
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
    const auto* index = number_in(cb, "Index", 1, 1e6);
    const auto* count = number_in(cb, "Count", 1, 1e6);
    if (!index || !count) return;
    for_each_unit(sim, cb, [&](Unit& u) {
        u.decrease_build_count(static_cast<int>(*index), static_cast<int>(*count),
                               sim.entity_registry(), L);
    });
}

/// IncreaseBuildCountInQueue: more of one of a factory's queued blueprints.
/// FA's queue display asks for 1 or 5 at a time; a request comes from the
/// network, so a count past kMaxBuildCountIncrease (which would only grow
/// the queue by that many orders) is refused.
constexpr f64 kMaxBuildCountIncrease = 1000;
void increase_build_count(SimState& sim, const SimCallbackEntry& cb) {
    const auto* index = number_in(cb, "Index", 1, 1e6);
    const auto* count = number_in(cb, "Count", 1, kMaxBuildCountIncrease);
    if (!index || !count) return;
    for_each_unit(sim, cb, [&](Unit& u) {
        u.increase_build_count(static_cast<int>(*index), static_cast<int>(*count));
    });
}

/// A dropped player's army is defeated.
void defeat_dropped_army(SimState& sim, const SimCallbackEntry& cb) {
    const auto* army = number_in(cb, "Army", 0, static_cast<f64>(sim.army_count()) - 1);
    if (!army) return;
    sim.defeat_army(static_cast<i32>(*army));
}

/// A dropped player's command source ends.
void terminate_source(SimState& sim, const SimCallbackEntry& cb) {
    const auto* source = number_in(cb, "Source", 0, static_cast<f64>(kEngineSource) - 1);
    if (!source) return;
    sim.terminate_command_source(static_cast<u32>(*source));
}

bool retargetable(CommandType type) {
    switch (type) {
    case CommandType::Move:
    case CommandType::Attack:
    case CommandType::Guard:
    case CommandType::Patrol:
    case CommandType::AggressiveMove:
    case CommandType::BuildMobile:
    case CommandType::Reclaim:
    case CommandType::Repair:
    case CommandType::Capture:
    case CommandType::TransportLoad:
    case CommandType::TransportUnload:
    case CommandType::Nuke:
    case CommandType::Tactical:
    case CommandType::Overcharge:
    case CommandType::Sacrifice:
    case CommandType::Teleport:
    case CommandType::Ferry:
    case CommandType::Dock: return true;
    default: return false;
    }
}

void forget_target(UnitCommand& c) {
    c.approached = false;
    c.site_cleared = false;
    c.prepared = false; // Moho's build task rewinds to TASKSTATE_Preparing
    c.clearing_prop_id = 0;
    c.clearing_approached = false;
    c.engaged = false;
    c.in_band = false;
    c.patrol_claimed.clear();
}

void set_command_target(SimState& sim, lua_State* L, const SimCallbackEntry& cb) {
    const auto* command = number_in(cb, "Command", 1, 4294967295.0);
    if (!command) {
        return;
    }
    std::vector<SimState::CommandToRetarget> orders;
    for_each_unit(sim, cb, [&](Unit& u) {
        for (UnitCommand* c : u.commands_with_id(static_cast<u32>(*command))) {
            if (c->from_patrol || c->from_guard) {
                continue;
            }
            const bool front = !u.command_queue().empty() && &u.command_queue().front() == c;
            orders.push_back({&u, c, front});
        }
    });
    std::optional<std::pair<f32, f32>> footprint;
    if (!orders.empty() && orders.front().command->type == CommandType::BuildMobile) {
        footprint = blueprint_footprint_cells(L, orders.front().command->blueprint_id);
    }
    sim.retarget_command(cb, orders, footprint);
}

void remove_command(SimState& sim, lua_State* L, const SimCallbackEntry& cb) {
    const auto* command = number_in(cb, "Command", 1, 4294967295.0);
    if (!command) {
        return;
    }
    SimContext ctx = sim.sim_context();
    ctx.L = L;
    for_each_unit(sim, cb, [&](Unit& u) { u.remove_command(static_cast<u32>(*command), ctx); });
}

void set_command_type(SimState& sim, const SimCallbackEntry& cb) {
    const auto* command = number_in(cb, "Command", 1, 4294967295.0);
    const auto* type = number_in(cb, "Type", 0, 255);
    if (!command || !type || static_cast<CommandType>(*type) != CommandType::Patrol) {
        return;
    }
    for_each_unit(sim, cb, [&](Unit& u) {
        for (UnitCommand* c : u.commands_with_id(static_cast<u32>(*command))) {
            if (c->type == CommandType::Move) {
                c->type = CommandType::Patrol;
                forget_target(*c);
            }
        }
    });
}

/// Whether the blueprint `id` makes a unit that moves (Moho's IsMobile: a
/// Physics.MotionType other than RULEUMT_None).
bool blueprint_is_mobile(lua_State* L, const std::string& id) {
    const int top = lua_gettop(L);
    bool mobile = false;
    if (blueprints::push_blueprint(L, id)) {
        lua_pushstring(L, "Physics");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "MotionType");
            lua_rawget(L, -2);
            mobile = lua_type(L, -1) == LUA_TSTRING &&
                     std::string_view(lua_tostring(L, -1)) != "RULEUMT_None";
        }
    }
    lua_settop(L, top);
    return mobile;
}

/// Moho's Sim::CreateUnit for CMDST_CreateUnit (faf-re Sim.cpp 0x00748AA0):
/// a cheat, so nothing unless cheats are on; nothing for an army out of the
/// game or a blueprint that is no unit's. Otherwise a complete unit, its
/// scripts told as CreateUnitHPR's are, at X, Z on the ground, facing
/// Heading. A structure stands on its footprint's cells, as
/// CreateUnitAtMouse places one (its anchor the cell its corner is in).
void create_unit(SimState& sim, lua_State* L, const SimCallbackEntry& cb) {
    if (!sim.cheats_enabled()) return;
    const auto* bp = arg<std::string>(cb, "Blueprint");
    const auto* army = number_in(cb, "Army", 0, static_cast<f64>(sim.army_count()) - 1);
    const auto* x = number_in(cb, "X", -1e6, 1e6);
    const auto* z = number_in(cb, "Z", -1e6, 1e6);
    const auto* heading = number_in(cb, "Heading", -1e6, 1e6);
    if (!bp || !army || !x || !z || !heading) return;
    const ArmyBrain* brain = sim.get_army(static_cast<i32>(*army));
    if (!brain || brain->is_defeated()) return;
    const auto* entry = sim.blueprint_store() ? sim.blueprint_store()->find(*bp) : nullptr;
    if (!entry || entry->type != blueprints::BlueprintType::Unit) {
        spdlog::info("CreateUnit: unresolved blueprint '{}' requested by source {}.", *bp,
                     sim.current_command_source());
        return;
    }
    auto at_x = static_cast<f32>(*x);
    auto at_z = static_cast<f32>(*z);
    if (!blueprint_is_mobile(L, entry->id)) {
        const auto [size_x, size_z] = blueprint_footprint_cells(L, entry->id);
        at_x = std::trunc(at_x - size_x * 0.5f) + size_x * 0.5f;
        at_z = std::trunc(at_z - size_z * 0.5f) + size_z * 0.5f;
    }
    const f32 at_y = sim.terrain() ? sim.terrain()->get_surface_height(at_x, at_z) : 0.0f;
    lua_pushstring(L, "CreateUnitHPR");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L, -1)) return;
    lua_pushstring(L, entry->id.c_str());
    lua_pushnumber(L, *army + 1);
    lua_pushnumber(L, at_x);
    lua_pushnumber(L, at_y);
    lua_pushnumber(L, at_z);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, *heading); // about the Y axis
    lua_pushnumber(L, 0);
    if (lua_pcall(L, 8, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        spdlog::warn("CreateUnit '{}': {}", *bp, err ? err : "(unknown)");
    }
}

/// Moho's Sim::DecreaseCommandCount (faf-re Sim.cpp 0x007496E0): order
/// `Command`, if its army is one the command's source may command, made
/// `Count` fewer (CUnitCommand::DecreaseCount). A factory build of more
/// loses that many; any other order, or one left at none, leaves every
/// queue that holds it.
void decrease_command_count(SimState& sim, lua_State* L, const SimCallbackEntry& cb) {
    const auto* command = number_in(cb, "Command", 1, 4294967295.0);
    const auto* count = number_in(cb, "Count", 1, 1e6);
    if (!command || !count) return;
    const auto id = static_cast<u32>(*command);
    const auto fewer = static_cast<i32>(*count);
    std::vector<u32> holders;
    sim.entity_registry().for_each_unit([&](Entity& e) {
        auto& u = static_cast<Unit&>(e);
        if (!u.destroyed() && !u.commands_with_id(id).empty()) holders.push_back(u.entity_id());
    });
    if (holders.empty()) return;
    const Entity* first = sim.entity_registry().find(holders.front());
    if (!first || !sim.okay_to_mess_with(first->army())) return;
    SimContext ctx = sim.sim_context();
    ctx.L = L;
    for (const u32 holder : holders) {
        auto* e = sim.entity_registry().find(holder);
        if (!e || !e->is_unit() || e->destroyed()) continue;
        auto& u = *static_cast<Unit*>(e);
        bool gone = false;
        for (UnitCommand* c : u.commands_with_id(id)) {
            if (c->type == CommandType::BuildFactory && c->count > fewer) c->count -= fewer;
            else gone = true;
        }
        if (gone) u.remove_command(id, ctx);
    }
}

void push_arg(lua_State* L, const SimCallbackArg& arg) {
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) lua_pushstring(L, v.c_str());
            else if constexpr (std::is_same_v<T, f64>) lua_pushnumber(L, v);
            else lua_pushboolean(L, v ? 1 : 0);
        },
        arg);
}

/// FA's /lua/SimCallbacks.lua DoCallback(func name, args, units).
void do_callback(SimState& sim, lua_State* L, const SimCallbackEntry& cb) {
    if (!push_callbacks_module(L)) return;
    lua_pushstring(L, "DoCallback");
    lua_rawget(L, -2);
    if (!lua_isfunction(L, -1)) return;

    lua_pushstring(L, cb.func_name.c_str());
    if (cb.lua_args) {
        if (!push_lua_bytes(L, *cb.lua_args)) {
            lua_pushnil(L);
        }
    } else if (cb.value) {
        push_arg(L, *cb.value);
    } else {
        lua_newtable(L);
        for (const auto& [key, val] : cb.args) {
            lua_pushstring(L, key.c_str());
            push_arg(L, val);
            lua_rawset(L, -3);
        }
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

namespace {

/// Moho's post-load (Sim.cpp, after a load): SimSync.SyncPlayableRect of
/// the playable area, then the global OnPostLoad(); a script error in
/// either is logged, as Moho's Warnf.
void post_load(SimState& sim, lua_State* L) {
    sim.end_pause();
    sim.note_post_load();
    const auto call = [&](const char* what, int args) {
        if (lua_pcall(L, args, 0, 0) != 0) {
            spdlog::warn("{} failed: {}", what, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    };
    if (sim.has_playable_rect()) {
        lua_pushstring(L, "import");
        lua_gettable(L, LUA_GLOBALSINDEX);
        if (lua_isfunction(L, -1)) {
            lua_pushstring(L, "/lua/SimSync.lua");
            if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
                lua_pushstring(L, "SyncPlayableRect");
                lua_gettable(L, -2);
                if (lua_isfunction(L, -1)) {
                    // A Rect, as Moho's SCR_ToLua<gpg::Rect2<int>> makes it:
                    // x0, y0, x1, y1.
                    lua_newtable(L);
                    const f32 corners[4] = {sim.playable_x0(), sim.playable_z0(), sim.playable_x1(),
                                            sim.playable_z1()};
                    const char* keys[4] = {"x0", "y0", "x1", "y1"};
                    for (int i = 0; i < 4; ++i) {
                        lua_pushstring(L, keys[i]);
                        lua_pushnumber(L, corners[i]);
                        lua_rawset(L, -3);
                    }
                    call("SyncPlayableRect in /lua/SimSync.lua", 1);
                } else {
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1); // the module, or import's error
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pushstring(L, "OnPostLoad");
    lua_gettable(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) call("OnPostLoad()", 0);
    else lua_pop(L, 1);
}

} // namespace

void SimState::retarget_command(const SimCallbackEntry& cb,
                                const std::vector<CommandToRetarget>& orders,
                                std::optional<std::pair<f32, f32>> footprint) const {
    if (orders.empty() || !retargetable(orders.front().command->type)) {
        return;
    }
    const UnitCommand& order = *orders.front().command;
    for (const auto& o : orders) {
        if (o.command->type == CommandType::BuildMobile && o.unit->build_target_id() != 0 &&
            o.front) {
            return;
        }
    }
    if (order.target_id != 0) {
        const auto* target = number_in(cb, "Target", 1, 4294967295.0);
        const Entity* old = entity_registry_.find(order.target_id);
        const Entity* to = target ? entity_registry_.find(static_cast<u32>(*target)) : nullptr;
        if (!old || old->destroyed() || !to || to->destroyed() || to->army() != old->army() ||
            to->is_unit() != old->is_unit()) {
            return;
        }
        for (const auto& o : orders) {
            o.command->target_id = to->entity_id();
            o.command->target_pos = to->position();
            forget_target(*o.command);
        }
        return;
    }
    const auto* x = number_in(cb, "X", -1e6, 1e6);
    const auto* z = number_in(cb, "Z", -1e6, 1e6);
    if (!x || !z) {
        return;
    }
    Vector3 point{static_cast<f32>(*x), 0.0f, static_cast<f32>(*z)};
    if (order.type == CommandType::BuildMobile && footprint) {
        snap_structure_center(point.x, point.z, footprint->first, footprint->second);
    }
    Vector3 mean{0.0f, 0.0f, 0.0f};
    for (const auto& o : orders) {
        mean.x += o.command->target_pos.x / static_cast<f32>(orders.size());
        mean.z += o.command->target_pos.z / static_cast<f32>(orders.size());
    }
    const bool no_rush = no_rush_active();
    for (const auto& o : orders) {
        UnitCommand* c = o.command;
        Vector3 to = point;
        if (!c->formation.empty()) {
            to.x = c->target_pos.x + point.x - mean.x;
            to.z = c->target_pos.z + point.z - mean.z;
        }
        to = clamp_to_playable(to, o.unit->army());
        if (no_rush && (c->type == CommandType::Move || c->type == CommandType::Attack)) {
            to = clamp_to_no_rush(*o.unit, to);
        }
        to.y = terrain() ? terrain()->get_surface_height(to.x, to.z) : 0.0f;
        c->target_pos = to;
        forget_target(*c);
    }
}

namespace {

/// Lua's base library loaders: dofile and require run a file from this
/// machine's own disk, the others make code of data. Whether the function at
/// `idx` is one, by name or under another (`x = dofile`).
bool is_base_loader(lua_State* L, const std::string& name, int idx) {
    constexpr std::array<const char*, 5> kLoaders{"dofile", "require", "loadfile", "loadstring",
                                                  "loadlib"};
    const lua_CFunction f = lua_tocfunction(L, idx);
    for (const char* loader : kLoaders) {
        if (name == loader) return true;
        lua_pushstring(L, loader);
        lua_rawget(L, LUA_GLOBALSINDEX);
        const bool same = f && lua_tocfunction(L, -1) == f;
        lua_pop(L, 1);
        if (same) return true;
    }
    return false;
}

/// Moho's Sim::ExecuteLuaInSim (0x00749A70): with cheats on, the sim's global
/// `Func` called with the value lua_args holds; an error, or a name that
/// isn't a function, logged. Moho calls any global; here a base library
/// loader is refused, since every peer runs the command and another
/// player's could otherwise run a file from this one's disk. Its callers
/// name the sim's own functions (SetArmyStatsSyncArmy, a path debugger's).
void exec_lua_in_sim(SimState& sim, lua_State* L, const SimCallbackEntry& cb) {
    if (!sim.cheats_enabled()) return;
    const auto* name = arg<std::string>(cb, "Func");
    if (!name) return;
    lua_pushlstring(L, name->data(), name->size());
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L, -1)) {
        spdlog::warn("Error running global lua function '{}':\nnot a function", *name);
        return;
    }
    if (is_base_loader(L, *name, -1)) {
        spdlog::warn("ExecLuaInSim: '{}' is one of Lua's loaders; not run", *name);
        return;
    }
    if (!cb.lua_args || !push_lua_bytes(L, *cb.lua_args)) lua_pushnil(L);
    if (lua_pcall(L, 1, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        spdlog::warn("Error running global lua function '{}':\n{}", *name, err ? err : "");
    }
}

/// Moho's Sim::WarpEntity (0x00748CD0, ICommandSink's
/// WarpEntity): a cheat, so nothing unless cheats are on; the unit warped as
/// Unit::Warp places it, its height and layer those of the place.
void warp_entity(SimState& sim, lua_State* L, const SimCallbackEntry& cb) {
    if (!sim.cheats_enabled()) return;
    const auto* id = number_in(cb, "Entity", 0, 4294967295.0);
    const auto* x = number_in(cb, "X", -1e6, 1e6);
    const auto* y = number_in(cb, "Y", -1e6, 1e6);
    const auto* z = number_in(cb, "Z", -1e6, 1e6);
    if (!id || !x || !y || !z) return;
    Entity* e = sim.entity_registry().find(static_cast<u32>(*id));
    if (!e || e->destroyed() || !e->is_unit()) return;
    static_cast<Unit*>(e)->warp(
        sim.terrain(), {static_cast<f32>(*x), static_cast<f32>(*y), static_cast<f32>(*z)}, L);
}

} // namespace

void SimState::run_sim_callback(const SimCallbackEntry& cb) {
    lua_State* L = L_;
    const int top = lua_gettop(L);
    const NotHumanInput not_human(*this);
    if (cb.func_name == kProcessInfoCallback) process_info(*this, L, cb);
    else if (cb.func_name == kUnitSettingCallback) unit_setting(*this, L, cb);
    else if (cb.func_name == kDecreaseBuildCountCallback) decrease_build_count(*this, L, cb);
    else if (cb.func_name == kIncreaseBuildCountCallback) increase_build_count(*this, cb);
    else if (cb.func_name == kDefeatArmyCallback) defeat_dropped_army(*this, cb);
    else if (cb.func_name == kCommandSourceTerminatedCallback) terminate_source(*this, cb);
    else if (cb.func_name == kPostLoadCallback) post_load(*this, L);
    else if (cb.func_name == kSaveGameCallback) {
        const auto name = cb.args.find("Name");
        if (name != cb.args.end() && std::holds_alternative<std::string>(name->second))
            request_save(std::get<std::string>(name->second));
    } else if (cb.func_name == kSetCommandTargetCallback) set_command_target(*this, L, cb);
    else if (cb.func_name == kRemoveCommandCallback) remove_command(*this, L, cb);
    else if (cb.func_name == kSetCommandTypeCallback) set_command_type(*this, cb);
    else if (cb.func_name == kCreateUnitCallback) create_unit(*this, L, cb);
    else if (cb.func_name == kWarpEntityCallback) warp_entity(*this, L, cb);
    else if (cb.func_name == kExecLuaInSimCallback) exec_lua_in_sim(*this, L, cb);
    else if (cb.func_name == kDecreaseCommandCountCallback) decrease_command_count(*this, L, cb);
    else do_callback(*this, L, cb);
    lua_settop(L, top);
}

} // namespace osc::sim
