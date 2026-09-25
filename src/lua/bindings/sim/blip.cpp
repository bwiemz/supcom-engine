// Intel blips: moho.blip_methods.
// Split out of moho_bindings.cpp by class (M191 step 2); what the files
// share is declared in lua/moho_bindings_internal.hpp.

#include "lua/moho_bindings.hpp"
#include "lua/moho_bindings_internal.hpp"
#include "lua/lua_stubs.hpp"
#include "core/dmath.hpp"
#include "sim/blueprint_categories.hpp"
#include "lua/category_utils.hpp"
#include "video/video_decoder.hpp"
#include "map/scmap_parser.hpp"
#include "lua/factory_queue.hpp"
#include "lua/order_helpers.hpp"
#include "lua/lua_state.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/army_brain.hpp"
#include "sim/build_placement.hpp"
#include "sim/bone_data.hpp"
#include "sim/collision.hpp"
#include "sim/entity.hpp"
#include "sim/entity_registry.hpp"
#include "sim/ieffect.hpp"
#include "sim/manipulator.hpp"
#include "core/test_status.hpp"
#include "sim/category_expr.hpp"
#include "sim/prop.hpp"
#include "sim/prop_script.hpp"
#include "sim/sim_state.hpp"
#include "sim/collision_beam.hpp"
#include "sim/projectile_script.hpp"
#include "sim/thread_manager.hpp"
#include "map/visibility_grid.hpp"
#include "sim/unit.hpp"
#include "sim/navigator.hpp"
#include "sim/platoon.hpp"
#include "sim/projectile.hpp"
#include "sim/shield.hpp"
#include "sim/unit_command.hpp"
#include "map/pathfinder.hpp"
#include "map/pathfinding_grid.hpp"
#include "sim/weapon.hpp"
#include "blueprints/blueprint_store.hpp"
#include "audio/sound_manager.hpp"
#include "ui/ui_control.hpp"
#include "ui/world_view.hpp"
#include "ui/font_metrics_provider.hpp"
#include "ui/keymap.hpp"
#include "ui/wld_ui_provider.hpp"
#include "sim/sim_callback_queue.hpp"
#include "map/terrain.hpp"
#include "vfs/virtual_file_system.hpp"
#include "core/front_end_data.hpp"
#include "core/game_state.hpp"
#include "core/localization.hpp"
#include "core/preferences.hpp"
#include "lua/beat_system.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/sim_sync.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>
#include <spdlog/spdlog.h>
#include <lua.h>
#include <lauxlib.h>

#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

// ---------------------------------------------------------------------------
// Blip methods — real implementations using visibility grid
// ---------------------------------------------------------------------------

// Helper: extract entity from blip table (same _c_object pattern as check_entity)
/// Read _c_entity_id from blip table (arg 1).
static u32 get_blip_entity_id(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_c_entity_id");
    lua_rawget(L, 1);
    u32 id = lua_isnumber(L, -1) ? static_cast<u32>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);
    return id;
}

/// The live entity behind a blip, or nullptr. Resolved by id: AI scripts
/// keep blips across ticks, and the entity may be gone.
static sim::Entity* check_blip_entity(lua_State* L) {
    auto* sim = get_sim(L);
    const u32 id = get_blip_entity_id(L);
    if (!sim || id == 0) return nullptr;
    auto* e = sim->entity_registry().find(id);
    return (e && !e->destroyed()) ? e : nullptr;
}

/// Read _c_req_army from blip table (arg 1). Returns 0-based army, -1 if absent.
static i32 get_blip_req_army(lua_State* L) {
    if (!lua_istable(L, 1)) return -1;
    lua_pushstring(L, "_c_req_army");
    lua_rawget(L, 1);
    i32 army = lua_isnumber(L, -1) ? static_cast<i32>(lua_tonumber(L, -1)) : -1;
    lua_pop(L, 1);
    return army;
}

// --- Blip methods (dead-reckoning + stealth-aware) ---

