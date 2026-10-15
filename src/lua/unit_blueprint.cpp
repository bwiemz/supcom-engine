#include "lua/unit_blueprint.hpp"

#include "blueprints/blueprint_store.hpp"
#include "blueprints/footprint.hpp"
#include "lua/sim_bindings.hpp"
#include "map/terrain.hpp"
#include "sim/blueprint_categories.hpp"
#include "sim/build_placement.hpp"
#include "sim/category_expr.hpp"
#include "sim/collision.hpp"
#include "sim/occupancy.hpp"
#include "sim/sim_state.hpp"
#include "sim/transport_slots.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

using blueprints::BlueprintEntry;
using blueprints::BlueprintStore;

/// A projectile blueprint's Physics, as the weapon firing it aims by them:
/// TrackTarget, UseGravity, MaxSpeed and StraightDownOrdinance. None when
/// the blueprint or its Physics table is missing.
static std::optional<sim::Weapon::ProjectilePhysics> projectile_physics(lua_State* L,
                                                                        const std::string& id) {
    const int top = lua_gettop(L);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, id.c_str());
        lua_rawget(L, -2);
    }
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "Physics");
        lua_rawget(L, -2);
    }
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        return std::nullopt;
    }
    sim::Weapon::ProjectilePhysics physics;
    lua_pushstring(L, "TrackTarget");
    lua_rawget(L, -2);
    if (lua_isboolean(L, -1)) {
        physics.track_target = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);
    lua_pushstring(L, "UseGravity");
    lua_rawget(L, -2);
    if (lua_isboolean(L, -1)) {
        physics.use_gravity = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);
    lua_pushstring(L, "MaxSpeed");
    lua_rawget(L, -2);
    if (lua_type(L, -1) == LUA_TNUMBER) {
        physics.max_speed = static_cast<f32>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
    lua_pushstring(L, "StraightDownOrdinance");
    lua_rawget(L, -2);
    if (lua_isboolean(L, -1)) physics.straight_down = lua_toboolean(L, -1) != 0;
    lua_settop(L, top);
    return physics;
}

/// Its health: MaxHealth, else Defense.MaxHealth. A unit being built starts
/// at 1.
static void read_health(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                        sim::Unit& unit, bool being_built) {
    auto hp = store.get_number_field(entry, "MaxHealth", L);
    if (!hp) {
        store.push_lua_table(entry, L);
        lua_pushstring(L, "Defense");
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "MaxHealth");
            lua_gettable(L, -2);
            if (lua_isnumber(L, -1)) hp = lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
    }
    if (hp) {
        unit.set_max_health(static_cast<f32>(*hp));
        unit.set_health(being_built ? 1.0f : static_cast<f32>(*hp));
    }
}

/// Physics: its top speed, and how it drives (M203).
static void read_drive(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                       sim::Unit& unit) {
    store.push_lua_table(entry, L);
    lua_pushstring(L, "Physics");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        const int physics = lua_gettop(L);
        const auto number = [&](const char* key, f32 fallback) {
            lua_pushstring(L, key);
            lua_rawget(L, physics);
            const f32 v =
                lua_type(L, -1) == LUA_TNUMBER ? static_cast<f32>(lua_tonumber(L, -1)) : fallback;
            lua_pop(L, 1);
            return v;
        };
        const f32 max_speed = number("MaxSpeed", 0);
        unit.set_max_speed(max_speed);
        constexpr f32 kDegToRad = 3.14159265358979f / 180.0f;
        sim::Unit::Drive drive;
        // Every retail mobile unit gives these; a unit without them
        // reaches its top speed in a second and turns 90 degrees in one.
        drive.max_accel = number("MaxAcceleration", 0);
        if (drive.max_accel <= 0) drive.max_accel = max_speed;
        drive.max_brake = number("MaxBrake", drive.max_accel);
        if (drive.max_brake <= 0) drive.max_brake = drive.max_accel;
        drive.turn_rate = number("TurnRate", 0) * kDegToRad;
        if (drive.turn_rate <= 0) drive.turn_rate = 90.0f * kDegToRad;
        drive.turn_radius = number("TurnRadius", 0);
        // FAF: MaxSpeedReverse defaults to MaxSpeed (every retail unit gives it).
        drive.max_speed_reverse = number("MaxSpeedReverse", max_speed);
        // Two retail blueprints spell it BackupDistance.
        drive.backup_distance = number("BackUpDistance", number("BackupDistance", 0));
        drive.rotate_threshold = number("RotateOnSpotThreshold", 0.5f);
        drive.turn_facing_rate = number("TurnFacingRate", 0) * kDegToRad;
        lua_pushstring(L, "RotateOnSpot");
        lua_rawget(L, physics);
        drive.rotate_on_spot = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        unit.set_drive(drive);
        unit.set_blueprint_elevation(number("Elevation", 0));
        unit.set_hover_physics({number("BankingSlope", 0), number("MaxAcceleration", 0),
                                number("WobbleFactor", 0), number("WobbleSpeed", 0)});
    }
    lua_pop(L, 2);
}

