// Manipulators: rotators, animators, aim controllers, sliders and the rest.
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

// entity:CreateProjectile(bp, dx, dy, dz) -> projectile Lua table
// Creates a projectile at entity position with optional velocity direction
/// A projectile blueprint's Physics.InitialSpeed (0 without one).
static f32 projectile_initial_speed(lua_State* L, sim::SimState* sim, const std::string& bp_id) {
    auto* store = sim ? sim->blueprint_store() : nullptr;
    const auto* entry = store && !bp_id.empty() ? store->find(bp_id) : nullptr;
    if (!entry) return 0;
    const int top = lua_gettop(L);
    store->push_lua_table(*entry, L);
    f32 speed = 0;
    lua_pushstring(L, "Physics");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "InitialSpeed");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) speed = static_cast<f32>(lua_tonumber(L, -1));
    }
    lua_settop(L, top);
    return speed;
}

/// Point a new projectile along `dir`, at its blueprint's InitialSpeed.
void aim_projectile(lua_State* L, sim::SimState* sim, sim::Projectile& p, const sim::Vector3& dir) {
    const f32 len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 1e-6f) return;
    const sim::Vector3 d{dir.x / len, dir.y / len, dir.z / len};
    p.set_orientation(sim::euler_to_quat(osc::dmath::atan2(d.x, d.z),
                                         osc::dmath::atan2(-d.y, std::sqrt(d.x * d.x + d.z * d.z)),
                                         0.0f));
    const f32 speed = projectile_initial_speed(L, sim, p.blueprint_id());
    p.velocity = {d.x * speed, d.y * speed, d.z * speed};
}

// --- Base manipulator methods ---

static int manip_Destroy(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    auto* m = lua_isuserdata(L, -1)
                  ? static_cast<sim::Manipulator*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    if (m) {
        // If a thread is WaitFor-ing on this manipulator, wake it so it
        // doesn't sleep forever at INT32_MAX.
        m->mark_destroyed();

        if (m->has_waiting_thread()) {
            lua_pushstring(L, "osc_thread_mgr");
            lua_rawget(L, LUA_REGISTRYINDEX);
            auto* mgr = lua_isuserdata(L, -1)
                ? static_cast<sim::ThreadManager*>(lua_touserdata(L, -1))
                : nullptr;
            lua_pop(L, 1);
            if (mgr) {
                lua_pushstring(L, "osc_sim_state");
                lua_rawget(L, LUA_REGISTRYINDEX);
                auto* ss = lua_isuserdata(L, -1)
                    ? static_cast<sim::SimState*>(lua_touserdata(L, -1))
                    : nullptr;
                lua_pop(L, 1);
                u32 tick = ss ? ss->tick_count() : 0;
                mgr->wake(*m, tick);
            }
            m->clear_waiting_thread();
        }
    }
    return 0;
}

static int manip_Enable(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) m->set_enabled(true);
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int manip_Disable(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) m->set_enabled(false);
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int manip_SetEnabled(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) m->set_enabled(lua_toboolean(L, 2) != 0);
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int manip_SetPrecedence(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) m->set_precedence(static_cast<i32>(lua_tonumber(L, 2)));
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int manip_IsEnabled(lua_State* L) {
    auto* m = check_manip_base(L);
    lua_pushboolean(L, m && m->enabled() ? 1 : 0);
    return 1;
}

// --- RotateManipulator methods ---