static int blip_GetPosition(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (e && !e->destroyed()) {
        // Entity alive — check if requesting army has current intel
        i32 req_army = get_blip_req_army(L);
        if (req_army >= 0 && req_army != e->army()) {
            auto* sim = get_sim(L);
            if (sim && sim->has_any_intel(e, static_cast<u32>(req_army))) {
                push_vector3(L, e->position());
            } else {
                // Dead-reckoning: return cached position
                u32 eid = e->entity_id();
                auto* snap = sim ? sim->get_blip_snapshot(
                    eid, static_cast<u32>(req_army)) : nullptr;
                if (snap)
                    push_vector3(L, snap->last_known_position);
                else
                    push_vector3(L, e->position()); // fallback
            }
        } else {
            push_vector3(L, e->position()); // own army sees real position
        }
    } else {
        // Entity destroyed — use blip cache
        u32 eid = get_blip_entity_id(L);
        i32 req_army = get_blip_req_army(L);
        auto* sim = get_sim(L);
        if (sim && req_army >= 0) {
            auto* snap = sim->get_blip_snapshot(eid,
                                                 static_cast<u32>(req_army));
            if (snap)
                push_vector3(L, snap->last_known_position);
            else
                push_vector3(L, {0, 0, 0});
        } else {
            push_vector3(L, {0, 0, 0});
        }
    }
    return 1;
}