/// One of its Weapon entries, the table at stack index `we`, its `index`-th
/// (from 0).
static std::unique_ptr<sim::Weapon> read_weapon(lua_State* L, int we, i32 index) {
    auto weapon = std::make_unique<sim::Weapon>();
    weapon->weapon_index = index;

    lua_pushstring(L, "Label");
    lua_gettable(L, we);
    if (lua_isstring(L, -1)) weapon->label = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_pushstring(L, "MaxRadius");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->max_range = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_pushstring(L, "MinRadius");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->min_range = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_pushstring(L, "RateOfFire");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->rate_of_fire = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_pushstring(L, "Damage");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->damage = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_pushstring(L, "DamageRadius");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->damage_radius = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_pushstring(L, "DamageType");
    lua_gettable(L, we);
    if (lua_isstring(L, -1)) weapon->damage_type = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_pushstring(L, "MuzzleVelocity");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->muzzle_velocity = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_pushstring(L, "MuzzleVelocityReduceDistance");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) {
        weapon->muzzle_velocity_reduce_distance = static_cast<f32>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
    lua_pushstring(L, "MuzzleVelocityRandom");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->muzzle_velocity_random = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_pushstring(L, "BallisticArc");
    lua_gettable(L, we);
    if (lua_type(L, -1) == LUA_TSTRING) {
        const std::string arc = lua_tostring(L, -1);
        weapon->ballistic_arc = arc == "RULEUBA_LowArc"    ? sim::Weapon::Arc::Low
                                : arc == "RULEUBA_HighArc" ? sim::Weapon::Arc::High
                                                           : sim::Weapon::Arc::None;
    }
    lua_pop(L, 1);

    lua_pushstring(L, "LeadTarget");
    lua_gettable(L, we);
    if (lua_isboolean(L, -1)) {
        weapon->lead_target = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);

    lua_pushstring(L, "FireOnDeath");
    lua_gettable(L, we);
    if (lua_isboolean(L, -1)) weapon->fire_on_death = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    lua_pushstring(L, "DummyWeapon");
    lua_gettable(L, we);
    weapon->dummy = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_pushstring(L, "WeaponCategory");
    lua_gettable(L, we);
    if (lua_type(L, -1) == LUA_TSTRING && std::string_view(lua_tostring(L, -1)) == "Death")
        weapon->dummy = true;
    lua_pop(L, 1);

    // Every retail nuke launcher writes ManualFire = 1.
    lua_pushstring(L, "ManualFire");
    lua_gettable(L, we);
    weapon->manual_fire =
        lua_toboolean(L, -1) != 0 && !(lua_isnumber(L, -1) && lua_tonumber(L, -1) == 0);
    lua_pop(L, 1);

    // Silo weapons (M206).
    lua_pushstring(L, "CountedProjectile");
    lua_gettable(L, we);
    weapon->counted_projectile = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_pushstring(L, "NukeWeapon");
    lua_gettable(L, we);
    weapon->nuke_weapon = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_pushstring(L, "MaxProjectileStorage");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->max_projectile_storage = static_cast<i32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    // Missile defence (M206b).
    lua_pushstring(L, "TargetType");
    lua_gettable(L, we);
    weapon->targets_projectiles = lua_type(L, -1) == LUA_TSTRING &&
                                  std::string_view(lua_tostring(L, -1)) == "RULEWTT_Projectile";
    lua_pop(L, 1);

    lua_pushstring(L, "OverChargeWeapon");
    lua_gettable(L, we);
    weapon->overcharge = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    // Targeting restrictions and cadence.
    for (auto [field, expr] :
         {std::pair{"TargetRestrictDisallow", &weapon->restrict_disallow},
          std::pair{"TargetRestrictOnlyAllow", &weapon->restrict_only_allow}}) {
        lua_pushstring(L, field);
        lua_gettable(L, we);
        if (lua_type(L, -1) == LUA_TSTRING) *expr = sim::parse_category_list(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    for (auto [field, flag] :
         {std::pair{"AboveWaterTargetsOnly", &weapon->above_water_targets_only},
          std::pair{"BelowWaterTargetsOnly", &weapon->below_water_targets_only},
          std::pair{"UseFiringSolutionInsteadOfAimBone", &weapon->use_firing_solution},
          std::pair{"AboveWaterFireOnly", &weapon->above_water_fire_only},
          std::pair{"AlwaysRecheckTarget", &weapon->always_recheck_target},
          std::pair{"PrefersPrimaryWeaponTarget", &weapon->prefers_primary_weapon_target},
          std::pair{"StopOnPrimaryWeaponBusy", &weapon->stop_on_primary_weapon_busy},
          std::pair{"YawOnlyOnTarget", &weapon->yaw_only_on_target},
          std::pair{"CannotAttackGround", &weapon->cannot_attack_ground},
          std::pair{"SlavedToBody", &weapon->slaved_to_body}}) {
        lua_pushstring(L, field);
        lua_gettable(L, we);
        if (lua_isboolean(L, -1)) {
            *flag = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
    }
    lua_pushstring(L, "TargetCheckInterval");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) {
        const f64 ticks = std::floor(lua_tonumber(L, -1) * 10.0 + 0.5);
        weapon->target_check_period = static_cast<u32>(std::clamp(ticks, 1.0, 1.0e4));
    }
    lua_pop(L, 1);
    lua_pushstring(L, "AttackGroundTries");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->attack_ground_tries = static_cast<i32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    for (auto [field, value] : {std::pair{"FiringTolerance", &weapon->firing_tolerance},
                                std::pair{"SlavedToBodyArcRange", &weapon->slaved_arc_range},
                                std::pair{"TrackingRadius", &weapon->tracking_radius},
                                std::pair{"HeadingArcCenter", &weapon->heading_arc_center},
                                std::pair{"HeadingArcRange", &weapon->heading_arc_range}}) {
        lua_pushstring(L, field);
        lua_gettable(L, we);
        if (lua_isnumber(L, -1)) *value = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    lua_pushstring(L, "MaxHeightDiff");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->max_height_diff = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    for (auto [field, value] :
         {std::pair{"ProjectileLifetime", &weapon->projectile_lifetime},
          std::pair{"ProjectileLifetimeUsesMultiplier", &weapon->projectile_lifetime_multiplier}}) {
        lua_pushstring(L, field);
        lua_gettable(L, we);
        if (lua_isnumber(L, -1)) *value = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    // A beam weapon is known by the beams its script makes
    // (CollisionBeamEntity.__init); how far they reach:
    lua_pushstring(L, "MaximumBeamLength");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->max_beam_length = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    // RackBones[1].MuzzleBones[1] → muzzle bone name (string)
    lua_pushstring(L, "RackBones");
    lua_gettable(L, we);
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1); // first rack
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "MuzzleBones");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_rawgeti(L, -1, 1); // first muzzle bone name
                if (lua_isstring(L, -1)) weapon->muzzle_bone_name = lua_tostring(L, -1);
                lua_pop(L, 1); // bone name
            }
            lua_pop(L, 1); // MuzzleBones
        }
        lua_pop(L, 1); // rack[1]
    }
    lua_pop(L, 1); // RackBones

    // FiringRandomness (angular scatter in radians)
    lua_pushstring(L, "FiringRandomness");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->firing_randomness = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    // ProjectileId (projectile blueprint reference)
    lua_pushstring(L, "ProjectileId");
    lua_gettable(L, we);
    if (lua_type(L, -1) == LUA_TSTRING) {
        std::string pid = lua_tostring(L, -1);
        std::transform(pid.begin(), pid.end(), pid.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        weapon->projectile_bp_id = pid;
        weapon->projectile_physics = projectile_physics(L, pid);
    }
    lua_pop(L, 1);

    // RangeCategory → fire_target_layer_caps bitmask
    lua_pushstring(L, "RangeCategory");
    lua_gettable(L, we);
    if (lua_isstring(L, -1)) {
        std::string rc = lua_tostring(L, -1);
        if (rc == "UWRC_AntiAir") weapon->fire_target_layer_caps = sim::layer_to_bit("Air");
        else if (rc == "UWRC_DirectFire")
            weapon->fire_target_layer_caps = sim::layer_to_bit("Land") |
                                             sim::layer_to_bit("Water") |
                                             sim::layer_to_bit("Seabed");
        else if (rc == "UWRC_AntiNavy")
            weapon->fire_target_layer_caps =
                sim::layer_to_bit("Water") | sim::layer_to_bit("Sub") | sim::layer_to_bit("Seabed");
        else if (rc == "UWRC_Countermeasure") weapon->fire_target_layer_caps = 0xFF; // all layers
        // Default (unrecognized): 0xFF = all layers
    }
    lua_pop(L, 1);

    // NeedToComputeBombDrop (boolean — bomb weapons)
    lua_pushstring(L, "NeedToComputeBombDrop");
    lua_gettable(L, we);
    if (lua_isboolean(L, -1) && lua_toboolean(L, -1)) weapon->need_compute_bomb_drop = true;
    lua_pop(L, 1);

    // AutoInitiateAttackCommand: an idle aircraft attacks on its own
    lua_pushstring(L, "AutoInitiateAttackCommand");
    lua_gettable(L, we);
    weapon->auto_initiate_attack_command = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);

    // BombDropThreshold (distance threshold for overhead check)
    lua_pushstring(L, "BombDropThreshold");
    lua_gettable(L, we);
    if (lua_isnumber(L, -1)) weapon->bomb_drop_threshold = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);

    lua_pushvalue(L, we);
    weapon->blueprint_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    spdlog::debug("  Weapon[{}]: {} range={} dmg={} rof={} vel={}", index, weapon->label,
                  weapon->max_range, weapon->damage, weapon->rate_of_fire, weapon->muzzle_velocity);
    return weapon;
}

