// Projectiles: moho.projectile_methods.
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

static sim::Projectile* check_projectile(lua_State* L, int idx = 1) {
    auto* e = check_entity(L, idx);
    if (e && e->is_projectile())
        return static_cast<sim::Projectile*>(e);
    return nullptr;
}

// ====================================================================
// projectile_methods — real implementations
// ====================================================================

static int proj_GetLauncher(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p || p->launcher_id == 0) { lua_pushnil(L); return 1; }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* launcher = sim->entity_registry().find(p->launcher_id);
    if (!launcher || launcher->destroyed() || launcher->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, launcher->lua_table_ref());
    return 1;
}

static int proj_GetCurrentSpeed(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) { lua_pushnumber(L, 0); return 1; }
    f32 speed = std::sqrt(p->velocity.x * p->velocity.x +
                          p->velocity.y * p->velocity.y +
                          p->velocity.z * p->velocity.z);
    lua_pushnumber(L, speed);
    return 1;
}

// GetVelocity(): its velocity per tick, as Moho's gives it -- SetVelocity
// takes it per second. Retail's scripts rely on the scale: the Miasma shell
// multiplies by 10 for its speed, and the split tactical missiles add a
// spread of about 1 to it.
static int proj_GetVelocity(lua_State* L) {
    auto* p = check_projectile(L);
    const sim::Vector3 v = p ? p->velocity : sim::Vector3{};
    lua_pushnumber(L, v.x * 0.1f);
    lua_pushnumber(L, v.y * 0.1f);
    lua_pushnumber(L, v.z * 0.1f);
    return 3;
}

// projectile:SetVelocity(vx, vy, vz) or projectile:SetVelocity(speed); the
// one-argument form keeps the direction of travel (or, at rest, the facing).
/// projectile:SetDamage(amount [, radius]) -- a script's own damage for
/// this projectile (retail's split tactical missiles set theirs in OnCreate).
static int proj_SetDamage(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) return 0;
    if (lua_isnumber(L, 2)) p->damage_amount = static_cast<f32>(lua_tonumber(L, 2));
    if (lua_isnumber(L, 3)) p->damage_radius = static_cast<f32>(lua_tonumber(L, 3));
    return 0;
}

static int proj_SetVelocity(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) { lua_pushvalue(L, 1); return 1; }
    if (!lua_isnumber(L, 3)) {
        const f32 speed = static_cast<f32>(luaL_checknumber(L, 2));
        sim::Vector3 dir = p->velocity;
        f32 len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len < 1e-6f) {
            dir = sim::quat_rotate(p->orientation(), {0.0f, 0.0f, 1.0f});
            len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        }
        if (len > 1e-6f) {
            p->velocity = {dir.x / len * speed, dir.y / len * speed, dir.z / len * speed};
        }
        lua_pushvalue(L, 1);
        return 1;
    }
    p->velocity.x = static_cast<f32>(luaL_checknumber(L, 2));
    p->velocity.y = static_cast<f32>(luaL_checknumber(L, 3));
    p->velocity.z = static_cast<f32>(luaL_checknumber(L, 4));
    lua_pushvalue(L, 1);
    return 1;
}

static int proj_SetLifetime(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->lifetime = static_cast<f32>(luaL_checknumber(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}

static int proj_SetNewTarget(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) return 0;
    auto* target = check_entity(L, 2);
    if (target && !target->destroyed()) {
        p->target_entity_id = target->entity_id();
        p->target_position = sim::collision_centre(*target);
        p->has_target_position = true;
    }
    return 0;
}

static int proj_SetNewTargetGround(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) return 0;
    if (lua_istable(L, 2)) {
        lua_rawgeti(L, 2, 1);
        p->target_position.x = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 2);
        p->target_position.y = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 3);
        p->target_position.z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        p->target_entity_id = 0; // ground target
        p->has_target_position = true;
    }
    return 0;
}

// SetNewTargetGroundXYZ(x, y, z): SetNewTargetGround with the point as three
// numbers. FAF's engine adds it, and its Projectile.lua (the tracking
// fuzziness of OnTrackTargetGround) calls it.
static int proj_SetNewTargetGroundXYZ(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) return 0;
    p->target_position = {static_cast<f32>(luaL_checknumber(L, 2)),
                          static_cast<f32>(luaL_checknumber(L, 3)),
                          static_cast<f32>(luaL_checknumber(L, 4))};
    p->target_entity_id = 0; // ground target
    p->has_target_position = true;
    return 0;
}