static int rotate_SetGoal(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->set_goal(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int rotate_SetSpeed(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->set_speed(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int rotate_SetAccel(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->set_accel(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int rotate_SetCurrentAngle(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->set_current_angle(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int rotate_GetCurrentAngle(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        lua_pushnumber(L, static_cast<sim::RotateManipulator*>(m)->current_angle());
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

static int rotate_SetSpinDown(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->set_spin_down(
            lua_toboolean(L, 2) != 0);
    }
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int rotate_SetTargetSpeed(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->set_target_speed(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int rotate_ClearGoal(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::RotateManipulator*>(m)->clear_goal();
    }
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

// --- AnimationManipulator methods ---

static int anim_PlayAnim(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        const char* path = lua_type(L, 2) == LUA_TSTRING
                               ? lua_tostring(L, 2) : "";
        bool loop = lua_toboolean(L, 3) != 0;
        auto* sim = get_sim(L);
        auto* cache = sim ? sim->anim_cache() : nullptr;
        static_cast<sim::AnimManipulator*>(m)->play_anim(path, loop, cache);
    }
    lua_pushvalue(L, 1); // return self for chaining
    return 1;
}

static int anim_SetRate(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AnimManipulator*>(m)->set_rate(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

/// GetRate(): the animator's playback rate (1 until SetRate changes it).
static int anim_GetRate(lua_State* L) {
    auto* m = check_manip_base(L);
    lua_pushnumber(L, m ? static_cast<sim::AnimManipulator*>(m)->rate() : 0.0);
    return 1;
}

static int anim_SetAnimationFraction(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AnimManipulator*>(m)->set_animation_fraction(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int anim_GetAnimationFraction(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        lua_pushnumber(L, static_cast<sim::AnimManipulator*>(m)->animation_fraction());
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

static int anim_GetAnimationDuration(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        lua_pushnumber(L, static_cast<sim::AnimManipulator*>(m)->animation_duration());
    } else {
        lua_pushnumber(L, 1);
    }
    return 1;
}

static int anim_GetAnimationTime(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        lua_pushnumber(L, static_cast<sim::AnimManipulator*>(m)->animation_time());
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

static int anim_SetAnimationTime(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AnimManipulator*>(m)->set_animation_time(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

// --- SlideManipulator methods ---

static int slide_SetGoal(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::SlideManipulator*>(m)->set_goal(
            static_cast<f32>(lua_tonumber(L, 2)),
            static_cast<f32>(lua_tonumber(L, 3)),
            static_cast<f32>(lua_tonumber(L, 4)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int slide_SetSpeed(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::SlideManipulator*>(m)->set_speed(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int slide_SetAccel(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::SlideManipulator*>(m)->set_accel(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1);
    return 1;
}

static int slide_SetWorldUnits(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::SlideManipulator*>(m)->set_world_units(
            lua_toboolean(L, 2) != 0);
    }
    lua_pushvalue(L, 1);
    return 1;
}

// --- AimManipulator methods ---

static int aim_SetFiringArc(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AimManipulator*>(m)->set_firing_arc(
            static_cast<f32>(lua_tonumber(L, 2)),
            static_cast<f32>(lua_tonumber(L, 3)),
            static_cast<f32>(lua_tonumber(L, 4)),
            static_cast<f32>(lua_tonumber(L, 5)),
            static_cast<f32>(lua_tonumber(L, 6)),
            static_cast<f32>(lua_tonumber(L, 7)));
    }
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int aim_SetHeadingPitch(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AimManipulator*>(m)->set_heading_pitch(
            static_cast<f32>(lua_tonumber(L, 2)),
            static_cast<f32>(lua_tonumber(L, 3)));
    }
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int aim_GetHeadingPitch(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        auto* aim = static_cast<sim::AimManipulator*>(m);
        lua_pushnumber(L, aim->heading());
        lua_pushnumber(L, aim->pitch());
    } else {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
    }
    return 2;
}

static int aim_OnTarget(lua_State* L) {
    auto* m = check_manip_base(L);
    lua_pushboolean(L, m && static_cast<sim::AimManipulator*>(m)->on_target() ? 1 : 0);
    return 1;
}

static int aim_SetResetPoseTime(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AimManipulator*>(m)->set_reset_pose_time(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int aim_SetAimHeadingOffset(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        static_cast<sim::AimManipulator*>(m)->set_aim_heading_offset(
            static_cast<f32>(lua_tonumber(L, 2)));
    }
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

// --- Method tables ---

// clang-format off
const MethodEntry manipulator_methods[] = {
    {"Destroy",                 manip_Destroy},
    {"Enable",                  manip_Enable},
    {"Disable",                 manip_Disable},
    {"SetEnabled",              manip_SetEnabled},
    {"SetPrecedence",           manip_SetPrecedence},
    {"IsEnabled",               manip_IsEnabled},
    {nullptr, nullptr},
};
// clang-format on

// clang-format off
const MethodEntry aim_manipulator_methods[] = {
    {"SetFiringArc",            aim_SetFiringArc},
    {"SetAimingArc",            aim_SetFiringArc},   // alias used by builder arm
    {"SetHeadingPitch",         aim_SetHeadingPitch},
    {"GetHeadingPitch",         aim_GetHeadingPitch},
    {"OnTarget",                aim_OnTarget},
    {"SetEnabled",              manip_SetEnabled},
    {"SetResetPoseTime",        aim_SetResetPoseTime},
    {"SetAimHeadingOffset",     aim_SetAimHeadingOffset},
    {nullptr, nullptr},
};
// clang-format on

// animator:SetBoneEnabled(boneName, enabled)
static int anim_SetBoneEnabled(lua_State* L) {
    auto* m = check_manip_base(L);
    if (!m || !m->owner()) { lua_pushvalue(L, 1); return 1; }
    auto* anim = static_cast<sim::AnimManipulator*>(m);
    i32 bone_idx = resolve_bone_index(m->owner(), L, 2);
    bool enabled = lua_toboolean(L, 3) != 0;
    anim->set_bone_enabled(bone_idx, enabled);
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

static int anim_SetBlendTime(lua_State* L) {
    auto* m = check_manip_base(L);
    if (m) {
        f32 seconds = static_cast<f32>(luaL_checknumber(L, 2));
        if (seconds < 0.0f) seconds = 0.0f;
        static_cast<sim::AnimManipulator*>(m)->set_blend_time(seconds);
    }
    lua_pushvalue(L, 1); // setters return self for chaining
    return 1;
}

// clang-format off
const MethodEntry animation_manipulator_methods[] = {
    {"PlayAnim",                anim_PlayAnim},
    {"SetRate",                 anim_SetRate},
    {"GetRate",                 anim_GetRate},
    {"SetAnimationFraction",    anim_SetAnimationFraction},
    {"GetAnimationFraction",    anim_GetAnimationFraction},
    {"GetAnimationDuration",    anim_GetAnimationDuration},
    {"GetAnimationTime",        anim_GetAnimationTime},
    {"SetAnimationTime",        anim_SetAnimationTime},
    {"SetBoneEnabled",          anim_SetBoneEnabled},
    {"SetBlendTime",            anim_SetBlendTime},
    {nullptr, nullptr},
};
// clang-format on

// clang-format off
const MethodEntry rotate_manipulator_methods[] = {
    {"SetGoal",                 rotate_SetGoal},
    {"SetSpeed",                rotate_SetSpeed},
    {"SetAccel",                rotate_SetAccel},
    {"SetCurrentAngle",         rotate_SetCurrentAngle},
    {"GetCurrentAngle",         rotate_GetCurrentAngle},
    {"SetSpinDown",             rotate_SetSpinDown},
    {"SetTargetSpeed",          rotate_SetTargetSpeed},
    {"ClearGoal",               rotate_ClearGoal},
    {nullptr, nullptr},
};
// clang-format on

// clang-format off
const MethodEntry slide_manipulator_methods[] = {
    {"SetGoal",                 slide_SetGoal},
    {"SetSpeed",                slide_SetSpeed},
    {"SetAccel",                slide_SetAccel},
    {"SetWorldUnits",           slide_SetWorldUnits},
    {nullptr, nullptr},
};
// clang-format on

// BuilderArmManipulator — FA scripts cache moho.BuilderArmManipulator.SetAimingArc
// clang-format off
const MethodEntry builder_arm_methods[] = {
    {"SetAimingArc",            aim_SetFiringArc},
    {nullptr, nullptr},
};
// clang-format on

int sequence_count(lua_State* L, int table_idx) {
    if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
    int count = 0;
    for (int i = 1;; ++i) {
        lua_rawgeti(L, table_idx, i);
        const bool present = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (!present) break;
        count = i;
    }
    return count;
}

} // namespace osc::lua