/// Its weapons, in the blueprint's order.
static void read_weapons(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                         sim::Unit& unit) {
    store.push_lua_table(entry, L);
    lua_pushstring(L, "Weapon");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        int wep_table = lua_gettop(L);
        for (int wi = 1;; wi++) {
            lua_rawgeti(L, wep_table, wi);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                continue;
            }
            int we = lua_gettop(L);
            unit.add_weapon(read_weapon(L, we, wi - 1));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 2);
}

/// Economy: its storage, production and upkeep.
static void read_economy(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                         sim::Unit& unit) {
    store.push_lua_table(entry, L);
    lua_pushstring(L, "Economy");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "StorageMass");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.economy().storage_mass = lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_pushstring(L, "StorageEnergy");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.economy().storage_energy = lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_pushstring(L, "ProductionPerSecondMass");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.economy().production_mass = lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_pushstring(L, "ProductionPerSecondEnergy");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.economy().production_energy = lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_pushstring(L, "MaintenanceConsumptionPerSecondEnergy");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.economy().consumption_energy = lua_tonumber(L, -1);
        lua_pop(L, 1);

        lua_pushstring(L, "ConsumptionPerSecondMass");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.economy().consumption_mass = lua_tonumber(L, -1);
        lua_pop(L, 1);

        if (unit.economy().production_mass > 0.0 || unit.economy().production_energy > 0.0)
            unit.economy().production_active = true;
    }
    lua_pop(L, 2);
}

/// Its categories.
static void read_categories(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                            sim::Unit& unit) {
    store.push_lua_table(entry, L);
    std::unordered_set<std::string> cats;
    sim::collect_blueprint_categories(L, lua_gettop(L), cats);
    for (const auto& cat : cats) unit.add_category(cat);
    lua_pop(L, 1);
}