// proj:SetMaxSpeed(speed) — set max speed, return self for chaining
static int proj_SetMaxSpeed(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->max_speed = static_cast<f32>(luaL_checknumber(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}

// proj:SetAcceleration(accel) — set linear acceleration, return self
static int proj_SetAcceleration(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->acceleration = static_cast<f32>(luaL_checknumber(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}

// proj:SetBallisticAcceleration(accel) — set vertical gravity, return self
static int proj_SetBallisticAcceleration(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->ballistic_accel = static_cast<f32>(luaL_checknumber(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}

// proj:SetTurnRate(rate) — store turn rate (no heading-based flight yet)
static int proj_SetTurnRate(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->turn_rate = static_cast<f32>(luaL_checknumber(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}

// proj:SetTurnRateByDist(rate) — same as SetTurnRate for now, return self
static int proj_SetTurnRateByDist(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->turn_rate = static_cast<f32>(luaL_checknumber(L, 2));
    lua_pushvalue(L, 1);
    return 1;
}

// --- Projectile target + guidance ---

static int proj_GetCurrentTargetPosition(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushnumber(L, 1); lua_pushnumber(L, p->target_position.x); lua_rawset(L, -3);
    lua_pushnumber(L, 2); lua_pushnumber(L, p->target_position.y); lua_rawset(L, -3);
    lua_pushnumber(L, 3); lua_pushnumber(L, p->target_position.z); lua_rawset(L, -3);
    return 1;
}

static int proj_GetCurrentTargetPositionXYZ(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0); return 3; }
    lua_pushnumber(L, p->target_position.x);
    lua_pushnumber(L, p->target_position.y);
    lua_pushnumber(L, p->target_position.z);
    return 3;
}

static int proj_GetTrackingTarget(lua_State* L) {
    auto* p = check_projectile(L);
    if (!p || p->target_entity_id == 0) { lua_pushnil(L); return 1; }
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* target = sim->entity_registry().find(p->target_entity_id);
    if (!target || target->lua_table_ref() < 0) { lua_pushnil(L); return 1; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, target->lua_table_ref());
    return 1;
}

static int proj_TrackTarget(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->tracking = (lua_toboolean(L, 2) != 0);
    lua_pushvalue(L, 1); // chains: TrackTarget(true):StayUnderwater(true)
    return 1;
}

static int proj_ChangeMaxZigZag(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->max_zig_zag = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

static int proj_ChangeZigZagFrequency(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->zig_zag_freq = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

static int proj_GetMaxZigZag(lua_State* L) {
    auto* p = check_projectile(L);
    lua_pushnumber(L, p ? p->max_zig_zag : 0);
    return 1;
}

static int proj_GetZigZagFrequency(lua_State* L) {
    auto* p = check_projectile(L);
    lua_pushnumber(L, p ? p->zig_zag_freq : 0);
    return 1;
}

static int proj_ChangeDetonateAboveHeight(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->detonate_above_height = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

static int proj_ChangeDetonateBelowHeight(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->detonate_below_height = static_cast<f32>(luaL_checknumber(L, 2));
    return 0;
}

// --- Projectile physics flags ---

static int proj_SetDestroyOnWater(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->destroy_on_water = (lua_toboolean(L, 2) != 0);
    return 0;
}

static int proj_SetStayUpright(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->stay_upright = (lua_toboolean(L, 2) != 0);
    return 0;
}

static int proj_SetVelocityAlign(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->velocity_align = (lua_toboolean(L, 2) != 0);
    return 0;
}

// SetScaleVelocity(sv) or (svx, svy, svz): how fast its draw scale grows
// (or shrinks) each second. Movement is untouched.
static int proj_SetScaleVelocity(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) {
        const auto sx = static_cast<f32>(luaL_checknumber(L, 2));
        p->scale_velocity = {sx, static_cast<f32>(luaL_optnumber(L, 3, sx)),
                             static_cast<f32>(luaL_optnumber(L, 4, sx))};
    }
    lua_pushvalue(L, 1); // return self for chaining
    return 1;
}

static int proj_SetLocalAngularVelocity(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) {
        p->angular_velocity.x = static_cast<f32>(luaL_checknumber(L, 2));
        p->angular_velocity.y = static_cast<f32>(luaL_checknumber(L, 3));
        p->angular_velocity.z = static_cast<f32>(luaL_checknumber(L, 4));
    }
    lua_pushvalue(L, 1); // return self for chaining
    return 1;
}

// --- Projectile collision + CreateChildProjectile (M51) ---

static int proj_SetCollision(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->collision_enabled = (lua_toboolean(L, 2) != 0);
    lua_pushvalue(L, 1); return 1; // return self for chaining
}

static int proj_SetCollideEntity(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->collide_entity = (lua_toboolean(L, 2) != 0);
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int proj_SetCollideSurface(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->collide_surface = (lua_toboolean(L, 2) != 0);
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int proj_StayUnderwater(lua_State* L) {
    auto* p = check_projectile(L);
    if (p) p->stay_underwater = (lua_toboolean(L, 2) != 0);
    lua_pushvalue(L, 1); // return self for chaining
    return 1;
}

static int proj_CreateChildProjectile(lua_State* L) {
    auto* parent = check_projectile(L);
    if (!parent || parent->destroyed()) { lua_pushnil(L); return 1; }

    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    // Its own blueprint (script class, mesh), its parent's flight: speed,
    // orientation and target. Retail's split missiles and cluster shells.
    auto child = std::make_unique<sim::Projectile>();
    child->set_blueprint_id(lowercase_arg(L, 2));
    child->set_position(parent->position());
    child->set_orientation(parent->orientation());
    child->set_army(parent->army());
    child->launcher_id = parent->launcher_id;
    child->velocity = parent->velocity;
    child->target_entity_id = parent->target_entity_id;
    child->target_position = parent->target_position;
    child->has_target_position = parent->has_target_position;
    apply_script_projectile_physics(L, *child);

    u32 child_id = sim->entity_registry().register_entity(std::move(child));
    auto* child_ptr = static_cast<sim::Projectile*>(
        sim->entity_registry().find(child_id));

    // Its script object, as for every projectile (OnCreate runs).
    sim::create_projectile_object(L, *child_ptr, under_water(sim, child_ptr->position()), true);
    return 1;
}

// clang-format off
const MethodEntry projectile_methods[] = {
    // Real implementations
    {"GetLauncher",                 proj_GetLauncher},
    {"GetCurrentSpeed",             proj_GetCurrentSpeed},
    {"GetVelocity",                 proj_GetVelocity},
    {"SetDamage",                   proj_SetDamage},
    {"SetVelocity",                 proj_SetVelocity},
    {"SetLifetime",                 proj_SetLifetime},
    {"SetNewTarget",                proj_SetNewTarget},
    {"SetNewTargetGround",          proj_SetNewTargetGround},
    {"SetNewTargetGroundXYZ",       proj_SetNewTargetGroundXYZ},
    // Stubs — return self for chaining
    {"GetTrackingTarget",           proj_GetTrackingTarget},
    {"GetCurrentTargetPosition",    proj_GetCurrentTargetPosition},
    {"GetCurrentTargetPositionXYZ", proj_GetCurrentTargetPositionXYZ},
    {"TrackTarget",                 proj_TrackTarget},
    {"SetScaleVelocity",            proj_SetScaleVelocity},
    {"SetMaxSpeed",                 proj_SetMaxSpeed},
    {"SetAcceleration",             proj_SetAcceleration},
    {"SetBallisticAcceleration",    proj_SetBallisticAcceleration},
    {"SetCollideEntity",            proj_SetCollideEntity},
    {"SetCollideSurface",           proj_SetCollideSurface},
    {"SetCollision",                proj_SetCollision},
    {"SetDestroyOnWater",           proj_SetDestroyOnWater},
    {"SetLocalAngularVelocity",     proj_SetLocalAngularVelocity},
    {"SetTurnRate",                 proj_SetTurnRate},
    {"SetStayUpright",              proj_SetStayUpright},
    {"SetVelocityAlign",            proj_SetVelocityAlign},
    {"StayUnderwater",              proj_StayUnderwater},
    {"CreateChildProjectile",       proj_CreateChildProjectile},
    {"ChangeMaxZigZag",             proj_ChangeMaxZigZag},
    {"ChangeZigZagFrequency",       proj_ChangeZigZagFrequency},
    {"GetMaxZigZag",                proj_GetMaxZigZag},
    {"GetZigZagFrequency",          proj_GetZigZagFrequency},
    {"ChangeDetonateAboveHeight",   proj_ChangeDetonateAboveHeight},
    {"ChangeDetonateBelowHeight",   proj_ChangeDetonateBelowHeight},
    {"SetTurnRateByDist",           proj_SetTurnRateByDist},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