static int blip_IsSeenNow(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed()) { lua_pushboolean(L, 0); return 1; }
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    auto* sim = get_sim(L);
    if (sim && sim->visibility_grid() && army >= 0) {
        auto& pos = e->position();
        lua_pushboolean(
            L, sim->visibility_grid()->has_vision(pos.x, pos.z,
                                                  static_cast<u32>(army)) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int blip_IsOnRadar(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed()) { lua_pushboolean(L, 0); return 1; }
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    auto* sim = get_sim(L);
    if (sim && army >= 0) {
        // Stealth-aware: RadarStealth negates radar unless observer has Omni
        lua_pushboolean(
            L, sim->has_effective_radar(e, static_cast<u32>(army)) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int blip_IsOnSonar(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed()) { lua_pushboolean(L, 0); return 1; }
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    auto* sim = get_sim(L);
    if (sim && army >= 0) {
        // Stealth-aware: SonarStealth negates sonar unless observer has Omni
        lua_pushboolean(
            L, sim->has_effective_sonar(e, static_cast<u32>(army)) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int blip_IsOnOmni(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed()) { lua_pushboolean(L, 0); return 1; }
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    auto* sim = get_sim(L);
    if (sim && sim->visibility_grid() && army >= 0) {
        auto& pos = e->position();
        lua_pushboolean(
            L, sim->visibility_grid()->has_omni(pos.x, pos.z,
                                                static_cast<u32>(army)) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int blip_IsSeenEver(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed()) {
        // Destroyed entity — if we have a blip cache entry, it was seen
        u32 eid = get_blip_entity_id(L);
        i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                      : -1;
        auto* sim = get_sim(L);
        if (sim && army >= 0) {
            auto* snap = sim->get_blip_snapshot(eid, static_cast<u32>(army));
            lua_pushboolean(L, snap ? 1 : 0);
        } else {
            lua_pushboolean(L, 0);
        }
        return 1;
    }
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    auto* sim = get_sim(L);
    if (sim && sim->visibility_grid() && army >= 0) {
        auto& pos = e->position();
        lua_pushboolean(
            L, sim->visibility_grid()->ever_seen(pos.x, pos.z,
                                                 static_cast<u32>(army)) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int blip_IsMaybeDead(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed()) {
        lua_pushboolean(L, 1); // entity dead → definitely maybe dead
        return 1;
    }
    // Entity alive — check if requesting army has current intel
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    if (army >= 0 && army == e->army()) {
        lua_pushboolean(L, 0); // own army always knows
        return 1;
    }
    auto* sim = get_sim(L);
    if (sim && army >= 0) {
        bool has_intel = sim->has_any_intel(e, static_cast<u32>(army));
        lua_pushboolean(L, has_intel ? 0 : 1);
    } else {
        lua_pushboolean(L, 1);
    }
    return 1;
}

static int blip_IsKnownFake(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed() || !e->is_unit()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // A unit is "known fake" if it has Jammer intel enabled AND the
    // requesting army has Omni coverage at the unit's position
    auto* unit = static_cast<sim::Unit*>(e);
    if (!unit->is_intel_enabled("Jammer")) {
        lua_pushboolean(L, 0);
        return 1;
    }
    i32 army = lua_isnumber(L, 2) ? static_cast<i32>(lua_tonumber(L, 2)) - 1
                                  : -1;
    auto* sim = get_sim(L);
    if (sim && sim->visibility_grid() && army >= 0) {
        auto& pos = e->position();
        lua_pushboolean(
            L, sim->visibility_grid()->has_omni(pos.x, pos.z,
                                                static_cast<u32>(army)) ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int blip_GetSource(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (!e || e->destroyed() || e->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, e->lua_table_ref());
    return 1;
}

static int blip_GetBlueprint(lua_State* L) {
    auto* e = check_blip_entity(L);
    std::string bp_id;
    if (e && !e->destroyed()) {
        bp_id = e->blueprint_id();
    } else {
        // Entity destroyed — use blip cache
        u32 eid = get_blip_entity_id(L);
        i32 req_army = get_blip_req_army(L);
        auto* sim = get_sim(L);
        if (sim && req_army >= 0) {
            auto* snap = sim->get_blip_snapshot(eid,
                                                 static_cast<u32>(req_army));
            if (snap) bp_id = snap->blueprint_id;
        }
    }
    if (bp_id.empty()) { lua_pushnil(L); return 1; }
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, bp_id.c_str());
        lua_rawget(L, -2);
        lua_remove(L, -2); // remove __blueprints table
    } else {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
    return 1;
}

static int blip_GetArmy(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (e && !e->destroyed()) {
        lua_pushnumber(L, e->army() >= 0 ? e->army() + 1 : -1);
        return 1;
    }
    // Entity destroyed — use blip cache
    u32 eid = get_blip_entity_id(L);
    i32 req_army = get_blip_req_army(L);
    auto* sim = get_sim(L);
    if (sim && req_army >= 0) {
        auto* snap = sim->get_blip_snapshot(eid, static_cast<u32>(req_army));
        if (snap && snap->entity_army >= 0) {
            lua_pushnumber(L, snap->entity_army + 1);
            return 1;
        }
    }
    lua_pushnumber(L, -1);
    return 1;
}

static int blip_GetAIBrain(lua_State* L) {
    auto* e = check_blip_entity(L);
    i32 army = -1;
    if (e && !e->destroyed()) {
        army = e->army();
    } else {
        u32 eid = get_blip_entity_id(L);
        i32 req_army = get_blip_req_army(L);
        auto* sim = get_sim(L);
        if (sim && req_army >= 0) {
            auto* snap = sim->get_blip_snapshot(eid,
                                                 static_cast<u32>(req_army));
            if (snap) army = snap->entity_army;
        }
    }
    if (army < 0) { lua_pushnil(L); return 1; }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* brain = sim->get_army(army);
    if (!brain || brain->lua_table_ref() < 0) { lua_pushnil(L); return 1; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, brain->lua_table_ref());
    return 1;
}

static int blip_BeenDestroyed(lua_State* L) {
    auto* e = check_blip_entity(L);
    if (e && !e->destroyed()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    // Entity pointer gone — check blip cache
    u32 eid = get_blip_entity_id(L);
    i32 req_army = get_blip_req_army(L);
    auto* sim = get_sim(L);
    if (sim && req_army >= 0) {
        auto* snap = sim->get_blip_snapshot(eid, static_cast<u32>(req_army));
        lua_pushboolean(L, (snap && snap->entity_dead) ? 1 : 0);
    } else {
        lua_pushboolean(L, 1); // no data → assume dead
    }
    return 1;
}

// clang-format off
const MethodEntry blip_methods[] = {
    {"GetSource",        blip_GetSource},
    {"IsOnRadar",        blip_IsOnRadar},
    {"IsOnSonar",        blip_IsOnSonar},
    {"IsOnOmni",         blip_IsOnOmni},
    {"IsSeenEver",       blip_IsSeenEver},
    {"IsSeenNow",        blip_IsSeenNow},
    {"GetBlueprint",     blip_GetBlueprint},
    {"GetPosition",      blip_GetPosition},
    {"GetArmy",          blip_GetArmy},
    {"GetAIBrain",       blip_GetAIBrain},
    {"BeenDestroyed",    blip_BeenDestroyed},
    {"IsKnownFake",      blip_IsKnownFake},
    {"IsMaybeDead",      blip_IsMaybeDead},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