/// Economy.BuildRate; MaxBuildDistance, how far past its footprint it builds,
/// reclaims and repairs (Moho's default 5); NeedToFaceTargetToBuild.
static void read_build(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                       sim::Unit& unit) {
    store.push_lua_table(entry, L);
    lua_pushstring(L, "Economy");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "BuildRate");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.set_build_rate(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
        lua_pushstring(L, "MaxBuildDistance");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.set_max_build_distance(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
        lua_pushstring(L, "NeedToFaceTargetToBuild");
        lua_gettable(L, -2);
        unit.set_need_to_face_target_to_build(lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
}

/// Defense: its threat levels (cached for threat queries), armour and regen.
static void read_defense(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                         sim::Unit& unit) {
    store.push_lua_table(entry, L);
    lua_pushstring(L, "Defense");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "SurfaceThreatLevel");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.set_surface_threat(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);

        lua_pushstring(L, "AirThreatLevel");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.set_air_threat(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);

        lua_pushstring(L, "SubThreatLevel");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.set_sub_threat(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);

        lua_pushstring(L, "EconomyThreatLevel");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.set_economy_threat(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);

        // ArmorType (for armor damage multipliers)
        lua_pushstring(L, "ArmorType");
        lua_gettable(L, -2);
        if (lua_type(L, -1) == LUA_TSTRING) unit.set_armor_type(lua_tostring(L, -1));
        lua_pop(L, 1);

        // RegenRate (base HP/sec regeneration)
        lua_pushstring(L, "RegenRate");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.set_regen_rate(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
}

/// Its collision box (until a script sets another), footprint and skirt.
static void read_shape(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                       sim::Unit& unit) {
    store.push_lua_table(entry, L);
    unit.set_default_collision_shape(sim::blueprint_collision_shape(L, lua_gettop(L)));
    lua_pop(L, 1);

    // Footprint (pathfinding obstacles, placement, ranges).
    store.push_lua_table(entry, L);
    const auto [sx, sz] = sim::blueprint_footprint(L, lua_gettop(L));
    unit.set_footprint_size(sx, sz);
    lua_pop(L, 1);

    // Physics.SkirtSizeX/Z, SkirtOffsetX/Z (adjacency, build ranges). As
    // Moho's: at least the footprint, never offset outward
    // (RUnitBlueprint's physics init).
    store.push_lua_table(entry, L);
    lua_pushstring(L, "Physics");
    lua_gettable(L, -2);
    f32 ssx = 0, ssz = 0, sox = 0, soz = 0;
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "SkirtSizeX");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) ssx = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_pushstring(L, "SkirtSizeZ");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) ssz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_pushstring(L, "SkirtOffsetX");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) sox = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_pushstring(L, "SkirtOffsetZ");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) soz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    unit.set_skirt(std::max(ssx, unit.footprint_size_x()), std::max(ssz, unit.footprint_size_z()),
                   std::min(sox, 0.0f), std::min(soz, 0.0f));
}

/// Transport: its capacity, class and slot layout; its size and density; its
/// AI table's staging, guard and attack rules; Air.TransportHoverHeight.
/// Read raw, past the blueprint tables' metamethods.
static void read_transport(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                           sim::Unit& unit) {
    store.push_lua_table(entry, L);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "Transport");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "Class1Capacity");
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1))
                unit.set_transport_capacity(static_cast<i32>(lua_tonumber(L, -1)));
            lua_pop(L, 1);

            lua_pushstring(L, "TransportClass");
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1))
                unit.set_transport_class(static_cast<i32>(lua_tonumber(L, -1)));
            lua_pop(L, 1);

            // Only AirClass units dock at a staging platform (M206r)
            lua_pushstring(L, "AirClass");
            lua_rawget(L, -2);
            unit.set_air_class(lua_toboolean(L, -1) != 0);
            lua_pop(L, 1);
            lua_pushstring(L, "DockingSlots");
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1)) unit.set_docking_slots(static_cast<i32>(lua_tonumber(L, -1)));
            lua_pop(L, 1);

            // A carrier's storage (M206q)
            lua_pushstring(L, "StorageSlots");
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1)) unit.set_storage_slots(static_cast<i32>(lua_tonumber(L, -1)));
            lua_pop(L, 1);

            // The slot layout (M206l), Moho's defaults for what is absent
            sim::TransportLayout layout;
            const auto field = [&](const char* name, i32& out) {
                lua_pushstring(L, name);
                lua_rawget(L, -2);
                if (lua_isnumber(L, -1)) out = static_cast<i32>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            };
            field("ClassGenericUpTo", layout.class_generic_up_to);
            field("Class2AttachSize", layout.class2_attach_size);
            field("Class3AttachSize", layout.class3_attach_size);
            field("Class4AttachSize", layout.class4_attach_size);
            field("ClassSAttachSize", layout.class_s_attach_size);
            unit.set_transport_layout(layout);
        }
        lua_pop(L, 1); // pop Transport (or nil)

        // SizeY: a carried unit with no AttachPoint bone hangs by its
        // centre, and what's attached to bone -1 sits there (M211k);
        // size and density: a transport loads the largest first.
        const auto number = [&](const char* name, f32 fallback) {
            lua_pushstring(L, name);
            lua_rawget(L, -2);
            const f32 v = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : fallback;
            lua_pop(L, 1);
            return v;
        };
        unit.set_size_y(number("SizeY", 1.0f));
        unit.set_size_xz(number("SizeX", 1.0f), number("SizeZ", 1.0f));
        unit.set_average_density(number("AverageDensity", 0.49f));
        // A staging platform's refuelling and repair (M206r)
        lua_pushstring(L, "AI");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            sim::StagingRules rules;
            rules.refuel_multiplier = number("RefuelingMultiplier", rules.refuel_multiplier);
            rules.repair_amount = number("RefuelingRepairAmount", rules.repair_amount);
            rules.repair_energy = number("RepairConsumeEnergy", rules.repair_energy);
            rules.repair_mass = number("RepairConsumeMass", rules.repair_mass);
            rules.scan_radius = number("StagingPlatformScanRadius", rules.scan_radius);
            unit.set_staging_rules(rules);
            unit.set_guard_scan_radius(number("GuardScanRadius", unit.guard_scan_radius()));
            unit.set_guard_return_radius(number("GuardReturnRadius", unit.guard_return_radius()));
            unit.set_attack_angle(number("AttackAngle", 0.0f));
            lua_pushstring(L, "NeedUnpack");
            lua_rawget(L, -2);
            unit.set_need_unpack(lua_toboolean(L, -1) != 0);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        // Air.TransportHoverHeight: how low a transport hovers to load
        lua_pushstring(L, "Air");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "TransportHoverHeight");
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1))
                unit.set_transport_hover_height(static_cast<f32>(lua_tonumber(L, -1)));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1); // pop bp table
}

/// Intel radii from blueprint (VisionRadius, RadarRadius, etc.). All
/// of it starts off: the unit's script switches it on (retail's
/// SetupIntel at OnStopBeingBuilt; WaterVision only when wet), as
/// Moho's handles start disabled (M215g).
static void read_intel(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                       sim::Unit& unit) {
    // Every unit has Vision and WaterVision, 10 unless its blueprint
    // says otherwise (Moho's RUnitBlueprint defaults).
    unit.add_intel("Vision", 10.0f);
    unit.add_intel("WaterVision", 10.0f);
    store.push_lua_table(entry, L);
    lua_pushstring(L, "Intel");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        struct IntelField {
            const char* bp_field;
            const char* intel_type;
        };
        static const IntelField fields[] = {
            {"VisionRadius", "Vision"}, {"WaterVisionRadius", "WaterVision"},
            {"RadarRadius", "Radar"},   {"SonarRadius", "Sonar"},
            {"OmniRadius", "Omni"},
        };
        for (auto& f : fields) {
            lua_pushstring(L, f.bp_field);
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1)) {
                const auto r = static_cast<f32>(lua_tonumber(L, -1));
                const std::string_view type = f.intel_type;
                if (type == "Vision" || type == "WaterVision")
                    unit.set_intel_radius(f.intel_type, std::max(r, 0.0f));
                else if (r > 0.0f) unit.add_intel(f.intel_type, r);
            }
            lua_pop(L, 1);
        }

        // Intel the unit has but that starts off: the unit script
        // switches it on (retail SetupIntel -> EnableUnitIntel), and
        // EnableIntel only works on intel the unit has.
        static const char* const flags[] = {"Cloak", "RadarStealth", "SonarStealth"};
        for (const char* f : flags) {
            lua_pushstring(L, f);
            lua_rawget(L, -2);
            if (lua_toboolean(L, -1)) unit.add_intel(f, 0.0f);
            lua_pop(L, 1);
        }
        static const IntelField field_radii[] = {
            {"CloakFieldRadius", "CloakField"},
            {"RadarStealthFieldRadius", "RadarStealthField"},
            {"SonarStealthFieldRadius", "SonarStealthField"},
        };
        for (auto& f : field_radii) {
            lua_pushstring(L, f.bp_field);
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1)) {
                f32 r = static_cast<f32>(lua_tonumber(L, -1));
                if (r > 0.0f) unit.add_intel(f.intel_type, r);
            }
            lua_pop(L, 1);
        }
        // {Min, Max} ranges; blueprint defaults are {0, 0}.
        static const IntelField ranges[] = {
            {"JamRadius", "Jammer"},
            {"SpoofRadius", "Spoof"},
        };
        for (auto& f : ranges) {
            lua_pushstring(L, f.bp_field);
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "Max");
                lua_rawget(L, -2);
                if (lua_isnumber(L, -1)) {
                    f32 r = static_cast<f32>(lua_tonumber(L, -1));
                    if (r > 0.0f) unit.add_intel(f.intel_type, r);
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
        // The jammer's fake blips (M215e): how many, and JamRadius's
        // {Min, Max}.
        {
            const auto number = [&](const char* key) {
                lua_pushstring(L, key);
                lua_rawget(L, -2);
                const f32 v = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
                lua_pop(L, 1);
                return v;
            };
            const f32 blips = number("JammerBlips");
            f32 jam_min = 0.0f;
            f32 jam_max = 0.0f;
            lua_pushstring(L, "JamRadius");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                jam_min = number("Min");
                jam_max = number("Max");
            }
            lua_pop(L, 1);
            if (blips > 0.0f) unit.set_jammer(static_cast<u32>(blips), jam_min, jam_max);
        }
    }
    lua_pop(L, 2); // pop Intel (or nil) + bp table
}

/// Its lifebar, as the strategic view draws it.
static void read_life_bar(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                          sim::Unit& unit) {
    store.push_lua_table(entry, L);
    const int bp = lua_gettop(L);
    sim::Unit::LifeBar bar;
    const auto number = [&](const char* key) {
        lua_pushstring(L, key);
        lua_rawget(L, bp);
        const f32 v = lua_type(L, -1) == LUA_TNUMBER ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
        lua_pop(L, 1);
        return v;
    };
    bar.size = number("LifeBarSize");
    bar.height = number("LifeBarHeight");
    bar.offset = number("LifeBarOffset");
    lua_pushstring(L, "LifeBarRender");
    lua_rawget(L, bp);
    bar.render = lua_isnil(L, -1) || lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_pushstring(L, "Display");
    lua_rawget(L, bp);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "HideLifebars");
        lua_rawget(L, -2);
        bar.hide = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    lua_pop(L, 2); // Display, bp
    unit.set_life_bar(bar);
}

/// Physics.MotionType, and the layer it starts on (for pathfinding);
/// its layer change offset, raised platforms and ground snap.
static void read_motion(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                        sim::Unit& unit) {
    store.push_lua_table(entry, L);
    lua_pushstring(L, "Physics");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "MotionType");
        lua_gettable(L, -2);
        if (lua_isstring(L, -1)) {
            std::string mt = lua_tostring(L, -1);
            unit.set_motion_type(mt);
            if (mt == "RULEUMT_Air") unit.set_layer("Air");
            else if (mt == "RULEUMT_Water" || mt == "RULEUMT_SurfacingSub") unit.set_layer("Water");
            else if (mt == "RULEUMT_Hover")
                unit.set_layer("Land"); // Hover units are always Land layer
            else if (mt == "RULEUMT_Amphibious" || mt == "RULEUMT_AmphibiousFloating")
                unit.set_layer("Land");
            // else keep default "Land"
        }
        lua_pop(L, 1);
        lua_pushstring(L, "LayerChangeOffsetHeight");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1))
            unit.set_layer_change_offset(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
        lua_pushstring(L, "RaisedPlatforms");
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            std::vector<f32> quads;
            const int n = luaL_getn(L, -1);
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(L, -1, i);
                quads.push_back(static_cast<f32>(lua_tonumber(L, -1)));
                lua_pop(L, 1);
            }
            unit.set_raised_platforms(std::move(quads));
        }
        lua_pop(L, 1);
        lua_pushstring(L, "StandUpright");
        lua_gettable(L, -2);
        const bool stand_upright = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_pushstring(L, "SinkLower");
        lua_gettable(L, -2);
        const bool sink_lower = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        unit.set_ground_snap_flags(stand_upright, sink_lower);
    }
    lua_pop(L, 2);
}

/// Its footprints, as Moho resolves them (RUnitBlueprintPhysics::
/// ComputeDerivedQuantities): its own Footprint, then for a mobile
/// unit the class of /lua/footprints.lua nearest it.
static void resolve_footprints(lua_State* L, const sim::SimState& sim, const BlueprintStore& store,
                               const BlueprintEntry& entry, sim::Unit& unit, f32 x, f32 z) {
    store.push_lua_table(entry, L);
    const int bp = lua_gettop(L);
    const auto number = [&](int t, const char* key) {
        lua_pushstring(L, key);
        lua_rawget(L, t);
        const f32 v = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
        lua_pop(L, 1);
        return v;
    };
    const auto cells = [](f32 size) {
        return static_cast<u8>(std::clamp(std::lround(size), 0L, 255L));
    };
    blueprints::Footprint own;
    own.size_x = cells(unit.footprint_size_x());
    own.size_z = cells(unit.footprint_size_z());
    lua_pushstring(L, "Footprint");
    lua_rawget(L, bp);
    if (lua_istable(L, -1)) {
        const int fp = lua_gettop(L);
        own.max_slope = number(fp, "MaxSlope");
        own.min_water_depth = number(fp, "MinWaterDepth");
        own.max_water_depth = number(fp, "MaxWaterDepth");
    }
    lua_pop(L, 1);
    std::string alt_motion;
    lua_pushstring(L, "Physics");
    lua_rawget(L, bp);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "AltMotionType");
        lua_rawget(L, -2);
        if (lua_isstring(L, -1)) alt_motion = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    lua_settop(L, bp - 1);
    const sim::PlacementRules& rules = structure_rules(L, sim, unit.unit_id());
    namespace oc = blueprints::occupancy;
    const u8 build_caps = (rules.on_land ? oc::kLand : 0) | (rules.on_seabed ? oc::kSeabed : 0) |
                          (rules.on_sub ? oc::kSub : 0) | (rules.on_water ? oc::kWater : 0);
    unit.set_footprints(blueprints::resolve_unit_footprints(
        store.footprint_classes(), own, unit.motion_type(), alt_motion, build_caps));
    // Where it is made decides its layer (Moho's Entity::GetStartingLayer),
    // for a structure and for a unit that walks or floats: a land tank made
    // in water, however shallow, stands on the seabed, where it drowns (the
    // original, probed). Ships, subs and aircraft keep their motion type's.
    const std::string& motion = unit.motion_type();
    const bool walks_or_floats = motion == "RULEUMT_Land" || motion == "RULEUMT_Amphibious" ||
                                 motion == "RULEUMT_AmphibiousFloating" ||
                                 motion == "RULEUMT_Hover" || motion == "RULEUMT_Biped";
    if ((!unit.is_mobile() || walks_or_floats) && sim.terrain()) {
        unit.set_layer(sim::starting_layer(unit.footprint().caps, unit.has_category("EXPERIMENTAL"),
                                           *sim.terrain(), x, z));
    }
}

/// A ship's or sub's Physics.Elevation (negative: its draft below the
/// surface) and DiveSurfaceSpeed.
static void read_naval(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                       sim::Unit& unit) {
    store.push_lua_table(entry, L);
    lua_pushstring(L, "Physics");
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "Elevation");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) {
            f32 elev = static_cast<f32>(lua_tonumber(L, -1));
            unit.set_blueprint_elevation_target(elev);
            unit.set_naval_draft(std::abs(elev));
        }
        lua_pop(L, 1);
        // How fast a sub dives and surfaces (M206o; Moho's default 1)
        lua_pushstring(L, "DiveSurfaceSpeed");
        lua_gettable(L, -2);
        if (lua_isnumber(L, -1)) unit.set_dive_surface_speed(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
}

/// An aircraft's Physics.Elevation: its flight altitude.
static void read_air_elevation(lua_State* L, const BlueprintStore& store,
                               const BlueprintEntry& entry, sim::Unit& unit) {
    store.push_lua_table(entry, L);
    lua_pushstring(L, "Physics");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "Elevation");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) {
            unit.set_blueprint_elevation_target(static_cast<f32>(lua_tonumber(L, -1)));
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 2); // Physics table (or nil) + bp table
}

/// An aircraft's Air table: its speeds, turning, landing and attack runs;
/// Physics.AttackElevation; Moho's fallbacks for what it leaves out.
static void read_air(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                     sim::Unit& unit) {
    store.push_lua_table(entry, L);
    lua_pushstring(L, "Air");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        // Air.MaxAirspeed
        lua_pushstring(L, "MaxAirspeed");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) unit.set_max_airspeed(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);

        // Air.TurnSpeed, in radians a second: Moho turns an aircraft
        // by up to TurnSpeed x 0.1 radians a tick (faf-re
        // CUnitMotion). Read as degrees, aircraft had turned 57
        // times too slowly.
        lua_pushstring(L, "TurnSpeed");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) unit.set_turn_rate_rad(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);

        // Air.FlyInWater: it may fly under the water's surface
        lua_pushstring(L, "FlyInWater");
        lua_rawget(L, -2);
        unit.set_fly_in_water(lua_toboolean(L, -1) != 0);
        lua_pop(L, 1);

        // Air.AutoLandTime and StartTurnDistance: an idle aircraft
        // lands that long after its orders run out, near its place
        // (item 6; Moho's StartTurnDistance defaults to 3 x SizeZ).
        {
            lua_pushstring(L, "AutoLandTime");
            lua_rawget(L, -2);
            const f32 land = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
            lua_pop(L, 1);
            lua_pushstring(L, "StartTurnDistance");
            lua_rawget(L, -2);
            f32 turn = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
            lua_pop(L, 1);
            if (turn == 0.0f) turn = unit.size_z() * 3.0f;
            unit.set_auto_land(land, turn);
        }

        // Air.AccelerateRate
        lua_pushstring(L, "AccelerateRate");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) unit.set_accel_rate(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);

        // A winged aircraft's attack runs (Moho's defaults where
        // a field is missing).
        const int air = lua_gettop(L);
        const auto number = [&](const char* key, f32 fallback) {
            lua_pushstring(L, key);
            lua_rawget(L, air);
            const f32 v =
                lua_type(L, -1) == LUA_TNUMBER ? static_cast<f32>(lua_tonumber(L, -1)) : fallback;
            lua_pop(L, 1);
            return v;
        };
        const auto flag = [&](const char* key) {
            lua_pushstring(L, key);
            lua_rawget(L, air);
            const bool v = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) != 0
                                                          : lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            return v;
        };
        sim::AirCombatRules rules;
        rules.winged = flag("Winged");
        rules.min_airspeed = number("MinAirspeed", 0.0f);
        rules.combat_turn_speed = number("CombatTurnSpeed", 1.0f);
        rules.tight_turn_multiplier = number("TightTurnMultiplier", 1.0f);
        rules.sustained_turn_threshold = number("SustainedTurnThreshold", 10.0f);
        rules.engage_distance = number("EngageDistance", 0.0f);
        rules.break_off_trigger = number("BreakOffTrigger", 0.0f);
        rules.break_off_distance = number("BreakOffDistance", 0.0f);
        rules.break_off_if_near_new_target = flag("BreakOffIfNearNewTarget");
        rules.random_break_off_distance_mult = number("RandomBreakOffDistanceMult", 1.5f);
        rules.random_min_change_combat_state_time = number("RandomMinChangeCombatStateTime", 3.0f);
        rules.random_max_change_combat_state_time = number("RandomMaxChangeCombatStateTime", 6.0f);
        rules.predict_ahead_for_bomb_drop = number("PredictAheadForBombDrop", 0.0f);
        rules.k_turn = number("KTurn", 3.0f);
        rules.k_turn_damping = number("KTurnDamping", 3.0f);
        rules.k_roll = number("KRoll", 3.0f);
        rules.k_roll_damping = number("KRollDamping", 3.0f);
        rules.bank_forward = flag("BankForward");
        rules.k_move = number("KMove", 1.0f);
        rules.k_move_damping = number("KMoveDamping", 1.0f);
        rules.k_lift = number("KLift", 1.0f);
        rules.k_lift_damping = number("KLiftDamping", 1.0f);
        rules.lift_factor = number("LiftFactor", 5.0f);
        // A hovering aircraft's circling (CalcCirclingOrientation).
        rules.hover_over_attack = flag("HoverOverAttack");
        {
            lua_pushstring(L, "CirclingDirChange");
            lua_rawget(L, air);
            rules.circling_dir_change = lua_isnil(L, -1) || flag("CirclingDirChange");
            lua_pop(L, 1);
        }
        rules.circling_turn_mult = number("CirclingTurnMult", 3.0f);
        rules.circling_radius_min = number("CirclingRadiusChangeMinRatio", 0.6f);
        rules.circling_radius_max = number("CirclingRadiusChangeMaxRatio", 0.9f);
        rules.circling_radius_vs_air_mult = number("CirclingRadiusVsAirMult", 1.0f);
        rules.circling_elevation_ratio = number("CirclingElevationChangeRatio", 0.25f);
        rules.circling_change_frequency = number("CirclingFlightChangeFrequency", 2.0f);
        rules.bank_factor = number("BankFactor", 0.5f);
        unit.set_air_combat_rules(rules);
    }
    lua_pop(L, 2); // Air table (or nil) + bp table

    // Physics.AttackElevation: its height over a ground target in a
    // run (0: its Elevation, as Moho derives it).
    {
        sim::AirCombatRules rules = unit.air_combat_rules();
        store.push_lua_table(entry, L);
        lua_pushstring(L, "Physics");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "AttackElevation");
            lua_rawget(L, -2);
            if (lua_type(L, -1) == LUA_TNUMBER)
                rules.attack_elevation = static_cast<f32>(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
        if (rules.attack_elevation <= 0.0f) rules.attack_elevation = unit.elevation_target();
        unit.set_air_combat_rules(rules);
    }

    // Fallbacks: if Air subtable was missing or incomplete
    if (unit.max_airspeed() <= 0 && unit.max_speed() > 0) unit.set_max_airspeed(unit.max_speed());
    if (unit.accel_rate() <= 0 && unit.max_airspeed() > 0)
        unit.set_accel_rate(unit.max_airspeed() * 0.5f);
    if (unit.turn_rate_rad() <= 0) {
        unit.set_turn_rate_rad(1.0f);
    }
    sim::AirCombatRules rules = unit.air_combat_rules();
    if (rules.min_airspeed <= 0.0f) {
        rules.min_airspeed = unit.max_airspeed();
    }
    rules.circling_min_airspeed = rules.min_airspeed;
    unit.set_air_combat_rules(rules);
}

/// General.CommandCaps / ToggleCaps: the orders and toggles a unit
/// starts with, as in Moho (scripts add and remove from there). The
/// UI's orders panel and command-mode clicks go by them.
static void read_command_caps(lua_State* L, const BlueprintStore& store,
                              const BlueprintEntry& entry, sim::Unit& unit) {
    store.push_lua_table(entry, L);
    lua_pushstring(L, "General");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        for (const bool command : {true, false}) {
            lua_pushstring(L, command ? "CommandCaps" : "ToggleCaps");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    if (lua_type(L, -2) == LUA_TSTRING && lua_toboolean(L, -1)) {
                        const std::string cap = lua_tostring(L, -2);
                        if (command) unit.add_command_cap(cap);
                        else unit.add_toggle_cap(cap);
                    }
                    lua_pop(L, 1); // value; key stays for lua_next
                }
            }
            lua_pop(L, 1); // caps table (or nil)
        }
        lua_pushstring(L, "SelectionPriority");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) {
            unit.set_selection_priority(static_cast<int>(lua_tonumber(L, -1)));
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 2);                // General (or nil) + bp table
    unit.snapshot_command_caps(); // what RestoreCommandCaps returns to
    unit.snapshot_toggle_caps();
}

/// An aircraft's Physics.FuelUseTime and FuelRechargeRate.
static void read_fuel(lua_State* L, const BlueprintStore& store, const BlueprintEntry& entry,
                      sim::Unit& unit) {
    store.push_lua_table(entry, L);
    lua_pushstring(L, "Physics");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "FuelUseTime");
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) {
            f32 t = static_cast<f32>(lua_tonumber(L, -1));
            unit.set_fuel_use_time(t);
            if (t > 0) unit.set_fuel_ratio(1.0f); // start with full fuel
            // FuelUseTime=0 means infinite fuel -- fuel_ratio_ stays at -1 (sentinel)
        }
        lua_pop(L, 1);
        lua_pushstring(L, "FuelRechargeRate"); // refuelling (M206r)
        lua_rawget(L, -2);
        if (lua_isnumber(L, -1)) unit.set_fuel_recharge_rate(static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    lua_pop(L, 2); // Physics table (or nil) + bp table
}

/// Where a finished unit stands as it is made, by its starting layer (Moho's
/// Unit constructor: IUnit::CalcSpawnElevation): on the ground or the seabed,
/// or at the water's surface. A sub is put at its depth (start_submerged),
/// and an aircraft at its height (create_unit_core). An experimental aircraft
/// starts on the ground (GetStartingLayer gives an EXPERIMENTAL footprint
/// with AIR the Land) and takes off on its first tick.
static void place_at_spawn(const sim::SimState& sim, sim::Unit& unit, f32 x, f32 z) {
    const map::Terrain* terrain = sim.terrain();
    if (!terrain) return;
    if (unit.can_fly() && unit.has_category("EXPERIMENTAL")) {
        unit.set_layer("Land");
        unit.set_lifts_off(true);
    }
    const std::string& layer = unit.layer();
    sim::Vector3 at = unit.position();
    if (layer == "Land" || layer == "Seabed") {
        at.y = terrain->get_terrain_height(x, z);
    } else if (layer == "Water" && terrain->has_water() &&
               terrain->water_elevation() > terrain->get_terrain_height(x, z)) {
        // (A ship made on dry ground keeps its place: not probed.)
        at.y = terrain->water_elevation();
    } else {
        return;
    }
    unit.set_position({x, at.y, z});
}

/// A surfacing sub made where it can dive starts submerged.
static void start_submerged(const sim::SimState& sim, sim::Unit& unit, f32 x, f32 z) {
    if (sim.terrain() &&
        std::strcmp(sim::starting_layer(unit.footprint().caps, unit.has_category("EXPERIMENTAL"),
                                        *sim.terrain(), x, z),
                    "Sub") == 0) {
        unit.start_submerged();
        unit.set_position({x, sim.terrain()->water_elevation() + unit.sub_elevation(), z});
    }
}

void read_unit_blueprint(lua_State* L, const sim::SimState& sim, const BlueprintStore& store,
                         const BlueprintEntry& entry, sim::Unit& unit, bool being_built, f32 x,
                         f32 z) {
    read_health(L, store, entry, unit, being_built);
    read_drive(L, store, entry, unit);
    read_weapons(L, store, entry, unit);
    read_economy(L, store, entry, unit);
    read_categories(L, store, entry, unit);
    read_build(L, store, entry, unit);
    read_defense(L, store, entry, unit);
    read_shape(L, store, entry, unit);
    read_transport(L, store, entry, unit);
    read_intel(L, store, entry, unit);
    read_life_bar(L, store, entry, unit);
    // The layer it moves on, then its footprints by it
    read_motion(L, store, entry, unit);
    resolve_footprints(L, sim, store, entry, unit, x, z);
    if (unit.is_naval()) read_naval(L, store, entry, unit);
    if (unit.motion_type() == "RULEUMT_SurfacingSub") start_submerged(sim, unit, x, z);
    if (unit.can_fly()) {
        read_air_elevation(L, store, entry, unit);
        read_air(L, store, entry, unit);
    }
    if (!being_built) place_at_spawn(sim, unit, x, z);
    read_command_caps(L, store, entry, unit);
    if (unit.is_air_unit()) read_fuel(L, store, entry, unit);
}

} // namespace osc::lua
