#include "lua/sim_bindings.hpp"
#include "lua/category_utils.hpp"
#include "lua/lua_state.hpp"
#include "core/game_state.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/bone_cache.hpp"
#include "sim/bone_data.hpp"
#include "sim/entity.hpp"
#include "sim/economy_event.hpp"
#include "sim/ieffect.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/prop.hpp"
#include "sim/shield.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"
#include "blueprints/blueprint_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

static sim::SimState* get_sim(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return sim;
}

/// Resolve an army argument that can be either a 1-based number or a name string.
/// Returns 0-based index, or -1 if not found.
static i32 resolve_army(lua_State* L, int arg, sim::SimState* sim) {
    if (lua_isnumber(L, arg)) {
        return static_cast<i32>(lua_tonumber(L, arg)) - 1;
    }
    if (lua_isstring(L, arg) && sim) {
        const char* name = lua_tostring(L, arg);
        auto* brain = sim->get_army_by_name(name);
        if (brain) return brain->index();
    }
    return -1;
}

// ====================================================================
// Entity creation
// ====================================================================

/// _c_CreateEntity(self, spec) — creates a C++ Entity and stores it.
static int l_c_CreateEntity(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return luaL_error(L, "_c_CreateEntity: no SimState");

    auto entity = std::make_unique<sim::Entity>();
    u32 id = sim->entity_registry().register_entity(std::move(entity));
    auto* ent = sim->entity_registry().find(id);

    // Store C++ pointer in the Lua table (self at index 1)
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ent);
    lua_rawset(L, 1);

    lua_pushstring(L, "_entity_id");
    lua_pushnumber(L, id);
    lua_rawset(L, 1);

    // Store Lua table ref for reverse lookup
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ent->set_lua_table_ref(ref);

    return 0;
}

// ====================================================================
// Shared unit creation core — creates C++ Unit + Lua table.
// Returns entity ID (0 on failure). Leaves Lua table on top of stack.
// If being_built: fraction_complete=0, health=1, is_being_built=true.
// Does NOT call any Lua callbacks — callers handle those.
// ====================================================================
static u32 create_unit_core(lua_State* L, const char* bp_id, int army,
                             f32 x, f32 y, f32 z, bool being_built) {
    auto* sim = get_sim(L);
    if (!sim) return 0;

    auto* store = sim->blueprint_store();

    auto unit = std::make_unique<sim::Unit>();
    unit->set_blueprint_id(bp_id);
    unit->set_unit_id(bp_id);
    unit->set_army(army);
    unit->set_position({x, y, z});

    if (being_built) {
        unit->set_fraction_complete(0.0f);
        unit->set_is_being_built(true);
    } else {
        unit->set_fraction_complete(1.0f);
    }

    // Read blueprint data
    if (store) {
        auto* entry = store->find(bp_id);
        if (entry) {
            // Health — try top-level MaxHealth, fall back to Defense.MaxHealth
            auto hp = store->get_number_field(*entry, "MaxHealth", L);
            if (!hp) {
                store->push_lua_table(*entry, L);
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
                unit->set_max_health(static_cast<f32>(*hp));
                unit->set_health(being_built ? 1.0f : static_cast<f32>(*hp));
            }

            // Physics.MaxSpeed
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Physics");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "MaxSpeed");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    unit->set_max_speed(static_cast<f32>(lua_tonumber(L, -1)));
                lua_pop(L, 1);
            }
            lua_pop(L, 2);

            // Weapons
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Weapon");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                int wep_table = lua_gettop(L);
                for (int wi = 1; ; wi++) {
                    lua_rawgeti(L, wep_table, wi);
                    if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
                    if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
                    int we = lua_gettop(L);
                    auto weapon = std::make_unique<sim::Weapon>();
                    weapon->weapon_index = wi - 1;

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

                    lua_pushstring(L, "FireOnDeath");
                    lua_gettable(L, we);
                    if (lua_isboolean(L, -1)) weapon->fire_on_death = lua_toboolean(L, -1) != 0;
                    lua_pop(L, 1);

                    lua_pushstring(L, "ManualFire");
                    lua_gettable(L, we);
                    if (lua_isboolean(L, -1)) weapon->manual_fire = lua_toboolean(L, -1) != 0;
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
                                if (lua_isstring(L, -1))
                                    weapon->muzzle_bone_name = lua_tostring(L, -1);
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
                    if (lua_isnumber(L, -1))
                        weapon->firing_randomness = static_cast<f32>(lua_tonumber(L, -1));
                    lua_pop(L, 1);

                    // ProjectileId (projectile blueprint reference)
                    lua_pushstring(L, "ProjectileId");
                    lua_gettable(L, we);
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        std::string pid = lua_tostring(L, -1);
                        std::transform(pid.begin(), pid.end(), pid.begin(),
                                       [](unsigned char c) { return std::tolower(c); });
                        weapon->projectile_bp_id = pid;
                    }
                    lua_pop(L, 1);

                    // RangeCategory → fire_target_layer_caps bitmask
                    lua_pushstring(L, "RangeCategory");
                    lua_gettable(L, we);
                    if (lua_isstring(L, -1)) {
                        std::string rc = lua_tostring(L, -1);
                        if (rc == "UWRC_AntiAir")
                            weapon->fire_target_layer_caps = sim::layer_to_bit("Air");
                        else if (rc == "UWRC_DirectFire")
                            weapon->fire_target_layer_caps = sim::layer_to_bit("Land") | sim::layer_to_bit("Water") | sim::layer_to_bit("Seabed");
                        else if (rc == "UWRC_AntiNavy")
                            weapon->fire_target_layer_caps = sim::layer_to_bit("Water") | sim::layer_to_bit("Sub") | sim::layer_to_bit("Seabed");
                        else if (rc == "UWRC_Countermeasure")
                            weapon->fire_target_layer_caps = 0xFF; // all layers
                        // Default (unrecognized): 0xFF = all layers
                    }
                    lua_pop(L, 1);

                    // NeedToComputeBombDrop (boolean — bomb weapons)
                    lua_pushstring(L, "NeedToComputeBombDrop");
                    lua_gettable(L, we);
                    if (lua_isboolean(L, -1) && lua_toboolean(L, -1))
                        weapon->need_compute_bomb_drop = true;
                    lua_pop(L, 1);

                    // BombDropThreshold (distance threshold for overhead check)
                    lua_pushstring(L, "BombDropThreshold");
                    lua_gettable(L, we);
                    if (lua_isnumber(L, -1))
                        weapon->bomb_drop_threshold = static_cast<f32>(lua_tonumber(L, -1));
                    lua_pop(L, 1);

                    lua_pushvalue(L, we);
                    weapon->blueprint_ref = luaL_ref(L, LUA_REGISTRYINDEX);

                    spdlog::debug("  Weapon[{}]: {} range={} dmg={} rof={} vel={}",
                                  wi - 1, weapon->label, weapon->max_range,
                                  weapon->damage, weapon->rate_of_fire,
                                  weapon->muzzle_velocity);

                    unit->add_weapon(std::move(weapon));
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 2);
        }

        // Economy
        {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Economy");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "StorageMass");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1)) unit->economy().storage_mass = lua_tonumber(L, -1);
                lua_pop(L, 1);

                lua_pushstring(L, "StorageEnergy");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1)) unit->economy().storage_energy = lua_tonumber(L, -1);
                lua_pop(L, 1);

                lua_pushstring(L, "ProductionPerSecondMass");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1)) unit->economy().production_mass = lua_tonumber(L, -1);
                lua_pop(L, 1);

                lua_pushstring(L, "ProductionPerSecondEnergy");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1)) unit->economy().production_energy = lua_tonumber(L, -1);
                lua_pop(L, 1);

                lua_pushstring(L, "MaintenanceConsumptionPerSecondEnergy");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1)) unit->economy().consumption_energy = lua_tonumber(L, -1);
                lua_pop(L, 1);

                lua_pushstring(L, "ConsumptionPerSecondMass");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1)) unit->economy().consumption_mass = lua_tonumber(L, -1);
                lua_pop(L, 1);

                if (unit->economy().production_mass > 0.0 ||
                    unit->economy().production_energy > 0.0)
                    unit->economy().production_active = true;
                if (unit->economy().consumption_energy > 0.0 ||
                    unit->economy().consumption_mass > 0.0)
                    unit->economy().consumption_active = true;
            }
            lua_pop(L, 2);
        }

        // Categories
        {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "CategoriesHash");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                int hash_tbl = lua_gettop(L);
                lua_pushnil(L);
                while (lua_next(L, hash_tbl) != 0) {
                    if (lua_isstring(L, -2)) {
                        std::string key = lua_tostring(L, -2);
                        unit->add_category(key);
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 2);
        }

        // Read BuildRate from blueprint Economy.BuildRate
        {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Economy");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "BuildRate");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    unit->set_build_rate(static_cast<f32>(lua_tonumber(L, -1)));
                lua_pop(L, 1);
            }
            lua_pop(L, 2);
        }

        // Defense threat levels (cached for threat queries)
        {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Defense");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "SurfaceThreatLevel");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    unit->set_surface_threat(static_cast<f32>(lua_tonumber(L, -1)));
                lua_pop(L, 1);

                lua_pushstring(L, "AirThreatLevel");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    unit->set_air_threat(static_cast<f32>(lua_tonumber(L, -1)));
                lua_pop(L, 1);

                lua_pushstring(L, "SubThreatLevel");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    unit->set_sub_threat(static_cast<f32>(lua_tonumber(L, -1)));
                lua_pop(L, 1);

                lua_pushstring(L, "EconomyThreatLevel");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    unit->set_economy_threat(static_cast<f32>(lua_tonumber(L, -1)));
                lua_pop(L, 1);

                // ArmorType (for armor damage multipliers)
                lua_pushstring(L, "ArmorType");
                lua_gettable(L, -2);
                if (lua_type(L, -1) == LUA_TSTRING)
                    unit->set_armor_type(lua_tostring(L, -1));
                lua_pop(L, 1);

                // RegenRate (base HP/sec regeneration)
                lua_pushstring(L, "RegenRate");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1))
                    unit->set_regen_rate(static_cast<f32>(lua_tonumber(L, -1)));
                lua_pop(L, 1);
            }
            lua_pop(L, 2);
        }

        // Footprint.SizeX / SizeZ (for pathfinding obstacle marking)
        {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Footprint");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                f32 sx = 0, sz = 0;
                lua_pushstring(L, "SizeX");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1)) sx = static_cast<f32>(lua_tonumber(L, -1));
                lua_pop(L, 1);
                lua_pushstring(L, "SizeZ");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1)) sz = static_cast<f32>(lua_tonumber(L, -1));
                lua_pop(L, 1);
                unit->set_footprint_size(sx, sz);
            }
            lua_pop(L, 2);
        }

        // Physics.SkirtSizeX/Z, SkirtOffsetX/Z (for adjacency detection)
        {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Physics");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                f32 ssx = 0, ssz = 0, sox = 0, soz = 0;
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
                unit->set_skirt(ssx, ssz, sox, soz);
            }
            lua_pop(L, 2);
        }

        // Transport.Class1Capacity / TransportClass (for cargo tracking)
        // NOTE: Using lua_rawget to avoid triggering metamethods on blueprint tables
        {
            store->push_lua_table(*entry, L);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "Transport");
                lua_rawget(L, -2);
                if (lua_istable(L, -1)) {
                    lua_pushstring(L, "Class1Capacity");
                    lua_rawget(L, -2);
                    if (lua_isnumber(L, -1))
                        unit->set_transport_capacity(static_cast<i32>(lua_tonumber(L, -1)));
                    lua_pop(L, 1);

                    lua_pushstring(L, "TransportClass");
                    lua_rawget(L, -2);
                    if (lua_isnumber(L, -1))
                        unit->set_transport_class(static_cast<i32>(lua_tonumber(L, -1)));
                    lua_pop(L, 1);
                }
                lua_pop(L, 1); // pop Transport (or nil)
            }
            lua_pop(L, 1); // pop bp table
        }

        // Intel radii from blueprint (VisionRadius, RadarRadius, etc.)
        {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Intel");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                struct IntelField {
                    const char* bp_field;
                    const char* intel_type;
                };
                static const IntelField fields[] = {
                    {"VisionRadius", "Vision"},
                    {"WaterVisionRadius", "WaterVision"},
                    {"RadarRadius", "Radar"},
                    {"SonarRadius", "Sonar"},
                    {"OmniRadius", "Omni"},
                };
                for (auto& f : fields) {
                    lua_pushstring(L, f.bp_field);
                    lua_rawget(L, -2);
                    if (lua_isnumber(L, -1)) {
                        f32 r = static_cast<f32>(lua_tonumber(L, -1));
                        if (r > 0.0f) unit->init_intel(f.intel_type, r);
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 2); // pop Intel (or nil) + bp table
        }

        // Physics.MotionType → layer override (for pathfinding)
        {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Physics");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "MotionType");
                lua_gettable(L, -2);
                if (lua_isstring(L, -1)) {
                    std::string mt = lua_tostring(L, -1);
                    unit->set_motion_type(mt);
                    if (mt == "RULEUMT_Air") unit->set_layer("Air");
                    else if (mt == "RULEUMT_Water" || mt == "RULEUMT_SurfacingSub")
                        unit->set_layer("Water");
                    else if (mt == "RULEUMT_Hover")
                        unit->set_layer("Land"); // Hover units are always Land layer
                    else if (mt == "RULEUMT_Amphibious" || mt == "RULEUMT_AmphibiousFloating")
                        unit->set_layer("Land");
                    // else keep default "Land"
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 2);
        }

        // Read Physics.Elevation for naval units (negative = draft below water surface)
        if (unit->is_naval()) {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Physics");
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "Elevation");
                lua_gettable(L, -2);
                if (lua_isnumber(L, -1)) {
                    f32 elev = static_cast<f32>(lua_tonumber(L, -1));
                    unit->set_elevation_target(elev);
                    unit->set_naval_draft(std::abs(elev));
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 2);
        }

        // Read Physics.Elevation for air units (target flight altitude)
        if (unit->is_air_unit()) {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Physics");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "Elevation");
                lua_rawget(L, -2);
                if (lua_isnumber(L, -1)) {
                    unit->set_elevation_target(static_cast<f32>(lua_tonumber(L, -1)));
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 2); // Physics table (or nil) + bp table
        }

        // Read Air subtable for air units
        if (unit->is_air_unit()) {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Air");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                // Air.MaxAirspeed
                lua_pushstring(L, "MaxAirspeed");
                lua_rawget(L, -2);
                if (lua_isnumber(L, -1))
                    unit->set_max_airspeed(static_cast<f32>(lua_tonumber(L, -1)));
                lua_pop(L, 1);

                // Air.TurnSpeed (degrees -> radians)
                lua_pushstring(L, "TurnSpeed");
                lua_rawget(L, -2);
                if (lua_isnumber(L, -1)) {
                    f32 deg = static_cast<f32>(lua_tonumber(L, -1));
                    unit->set_turn_rate_rad(deg * 3.14159265f / 180.0f);
                }
                lua_pop(L, 1);

                // Air.AccelerateRate
                lua_pushstring(L, "AccelerateRate");
                lua_rawget(L, -2);
                if (lua_isnumber(L, -1))
                    unit->set_accel_rate(static_cast<f32>(lua_tonumber(L, -1)));
                lua_pop(L, 1);
            }
            lua_pop(L, 2); // Air table (or nil) + bp table

            // Fallbacks: if Air subtable was missing or incomplete
            if (unit->max_airspeed() <= 0 && unit->max_speed() > 0)
                unit->set_max_airspeed(unit->max_speed());
            if (unit->accel_rate() <= 0 && unit->max_airspeed() > 0)
                unit->set_accel_rate(unit->max_airspeed() * 0.5f);
            if (unit->turn_rate_rad() <= 0)
                unit->set_turn_rate_rad(1.5f); // ~86 deg/s default
        }

        // Read General.CrashDamage for air units
        if (unit->is_air_unit()) {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "General");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "CrashDamage");
                lua_rawget(L, -2);
                if (lua_isnumber(L, -1))
                    unit->set_crash_damage(static_cast<f32>(lua_tonumber(L, -1)));
                lua_pop(L, 1);
            }
            lua_pop(L, 2); // General table (or nil) + bp table
        }

        // Read Physics.FuelUseTime for air units
        if (unit->is_air_unit()) {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Physics");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "FuelUseTime");
                lua_rawget(L, -2);
                if (lua_isnumber(L, -1)) {
                    f32 t = static_cast<f32>(lua_tonumber(L, -1));
                    unit->set_fuel_use_time(t);
                    if (t > 0) unit->set_fuel_ratio(1.0f); // start with full fuel
                    // FuelUseTime=0 means infinite fuel -- fuel_ratio_ stays at -1 (sentinel)
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 2); // Physics table (or nil) + bp table
        }
    }

    // Wire bone data from BoneCache + init animated bone matrices
    auto* bc = sim->bone_cache();
    if (bc) {
        unit->set_bone_data(bc->get(bp_id, L));
        unit->init_animated_bones();
    }

    u32 id = sim->entity_registry().register_entity(std::move(unit));
    auto* unit_ptr = static_cast<sim::Unit*>(sim->entity_registry().find(id));
    unit_ptr->navigator().set_sim_state(sim);

    // Set weapon owner back-pointers now that entity ID is assigned
    for (i32 wi = 0; wi < unit_ptr->weapon_count(); ++wi) {
        auto* w = unit_ptr->get_weapon(wi);
        if (w) w->owner_entity_id = id;
    }

    // Create Lua instance table
    lua_newtable(L);

    // Set metatable: prefer __unit_class, fall back to moho.unit_methods
    lua_pushstring(L, "__unit_class");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "moho");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "unit_methods");
            lua_rawget(L, -2);
            lua_remove(L, -2);
        }
    }
    if (lua_istable(L, -1)) {
        lua_setmetatable(L, -2);
    } else {
        lua_pop(L, 1);
    }

    // _c_object
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, unit_ptr);
    lua_rawset(L, -3);

    // Store sim generation for stale-handle detection across reloads
    lua_pushstring(L, "_c_sim_gen");
    lua_pushnumber(L, static_cast<f64>(sim::SimState::sim_generation()));
    lua_rawset(L, -3);

    // Standard fields
    lua_pushstring(L, "EntityId");
    lua_pushnumber(L, id);
    lua_rawset(L, -3);
    lua_pushstring(L, "Army");
    lua_pushnumber(L, army + 1);
    lua_rawset(L, -3);

    // Veterancy fields
    lua_pushstring(L, "VetInstigators");
    lua_newtable(L);
    lua_rawset(L, -3);
    lua_pushstring(L, "VetDamage");
    lua_newtable(L);
    lua_rawset(L, -3);
    lua_pushstring(L, "VetDamageTaken");
    lua_pushnumber(L, 0);
    lua_rawset(L, -3);

    // Layer
    lua_pushstring(L, "Layer");
    lua_pushstring(L, unit_ptr->layer().c_str());
    lua_rawset(L, -3);

    // UnitId field (FA reads self.UnitId)
    lua_pushstring(L, "UnitId");
    lua_pushstring(L, bp_id);
    lua_rawset(L, -3);

    // Store Lua table ref
    lua_pushvalue(L, -1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    unit_ptr->set_lua_table_ref(ref);

    // Air units: start at flight altitude
    if (unit_ptr->is_air_unit()) {
        unit_ptr->set_current_altitude(unit_ptr->elevation_target());
        // Set Y position above terrain
        f32 terrain_h = 0;
        if (sim && sim->terrain())
            terrain_h = sim->terrain()->get_terrain_height(unit_ptr->position().x,
                                                           unit_ptr->position().z);
        auto p = unit_ptr->position();
        p.y = terrain_h + unit_ptr->elevation_target();
        unit_ptr->set_position(p);
        // Initialize heading and orientation
        unit_ptr->set_heading(0);
        unit_ptr->set_orientation(sim::euler_to_quat(0, 0, 0));
    }

    spdlog::debug("Created unit {} (entity #{}) at ({}, {}, {})",
                  bp_id, id, x, y, z);
    return id; // Lua table left on stack
}

/// Helper: call a Lua method on the table at stack_top. Pops nothing extra.
static void call_lua_method(lua_State* L, int table_idx, const char* method,
                             int nargs, const char* label) {
    lua_pushstring(L, method);
    lua_gettable(L, table_idx);
    if (lua_isfunction(L, -1)) {
        // Push self + any args that caller already pushed above the function
        // Caller must push args AFTER calling this, so we do it inline:
        // Actually, the caller passes nargs already-pushed values.
        if (lua_pcall(L, nargs, 0, 0) != 0) {
            spdlog::warn("{} error: {}", label, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1); // pop non-function
        // Also pop the nargs that were pushed for it
        if (nargs > 0) lua_pop(L, nargs);
    }
}

/// CreateUnit(blueprintId, army, x, y, z, qx, qy, qz, qw, layer)
static int l_CreateUnit(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return luaL_error(L, "CreateUnit: no SimState");

    const char* bp_id = luaL_checkstring(L, 1);
    int army = resolve_army(L, 2, sim);
    if (army < 0 || army >= static_cast<int>(sim->army_count())) {
        spdlog::warn("CreateUnit: invalid army index");
        lua_pushnil(L);
        return 1;
    }

    f32 x = 0, y = 0, z = 0;
    if (lua_gettop(L) >= 5) {
        x = static_cast<f32>(lua_tonumber(L, 3));
        y = static_cast<f32>(lua_tonumber(L, 4));
        z = static_cast<f32>(lua_tonumber(L, 5));
    }

    u32 id = create_unit_core(L, bp_id, army, x, y, z, /*being_built=*/false);
    if (id == 0) { lua_pushnil(L); return 1; }

    // Lua table is now on top of stack
    int tbl = lua_gettop(L);

    // OnPreCreate
    lua_pushstring(L, "OnPreCreate");
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("Unit OnPreCreate error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    // OnCreate
    lua_pushstring(L, "OnCreate");
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("Unit OnCreate error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    // OnStopBeingBuilt (only for pre-placed units)
    auto* unit_ptr = static_cast<sim::Unit*>(sim->entity_registry().find(id));
    if (unit_ptr && !unit_ptr->is_being_built()) {
        lua_pushstring(L, "OnStopBeingBuilt");
        lua_gettable(L, tbl);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, tbl);
            lua_pushnil(L);
            lua_pushstring(L, unit_ptr->layer().c_str());
            if (lua_pcall(L, 3, 0, 0) != 0) {
                spdlog::warn("Unit OnStopBeingBuilt error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }

        // Fire adjacency callbacks for pre-placed structures
        unit_ptr = static_cast<sim::Unit*>(sim->entity_registry().find(id));
        if (unit_ptr && !unit_ptr->destroyed()) {
            unit_ptr->fire_adjacency_callbacks(sim->entity_registry(), L);
        }
    }

    return 1; // return Lua table
}

/// Internal: create a unit in "being built" state.
/// Called from C++ build processing via Lua registry.
/// Args: (bp_id, army_1based, x, y, z)
/// Returns: entity_id, lua_table (2 values)
static int l_create_building_unit(lua_State* L) {
    const char* bp_id = luaL_checkstring(L, 1);
    int army = static_cast<int>(lua_tonumber(L, 2)) - 1;
    f32 x = static_cast<f32>(lua_tonumber(L, 3));
    f32 y = static_cast<f32>(lua_tonumber(L, 4));
    f32 z = static_cast<f32>(lua_tonumber(L, 5));

    u32 id = create_unit_core(L, bp_id, army, x, y, z, /*being_built=*/true);
    if (id == 0) {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }

    // Lua table on top of stack
    int tbl = lua_gettop(L);

    // OnPreCreate
    lua_pushstring(L, "OnPreCreate");
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("Building OnPreCreate error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    // OnCreate
    lua_pushstring(L, "OnCreate");
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("Building OnCreate error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    // Do NOT call OnStopBeingBuilt — that happens when construction finishes

    // Return entity_id and the Lua table
    lua_pushnumber(L, id);
    lua_pushvalue(L, tbl);
    // Remove the original table copy, leave the two return values
    lua_remove(L, tbl);
    return 2;
}

// Alternate forms
static int l_CreateUnitHPR(lua_State* L) {
    // CreateUnitHPR(bp, army, x, y, z, heading, pitch, roll)
    // Forward to CreateUnit with position only
    return l_CreateUnit(L);
}

static int l_CreateUnit2(lua_State* L) {
    // CreateUnit2(bp, army, layer, x, z, heading)
    auto* sim = get_sim(L);
    if (!sim) return luaL_error(L, "CreateUnit2: no SimState");

    // Copy string before clearing stack (bp_id points into Lua-managed memory)
    std::string bp_id_str(luaL_checkstring(L, 1));
    int army = resolve_army(L, 2, sim);
    // layer at 3 (ignored for now)
    f32 x = static_cast<f32>(lua_tonumber(L, 4));
    f32 z = static_cast<f32>(lua_tonumber(L, 5));

    // Rewrite stack for l_CreateUnit: bp, army, x, 0, z
    lua_settop(L, 0);
    lua_pushstring(L, bp_id_str.c_str());
    lua_pushnumber(L, army + 1); // re-encode as 1-based; l_CreateUnit calls resolve_army
    lua_pushnumber(L, x);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, z);
    return l_CreateUnit(L);
}

static int l_CreateInitialArmyUnit(lua_State* L) {
    // CreateInitialArmyUnit(army, unitBpId) — note reversed arg order from CreateUnit
    auto* sim = get_sim(L);
    if (!sim) return luaL_error(L, "CreateInitialArmyUnit: no SimState");

    std::string army_str(luaL_checkstring(L, 1));
    std::string bp_id(luaL_checkstring(L, 2));

    i32 army_idx = resolve_army(L, 1, sim);
    auto* brain = sim->get_army(army_idx);
    if (!brain) {
        spdlog::warn("CreateInitialArmyUnit: army '{}' not found", army_str);
        lua_pushnil(L);
        return 1;
    }

    // Read storage from blueprint for initial resource gift
    // (mirrors GiveInitialResources in ACUUnit.lua which errors before running)
    f64 gift_mass = 0.0, gift_energy = 0.0;
    auto* store = sim->blueprint_store();
    auto* entry = store ? store->find(bp_id) : nullptr;
    if (entry) {
        store->push_lua_table(*entry, L);
        lua_pushstring(L, "Economy");
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "StorageMass");
            lua_gettable(L, -2);
            if (lua_isnumber(L, -1)) gift_mass = lua_tonumber(L, -1);
            lua_pop(L, 1);

            lua_pushstring(L, "StorageEnergy");
            lua_gettable(L, -2);
            if (lua_isnumber(L, -1)) gift_energy = lua_tonumber(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 2); // Economy + bp_table
    }

    const auto& pos = brain->start_position();
    lua_settop(L, 0);
    lua_pushstring(L, bp_id.c_str());
    lua_pushnumber(L, army_idx + 1); // 1-based for resolve_army
    lua_pushnumber(L, pos.x);
    lua_pushnumber(L, pos.y);
    lua_pushnumber(L, pos.z);
    int result = l_CreateUnit(L);

    // Give initial resources (mirrors GiveInitialResources in ACUUnit.lua)
    if (gift_mass > 0.0 || gift_energy > 0.0) {
        auto& econ = brain->economy();
        econ.mass.stored = std::min(econ.mass.stored + gift_mass,
                                     econ.mass.max_storage);
        econ.energy.stored = std::min(econ.energy.stored + gift_energy,
                                       econ.energy.max_storage);
        spdlog::debug("GiveInitialResources: army={} mass={:.0f} energy={:.0f}",
                      army_idx, gift_mass, gift_energy);
    }

    return result;
}

// ====================================================================
// Thread / fiber system
// ====================================================================

static int l_ForkThread(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) {
        // Fallback: return nil if no sim
        lua_pushnil(L);
        return 1;
    }
    return sim->thread_manager().fork_thread(L);
}

static int l_KillThread(lua_State* L) {
    // KillThread accepts a thread wrapper table (from ForkThread)
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "_c_ref");
        lua_rawget(L, 1);
        if (lua_isnumber(L, -1)) {
            int ref = static_cast<int>(lua_tonumber(L, -1));
            auto* sim = get_sim(L);
            if (sim && ref >= 0) {
                sim->thread_manager().kill_thread(ref);
            }
        }
        lua_pop(L, 1);
    }
    return 0;
}

static int l_CurrentThread(lua_State* L) {
    // Lua 5.0 doesn't have lua_pushthread.
    // Return nil — threads are rarely inspected directly.
    lua_pushnil(L);
    return 1;
}

static int l_SuspendCurrentThread(lua_State* L) {
    // Equivalent to WaitTicks(1). Only safe inside a coroutine.
    // Guard: if we're the main thread with no active call frames, just return.
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar) == 0 && lua_gettop(L) == 0) {
        spdlog::warn("SuspendCurrentThread called outside coroutine context");
        return 0;
    }
    return lua_yield(L, 0);
}

static int l_ResumeThread(lua_State*) { return 0; }

// ====================================================================
// Game state queries
// ====================================================================

static int l_GetGameTimeSeconds(lua_State* L) {
    auto* sim = get_sim(L);
    lua_pushnumber(L, sim ? sim->game_time() : 0);
    return 1;
}

static int l_GetGameTick(lua_State* L) {
    auto* sim = get_sim(L);
    lua_pushnumber(L, sim ? sim->tick_count() : 0);
    return 1;
}

static int l_SecondsPerTick(lua_State* L) {
    lua_pushnumber(L, sim::SimState::SECONDS_PER_TICK);
    return 1;
}

static int l_ListArmies(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) {
        lua_newtable(L);
        return 1;
    }
    lua_newtable(L);
    for (size_t i = 0; i < sim->army_count(); i++) {
        auto* brain = sim->army_at(i);
        if (brain) {
            lua_pushnumber(L, static_cast<int>(i) + 1);
            lua_pushstring(L, brain->name().c_str());
            lua_settable(L, -3);
        }
    }
    return 1;
}

static int l_GetFocusArmy(lua_State* L) {
    lua_pushstring(L, "__osc_focus_army");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushnumber(L, 0); // default: player army 0
    }
    return 1;
}

static int l_SetFocusArmy(lua_State* L) {
    int army = static_cast<int>(luaL_checknumber(L, 1));
    lua_pushstring(L, "__osc_focus_army");
    lua_pushnumber(L, army);
    lua_rawset(L, LUA_REGISTRYINDEX);
    spdlog::debug("SetFocusArmy: {}", army);
    return 0;
}

static int l_GetArmyBrain(lua_State* L) {
    auto* sim = get_sim(L);
    i32 army_idx = resolve_army(L, 1, sim);
    if (sim) {
        auto* brain = sim->get_army(army_idx);
        if (brain && brain->lua_table_ref() >= 0) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, brain->lua_table_ref());
            return 1;
        }
    }
    // Fallback: return a stub table with metatable
    lua_newtable(L);
    lua_getglobal(L, "moho");
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "aibrain_methods");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_setmetatable(L, -3);
        } else {
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1); // pop moho
    return 1;
}

// ====================================================================
// Entity queries
// ====================================================================

static int l_GetEntityById(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) {
        lua_pushnil(L);
        return 1;
    }
    u32 id = static_cast<u32>(luaL_checknumber(L, 1));
    auto* entity = sim->entity_registry().find(id);
    if (!entity || entity->lua_table_ref() < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, entity->lua_table_ref());
    return 1;
}

static int l_Warp(lua_State* L) {
    // Warp(entity, position) — entity is self table, position is vector
    auto* sim = get_sim(L);
    if (!sim) return 0;

    // Get entity from first arg
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    auto* entity = static_cast<sim::Entity*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!entity) return 0;

    // Get position from second arg
    if (lua_istable(L, 2)) {
        sim::Vector3 v;
        lua_rawgeti(L, 2, 1);
        v.x = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 2);
        v.y = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 2, 3);
        v.z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        entity->set_position(v);
    }
    return 0;
}

static int l_IsDestroyed(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    auto* entity = static_cast<sim::Entity*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    lua_pushboolean(L, !entity || entity->destroyed());
    return 1;
}

// ====================================================================
// Type checks
// ====================================================================

static int l_IsUnit(lua_State* L) {
    if (!lua_istable(L, 1)) { lua_pushboolean(L, 0); return 1; }
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    auto* e = lua_isuserdata(L, -1)
        ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
        : nullptr;
    lua_pop(L, 1);
    lua_pushboolean(L, e && e->is_unit() ? 1 : 0);
    return 1;
}

static int l_IsEntity(lua_State* L) {
    if (!lua_istable(L, 1)) { lua_pushboolean(L, 0); return 1; }
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    bool is_entity = lua_isuserdata(L, -1);
    lua_pop(L, 1);
    lua_pushboolean(L, is_entity ? 1 : 0);
    return 1;
}

static int l_ArmyGetHandicap(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

// ====================================================================
// Terrain queries
// ====================================================================

static int l_GetTerrainHeight(lua_State* L) {
    auto* sim = get_sim(L);
    f32 x = static_cast<f32>(luaL_checknumber(L, 1));
    f32 z = static_cast<f32>(luaL_checknumber(L, 2));
    if (sim && sim->terrain()) {
        lua_pushnumber(L, sim->terrain()->get_terrain_height(x, z));
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

static int l_GetSurfaceHeight(lua_State* L) {
    auto* sim = get_sim(L);
    f32 x = static_cast<f32>(luaL_checknumber(L, 1));
    f32 z = static_cast<f32>(luaL_checknumber(L, 2));
    if (sim && sim->terrain()) {
        lua_pushnumber(L, sim->terrain()->get_surface_height(x, z));
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

static int l_GetTerrainType(lua_State* L) {
    // Terrain type parsing deferred — always returns "Default" for now
    lua_newtable(L);
    lua_pushstring(L, "Name");
    lua_pushstring(L, "Default");
    lua_rawset(L, -3);
    return 1;
}

// ====================================================================
// Categories system
// ====================================================================

// Category metatable — supports __add (union), __sub (difference),
// __mul (intersection) for combining categories into compound trees.
static int category_compound(lua_State* L, const char* op) {
    lua_newtable(L);
    lua_pushstring(L, "__op");    lua_pushstring(L, op);  lua_rawset(L, -3);
    lua_pushstring(L, "__left");  lua_pushvalue(L, 1);    lua_rawset(L, -3);
    lua_pushstring(L, "__right"); lua_pushvalue(L, 2);    lua_rawset(L, -3);
    // Apply category metatable so further chaining works
    lua_pushstring(L, "osc_category_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);
    return 1;
}

static int category_add(lua_State* L) {
    return category_compound(L, "union");
}

static int category_sub(lua_State* L) {
    return category_compound(L, "difference");
}

static int category_mul(lua_State* L) {
    return category_compound(L, "intersection");
}

static int category_tostring(lua_State* L) {
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "__name");
        lua_rawget(L, 1);
        if (lua_isstring(L, -1)) return 1;
        lua_pop(L, 1);
    }
    lua_pushstring(L, "EntityCategory");
    return 1;
}

// categories.__index: lazy-create category objects on demand
static int categories_index(lua_State* L) {
    const char* name = lua_tostring(L, 2);
    if (!name) {
        lua_pushnil(L);
        return 1;
    }

    // Create a category object table
    lua_newtable(L);
    lua_pushstring(L, "__name");
    lua_pushstring(L, name);
    lua_rawset(L, -3);

    // Set category metatable (with __add, __sub, __tostring)
    lua_pushstring(L, "osc_category_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);

    // Cache it in the categories table
    lua_pushvalue(L, 2); // key
    lua_pushvalue(L, -2); // value
    lua_rawset(L, 1); // categories[key] = value

    return 1;
}

static void setup_categories(lua_State* L) {
    // Create category metatable
    lua_newtable(L);
    lua_pushstring(L, "__add");
    lua_pushcfunction(L, category_add);
    lua_rawset(L, -3);
    lua_pushstring(L, "__sub");
    lua_pushcfunction(L, category_sub);
    lua_rawset(L, -3);
    lua_pushstring(L, "__mul");
    lua_pushcfunction(L, category_mul);
    lua_rawset(L, -3);
    lua_pushstring(L, "__tostring");
    lua_pushcfunction(L, category_tostring);
    lua_rawset(L, -3);

    // Store in registry
    lua_pushstring(L, "osc_category_mt");
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);

    // Create the categories global table with __index metamethod
    lua_newtable(L); // categories table

    lua_newtable(L); // metatable
    lua_pushstring(L, "__index");
    lua_pushcfunction(L, categories_index);
    lua_rawset(L, -3);
    lua_setmetatable(L, -2);

    lua_setglobal(L, "categories");
}

// ====================================================================
// Damage system
// ====================================================================

// Damage(instigator, target, amount, damageType)
// or: Damage(instigator, target, amount, vector, damageType)
// FA canonical signature. Calls target:OnDamage(instigator, amount, vector, damageType).
static int l_Damage(lua_State* L) {
    int nargs = lua_gettop(L);
    // arg1 = instigator, arg2 = target, arg3 = amount
    // 4 args: arg4 = damageType (no vector)
    // 5 args: arg4 = vector, arg5 = damageType
    if (!lua_istable(L, 2)) return 0; // target must be a table

    f32 amount = static_cast<f32>(lua_tonumber(L, 3));
    if (amount <= 0) return 0;

    int vector_idx = (nargs >= 5) ? 4 : 0;
    int dtype_idx = (nargs >= 5) ? 5 : 4;

    // Apply armor multiplier
    auto* sim = get_sim(L);
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 2);
    auto* target_e = lua_isuserdata(L, -1)
                         ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                         : nullptr;
    lua_pop(L, 1);
    if (target_e && target_e->is_unit() && sim) {
        auto* target_unit = static_cast<sim::Unit*>(target_e);
        // can_take_damage guard
        if (!target_unit->can_take_damage()) return 0;
        const char* dtype = (dtype_idx > 0 && lua_type(L, dtype_idx) == LUA_TSTRING)
                            ? lua_tostring(L, dtype_idx) : "Normal";
        amount *= sim->armor_definition().get_multiplier(
            target_unit->armor_type(), dtype);
        if (amount <= 0) return 0;
        // Track attacker
        if (lua_istable(L, 1)) {
            lua_pushstring(L, "_c_object");
            lua_rawget(L, 1);
            auto* instigator = lua_isuserdata(L, -1)
                ? static_cast<sim::Entity*>(lua_touserdata(L, -1)) : nullptr;
            lua_pop(L, 1);
            if (instigator) target_unit->set_last_attacker_id(instigator->entity_id());
        }
    }

    // Look up OnDamage method on the target
    lua_pushstring(L, "OnDamage");
    lua_gettable(L, 2); // target["OnDamage"]
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        // Fallback: directly reduce health if no OnDamage handler
        if (target_e && !target_e->destroyed()) {
            target_e->set_health(target_e->health() - amount);
        }
        return 0;
    }

    // Call target:OnDamage(instigator, amount, vector, damageType)
    lua_pushvalue(L, 2); // self (target)
    lua_pushvalue(L, 1); // instigator
    lua_pushnumber(L, amount);
    if (vector_idx > 0) {
        lua_pushvalue(L, vector_idx); // vector
    } else {
        lua_pushnil(L); // no vector provided
    }
    lua_pushvalue(L, dtype_idx); // damageType
    if (lua_pcall(L, 5, 0, 0) != 0) {
        spdlog::warn("OnDamage error: {}", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return 0;
}

// Helper: extract x/z from a Lua position table (array-style [1],[2],[3])
static void extract_position_xz(lua_State* L, int idx, f32& x, f32& z) {
    if (!lua_istable(L, idx)) return;
    lua_pushnumber(L, 1);
    lua_gettable(L, idx);
    x = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushnumber(L, 3);
    lua_gettable(L, idx);
    z = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
}

// Helper: get the C++ Entity* from a Lua entity table
static sim::Entity* extract_entity(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* e = lua_isuserdata(L, -1)
                  ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    return e;
}

// Helper: call OnDamage on a target entity via its Lua table registry ref.
// Returns true if the call succeeded (regardless of whether OnDamage existed).
static bool call_ondamage(lua_State* L, int target_ref,
                          int instigator_idx, f32 amount,
                          int damageType_idx) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, target_ref);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }

    // Apply armor multiplier
    lua_pushstring(L, "_c_object");
    lua_rawget(L, -2);
    auto* e = lua_isuserdata(L, -1)
                  ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    if (e && e->is_unit()) {
        auto* sim = get_sim(L);
        const char* dtype = (damageType_idx > 0 && lua_type(L, damageType_idx) == LUA_TSTRING)
                            ? lua_tostring(L, damageType_idx) : "Normal";
        auto* u = static_cast<sim::Unit*>(e);
        if (sim) {
            amount *= sim->armor_definition().get_multiplier(
                u->armor_type(), dtype);
        }
    }
    if (amount <= 0) { lua_pop(L, 1); return true; }

    lua_pushstring(L, "OnDamage");
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2); // pop non-function + target table
        return true;    // no handler is not an error
    }

    lua_pushvalue(L, -2);            // self (target)
    if (instigator_idx > 0)
        lua_pushvalue(L, instigator_idx);
    else
        lua_pushnil(L);              // no instigator
    lua_pushnumber(L, amount);
    lua_pushnil(L);                  // vector (nil for area damage)
    if (damageType_idx > 0)
        lua_pushvalue(L, damageType_idx);
    else
        lua_pushnil(L);
    if (lua_pcall(L, 5, 0, 0) != 0) {
        spdlog::warn("OnDamage error: {}", lua_tostring(L, -1));
        lua_pop(L, 1);
    }

    lua_pop(L, 1); // pop target table
    return true;
}

// DamageArea(instigator, position, radius, amount, damageType, damageFriendly, damageSelf)
static int l_DamageArea(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return 0;

    f32 px = 0, pz = 0;
    extract_position_xz(L, 2, px, pz);
    f32 radius = static_cast<f32>(lua_tonumber(L, 3));
    f32 amount = static_cast<f32>(lua_tonumber(L, 4));
    if (amount <= 0 || radius <= 0) return 0;

    auto targets = sim->entity_registry().collect_in_radius(px, pz, radius);

    // Get instigator info for friendly-fire checks
    auto* instigator = extract_entity(L, 1);
    i32 instigator_army = instigator ? instigator->army() : -1;
    bool damage_friendly = lua_toboolean(L, 6) != 0;
    bool damage_self = lua_isboolean(L, 7) ? (lua_toboolean(L, 7) != 0) : false;

    for (u32 eid : targets) {
        auto* target = sim->entity_registry().find(eid);
        if (!target || target->destroyed()) continue;
        int ref = target->lua_table_ref();
        if (ref < 0) continue;
        // can_take_damage guard
        if (target->is_unit() &&
            !static_cast<sim::Unit*>(target)->can_take_damage()) continue;

        // Skip friendly units if damageFriendly is false
        if (!damage_friendly && instigator_army >= 0 &&
            target->army() == instigator_army)
            continue;

        // Skip self
        if (!damage_self && instigator && target == instigator) continue;

        call_ondamage(L, ref, 1, amount, 5);
    }
    return 0;
}

// DamageRing(instigator, position, innerRadius, outerRadius, amount, damageType, damageFriendly, damageSelf)
// Same as DamageArea but args shift by 1 (innerRadius at 3, outerRadius at 4).
// For now, use outer radius only (inner radius filtering is a refinement).
static int l_DamageRing(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return 0;

    f32 px = 0, pz = 0;
    extract_position_xz(L, 2, px, pz);
    // innerRadius at 3, outerRadius at 4
    f32 inner_radius = static_cast<f32>(lua_tonumber(L, 3));
    f32 outer_radius = static_cast<f32>(lua_tonumber(L, 4));
    f32 amount = static_cast<f32>(lua_tonumber(L, 5));
    if (amount <= 0 || outer_radius <= 0) return 0;
    f32 inner_r2 = inner_radius * inner_radius;

    auto targets =
        sim->entity_registry().collect_in_radius(px, pz, outer_radius);

    auto* instigator = extract_entity(L, 1);
    i32 instigator_army = instigator ? instigator->army() : -1;
    bool damage_friendly = lua_toboolean(L, 7) != 0;
    bool damage_self =
        lua_isboolean(L, 8) ? (lua_toboolean(L, 8) != 0) : false;

    for (u32 eid : targets) {
        auto* target = sim->entity_registry().find(eid);
        if (!target || target->destroyed()) continue;
        int ref = target->lua_table_ref();
        if (ref < 0) continue;
        // can_take_damage guard
        if (target->is_unit() &&
            !static_cast<sim::Unit*>(target)->can_take_damage()) continue;

        // Exclude entities inside the inner radius
        if (inner_r2 > 0) {
            f32 dx = target->position().x - px;
            f32 dz = target->position().z - pz;
            if (dx * dx + dz * dz < inner_r2) continue;
        }

        if (!damage_friendly && instigator_army >= 0 &&
            target->army() == instigator_army)
            continue;
        if (!damage_self && instigator && target == instigator) continue;

        call_ondamage(L, ref, 1, amount, 6);
    }
    return 0;
}

// ====================================================================
// Speed/pause control (M145d)
// ====================================================================

static int l_sim_RequestPause(lua_State* L) {
    lua_pushstring(L, "__osc_game_state_mgr");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* mgr = static_cast<osc::GameStateManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (mgr) mgr->set_paused(true, nullptr);
    return 0;
}

static int l_sim_ResumeSim(lua_State* L) {
    lua_pushstring(L, "__osc_game_state_mgr");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* mgr = static_cast<osc::GameStateManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (mgr) mgr->set_paused(false, nullptr);
    return 0;
}

// ====================================================================
// Misc stubs
// ====================================================================

static int stub_noop(lua_State*) { return 0; }
static int stub_false(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}
static int stub_true(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}
static int stub_zero(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}
static int stub_nil(lua_State* L) {
    lua_pushnil(L);
    return 1;
}
static int stub_empty_table(lua_State* L) {
    lua_newtable(L);
    return 1;
}
static int stub_return_1000(lua_State* L) {
    lua_pushnumber(L, 1000);
    return 1;
}

// Returns a dummy object (table) whose methods are no-ops that return self
// for chaining (e.g., CreateAnimator(self):PlayAnim(anim):SetRate(1)).
// The metatable's __index returns a function that returns self.
static int dummy_method(lua_State* L) {
    // Return self (first arg) for method chaining
    lua_pushvalue(L, 1);
    return 1;
}
static int dummy_index(lua_State* L) {
    // For any key lookup, return the dummy_method function
    lua_pushcfunction(L, dummy_method);
    return 1;
}
static int stub_dummy_object(lua_State* L) {
    lua_newtable(L); // the dummy object

    // Create or reuse a shared metatable stored in the registry
    lua_pushstring(L, "__dummy_object_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L); // metatable

        lua_pushstring(L, "__index");
        lua_pushcfunction(L, dummy_index);
        lua_rawset(L, -3);

        // Store in registry for reuse
        lua_pushstring(L, "__dummy_object_mt");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_setmetatable(L, -2);
    return 1;
}

// ====================================================================
// IEffect creation helpers
// ====================================================================

/// Extract Entity* from a Lua arg (table with _c_object lightuserdata).
static sim::Entity* effect_check_entity(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* e = lua_isuserdata(L, -1)
                  ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    return e;
}

/// Push a new IEffect Lua table onto the stack with _c_object and __osc_ieffect_mt.
static void push_ieffect_table(lua_State* L, sim::IEffect* fx) {
    lua_newtable(L); // the effect table

    // _c_object = lightuserdata
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, fx);
    lua_rawset(L, -3);

    // Set metatable: cached __osc_ieffect_mt in registry
    lua_pushstring(L, "__osc_ieffect_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        // First time: build from moho.IEffect
        lua_pop(L, 1);
        lua_newtable(L); // metatable

        // __index = moho.IEffect (after flattening, has all methods)
        lua_pushstring(L, "__index");
        lua_pushstring(L, "moho");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "IEffect");
            lua_rawget(L, -2);
            lua_remove(L, -2); // remove moho table
        }
        if (!lua_istable(L, -1)) {
            // moho.IEffect not available yet — use dummy __index
            lua_pop(L, 1);
            lua_pushcfunction(L, [](lua_State* Ls) -> int {
                lua_pushvalue(Ls, 1);
                return 1;
            });
        }
        lua_rawset(L, -3); // mt.__index = moho.IEffect or dummy

        // Cache it
        lua_pushstring(L, "__osc_ieffect_mt");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_setmetatable(L, -2);
}

// CreateEmitterAtEntity(entity, army, blueprintPath)
// CreateEmitterOnEntity(entity, army, blueprintPath)  (identical semantics)
static int l_CreateEmitterAtEntity(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* entity = effect_check_entity(L, 1);
    i32 army = static_cast<i32>(luaL_optnumber(L, 2, 0));
    const char* bp = luaL_optstring(L, 3, "");
    auto* fx = sim->effect_registry().create();
    fx->set_type(sim::EffectType::EMITTER_AT_ENTITY);
    fx->set_entity_id(entity ? entity->entity_id() : 0);
    fx->set_army(army);
    fx->set_blueprint_path(bp);
    push_ieffect_table(L, fx);
    return 1;
}

// CreateEmitterAtBone(entity, boneIndex, army, blueprintPath)
static int l_CreateEmitterAtBone(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* entity = effect_check_entity(L, 1);
    i32 bone = static_cast<i32>(luaL_optnumber(L, 2, -1));
    i32 army = static_cast<i32>(luaL_optnumber(L, 3, 0));
    const char* bp = luaL_optstring(L, 4, "");
    auto* fx = sim->effect_registry().create();
    fx->set_type(sim::EffectType::EMITTER_AT_BONE);
    fx->set_entity_id(entity ? entity->entity_id() : 0);
    fx->set_bone_index(bone);
    fx->set_army(army);
    fx->set_blueprint_path(bp);
    push_ieffect_table(L, fx);
    return 1;
}

// CreateAttachedEmitter(entity, boneIndex, army, blueprintPath)
static int l_CreateAttachedEmitter(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* entity = effect_check_entity(L, 1);
    i32 bone = static_cast<i32>(luaL_optnumber(L, 2, -1));
    i32 army = static_cast<i32>(luaL_optnumber(L, 3, 0));
    const char* bp = luaL_optstring(L, 4, "");
    auto* fx = sim->effect_registry().create();
    fx->set_type(sim::EffectType::ATTACHED_EMITTER);
    fx->set_entity_id(entity ? entity->entity_id() : 0);
    fx->set_bone_index(bone);
    fx->set_army(army);
    fx->set_blueprint_path(bp);
    push_ieffect_table(L, fx);
    return 1;
}

// CreateBeamEmitter(blueprintPath, army)
static int l_CreateBeamEmitter(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    const char* bp = luaL_optstring(L, 1, "");
    i32 army = static_cast<i32>(luaL_optnumber(L, 2, 0));
    auto* fx = sim->effect_registry().create();
    fx->set_type(sim::EffectType::BEAM_EMITTER);
    fx->set_army(army);
    fx->set_blueprint_path(bp);
    push_ieffect_table(L, fx);
    return 1;
}

// CreateAttachedBeam(entity, bone, army, length, thickness, blueprintPath)
static int l_CreateAttachedBeam(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* entity = effect_check_entity(L, 1);
    i32 bone = static_cast<i32>(luaL_optnumber(L, 2, -1));
    i32 army = static_cast<i32>(luaL_optnumber(L, 3, 0));
    // length and thickness at args 4,5 stored as params for future rendering
    const char* bp = luaL_optstring(L, 6, "");
    auto* fx = sim->effect_registry().create();
    fx->set_type(sim::EffectType::ATTACHED_BEAM);
    fx->set_entity_id(entity ? entity->entity_id() : 0);
    fx->set_bone_index(bone);
    fx->set_army(army);
    fx->set_blueprint_path(bp);
    fx->set_param("LENGTH", luaL_optnumber(L, 4, 1.0));
    fx->set_param("THICKNESS", luaL_optnumber(L, 5, 0.1));
    push_ieffect_table(L, fx);
    return 1;
}

// AttachBeamToEntity(emitter, entity, bone, army) → emitter (modified in place)
static int l_AttachBeamToEntity(lua_State* L) {
    // arg1 = emitter (IEffect table), arg2 = entity, arg3 = bone, arg4 = army
    // Read _c_object from emitter
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "_c_object");
        lua_rawget(L, 1);
        auto* fx = static_cast<sim::IEffect*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        if (fx) {
            auto* entity = effect_check_entity(L, 2);
            fx->set_entity_id(entity ? entity->entity_id() : 0);
            fx->set_bone_index(static_cast<i32>(luaL_optnumber(L, 3, -1)));
        }
    }
    lua_pushvalue(L, 1); // return emitter
    return 1;
}

// AttachBeamEntityToEntity(startEntity, startBone, endEntity, endBone, army, bp)
// Also used as CreateBeamEntityToEntity
static int l_AttachBeamEntityToEntity(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* start_ent = effect_check_entity(L, 1);
    i32 start_bone = static_cast<i32>(luaL_optnumber(L, 2, -1));
    auto* end_ent = effect_check_entity(L, 3);
    i32 end_bone = static_cast<i32>(luaL_optnumber(L, 4, -1));
    i32 army = static_cast<i32>(luaL_optnumber(L, 5, 0));
    const char* bp = luaL_optstring(L, 6, "");
    auto* fx = sim->effect_registry().create();
    fx->set_type(sim::EffectType::BEAM_ENTITY_TO_ENTITY);
    fx->set_entity_id(start_ent ? start_ent->entity_id() : 0);
    fx->set_bone_index(start_bone);
    fx->set_target_entity_id(end_ent ? end_ent->entity_id() : 0);
    fx->set_target_bone_index(end_bone);
    fx->set_army(army);
    fx->set_blueprint_path(bp);
    push_ieffect_table(L, fx);
    return 1;
}

// CreateLightParticle(entity, bone, army, size, duration, glowTex, rampTex)
// CreateLightParticleIntel — identical signature
static int l_CreateLightParticle(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return 0;
    auto* entity = effect_check_entity(L, 1);
    i32 bone = static_cast<i32>(luaL_optnumber(L, 2, -1));
    i32 army = static_cast<i32>(luaL_optnumber(L, 3, 0));
    f32 size = static_cast<f32>(luaL_optnumber(L, 4, 1.0));
    f32 duration = static_cast<f32>(luaL_optnumber(L, 5, 1.0));
    const char* glow = luaL_optstring(L, 6, "");
    const char* ramp = luaL_optstring(L, 7, "");
    auto* fx = sim->effect_registry().create();
    fx->set_type(sim::EffectType::LIGHT_PARTICLE);
    fx->set_entity_id(entity ? entity->entity_id() : 0);
    fx->set_bone_index(bone);
    fx->set_army(army);
    fx->set_light_size(size);
    fx->set_light_duration(duration);
    fx->set_glow_texture(glow);
    fx->set_ramp_texture(ramp);
    // Light particles are fire-and-forget, no method chaining needed.
    // Return nil (same as original stub_noop) — FA doesn't use the return value.
    return 0;
}

// CreateDecal(position, heading, tex1, tex2, shaderType, sizeX, sizeZ, lod, duration, army, fidelity)
static int l_CreateDecal(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* fx = sim->effect_registry().create();
    fx->set_type(sim::EffectType::DECAL);
    // position at arg 1 (table with [1],[2],[3])
    if (lua_istable(L, 1)) {
        lua_rawgeti(L, 1, 1); lua_rawgeti(L, 1, 2); lua_rawgeti(L, 1, 3);
        fx->set_offset(
            static_cast<f32>(lua_tonumber(L, -3)),
            static_cast<f32>(lua_tonumber(L, -2)),
            static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 3);
    }
    fx->set_param("ROTATION", luaL_optnumber(L, 2, 0));
    fx->set_blueprint_path(luaL_optstring(L, 3, ""));     // tex1 (albedo)
    fx->set_glow_texture(luaL_optstring(L, 4, ""));       // tex2 (normals)
    fx->set_ramp_texture(luaL_optstring(L, 5, "Albedo")); // shader type
    fx->set_param("WIDTH", luaL_optnumber(L, 6, 1));
    fx->set_param("HEIGHT", luaL_optnumber(L, 7, 1));
    fx->set_param("LOD_CUTOFF", luaL_optnumber(L, 8, 200));
    f64 lifetime = luaL_optnumber(L, 9, 0);
    fx->set_param("LIFETIME", lifetime);
    if (lifetime > 0) fx->set_birth_time(sim->game_time());
    fx->set_army(static_cast<i32>(luaL_optnumber(L, 10, -1)));
    fx->set_param("FIDELITY", luaL_optnumber(L, 11, 0));
    // Return CDecalHandle (for TrashBag)
    push_ieffect_table(L, fx);
    return 1;
}

// CreateSplat(position, heading, texture, sizeX, sizeZ, lod, duration, army, fidelity)
static int l_CreateSplat(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }
    auto* fx = sim->effect_registry().create();
    fx->set_type(sim::EffectType::SPLAT);
    if (lua_istable(L, 1)) {
        lua_rawgeti(L, 1, 1); lua_rawgeti(L, 1, 2); lua_rawgeti(L, 1, 3);
        fx->set_offset(
            static_cast<f32>(lua_tonumber(L, -3)),
            static_cast<f32>(lua_tonumber(L, -2)),
            static_cast<f32>(lua_tonumber(L, -1)));
        lua_pop(L, 3);
    }
    fx->set_param("ROTATION", luaL_optnumber(L, 2, 0));
    fx->set_blueprint_path(luaL_optstring(L, 3, ""));
    fx->set_param("WIDTH", luaL_optnumber(L, 4, 1));
    fx->set_param("HEIGHT", luaL_optnumber(L, 5, 1));
    fx->set_param("LOD_CUTOFF", luaL_optnumber(L, 6, 200));
    f64 lifetime = luaL_optnumber(L, 7, 0);
    fx->set_param("LIFETIME", lifetime);
    if (lifetime > 0) fx->set_birth_time(sim->game_time());
    fx->set_army(static_cast<i32>(luaL_optnumber(L, 8, -1)));
    fx->set_param("FIDELITY", luaL_optnumber(L, 9, 0));
    push_ieffect_table(L, fx);
    return 1;
}

// CreateSplatOnBone(entity, offset, bone, texture, sizeX, sizeZ, lod, duration, army)
static int l_CreateSplatOnBone(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    // arg1 = entity table, arg2 = offset vector, arg3 = bone index
    auto* entity = effect_check_entity(L, 1);
    f32 ox = 0, oy = 0, oz = 0;
    if (lua_istable(L, 2)) {
        lua_rawgeti(L, 2, 1); lua_rawgeti(L, 2, 2); lua_rawgeti(L, 2, 3);
        ox = static_cast<f32>(lua_tonumber(L, -3));
        oy = static_cast<f32>(lua_tonumber(L, -2));
        oz = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 3);
    }
    i32 bone = static_cast<i32>(luaL_optnumber(L, 3, 0));

    auto* fx = sim->effect_registry().create();
    fx->set_type(sim::EffectType::SPLAT);
    if (entity) {
        fx->set_entity_id(entity->entity_id());
        // Use entity position + offset as splat position
        const auto& pos = entity->position();
        fx->set_offset(pos.x + ox, pos.y + oy, pos.z + oz);
    } else {
        fx->set_offset(ox, oy, oz);
    }
    fx->set_bone_index(bone);
    fx->set_blueprint_path(luaL_optstring(L, 4, ""));
    fx->set_param("WIDTH", luaL_optnumber(L, 5, 1));
    fx->set_param("HEIGHT", luaL_optnumber(L, 6, 1));
    fx->set_param("LOD_CUTOFF", luaL_optnumber(L, 7, 200));
    f64 lifetime = luaL_optnumber(L, 8, 0);
    fx->set_param("LIFETIME", lifetime);
    if (lifetime > 0) fx->set_birth_time(sim->game_time());
    fx->set_army(static_cast<i32>(luaL_optnumber(L, 9, -1)));
    push_ieffect_table(L, fx);
    return 1;
}

// ====================================================================
// Manipulator helpers — extract unit from self, resolve bones, set metatable
// ====================================================================

/// Extract Unit* from a Lua self table (arg 1) containing _c_object.
static sim::Unit* manip_check_unit(lua_State* L, int idx = 1) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* e = lua_isuserdata(L, -1)
                  ? static_cast<sim::Entity*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    if (e && e->is_unit()) return static_cast<sim::Unit*>(e);
    return nullptr;
}

/// Resolve bone argument (string name or number) → bone index. Returns 0 if
/// not found. Same logic as moho_bindings resolve_bone_index.
static i32 manip_resolve_bone(const sim::Entity* e, lua_State* L, int arg) {
    auto* bd = e->bone_data();
    if (!bd) return 0;
    if (lua_type(L, arg) == LUA_TSTRING) {
        std::string name = lua_tostring(L, arg);
        i32 idx = bd->find_bone(name);
        return (idx >= 0) ? idx : 0;
    }
    if (lua_type(L, arg) == LUA_TNUMBER) {
        i32 idx = static_cast<i32>(lua_tonumber(L, arg));
        return bd->is_valid(idx) ? idx : 0;
    }
    return 0;
}

/// Set a cached metatable for a manipulator Lua table.
/// Registry key = cache_key, copies methods from moho.{moho_class_name}.
/// Also copies inherited methods from moho.manipulator_methods.
static void set_manip_metatable(lua_State* L, int table_idx,
                                 const char* cache_key,
                                 const char* moho_class_name) {
    lua_pushstring(L, cache_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L); // mt
        int mt_idx = lua_gettop(L);

        // mt.__index = mt (self-referencing for method lookup)
        lua_pushstring(L, "__index");
        lua_pushvalue(L, mt_idx);
        lua_rawset(L, mt_idx);

        // Copy methods from moho.{class_name} and moho.manipulator_methods
        lua_pushstring(L, "moho");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            int moho_idx = lua_gettop(L);

            // Copy base manipulator_methods first
            lua_pushstring(L, "manipulator_methods");
            lua_rawget(L, moho_idx);
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    lua_pushvalue(L, -2); // key
                    lua_pushvalue(L, -2); // value
                    lua_rawset(L, mt_idx);
                    lua_pop(L, 1); // pop value, keep key
                }
            }
            lua_pop(L, 1); // manipulator_methods

            // Copy specialized methods (override base where needed)
            lua_pushstring(L, moho_class_name);
            lua_rawget(L, moho_idx);
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    lua_pushvalue(L, -2);
                    lua_pushvalue(L, -2);
                    lua_rawset(L, mt_idx);
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1); // class table
        }
        lua_pop(L, 1); // moho

        // Cache in registry
        lua_pushstring(L, cache_key);
        lua_pushvalue(L, mt_idx);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_setmetatable(L, table_idx);
}

// ====================================================================
// CreateRotator(unit, bone, axis, goal, speed [, accel [, targetSpeed]])
// ====================================================================
static int l_CreateRotator(lua_State* L) {
    auto* unit = manip_check_unit(L, 1);
    if (!unit) return stub_dummy_object(L);

    i32 bone = manip_resolve_bone(unit, L, 2);

    char axis = 'y';
    if (lua_type(L, 3) == LUA_TSTRING) {
        const char* s = lua_tostring(L, 3);
        if (s[0] == 'x' || s[0] == 'X') axis = 'x';
        else if (s[0] == 'z' || s[0] == 'Z') axis = 'z';
    }

    auto manip = std::make_unique<sim::RotateManipulator>();
    manip->set_bone_index(bone);
    manip->set_axis(axis);

    // arg 4 = goal (nil for continuous), arg 5 = speed
    if (lua_type(L, 4) == LUA_TNUMBER) {
        manip->set_goal(static_cast<f32>(lua_tonumber(L, 4)));
    }
    if (lua_type(L, 5) == LUA_TNUMBER) {
        manip->set_speed(static_cast<f32>(lua_tonumber(L, 5)));
    }
    if (lua_type(L, 6) == LUA_TNUMBER) {
        manip->set_accel(static_cast<f32>(lua_tonumber(L, 6)));
    }
    if (lua_type(L, 7) == LUA_TNUMBER) {
        manip->set_target_speed(static_cast<f32>(lua_tonumber(L, 7)));
    }

    auto* raw = unit->add_manipulator(std::move(manip));

    // Create Lua table with _c_object
    lua_newtable(L);
    int tbl = lua_gettop(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, tbl);

    set_manip_metatable(L, tbl, "__osc_rotate_mt", "RotateManipulator");
    return 1;
}

// ====================================================================
// CreateAnimator(unit [, looping])
// ====================================================================
static int l_CreateAnimator(lua_State* L) {
    auto* unit = manip_check_unit(L, 1);
    if (!unit) return stub_dummy_object(L);

    auto manip = std::make_unique<sim::AnimManipulator>();
    auto* raw = unit->add_manipulator(std::move(manip));

    lua_newtable(L);
    int tbl = lua_gettop(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, tbl);

    set_manip_metatable(L, tbl, "__osc_anim_mt", "AnimationManipulator");
    return 1;
}

// ====================================================================
// CreateSlider(unit, bone [, goalX, goalY, goalZ [, speed [, worldUnits]]])
// ====================================================================
static int l_CreateSlider(lua_State* L) {
    auto* unit = manip_check_unit(L, 1);
    if (!unit) return stub_dummy_object(L);

    i32 bone = manip_resolve_bone(unit, L, 2);

    auto manip = std::make_unique<sim::SlideManipulator>();
    manip->set_bone_index(bone);

    if (lua_type(L, 3) == LUA_TNUMBER &&
        lua_type(L, 4) == LUA_TNUMBER &&
        lua_type(L, 5) == LUA_TNUMBER) {
        manip->set_goal(static_cast<f32>(lua_tonumber(L, 3)),
                         static_cast<f32>(lua_tonumber(L, 4)),
                         static_cast<f32>(lua_tonumber(L, 5)));
    }
    if (lua_type(L, 6) == LUA_TNUMBER) {
        manip->set_speed(static_cast<f32>(lua_tonumber(L, 6)));
    }
    if (lua_gettop(L) >= 7) {
        manip->set_world_units(lua_toboolean(L, 7) != 0);
    }

    auto* raw = unit->add_manipulator(std::move(manip));

    lua_newtable(L);
    int tbl = lua_gettop(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, tbl);

    set_manip_metatable(L, tbl, "__osc_slide_mt", "SlideManipulator");
    return 1;
}

// ====================================================================
// CreateAimController(weapon_or_unit, name, yawBone [, pitchBone, muzzleBone])
// ====================================================================
static int l_CreateAimController(lua_State* L) {
    // arg 1 can be either a weapon table or a unit table
    sim::Unit* unit = nullptr;
    if (lua_istable(L, 1)) {
        // Try weapon first (_c_object → Weapon*)
        lua_pushstring(L, "_c_object");
        lua_rawget(L, 1);
        if (lua_isuserdata(L, -1)) {
            auto* ptr = lua_touserdata(L, -1);
            // Check if it's an Entity (unit) or Weapon
            auto* ent = static_cast<sim::Entity*>(ptr);
            if (ent && ent->is_unit()) {
                unit = static_cast<sim::Unit*>(ent);
            }
        }
        lua_pop(L, 1);

        // If not a unit, try _c_unit (weapon tables store owner here)
        if (!unit) {
            lua_pushstring(L, "_c_unit");
            lua_rawget(L, 1);
            if (lua_isuserdata(L, -1)) {
                unit = static_cast<sim::Unit*>(lua_touserdata(L, -1));
            }
            lua_pop(L, 1);
        }
    }

    if (!unit) return stub_dummy_object(L);

    auto manip = std::make_unique<sim::AimManipulator>();

    // arg 2 = name (string, ignored for now — just for labeling)
    // arg 3 = yawBone
    if (lua_gettop(L) >= 3) {
        manip->set_yaw_bone(manip_resolve_bone(unit, L, 3));
    }
    // arg 4 = pitchBone
    if (lua_gettop(L) >= 4) {
        manip->set_pitch_bone(manip_resolve_bone(unit, L, 4));
    }
    // arg 5 = muzzleBone
    if (lua_gettop(L) >= 5) {
        manip->set_muzzle_bone(manip_resolve_bone(unit, L, 5));
    }

    auto* raw = unit->add_manipulator(std::move(manip));

    lua_newtable(L);
    int tbl = lua_gettop(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, tbl);

    set_manip_metatable(L, tbl, "__osc_aim_mt", "AimManipulator");
    return 1;
}

// ====================================================================
// CreateBuilderArmController(unit, yawBone, pitchBone, aimBone)
// ====================================================================
static int l_CreateBuilderArmController(lua_State* L) {
    auto* unit = manip_check_unit(L, 1);
    if (!unit) return stub_dummy_object(L);

    auto manip = std::make_unique<sim::AimManipulator>();

    // arg 2 = yawBone
    if (lua_gettop(L) >= 2) {
        manip->set_yaw_bone(manip_resolve_bone(unit, L, 2));
    }
    // arg 3 = pitchBone
    if (lua_gettop(L) >= 3) {
        manip->set_pitch_bone(manip_resolve_bone(unit, L, 3));
    }
    // arg 4 = aimBone (used as muzzle reference)
    if (lua_gettop(L) >= 4) {
        manip->set_muzzle_bone(manip_resolve_bone(unit, L, 4));
    }

    auto* raw = unit->add_manipulator(std::move(manip));

    lua_newtable(L);
    int tbl = lua_gettop(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, tbl);

    // Use AimManipulator metatable — has SetAimingArc, SetPrecedence, etc.
    set_manip_metatable(L, tbl, "__osc_aim_mt", "AimManipulator");
    return 1;
}

// ====================================================================
// CreateCollisionDetector(unit) -> collision detector manipulator
// ====================================================================
static int l_CreateCollisionDetector(lua_State* L) {
    auto* unit = manip_check_unit(L, 1);
    if (!unit) return stub_dummy_object(L);

    auto manip = std::make_unique<sim::CollisionDetectorManipulator>();
    auto* raw = unit->add_manipulator(std::move(manip));

    lua_newtable(L);
    int tbl = lua_gettop(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, tbl);

    set_manip_metatable(L, tbl, "__osc_coldet_mt", "CollisionManipulator");
    return 1;
}

// ====================================================================
// CreateFootPlantController(unit, footBone, kneeBone, hipBone,
//                           straightLegs, maxFootFall) -> manipulator
// ====================================================================
static int l_CreateFootPlantController(lua_State* L) {
    auto* unit = manip_check_unit(L, 1);
    if (!unit) return stub_dummy_object(L);

    auto manip = std::make_unique<sim::FootPlantManipulator>();
    if (lua_gettop(L) >= 2) manip->set_foot_bone(manip_resolve_bone(unit, L, 2));
    if (lua_gettop(L) >= 3) manip->set_knee_bone(manip_resolve_bone(unit, L, 3));
    if (lua_gettop(L) >= 4) manip->set_hip_bone(manip_resolve_bone(unit, L, 4));
    if (lua_gettop(L) >= 5) manip->set_straight_legs(lua_toboolean(L, 5) != 0);
    if (lua_gettop(L) >= 6) manip->set_max_foot_fall(static_cast<f32>(luaL_optnumber(L, 6, 0.0)));

    auto* raw = unit->add_manipulator(std::move(manip));

    lua_newtable(L);
    int tbl = lua_gettop(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, tbl);

    set_manip_metatable(L, tbl, "__osc_footplant_mt", "FootPlantManipulator");
    return 1;
}

// ====================================================================
// CreateSlaver(unit, slaveBone, masterBone) -> slaver manipulator
// ====================================================================
static int l_CreateSlaver(lua_State* L) {
    auto* unit = manip_check_unit(L, 1);
    if (!unit) return stub_dummy_object(L);

    auto manip = std::make_unique<sim::SlaverManipulator>();
    if (lua_gettop(L) >= 2) manip->set_slave_bone(manip_resolve_bone(unit, L, 2));
    if (lua_gettop(L) >= 3) manip->set_master_bone(manip_resolve_bone(unit, L, 3));

    auto* raw = unit->add_manipulator(std::move(manip));

    lua_newtable(L);
    int tbl = lua_gettop(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, tbl);

    set_manip_metatable(L, tbl, "__osc_slaver_mt", "SlaveManipulator");
    return 1;
}

// ====================================================================
// CreateStorageManipulator(unit) -> storage manipulator (visual)
// ====================================================================
static int l_CreateStorageManipulator(lua_State* L) {
    auto* unit = manip_check_unit(L, 1);
    if (!unit) return stub_dummy_object(L);

    auto manip = std::make_unique<sim::StorageManipulator>();
    auto* raw = unit->add_manipulator(std::move(manip));

    lua_newtable(L);
    int tbl = lua_gettop(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, tbl);

    set_manip_metatable(L, tbl, "__osc_storage_mt", "StorageManipulator");
    return 1;
}

// ====================================================================
// CreateThrustController(unit) -> thrust manipulator (visual)
// ====================================================================
static int l_CreateThrustController(lua_State* L) {
    auto* unit = manip_check_unit(L, 1);
    if (!unit) return stub_dummy_object(L);

    auto manip = std::make_unique<sim::ThrustManipulator>();
    auto* raw = unit->add_manipulator(std::move(manip));

    lua_newtable(L);
    int tbl = lua_gettop(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, raw);
    lua_rawset(L, tbl);

    set_manip_metatable(L, tbl, "__osc_thrust_mt", "ThrustManipulator");
    return 1;
}

// ====================================================================
// CreateResourceDeposit(type, x, y, z, size) -> nil
// ====================================================================
static int l_CreateResourceDeposit(lua_State* L) {
    auto* sim_state = get_sim(L);
    if (!sim_state) return 0;

    const char* type_str = luaL_optstring(L, 1, "Mass");
    f32 x = static_cast<f32>(luaL_optnumber(L, 2, 0.0));
    f32 y = static_cast<f32>(luaL_optnumber(L, 3, 0.0));
    f32 z = static_cast<f32>(luaL_optnumber(L, 4, 0.0));
    f32 size = static_cast<f32>(luaL_optnumber(L, 5, 1.0));

    sim::ResourceDeposit dep;
    dep.x = x;
    dep.y = y;
    dep.z = z;
    dep.size = size;
    if (type_str && (type_str[0] == 'H' || type_str[0] == 'h'))
        dep.type = sim::ResourceDeposit::Hydrocarbon;
    else
        dep.type = sim::ResourceDeposit::Mass;

    sim_state->add_resource_deposit(dep);
    return 0;
}

// ====================================================================
// WaitFor(waitable) — yield until manipulator/economy event completes
// ====================================================================
static int l_WaitFor(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;

    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    auto* waitable = lua_isuserdata(L, -1)
                         ? static_cast<sim::Waitable*>(lua_touserdata(L, -1))
                         : nullptr;
    lua_pop(L, 1);

    if (!waitable || waitable->is_cancelled()) return 0;

    // If already done, return immediately (no yield)
    if (waitable->is_done()) return 0;

    // Yield with the waitable pointer as a lightuserdata sentinel.
    // ThreadManager::resume_all() recognizes this and sets wait_until_tick
    // to INT32_MAX. The owner (manipulator tick or economy event tick)
    // will wake the thread when the waitable completes.
    lua_pushlightuserdata(L, waitable);
    return lua_yield(L, 1);
}

// ====================================================================
// CreateEconomyEvent(unit, massAmount, energyAmount, duration) -> handle
// ====================================================================
static int l_CreateEconomyEvent(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    auto* unit = extract_entity(L, 1);
    u32 unit_id = (unit && !unit->destroyed()) ? unit->entity_id() : 0;
    f64 energy = luaL_optnumber(L, 2, 0.0);
    f64 mass = luaL_optnumber(L, 3, 0.0);
    f64 duration = luaL_optnumber(L, 4, 0.0);

    auto* evt = sim->economy_events().create(unit_id, mass, energy, duration);

    // Return a Lua table with _c_object = lightuserdata(EconomyEvent*)
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, static_cast<sim::Waitable*>(evt));
    lua_rawset(L, -3);
    return 1;
}

// EconomyEventIsDone(handle) -> bool
static int l_EconomyEventIsDone(lua_State* L) {
    if (!lua_istable(L, 1)) { lua_pushboolean(L, 1); return 1; }
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    auto* waitable = lua_isuserdata(L, -1)
                         ? static_cast<sim::Waitable*>(lua_touserdata(L, -1))
                         : nullptr;
    lua_pop(L, 1);
    lua_pushboolean(L, (!waitable || waitable->is_done() || waitable->is_cancelled()) ? 1 : 0);
    return 1;
}

// RemoveEconomyEvent(unit, handle) — cancel the economy event
static int l_RemoveEconomyEvent(lua_State* L) {
    // arg1 = unit (not used), arg2 = handle table
    int handle_idx = lua_istable(L, 2) ? 2 : 1;
    if (!lua_istable(L, handle_idx)) return 0;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, handle_idx);
    auto* waitable = lua_isuserdata(L, -1)
                         ? static_cast<sim::Waitable*>(lua_touserdata(L, -1))
                         : nullptr;
    lua_pop(L, 1);
    if (waitable) {
        auto* evt = dynamic_cast<sim::EconomyEvent*>(waitable);
        if (evt) evt->cancel();
    }
    return 0;
}

// ArmyIsCivilian(army_index) -> bool
static int l_ArmyIsCivilian(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushboolean(L, 0); return 1; }
    i32 idx = resolve_army(L, 1, sim);
    if (idx < 0 || idx >= sim->army_count()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    auto* brain = sim->get_army(idx);
    lua_pushboolean(L, brain && brain->is_civilian() ? 1 : 0);
    return 1;
}

// ArmyIsOutOfGame(army_index) -> bool
static int l_ArmyIsOutOfGame(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushboolean(L, 0); return 1; }
    i32 idx = resolve_army(L, 1, sim);
    if (idx < 0 || idx >= sim->army_count()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    auto* brain = sim->get_army(idx);
    lua_pushboolean(L, brain && brain->is_defeated() ? 1 : 0);
    return 1;
}

// EntityCategoryCount(category, units_table) -> number
static int l_EntityCategoryCount(lua_State* L) {
    if (!lua_istable(L, 1) || !lua_istable(L, 2)) {
        lua_pushnumber(L, 0);
        return 1;
    }
    int count = 0;
    for (int i = 1; ; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

        int unit_tbl = lua_gettop(L);
        auto* entity = extract_entity(L, unit_tbl);
        if (entity && entity->is_unit() && !entity->destroyed()) {
            auto* unit = static_cast<sim::Unit*>(entity);
            if (osc::lua::unit_matches_category(L, 1, unit->categories())) {
                count++;
            }
        }
        lua_pop(L, 1);
    }
    lua_pushnumber(L, count);
    return 1;
}

// GetUnitBlueprintByName(bp_id) -> blueprint table or nil
static int l_GetUnitBlueprintByName(lua_State* L) {
    const char* bp_id = luaL_checkstring(L, 1);
    // Look up __blueprints[bp_id]
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, bp_id);
    lua_rawget(L, -2);
    lua_remove(L, -2); // remove __blueprints table
    return 1;
}

// EntityCategoryContains(category, entity) -> bool
static int l_EntityCategoryContains(lua_State* L) {
    if (!lua_istable(L, 1) || !lua_istable(L, 2)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    auto* entity = extract_entity(L, 2);
    if (!entity || !entity->is_unit() || entity->destroyed()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    auto* unit = static_cast<sim::Unit*>(entity);
    bool match = osc::lua::unit_matches_category(L, 1, unit->categories());
    lua_pushboolean(L, match ? 1 : 0);
    return 1;
}

// Helper: iterate a 1-based Lua array of unit tables, filter by category match.
// If keep_matches is true, keep units that match (FilterDown);
// if false, keep units that don't match (FilterOut).
static int category_filter(lua_State* L, bool keep_matches) {
    lua_newtable(L);
    int result = lua_gettop(L);
    int out_idx = 1;

    if (!lua_istable(L, 1) || !lua_istable(L, 2)) return 1;

    for (int i = 1; ; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }

        int unit_tbl = lua_gettop(L);
        auto* entity = extract_entity(L, unit_tbl);
        bool matches = false;
        if (entity && entity->is_unit() && !entity->destroyed()) {
            auto* unit = static_cast<sim::Unit*>(entity);
            matches =
                osc::lua::unit_matches_category(L, 1, unit->categories());
        }

        if (matches == keep_matches) {
            lua_pushnumber(L, out_idx++);
            lua_pushvalue(L, unit_tbl);
            lua_rawset(L, result);
        }
        lua_pop(L, 1); // pop unit table
    }
    return 1;
}

// EntityCategoryFilterDown(category, unitList) -> filtered table
static int l_EntityCategoryFilterDown(lua_State* L) {
    return category_filter(L, true);
}

// EntityCategoryFilterOut(category, unitList) -> filtered table
static int l_EntityCategoryFilterOut(lua_State* L) {
    return category_filter(L, false);
}

// EntityCategoryGetUnitList(category) -> table of blueprint IDs
static int l_EntityCategoryGetUnitList(lua_State* L) {
    lua_newtable(L);
    int result = lua_gettop(L);
    int out_idx = 1;

    auto* sim = get_sim(L);
    if (!sim || !sim->blueprint_store() || !lua_istable(L, 1)) return 1;

    auto* store = sim->blueprint_store();
    auto entries = store->get_all(blueprints::BlueprintType::Unit);
    for (const auto* entry : entries) {
        store->push_lua_table(*entry, L);
        int bp_tbl = lua_gettop(L);

        // Read CategoriesHash from this blueprint
        lua_pushstring(L, "CategoriesHash");
        lua_gettable(L, bp_tbl);
        if (lua_istable(L, -1)) {
            // Collect category strings into a set
            std::unordered_set<std::string> bp_cats;
            int hash_tbl = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, hash_tbl) != 0) {
                if (lua_isstring(L, -2))
                    bp_cats.insert(lua_tostring(L, -2));
                lua_pop(L, 1);
            }

            if (osc::lua::categories_match(L, 1, bp_cats)) {
                lua_pushnumber(L, out_idx++);
                lua_pushstring(L, entry->id.c_str());
                lua_rawset(L, result);
            }
        }
        lua_pop(L, 2); // CategoriesHash + bp_table
    }
    return 1;
}

// Helper: create a simple category table with __name and metatable
static void push_simple_category(lua_State* L, const char* name) {
    lua_newtable(L);
    lua_pushstring(L, "__name");
    lua_pushstring(L, name);
    lua_rawset(L, -3);
    lua_pushstring(L, "osc_category_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_setmetatable(L, -2);
}

// ParseEntityCategory(str) -> category table
// Handles single word ("COMMAND") and space-separated intersection ("TECH1 LAND MOBILE")
static int l_ParseEntityCategory(lua_State* L) {
    const char* raw = lua_tostring(L, 1);
    if (!raw) {
        lua_newtable(L);
        return 1;
    }

    // Uppercase the input
    std::string input(raw);
    for (auto& c : input)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // Split by spaces
    std::vector<std::string> words;
    std::string word;
    for (char c : input) {
        if (c == ' ') {
            if (!word.empty()) { words.push_back(word); word.clear(); }
        } else {
            word += c;
        }
    }
    if (!word.empty()) words.push_back(word);

    if (words.empty()) {
        lua_newtable(L);
        return 1;
    }

    if (words.size() == 1) {
        push_simple_category(L, words[0].c_str());
        return 1;
    }

    // Multiple words: build intersection chain
    // Start with last word, chain backwards
    push_simple_category(L, words.back().c_str());
    for (int i = static_cast<int>(words.size()) - 2; i >= 0; i--) {
        // Stack top is the right operand (accumulated so far)
        int right = lua_gettop(L);
        push_simple_category(L, words[i].c_str());
        int left = lua_gettop(L);

        // Build intersection node
        lua_newtable(L);
        lua_pushstring(L, "__op");
        lua_pushstring(L, "intersection");
        lua_rawset(L, -3);
        lua_pushstring(L, "__left");
        lua_pushvalue(L, left);
        lua_rawset(L, -3);
        lua_pushstring(L, "__right");
        lua_pushvalue(L, right);
        lua_rawset(L, -3);
        lua_pushstring(L, "osc_category_mt");
        lua_rawget(L, LUA_REGISTRYINDEX);
        lua_setmetatable(L, -2);

        // Remove left and right from stack, keep only the new node.
        // Must remove highest index first to avoid index shift.
        lua_remove(L, left);
        lua_remove(L, right);
    }
    return 1;
}

// GetUnitsInRect(x1,z1,x2,z2) or GetUnitsInRect(rect) -> table of units or nil
static int l_GetUnitsInRect(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    f32 x0, z0, x1, z1;
    if (lua_istable(L, 1)) {
        // Rect table: {x0, y0, x1, y1} where y = z coordinate
        lua_rawgeti(L, 1, 1); x0 = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_rawgeti(L, 1, 2); z0 = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_rawgeti(L, 1, 3); x1 = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_rawgeti(L, 1, 4); z1 = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
    } else {
        x0 = static_cast<f32>(lua_tonumber(L, 1));
        z0 = static_cast<f32>(lua_tonumber(L, 2));
        x1 = static_cast<f32>(lua_tonumber(L, 3));
        z1 = static_cast<f32>(lua_tonumber(L, 4));
    }

    auto ids = sim->entity_registry().collect_in_rect(x0, z0, x1, z1);

    // Return nil if no units found (FA uses `GetUnitsInRect(...) or {}`)
    bool found_any = false;
    lua_newtable(L);
    int result = lua_gettop(L);
    int idx = 1;
    for (u32 eid : ids) {
        auto* entity = sim->entity_registry().find(eid);
        if (!entity || !entity->is_unit() || entity->destroyed()) continue;
        auto* unit = static_cast<sim::Unit*>(entity);
        if (unit->lua_table_ref() < 0) continue;

        lua_pushnumber(L, idx++);
        lua_rawgeti(L, LUA_REGISTRYINDEX, unit->lua_table_ref());
        lua_rawset(L, result);
        found_any = true;
    }

    if (!found_any) {
        lua_pop(L, 1); // pop empty table
        lua_pushnil(L);
    }
    return 1;
}

// Math helpers
static int l_EulerToQuaternion(lua_State* L) {
    f32 heading = static_cast<f32>(lua_tonumber(L, 1));
    f32 pitch = static_cast<f32>(lua_tonumber(L, 2));
    f32 roll = static_cast<f32>(lua_tonumber(L, 3));

    f32 ch = std::cos(heading * 0.5f), sh = std::sin(heading * 0.5f);
    f32 cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    f32 cr = std::cos(roll * 0.5f), sr = std::sin(roll * 0.5f);

    lua_newtable(L);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, sr * cp * ch - cr * sp * sh); // x
    lua_settable(L, -3);
    lua_pushnumber(L, 2);
    lua_pushnumber(L, cr * sp * ch + sr * cp * sh); // y
    lua_settable(L, -3);
    lua_pushnumber(L, 3);
    lua_pushnumber(L, cr * cp * sh - sr * sp * ch); // z
    lua_settable(L, -3);
    lua_pushnumber(L, 4);
    lua_pushnumber(L, cr * cp * ch + sr * sp * sh); // w
    lua_settable(L, -3);
    return 1;
}

static int l_OrientFromDir(lua_State* L) {
    // Simplified: return identity quaternion
    lua_newtable(L);
    lua_pushnumber(L, 1); lua_pushnumber(L, 0); lua_settable(L, -3);
    lua_pushnumber(L, 2); lua_pushnumber(L, 0); lua_settable(L, -3);
    lua_pushnumber(L, 3); lua_pushnumber(L, 0); lua_settable(L, -3);
    lua_pushnumber(L, 4); lua_pushnumber(L, 1); lua_settable(L, -3);
    return 1;
}

// ====================================================================
// Vector math — engine-provided C++ globals
// ====================================================================

// Retrieves or creates the shared vector metatable from the registry.
// The metatable provides __index for named access (x->1, y->2, z->3)
// and __newindex for named assignment.
static void push_vector_metatable(lua_State* L) {
    lua_pushstring(L, "osc_vector_mt");
    lua_gettable(L, LUA_REGISTRYINDEX);
    if (!lua_isnil(L, -1)) return; // already created
    lua_pop(L, 1); // pop nil

    lua_newtable(L); // the metatable

    // __index: map "x"->1, "y"->2, "z"->3
    lua_pushstring(L, "__index");
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
        const char* key = lua_tostring(Ls, 2);
        if (key) {
            if (key[0] == 'x' && key[1] == '\0') { lua_rawgeti(Ls, 1, 1); return 1; }
            if (key[0] == 'y' && key[1] == '\0') { lua_rawgeti(Ls, 1, 2); return 1; }
            if (key[0] == 'z' && key[1] == '\0') { lua_rawgeti(Ls, 1, 3); return 1; }
        }
        lua_pushnil(Ls);
        return 1;
    });
    lua_rawset(L, -3);

    // __newindex: map "x"->1, "y"->2, "z"->3
    lua_pushstring(L, "__newindex");
    lua_pushcfunction(L, [](lua_State* Ls) -> int {
        const char* key = lua_tostring(Ls, 2);
        if (key) {
            if (key[0] == 'x' && key[1] == '\0') { lua_pushvalue(Ls, 3); lua_rawseti(Ls, 1, 1); return 0; }
            if (key[0] == 'y' && key[1] == '\0') { lua_pushvalue(Ls, 3); lua_rawseti(Ls, 1, 2); return 0; }
            if (key[0] == 'z' && key[1] == '\0') { lua_pushvalue(Ls, 3); lua_rawseti(Ls, 1, 3); return 0; }
        }
        lua_rawset(Ls, 1);
        return 0;
    });
    lua_rawset(L, -3);

    // Store in registry
    lua_pushstring(L, "osc_vector_mt");
    lua_pushvalue(L, -2);
    lua_settable(L, LUA_REGISTRYINDEX);
}

static void push_vec3(lua_State* L, f32 x, f32 y, f32 z) {
    lua_newtable(L);
    lua_pushnumber(L, 1); lua_pushnumber(L, x); lua_rawset(L, -3);
    lua_pushnumber(L, 2); lua_pushnumber(L, y); lua_rawset(L, -3);
    lua_pushnumber(L, 3); lua_pushnumber(L, z); lua_rawset(L, -3);
    push_vector_metatable(L);
    lua_setmetatable(L, -2);
}

static void read_vec3(lua_State* L, int idx, f32& x, f32& y, f32& z) {
    lua_rawgeti(L, idx, 1); x = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
    lua_rawgeti(L, idx, 2); y = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
    lua_rawgeti(L, idx, 3); z = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
}

static int l_Vector(lua_State* L) {
    f32 x = static_cast<f32>(lua_tonumber(L, 1));
    f32 y = static_cast<f32>(lua_tonumber(L, 2));
    f32 z = static_cast<f32>(lua_tonumber(L, 3));
    push_vec3(L, x, y, z);
    return 1;
}

static int l_Vector2(lua_State* L) {
    f32 x = static_cast<f32>(lua_tonumber(L, 1));
    f32 y = static_cast<f32>(lua_tonumber(L, 2));
    lua_newtable(L);
    lua_pushnumber(L, 1); lua_pushnumber(L, x); lua_rawset(L, -3);
    lua_pushnumber(L, 2); lua_pushnumber(L, y); lua_rawset(L, -3);
    push_vector_metatable(L);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_VDist3(lua_State* L) {
    f32 ax, ay, az, bx, by, bz;
    read_vec3(L, 1, ax, ay, az);
    read_vec3(L, 2, bx, by, bz);
    f32 dx = bx - ax, dy = by - ay, dz = bz - az;
    lua_pushnumber(L, std::sqrt(dx*dx + dy*dy + dz*dz));
    return 1;
}

static int l_VDist3Sq(lua_State* L) {
    f32 ax, ay, az, bx, by, bz;
    read_vec3(L, 1, ax, ay, az);
    read_vec3(L, 2, bx, by, bz);
    f32 dx = bx - ax, dy = by - ay, dz = bz - az;
    lua_pushnumber(L, dx*dx + dy*dy + dz*dz);
    return 1;
}

static int l_VDist2(lua_State* L) {
    f32 x1 = static_cast<f32>(lua_tonumber(L, 1));
    f32 y1 = static_cast<f32>(lua_tonumber(L, 2));
    f32 x2 = static_cast<f32>(lua_tonumber(L, 3));
    f32 y2 = static_cast<f32>(lua_tonumber(L, 4));
    f32 dx = x2 - x1, dy = y2 - y1;
    lua_pushnumber(L, std::sqrt(dx*dx + dy*dy));
    return 1;
}

static int l_VDist2Sq(lua_State* L) {
    f32 x1 = static_cast<f32>(lua_tonumber(L, 1));
    f32 y1 = static_cast<f32>(lua_tonumber(L, 2));
    f32 x2 = static_cast<f32>(lua_tonumber(L, 3));
    f32 y2 = static_cast<f32>(lua_tonumber(L, 4));
    f32 dx = x2 - x1, dy = y2 - y1;
    lua_pushnumber(L, dx*dx + dy*dy);
    return 1;
}

static int l_VAdd(lua_State* L) {
    f32 ax, ay, az, bx, by, bz;
    read_vec3(L, 1, ax, ay, az);
    read_vec3(L, 2, bx, by, bz);
    push_vec3(L, ax+bx, ay+by, az+bz);
    return 1;
}

static int l_VDiff(lua_State* L) {
    f32 ax, ay, az, bx, by, bz;
    read_vec3(L, 1, ax, ay, az);
    read_vec3(L, 2, bx, by, bz);
    push_vec3(L, ax-bx, ay-by, az-bz);
    return 1;
}

static int l_VMult(lua_State* L) {
    f32 vx, vy, vz;
    read_vec3(L, 1, vx, vy, vz);
    f32 s = static_cast<f32>(lua_tonumber(L, 2));
    push_vec3(L, vx*s, vy*s, vz*s);
    return 1;
}

static int l_VDot(lua_State* L) {
    f32 ax, ay, az, bx, by, bz;
    read_vec3(L, 1, ax, ay, az);
    read_vec3(L, 2, bx, by, bz);
    lua_pushnumber(L, ax*bx + ay*by + az*bz);
    return 1;
}

static int l_VPerpDot(lua_State* L) {
    f32 ax, ay, az, bx, by, bz;
    read_vec3(L, 1, ax, ay, az);
    read_vec3(L, 2, bx, by, bz);
    lua_pushnumber(L, az * bx - ax * bz);
    return 1;
}

static int l_MATH_IRound(lua_State* L) {
    lua_pushnumber(L, std::round(lua_tonumber(L, 1)));
    return 1;
}

static int l_MATH_Lerp(lua_State* L) {
    f64 s = lua_tonumber(L, 1);
    f64 a = lua_tonumber(L, 2);
    f64 b = lua_tonumber(L, 3);
    lua_pushnumber(L, a + (b - a) * s);
    return 1;
}

// Deterministic RNG for replay consistency. Seeded per-sim.
static std::mt19937 s_sim_rng{42};

static int l_Random(lua_State* L) {
    if (lua_gettop(L) == 0) {
        std::uniform_real_distribution<f64> dist(0.0, 1.0);
        lua_pushnumber(L, dist(s_sim_rng));
        return 1;
    }
    int lo = static_cast<int>(lua_tonumber(L, 1));
    int hi = lua_gettop(L) >= 2 ? static_cast<int>(lua_tonumber(L, 2)) : lo;
    if (lo > hi) std::swap(lo, hi);
    std::uniform_int_distribution<int> dist(lo, hi);
    lua_pushnumber(L, dist(s_sim_rng));
    return 1;
}

static int l_STR_GetTokens(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    const char* delim = luaL_checkstring(L, 2);
    lua_newtable(L);
    int idx = 1;
    std::string s(str);
    std::string d(delim);
    size_t pos = 0;
    while ((pos = s.find(d)) != std::string::npos) {
        lua_pushnumber(L, idx++);
        lua_pushstring(L, s.substr(0, pos).c_str());
        lua_rawset(L, -3);
        s.erase(0, pos + d.length());
    }
    if (!s.empty()) {
        lua_pushnumber(L, idx);
        lua_pushstring(L, s.c_str());
        lua_rawset(L, -3);
    }
    return 1;
}

static int l_STR_Utf8Len(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    // Simple: count non-continuation bytes
    int len = 0;
    for (const char* p = str; *p; p++) {
        if ((*p & 0xC0) != 0x80) len++;
    }
    lua_pushnumber(L, len);
    return 1;
}

static int l_STR_Utf8SubString(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    lua_pushstring(L, str); // simplified
    return 1;
}

static int l_STR_itox(lua_State* L) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%x", static_cast<unsigned>(lua_tonumber(L, 1)));
    lua_pushstring(L, buf);
    return 1;
}

static int l_STR_xtoi(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    lua_pushnumber(L, static_cast<int>(std::strtoul(s, nullptr, 16)));
    return 1;
}

static int l_Trace(lua_State*) { return 0; }

// ====================================================================
// Army / Alliance / Session — real implementations
// ====================================================================

static int l_SetAlliance(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return 0;
    i32 a1 = resolve_army(L, 1, sim);
    i32 a2 = resolve_army(L, 2, sim);
    const char* type = luaL_checkstring(L, 3);
    sim::Alliance alliance = sim::Alliance::Enemy;
    if (std::strcmp(type, "Ally") == 0) alliance = sim::Alliance::Ally;
    else if (std::strcmp(type, "Neutral") == 0) alliance = sim::Alliance::Neutral;
    sim->set_alliance(a1, a2, alliance);
    return 0;
}

static int l_IsAlly(lua_State* L) {
    auto* sim = get_sim(L);
    i32 a1 = resolve_army(L, 1, sim);
    i32 a2 = resolve_army(L, 2, sim);
    lua_pushboolean(L, sim && sim->is_ally(a1, a2));
    return 1;
}

static int l_IsEnemy(lua_State* L) {
    auto* sim = get_sim(L);
    i32 a1 = resolve_army(L, 1, sim);
    i32 a2 = resolve_army(L, 2, sim);
    lua_pushboolean(L, sim && sim->is_enemy(a1, a2));
    return 1;
}

static int l_IsNeutral(lua_State* L) {
    auto* sim = get_sim(L);
    i32 a1 = resolve_army(L, 1, sim);
    i32 a2 = resolve_army(L, 2, sim);
    lua_pushboolean(L, sim && sim->is_neutral(a1, a2));
    return 1;
}

static int l_SetArmyUnitCap(lua_State* L) {
    auto* sim = get_sim(L);
    i32 army = resolve_army(L, 1, sim);
    i32 cap = static_cast<i32>(luaL_checknumber(L, 2));
    if (sim) {
        auto* brain = sim->get_army(army);
        if (brain) brain->set_unit_cap(cap);
    }
    return 0;
}

static int l_GetArmyUnitCap(lua_State* L) {
    auto* sim = get_sim(L);
    i32 army = resolve_army(L, 1, sim);
    if (sim) {
        auto* brain = sim->get_army(army);
        if (brain) {
            lua_pushnumber(L, brain->unit_cap());
            return 1;
        }
    }
    lua_pushnumber(L, 1000);
    return 1;
}

static int l_GetArmyUnitCostTotal(lua_State* L) {
    auto* sim = get_sim(L);
    i32 army = resolve_army(L, 1, sim);
    if (sim) {
        auto* brain = sim->get_army(army);
        if (brain) {
            lua_pushnumber(L, brain->get_unit_cost_total(sim->entity_registry()));
            return 1;
        }
    }
    lua_pushnumber(L, 0);
    return 1;
}

static int l_SetArmyOutOfGame(lua_State* L) {
    auto* sim = get_sim(L);
    i32 army = resolve_army(L, 1, sim);
    if (sim) {
        auto* brain = sim->get_army(army);
        if (brain) brain->set_state(sim::BrainState::Defeat);
    }
    return 0;
}

static int l_SetArmyEconomy(lua_State* L) {
    auto* sim = get_sim(L);
    i32 army = resolve_army(L, 1, sim);
    f64 mass = luaL_checknumber(L, 2);
    f64 energy = luaL_checknumber(L, 3);
    if (sim) {
        auto* brain = sim->get_army(army);
        if (brain) brain->set_stored_resources(mass, energy);
    }
    return 0;
}

static int l_SetArmyColor(lua_State* L) {
    auto* sim = get_sim(L);
    i32 army = resolve_army(L, 1, sim);
    u8 r = static_cast<u8>(luaL_checknumber(L, 2));
    u8 g = static_cast<u8>(luaL_checknumber(L, 3));
    u8 b = static_cast<u8>(luaL_checknumber(L, 4));
    if (sim) {
        auto* brain = sim->get_army(army);
        if (brain) brain->set_color(r, g, b);
    }
    return 0;
}

static int l_InternalCreateArmy(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return 0;
    const char* name = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr;
    if (!name) return 0;

    auto* existing = sim->get_army_by_name(name);
    if (!existing) {
        spdlog::info("InternalCreateArmy: creating '{}'", name);
        sim->add_army(name, name);
    }
    return 0;
}

// InitializeArmyAI(armyName) — imports aibrain.lua, applies AIBrain class
// to the brain table, and calls OnCreateHuman or OnCreateAI.
static int l_InitializeArmyAI(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto* sim = get_sim(L);
    if (!sim) return 0;

    auto* brain_obj = sim->get_army_by_name(name);
    if (!brain_obj || brain_obj->lua_table_ref() < 0) {
        spdlog::warn("InitializeArmyAI: no brain for '{}'", name);
        return 0;
    }

    // Get brain Lua table from registry
    lua_rawgeti(L, LUA_REGISTRYINDEX, brain_obj->lua_table_ref());
    int brain_idx = lua_gettop(L);

    // Determine if human from ScenarioInfo.ArmySetup[name].Human
    bool is_human = true;
    lua_pushstring(L, "ScenarioInfo");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "ArmySetup");
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, name);
            lua_gettable(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "Human");
                lua_gettable(L, -2);
                is_human = lua_toboolean(L, -1) != 0;
                lua_pop(L, 1); // Human
            }
            lua_pop(L, 1); // info
        }
        lua_pop(L, 1); // ArmySetup
    }
    lua_pop(L, 1); // ScenarioInfo

    // Check if the brain already has OnCreateHuman/OnCreateAI
    // (AI brains may have been setmetatable'd in OnCreateArmyBrain Lua code)
    const char* method = is_human ? "OnCreateHuman" : "OnCreateAI";
    lua_pushstring(L, method);
    lua_gettable(L, brain_idx);
    bool has_method = lua_isfunction(L, -1);
    lua_pop(L, 1);

    if (!has_method) {
        // Import aibrain.lua and set AIBrain class as the brain's metatable
        lua_getglobal(L, "import");
        lua_pushstring(L, "/lua/aibrain.lua");
        if (lua_pcall(L, 1, 1, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            spdlog::warn("InitializeArmyAI: aibrain.lua import failed: {}",
                         err ? err : "?");
            lua_pop(L, 2); // error + brain
            return 0;
        }
        // Stack: brain, module_table
        lua_pushstring(L, "AIBrain");
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            // setmetatable(brain, AIBrain) — AIBrain has __index = AIBrain
            // via the Class() system, so method lookups work through it.
            // AIBrain inherits from moho.aibrain_methods, so C++ methods
            // are also accessible.
            lua_setmetatable(L, brain_idx);
        } else {
            spdlog::warn("InitializeArmyAI: AIBrain class not found in module");
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // module_table
    }

    // Call brain:OnCreateHuman(plan) or brain:OnCreateAI(plan)
    lua_pushstring(L, method);
    lua_gettable(L, brain_idx);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, brain_idx); // self
        // Get plan name from brain.PlanName (set by SetArmyPlans)
        lua_pushstring(L, "PlanName");
        lua_gettable(L, brain_idx);
        if (!lua_isstring(L, -1)) {
            lua_pop(L, 1);
            lua_pushstring(L, "none");
        }
        if (lua_pcall(L, 2, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            spdlog::warn("InitializeArmyAI: {}('{}') failed: {}",
                         method, name, err ? err : "?");
            lua_pop(L, 1);
        } else {
            spdlog::info("InitializeArmyAI: {} for '{}'", method, name);
        }
    } else {
        lua_pop(L, 1);
        spdlog::warn("InitializeArmyAI: {} not available for '{}'", method,
                     name);
    }

    lua_pop(L, 1); // brain
    return 0;
}

static int l_SetArmyFactionIndex(lua_State* L) {
    auto* sim = get_sim(L);
    i32 army = resolve_army(L, 1, sim);
    i32 faction = static_cast<i32>(luaL_checknumber(L, 2));
    if (sim) {
        auto* brain = sim->get_army(army);
        if (brain) brain->set_faction(faction);
    }
    return 0;
}

static int l_SetArmyStart(lua_State* L) {
    auto* sim = get_sim(L);
    i32 army = resolve_army(L, 1, sim);
    f32 x = static_cast<f32>(luaL_checknumber(L, 2));
    f32 z = static_cast<f32>(luaL_checknumber(L, 3));
    if (sim) {
        auto* brain = sim->get_army(army);
        if (brain) brain->set_start_position({x, 0, z});
    }
    return 0;
}

static int l_SessionIsActive(lua_State* L) {
    // Check if session_active flag is stored in registry
    lua_pushstring(L, "osc_session_active");
    lua_rawget(L, LUA_REGISTRYINDEX);
    bool active = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_pushboolean(L, active);
    return 1;
}

static int l_SessionGetScenarioInfo(lua_State* L) {
    lua_getglobal(L, "ScenarioInfo");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    return 1;
}

static int l_ShouldCreateInitialArmyUnits(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

// ====================================================================
// Orders / Commands
// ====================================================================

// Helper: iterate a Lua table of unit tables, calling fn for each valid Unit*.
static void for_each_unit_in_table(lua_State* L, int table_idx,
                                    void (*fn)(sim::Unit*, void*),
                                    void* ctx) {
    if (!lua_istable(L, table_idx)) return;
    lua_pushnil(L);
    while (lua_next(L, table_idx) != 0) {
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "_c_object");
            lua_rawget(L, -2);
            if (lua_isuserdata(L, -1)) {
                auto* e = static_cast<sim::Entity*>(lua_touserdata(L, -1));
                if (e && e->is_unit() && !e->destroyed())
                    fn(static_cast<sim::Unit*>(e), ctx);
            }
            lua_pop(L, 1); // _c_object
        }
        lua_pop(L, 1); // value (keep key for lua_next)
    }
}

// Helper: extract Vector3 position from a Lua table at stack index.
static sim::Vector3 extract_position(lua_State* L, int idx) {
    sim::Vector3 pos;
    if (!lua_istable(L, idx)) return pos;
    lua_pushnumber(L, 1);
    lua_gettable(L, idx);
    pos.x = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushnumber(L, 2);
    lua_gettable(L, idx);
    pos.y = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushnumber(L, 3);
    lua_gettable(L, idx);
    pos.z = static_cast<f32>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return pos;
}

// IssueMove(units_table, position)
static int l_IssueMove(lua_State* L) {
    auto target_pos = extract_position(L, 2);
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Move;
    cmd.target_pos = target_pos;
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), false);
    }, &cmd);
    return 0;
}

// IssueAggressiveMove(units_table, position) — move for now, attack later
static int l_IssueAggressiveMove(lua_State* L) {
    return l_IssueMove(L);
}

// IssueStop(units_table)
static int l_IssueStop(lua_State* L) {
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void*) {
        u->clear_commands();
    }, nullptr);
    return 0;
}

// IssueClearCommands(units_table)
static int l_IssueClearCommands(lua_State* L) {
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void*) {
        u->clear_commands();
    }, nullptr);
    return 0;
}

// IssueAttack(units_table, target_entity)
static int l_IssueAttack(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return 0;

    // Extract target entity from arg 2
    auto* target = extract_entity(L, 2);
    if (!target || target->destroyed()) return 0;

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Attack;
    cmd.target_id = target->entity_id();
    cmd.target_pos = target->position();
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

// IssueGuard(units_table, target_entity)
static int l_IssueGuard(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return 0;

    auto* target = extract_entity(L, 2);
    if (!target || target->destroyed() || !target->is_unit()) return 0;

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Guard;
    cmd.target_id = target->entity_id();
    cmd.target_pos = target->position();
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

// IssueRepair(units_table, target_entity)
static int l_IssueRepair(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return 0;

    auto* target = extract_entity(L, 2);
    if (!target || target->destroyed() || !target->is_unit()) return 0;

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Repair;
    cmd.target_id = target->entity_id();
    cmd.target_pos = target->position();
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

// IssueCapture(units_table, target)
static int l_IssueCapture(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return 0;

    auto* target = extract_entity(L, 2);
    if (!target || target->destroyed() || !target->is_unit()) return 0;

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Capture;
    cmd.target_id = target->entity_id();
    cmd.target_pos = target->position();
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

static int l_IssueDive(lua_State* L) {
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Dive;
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

// ChangeUnitArmy(unit, toArmy [, noRestrictions])
static int l_ChangeUnitArmy(lua_State* L) {
    // Arg 1: unit Lua table
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return 0; }
    auto* unit = static_cast<sim::Unit*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!unit || unit->destroyed()) return 0;

    // Arg 2: toArmy (1-based Lua army index)
    int to_army_lua = static_cast<int>(luaL_checknumber(L, 2));
    int to_army_cpp = to_army_lua - 1; // 0-based for C++

    spdlog::info("ChangeUnitArmy: entity #{} from army {} to army {}",
                 unit->entity_id(), unit->army(), to_army_cpp);

    // Change army on C++ side
    unit->set_army(to_army_cpp);

    // Update Army field on Lua table
    lua_pushstring(L, "Army");
    lua_pushnumber(L, to_army_lua);
    lua_rawset(L, 1);

    // Update Brain field: ArmyBrains[toArmy]
    lua_pushstring(L, "ArmyBrains");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, to_army_lua);
        if (!lua_isnil(L, -1)) {
            lua_pushstring(L, "Brain");
            lua_pushvalue(L, -2); // brain table
            lua_rawset(L, 1);    // unit_table.Brain = brain
        }
        lua_pop(L, 1); // brain
    }
    lua_pop(L, 1); // ArmyBrains

    // Fire on-given callbacks registered via moho AddOnGivenCallback
    // Snapshot to guard against mutation from callbacks (push_back/clear)
    std::vector<int> cbs_snapshot = unit->on_given_callbacks();
    for (int ref : cbs_snapshot) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, 1); // pass unit table (now with new army)
            if (lua_pcall(L, 1, 0, 0) != 0) {
                spdlog::warn("OnGivenCallback error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
    }

    // Return the unit's Lua table (same entity, new army)
    lua_pushvalue(L, 1);
    return 1;
}

// IssueUpgrade(units_table, blueprintId)
static int l_IssueUpgrade(lua_State* L) {
    const char* bp_id = luaL_checkstring(L, 2);

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Upgrade;
    cmd.blueprint_id = bp_id;
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

// IssueEnhancement(units_table, enhancementName)
static int l_IssueEnhancement(lua_State* L) {
    const char* enh_name = luaL_checkstring(L, 2);

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Enhance;
    cmd.blueprint_id = enh_name;

    struct Ctx { sim::UnitCommand cmd; lua_State* L; };
    Ctx ctx{cmd, L};
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        auto* ec = static_cast<Ctx*>(c);
        // Cancel any in-progress enhancement before clearing queue
        if (u->is_enhancing()) {
            u->cancel_enhance(ec->L);
        }
        u->push_command(ec->cmd, true);
    }, &ctx);
    return 0;
}

// IssueBuildMobile(units_table, position, blueprintId, {})
static int l_IssueBuildMobile(lua_State* L) {
    auto target_pos = extract_position(L, 2);
    const char* bp_id = luaL_checkstring(L, 3);

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::BuildMobile;
    cmd.target_pos = target_pos;
    cmd.blueprint_id = bp_id;

    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), false);
    }, &cmd);
    return 0;
}

// IssueBuildFactory(units_table, blueprintId, count)
static int l_IssueBuildFactory(lua_State* L) {
    const char* bp_id = luaL_checkstring(L, 2);
    int count = lua_isnumber(L, 3) ? static_cast<int>(lua_tonumber(L, 3)) : 1;

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::BuildFactory;
    cmd.blueprint_id = bp_id;

    for (int i = 0; i < count; i++) {
        for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
            u->push_command(*static_cast<sim::UnitCommand*>(c), false);
        }, &cmd);
    }
    return 0;
}

// IssueMoveOffFactory(units_table, position) — clears commands, issues Move
static int l_IssueMoveOffFactory(lua_State* L) {
    auto target_pos = extract_position(L, 2);
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Move;
    cmd.target_pos = target_pos;
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

// IssueFactoryRallyPoint(units_table, position) — sets rally point on each unit
static int l_IssueFactoryRallyPoint(lua_State* L) {
    auto pos = extract_position(L, 2);
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->set_rally_point(*static_cast<sim::Vector3*>(c));
    }, &pos);
    return 0;
}

// IssueClearFactoryCommands(units_table) — clears all commands
static int l_IssueClearFactoryCommands(lua_State* L) {
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void*) {
        u->clear_commands();
    }, nullptr);
    return 0;
}

// IssueReclaim(units_table, target_entity)
static int l_IssueReclaim(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return 0;

    auto* target = extract_entity(L, 2);
    if (!target || target->destroyed()) return 0;

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Reclaim;
    cmd.target_id = target->entity_id();
    cmd.target_pos = target->position();
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

// CreateProp(position, blueprint_path) -> prop Lua table
static int l_CreateProp(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    // Read position from arg 1: {x, y, z} table
    f32 x = 0, y = 0, z = 0;
    if (lua_istable(L, 1)) {
        lua_rawgeti(L, 1, 1);
        if (lua_isnumber(L, -1)) x = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 1, 2);
        if (lua_isnumber(L, -1)) y = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 1, 3);
        if (lua_isnumber(L, -1)) z = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    const char* bp_path = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";

    auto prop = std::make_unique<sim::Prop>();
    prop->set_blueprint_id(bp_path);
    prop->set_position({x, y, z});
    prop->set_fraction_complete(1.0f);

    u32 id = sim->entity_registry().register_entity(std::move(prop));
    auto* prop_ptr = sim->entity_registry().find(id);
    if (!prop_ptr) { lua_pushnil(L); return 1; }

    // Create Lua table
    lua_newtable(L);

    // Set metatable: prefer __prop_class, fall back to moho.prop_methods
    lua_pushstring(L, "__prop_class");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "moho");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "prop_methods");
            lua_rawget(L, -2);
            lua_remove(L, -2);
        }
    }
    if (lua_istable(L, -1)) {
        lua_setmetatable(L, -2);
    } else {
        lua_pop(L, 1);
    }

    // _c_object lightuserdata
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, prop_ptr);
    lua_rawset(L, -3);

    // EntityId
    lua_pushstring(L, "EntityId");
    lua_pushnumber(L, id);
    lua_rawset(L, -3);

    // Store Lua table ref on entity
    lua_pushvalue(L, -1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    prop_ptr->set_lua_table_ref(ref);

    spdlog::info("CreateProp: entity #{} at ({:.1f}, {:.1f}, {:.1f}) bp={}",
                 id, x, y, z, bp_path);

    // Try to look up prop blueprint and set GetBlueprint support
    auto* store = sim->blueprint_store();
    if (store) {
        auto* entry = store->find(bp_path);
        if (entry) {
            // Blueprint found — store ref for GetBlueprint
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Blueprint");
            lua_pushvalue(L, -2);
            lua_rawset(L, -4); // set prop_table.Blueprint = bp_table
            lua_pop(L, 1);
        }
    }

    // Call OnCreate if available
    int tbl = lua_gettop(L);
    lua_pushstring(L, "OnCreate");
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl); // self
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("Prop OnCreate error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    return 1; // prop Lua table on stack
}

// _c_CreateShield(self, spec) — creates C++ Shield entity and wires to Lua table
// Called from ClassShield.__init in shield.lua. self is the already-constructed
// Shield Lua table, spec has shield params + Owner (the owning unit Lua table).
static int l_CreateShield(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) return 0;

    // arg 1 = self (shield Lua table), arg 2 = spec table
    if (!lua_istable(L, 1) || !lua_istable(L, 2)) {
        spdlog::warn("_c_CreateShield: expected (self_table, spec_table)");
        return 0;
    }

    // Read spec.Owner to get the owning unit
    lua_pushstring(L, "Owner");
    lua_rawget(L, 2);
    if (!lua_istable(L, -1)) {
        spdlog::warn("_c_CreateShield: spec.Owner is not a table");
        lua_pop(L, 1);
        return 0;
    }
    int owner_tbl = lua_gettop(L);

    // Get owner's _c_object
    lua_pushstring(L, "_c_object");
    lua_rawget(L, owner_tbl);
    auto* owner_entity = static_cast<sim::Entity*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!owner_entity || !owner_entity->is_unit()) {
        spdlog::warn("_c_CreateShield: spec.Owner has no valid _c_object unit");
        lua_pop(L, 1); // pop owner table
        return 0;
    }

    auto* owner = static_cast<sim::Unit*>(owner_entity);

    // Read spec.Size
    lua_pushstring(L, "Size");
    lua_rawget(L, 2);
    f32 size = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 10.0f;
    lua_pop(L, 1);

    // Read spec.ShieldMaxHealth
    lua_pushstring(L, "ShieldMaxHealth");
    lua_rawget(L, 2);
    f32 max_hp = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 250.0f;
    lua_pop(L, 1);

    // Read shield type from spec flags
    std::string shield_type = "Bubble"; // default
    lua_pushstring(L, "PersonalShield");
    lua_rawget(L, 2);
    if (lua_toboolean(L, -1)) shield_type = "Personal";
    lua_pop(L, 1);
    if (shield_type != "Personal") {
        lua_pushstring(L, "PersonalBubble");
        lua_rawget(L, 2);
        if (lua_toboolean(L, -1)) shield_type = "Personal";
        lua_pop(L, 1);
    }

    lua_pop(L, 1); // pop owner table

    // Create Shield C++ entity
    auto shield = std::make_unique<sim::Shield>();
    shield->set_army(owner->army());
    shield->set_position(owner->position());
    shield->set_blueprint_id("shield");
    shield->owner_id = owner->entity_id();
    shield->size = size;
    shield->shield_type = shield_type;
    shield->set_max_health(max_hp);
    shield->set_health(max_hp);
    shield->set_fraction_complete(1.0f);

    u32 id = sim->entity_registry().register_entity(std::move(shield));
    auto* shield_ptr = static_cast<sim::Shield*>(sim->entity_registry().find(id));
    if (!shield_ptr) {
        spdlog::warn("_c_CreateShield: failed to register shield entity");
        lua_pop(L, 1); // pop owner table
        return 0;
    }

    // Set _c_object on self table (arg 1)
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, shield_ptr);
    lua_rawset(L, 1);

    // Set EntityId on self table
    lua_pushstring(L, "EntityId");
    lua_pushnumber(L, id);
    lua_rawset(L, 1);

    // Store Lua table ref on the C++ entity
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    shield_ptr->set_lua_table_ref(ref);

    // Store shield entity ID on owner unit for O(1) lookup
    owner->set_shield_entity_id(id);

    spdlog::info("_c_CreateShield: entity #{} for owner #{} (army {}), size={:.1f}, maxHP={:.0f}",
                 id, owner->entity_id(), owner->army(), size, max_hp);

    // NOTE: Do NOT call OnCreate from here! Entity.__post_init (inherited by Shield)
    // calls self:OnCreate(spec) after __init returns. Calling it here too would
    // invoke OnCreate twice, causing ForkThread(self.RegenThread) to fail on the
    // second call (self.RegenThread already set to thread wrapper from first call).

    return 0; // _c_CreateShield returns nothing (self is already on caller's stack)
}

// CreatePropHPR(blueprint_path, x, y, z, heading, pitch, roll) -> prop Lua table
static int l_CreatePropHPR(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    const char* bp_path = lua_isstring(L, 1) ? lua_tostring(L, 1) : "";
    f32 x = lua_isnumber(L, 2) ? static_cast<f32>(lua_tonumber(L, 2)) : 0;
    f32 y = lua_isnumber(L, 3) ? static_cast<f32>(lua_tonumber(L, 3)) : 0;
    f32 z = lua_isnumber(L, 4) ? static_cast<f32>(lua_tonumber(L, 4)) : 0;
    f32 heading = lua_isnumber(L, 5) ? static_cast<f32>(lua_tonumber(L, 5)) : 0;
    f32 pitch   = lua_isnumber(L, 6) ? static_cast<f32>(lua_tonumber(L, 6)) : 0;
    f32 roll    = lua_isnumber(L, 7) ? static_cast<f32>(lua_tonumber(L, 7)) : 0;

    auto prop = std::make_unique<sim::Prop>();
    prop->set_blueprint_id(bp_path);
    prop->set_position({x, y, z});
    prop->set_orientation(sim::euler_to_quat(heading, pitch, roll));
    prop->set_fraction_complete(1.0f);

    u32 id = sim->entity_registry().register_entity(std::move(prop));
    auto* prop_ptr = sim->entity_registry().find(id);
    if (!prop_ptr) { lua_pushnil(L); return 1; }

    // Create Lua table with same pattern as l_CreateProp
    lua_newtable(L);

    lua_pushstring(L, "__prop_class");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "moho");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "prop_methods");
            lua_rawget(L, -2);
            lua_remove(L, -2);
        }
    }
    if (lua_istable(L, -1)) {
        lua_setmetatable(L, -2);
    } else {
        lua_pop(L, 1);
    }

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, prop_ptr);
    lua_rawset(L, -3);

    lua_pushstring(L, "EntityId");
    lua_pushnumber(L, id);
    lua_rawset(L, -3);

    lua_pushvalue(L, -1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    prop_ptr->set_lua_table_ref(ref);

    spdlog::debug("CreatePropHPR: entity #{} at ({:.1f}, {:.1f}, {:.1f}) bp={}",
                  id, x, y, z, bp_path);

    // Look up blueprint
    auto* store = sim->blueprint_store();
    if (store) {
        auto* entry = store->find(bp_path);
        if (entry) {
            store->push_lua_table(*entry, L);
            lua_pushstring(L, "Blueprint");
            lua_pushvalue(L, -2);
            lua_rawset(L, -4);
            lua_pop(L, 1);
        }
    }

    // Call OnCreate
    int tbl = lua_gettop(L);
    lua_pushstring(L, "OnCreate");
    lua_gettable(L, tbl);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, tbl);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("PropHPR OnCreate error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    return 1;
}

// GetReclaimablesInRect(rect) -> table of prop Lua tables
static int l_GetReclaimablesInRect(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) {
        lua_newtable(L);
        return 1;
    }

    // Read rect: can be {x0, z0, x1, z1} table or Rect(x0, z0, x1, z1)
    f32 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    if (lua_istable(L, 1)) {
        lua_rawgeti(L, 1, 1);
        if (lua_isnumber(L, -1)) x0 = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 1, 2);
        if (lua_isnumber(L, -1)) z0 = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 1, 3);
        if (lua_isnumber(L, -1)) x1 = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        lua_rawgeti(L, 1, 4);
        if (lua_isnumber(L, -1)) z1 = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
    }

    auto ids = sim->entity_registry().collect_in_rect(x0, z0, x1, z1);

    lua_newtable(L);
    int result = lua_gettop(L);
    int idx = 1;

    for (u32 eid : ids) {
        auto* e = sim->entity_registry().find(eid);
        if (!e || e->destroyed() || !e->is_prop()) continue;
        if (e->lua_table_ref() < 0) continue;

        lua_pushnumber(L, idx++);
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->lua_table_ref());
        lua_rawset(L, result);
    }

    return 1;
}

// IssuePatrol(units_table, position) — append patrol waypoint (no clear)
static int l_IssuePatrol(lua_State* L) {
    auto target_pos = extract_position(L, 2);
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Patrol;
    cmd.target_pos = target_pos;
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), false);
    }, &cmd);
    return 0;
}

// Stub for unimplemented order types — just does nothing
// IssueTransportLoad(units_table, transport_unit_table)
// Ground units → load into transport
static int l_IssueTransportLoad(lua_State* L) {
    // arg 1 = table of ground units to load
    // arg 2 = transport unit (single unit table)
    auto* target = extract_entity(L, 2);
    if (!target || target->destroyed() || !target->is_unit()) return 0;

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::TransportLoad;
    cmd.target_id = target->entity_id();
    cmd.target_pos = target->position();

    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);

    return 0;
}

// IssueTransportUnload(transports_table, position)
// Transports → unload all cargo at position
static int l_IssueTransportUnload(lua_State* L) {
    auto target_pos = extract_position(L, 2);

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::TransportUnload;
    cmd.target_pos = target_pos;

    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);

    return 0;
}

// IssueNuke(units_table, position) — fire nuke from silo
static int l_IssueNuke(lua_State* L) {
    auto target_pos = extract_position(L, 2);
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Nuke;
    cmd.target_pos = target_pos;
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

// IssueTactical(units_table, target) — fire tactical missile (entity or position)
static int l_IssueTactical(lua_State* L) {
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Tactical;

    // Target can be entity or position
    auto* target = extract_entity(L, 2);
    if (target && !target->destroyed()) {
        cmd.target_id = target->entity_id();
        cmd.target_pos = target->position();
    } else {
        cmd.target_pos = extract_position(L, 2);
    }
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

// IssueOvercharge(units_table, target_entity) — overcharge attack
static int l_IssueOvercharge(lua_State* L) {
    auto* target = extract_entity(L, 2);
    if (!target || target->destroyed()) return 0;

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Overcharge;
    cmd.target_id = target->entity_id();
    cmd.target_pos = target->position();
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

// IssueSacrifice(units_table, target_entity) — sacrifice unit to build target
static int l_IssueSacrifice(lua_State* L) {
    auto* target = extract_entity(L, 2);
    if (!target || target->destroyed() || !target->is_unit()) return 0;

    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Sacrifice;
    cmd.target_id = target->entity_id();
    cmd.target_pos = target->position();
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

// IssueTeleport(units_table, location) — teleport to position
static int l_IssueTeleport(lua_State* L) {
    auto target_pos = extract_position(L, 2);
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Teleport;
    cmd.target_pos = target_pos;
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        u->push_command(*static_cast<sim::UnitCommand*>(c), true);
    }, &cmd);
    return 0;
}

// IssueFerry(units_table, waypoint) — ferry route waypoint for transports
static int l_IssueFerry(lua_State* L) {
    auto target_pos = extract_position(L, 2);
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::Ferry;
    cmd.target_pos = target_pos;
    for_each_unit_in_table(L, 1, [](sim::Unit* u, void* c) {
        // Ferry appends like Patrol, doesn't clear
        u->push_command(*static_cast<sim::UnitCommand*>(c), false);
    }, &cmd);
    return 0;
}

// ====================================================================
// Registration
// ====================================================================

void register_sim_bindings(LuaState& state, sim::SimState& sim) {
    lua_State* L = state.raw();

    // Entity creation
    state.register_function("_c_CreateEntity", l_c_CreateEntity);
    state.register_function("CreateUnit", l_CreateUnit);
    state.register_function("CreateUnitHPR", l_CreateUnitHPR);
    state.register_function("CreateUnit2", l_CreateUnit2);
    state.register_function("CreateInitialArmyUnit", l_CreateInitialArmyUnit);

    // Internal: create building unit (called from C++ build processing)
    lua_pushstring(L, "__osc_create_building_unit");
    lua_pushcfunction(L, l_create_building_unit);
    lua_rawset(L, LUA_REGISTRYINDEX);

    // Thread/fiber — register ThreadManager in registry for Destroy()
    sim.thread_manager().register_in_registry(L);
    state.register_function("ForkThread", l_ForkThread);
    state.register_function("KillThread", l_KillThread);
    state.register_function("CurrentThread", l_CurrentThread);
    state.register_function("SuspendCurrentThread", l_SuspendCurrentThread);
    state.register_function("ResumeThread", l_ResumeThread);

    // Game state
    state.register_function("GetGameTimeSeconds", l_GetGameTimeSeconds);
    state.register_function("GetGameTick", l_GetGameTick);
    state.register_function("SecondsPerTick", l_SecondsPerTick);
    state.register_function("ListArmies", l_ListArmies);
    state.register_function("GetFocusArmy", l_GetFocusArmy);
    state.register_function("SetFocusArmy", l_SetFocusArmy);
    state.register_function("GetArmyBrain", l_GetArmyBrain);
    state.register_function("IsGameOver", [](lua_State* L) -> int {
        auto* sim = get_sim(L);
        lua_pushboolean(L, sim && sim->player_result() != 0);
        return 1;
    });

    // Entity queries
    state.register_function("GetEntityById", l_GetEntityById);
    state.register_function("Warp", l_Warp);
    state.register_function("IsDestroyed", l_IsDestroyed);
    state.register_function("IsUnit", l_IsUnit);
    state.register_function("IsEntity", l_IsEntity);
    state.register_function("ArmyGetHandicap", l_ArmyGetHandicap);
    state.register_function("_c_CreateShield", l_CreateShield);

    // Terrain
    state.register_function("GetTerrainHeight", l_GetTerrainHeight);
    state.register_function("GetSurfaceHeight", l_GetSurfaceHeight);
    state.register_function("GetTerrainType", l_GetTerrainType);
    state.register_function("GetTerrainTypeOffset", l_GetTerrainType);
    state.register_function("GetPlayableRect", [](lua_State* L) -> int {
        auto* sim = get_sim(L);
        lua_newtable(L);
        if (sim) {
            f32 x0 = 0, z0 = 0, x1 = 0, z1 = 0;
            if (sim->has_playable_rect()) {
                x0 = sim->playable_x0(); z0 = sim->playable_z0();
                x1 = sim->playable_x1(); z1 = sim->playable_z1();
            } else if (sim->terrain()) {
                x1 = static_cast<f32>(sim->terrain()->map_width());
                z1 = static_cast<f32>(sim->terrain()->map_height());
            }
            lua_pushnumber(L, 1); lua_pushnumber(L, x0); lua_settable(L, -3);
            lua_pushnumber(L, 2); lua_pushnumber(L, z0); lua_settable(L, -3);
            lua_pushnumber(L, 3); lua_pushnumber(L, x1); lua_settable(L, -3);
            lua_pushnumber(L, 4); lua_pushnumber(L, z1); lua_settable(L, -3);
        }
        return 1;
    });
    state.register_function("SetPlayableRect", [](lua_State* L) -> int {
        auto* sim = get_sim(L);
        if (!sim) return 0;
        f32 x0 = static_cast<f32>(lua_tonumber(L, 1));
        f32 z0 = static_cast<f32>(lua_tonumber(L, 2));
        f32 x1 = static_cast<f32>(lua_tonumber(L, 3));
        f32 z1 = static_cast<f32>(lua_tonumber(L, 4));
        sim->set_playable_rect(x0, z0, x1, z1);
        return 0;
    });
    state.register_function("FlattenMapRect", stub_noop);

    // Categories
    setup_categories(L);
    state.register_function("ParseEntityCategory", l_ParseEntityCategory);
    state.register_function("EntityCategoryContains",
                            l_EntityCategoryContains);
    state.register_function("EntityCategoryFilterDown",
                            l_EntityCategoryFilterDown);
    state.register_function("EntityCategoryFilterOut",
                            l_EntityCategoryFilterOut);
    state.register_function("EntityCategoryGetUnitList",
                            l_EntityCategoryGetUnitList);
    state.register_function("EntityCategoryCount", l_EntityCategoryCount);

    // Math / vector
    state.register_function("EulerToQuaternion", l_EulerToQuaternion);
    state.register_function("OrientFromDir", l_OrientFromDir);
    state.register_function("Vector", l_Vector);
    state.register_function("Vector2", l_Vector2);
    state.register_function("VDist3", l_VDist3);
    state.register_function("VDist3Sq", l_VDist3Sq);
    state.register_function("VDist2", l_VDist2);
    state.register_function("VDist2Sq", l_VDist2Sq);
    state.register_function("VAdd", l_VAdd);
    state.register_function("VDiff", l_VDiff);
    state.register_function("VMult", l_VMult);
    state.register_function("VDot", l_VDot);
    state.register_function("VPerpDot", l_VPerpDot);
    state.register_function("MATH_IRound", l_MATH_IRound);
    state.register_function("MATH_Lerp", l_MATH_Lerp);
    state.register_function("Random", l_Random);

    // String utilities
    state.register_function("STR_GetTokens", l_STR_GetTokens);
    state.register_function("STR_Utf8Len", l_STR_Utf8Len);
    state.register_function("STR_Utf8SubString", l_STR_Utf8SubString);
    state.register_function("STR_itox", l_STR_itox);
    state.register_function("STR_xtoi", l_STR_xtoi);

    // Debug/trace
    state.register_function("Trace", l_Trace);

    // Visual debug drawing (no-op in headless engine — navutils.lua and
    // debug.lua cache these as upvalues at file scope)
    state.register_function("DrawCircle", stub_noop);
    state.register_function("DrawLinePop", stub_noop);
    state.register_function("DrawLine", stub_noop);
    state.register_function("DebugGetSelection", stub_nil);

    // Effects — real IEffect creation (state tracking, rendering deferred)
    state.register_function("CreateEmitterAtBone", l_CreateEmitterAtBone);
    state.register_function("CreateEmitterAtEntity", l_CreateEmitterAtEntity);
    state.register_function("CreateEmitterOnEntity", l_CreateEmitterAtEntity); // same semantics
    state.register_function("CreateAttachedEmitter", l_CreateAttachedEmitter);
    state.register_function("CreateAttachedBeam", l_CreateAttachedBeam);
    state.register_function("AttachBeamToEntity", l_AttachBeamToEntity);
    state.register_function("AttachBeamEntityToEntity", l_AttachBeamEntityToEntity);
    state.register_function("CreateBeamEntityToEntity", l_AttachBeamEntityToEntity); // same
    state.register_function("CreateBeamEmitter", l_CreateBeamEmitter);
    state.register_function("CreateBeamEmitterOnEntity", l_CreateAttachedEmitter); // same sig as attached
    state.register_function("CreateLightParticle", l_CreateLightParticle);
    state.register_function("CreateLightParticleIntel", l_CreateLightParticle); // same
    state.register_function("CreateDecal", l_CreateDecal);
    state.register_function("CreateSplat", l_CreateSplat);
    state.register_function("CreateSplatOnBone", l_CreateSplatOnBone);
    // These Create* functions return dummy objects whose methods are
    // no-ops that return self for chaining.  FA Lua code calls methods
    // on the returned objects (e.g., CreateAnimator(self):PlayAnim():SetRate(1)).
    state.register_function("CreateAnimator", l_CreateAnimator);
    state.register_function("CreateAimController", l_CreateAimController);
    state.register_function("CreateBuilderArmController", l_CreateBuilderArmController);
    state.register_function("CreateCollisionDetector", l_CreateCollisionDetector);
    state.register_function("CreateFootPlantController", l_CreateFootPlantController);
    state.register_function("CreateRotator", l_CreateRotator);
    state.register_function("CreateSlider", l_CreateSlider);
    state.register_function("CreateSlaver", l_CreateSlaver);
    state.register_function("CreateStorageManipulator", l_CreateStorageManipulator);
    state.register_function("CreateThrustController", l_CreateThrustController);

    // WaitFor(manipulator) — real implementation with yield/resume
    state.register_function("WaitFor", l_WaitFor);
    state.register_function("CreateProp", l_CreateProp);
    state.register_function("CreatePropHPR", l_CreatePropHPR);

    // Economy events — real implementations
    state.register_function("CreateEconomyEvent", l_CreateEconomyEvent);
    state.register_function("EconomyEventIsDone", l_EconomyEventIsDone);
    state.register_function("RemoveEconomyEvent", l_RemoveEconomyEvent);

    // Army — real implementations
    state.register_function("SetAlliance", l_SetAlliance);
    state.register_function("IsAlly", l_IsAlly);
    state.register_function("IsEnemy", l_IsEnemy);
    state.register_function("IsNeutral", l_IsNeutral);
    state.register_function("SetCommandSource", stub_noop);
    state.register_function("ArmyInitializePrebuiltUnits", [](lua_State* L) -> int {
        // In skirmish, there are no prebuilt units — the ACU is spawned by
        // SetupSession -> army brain Lua code. Campaign save parsing is out of scope.
        spdlog::debug("ArmyInitializePrebuiltUnits called for army: {}",
                      lua_tostring(L, 1) ? lua_tostring(L, 1) : "(nil)");
        return 0;
    });
    state.register_function("SetArmyUnitCap", l_SetArmyUnitCap);
    state.register_function("GetArmyUnitCap", l_GetArmyUnitCap);
    state.register_function("GetArmyUnitCostTotal", l_GetArmyUnitCostTotal);
    state.register_function("SetArmyOutOfGame", l_SetArmyOutOfGame);
    state.register_function("SetArmyEconomy", l_SetArmyEconomy);
    state.register_function("SetArmyColor", l_SetArmyColor);
    state.register_function("ChangeUnitArmy", l_ChangeUnitArmy);
    state.register_function("AddBuildRestriction", [](lua_State* L) -> int {
        auto* sim = get_sim(L);
        if (!sim) return 0;
        i32 army = resolve_army(L, 1, sim);
        if (army < 0) return 0;
        const char* cat = lua_tostring(L, 2);
        if (!cat) return 0;
        auto* brain = sim->get_army(army);
        if (brain) brain->add_build_restriction(cat);
        return 0;
    });
    state.register_function("RemoveBuildRestriction", [](lua_State* L) -> int {
        auto* sim = get_sim(L);
        if (!sim) return 0;
        i32 army = resolve_army(L, 1, sim);
        if (army < 0) return 0;
        const char* cat = lua_tostring(L, 2);
        if (!cat) return 0;
        auto* brain = sim->get_army(army);
        if (brain) brain->remove_build_restriction(cat);
        return 0;
    });
    state.register_function("SetArmyShowScore", stub_noop);

    // Damage
    state.register_function("Damage", l_Damage);
    state.register_function("DamageArea", l_DamageArea);
    state.register_function("DamageRing", l_DamageRing);

    // Orders (all stubs)
    state.register_function("IssueMove", l_IssueMove);
    state.register_function("IssueAggressiveMove", l_IssueAggressiveMove);
    state.register_function("IssuePatrol", l_IssuePatrol);
    state.register_function("IssueAttack", l_IssueAttack);
    state.register_function("IssueGuard", l_IssueGuard);
    state.register_function("IssueCapture", l_IssueCapture);
    state.register_function("IssueReclaim", l_IssueReclaim);
    state.register_function("IssueRepair", l_IssueRepair);
    state.register_function("IssueBuildFactory", l_IssueBuildFactory);
    state.register_function("IssueBuildMobile", l_IssueBuildMobile);
    state.register_function("IssueBuildAllMobile", l_IssueBuildMobile);
    state.register_function("IssueClearCommands", l_IssueClearCommands);
    state.register_function("IssueStop", l_IssueStop);
    state.register_function("IssueMoveOffFactory", l_IssueMoveOffFactory);
    state.register_function("IssueFactoryRallyPoint", l_IssueFactoryRallyPoint);
    state.register_function("IssueClearFactoryCommands", l_IssueClearFactoryCommands);
    state.register_function("IssueTransportLoad", l_IssueTransportLoad);
    state.register_function("IssueTransportUnload", l_IssueTransportUnload);
    state.register_function("IssueTransportUnloadSpecific", l_IssueTransportUnload);
    state.register_function("IssueFerry", l_IssueFerry);
    state.register_function("IssueNuke", l_IssueNuke);
    state.register_function("IssueTactical", l_IssueTactical);
    state.register_function("IssueOvercharge", l_IssueOvercharge);
    state.register_function("IssueSacrifice", l_IssueSacrifice);
    state.register_function("IssueTeleport", l_IssueTeleport);
    state.register_function("IssueUpgrade", l_IssueUpgrade);
    state.register_function("IssuePause", stub_noop);  // unused in FA
    state.register_function("IssueScript", stub_noop);  // EnhanceTask uses IssueEnhancement
    state.register_function("IssueDive", l_IssueDive);
    state.register_function("IssueEnhancement", l_IssueEnhancement);

    // Spatial queries
    state.register_function("GetUnitsInRect", l_GetUnitsInRect);
    state.register_function("GetReclaimablesInRect", l_GetReclaimablesInRect);
    state.register_function("GetUnitBlueprintByName", l_GetUnitBlueprintByName);

    // Audio
    state.register_function("AudioSetLanguage", stub_noop);

    // Misc stubs
    state.register_function("ResetSyncTable", stub_noop);
    state.register_function("EndGame", [](lua_State* L) -> int {
        auto* sim = get_sim(L);
        if (sim) {
            sim->set_game_ended(true);
            spdlog::info("EndGame called — game ended at tick {}",
                         sim->tick_count());
        }
        return 0;
    });
    state.register_function("GetVersion", [](lua_State* L) -> int {
        lua_pushstring(L, "OpenSupCom 0.1.0");
        return 1;
    });
    state.register_function("GetMapSize", [](lua_State* L) -> int {
        auto* sim = get_sim(L);
        if (sim && sim->terrain()) {
            lua_pushnumber(L, sim->terrain()->map_width());
            lua_pushnumber(L, sim->terrain()->map_height());
        } else {
            lua_pushnumber(L, 512);
            lua_pushnumber(L, 512);
        }
        return 2;
    });
    state.register_function("SpecFootprints", stub_noop);
    state.register_function("BlueprintLoaderUpdateProgress", stub_noop);
    state.register_function("ShouldCreateInitialArmyUnits", l_ShouldCreateInitialArmyUnits);

    // Engine globals
    lua_pushstring(L, "us");
    lua_setglobal(L, "__language");

    // HasLocalizedVO — check if localized voice-over exists for a language
    state.register_function("HasLocalizedVO", [](lua_State* Ls) -> int {
        const char* lang = lua_tostring(Ls, 1);
        lua_pushboolean(Ls, lang && std::string(lang) == "us");
        return 1;
    });

    // Session/network
    state.register_function("GpgNetSend", stub_noop);
    state.register_function("SessionIsActive", l_SessionIsActive);
    state.register_function("SessionIsMultiplayer", stub_false);
    state.register_function("SessionIsReplay", stub_false);
    state.register_function("SessionGetScenarioInfo", l_SessionGetScenarioInfo);
    state.register_function("GetCurrentCommandSource", stub_zero);
    state.register_function("RequestPause", l_sim_RequestPause);
    state.register_function("SimConExecute", stub_noop);
    state.register_function("BeginLogging", stub_noop);
    state.register_function("EndLogging", stub_noop);
    state.register_function("SuspendSim", stub_noop);
    state.register_function("ResumeSim", l_sim_ResumeSim);

    // Time/profiling
    state.register_function("GetSystemTimeSecondsOnlyUseForProfileUseRealClock",
                            l_GetGameTimeSeconds);
    state.register_function("GetFrustumTick", stub_zero);

    // Army init functions
    state.register_function("InternalCreateArmy", l_InternalCreateArmy);
    state.register_function("InitializeArmyAI", l_InitializeArmyAI);
    state.register_function("SetArmyStart", l_SetArmyStart);
    state.register_function("SetArmyPlans", stub_noop);
    state.register_function("SetArmyFactionIndex", l_SetArmyFactionIndex);
    state.register_function("SetArmyColorIndex", stub_noop);
    state.register_function("SetArmyAIPersonality", stub_noop);
    state.register_function("SetIgnoreArmyUnitCap", stub_noop);
    state.register_function("CreateResourceDeposit", l_CreateResourceDeposit);
    state.register_function("CreatePropInSimCallback", stub_noop);
    state.register_function("ArmyIsCivilian", l_ArmyIsCivilian);
    state.register_function("ArmyIsOutOfGame", l_ArmyIsOutOfGame);

    // Game global — FA uses Game.IsRestricted(bp_id, army_index) in OnStopBeingBuilt
    {
        lua_newtable(L);
        lua_pushstring(L, "IsRestricted");
        lua_pushcfunction(L, [](lua_State* Ls) -> int {
            lua_pushboolean(Ls, 0);
            return 1;
        });
        lua_rawset(L, -3);
        lua_pushstring(L, "Game");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_GLOBALSINDEX);
        lua_pop(L, 1);
    }

    // Active mods (empty table)
    lua_newtable(L);
    lua_setglobal(L, "__active_mods");

    // Empty tables needed by various init scripts
    state.set_global_table("__modules");
    state.set_global_table("__diskwatch");
    state.set_global_table("Sync");
    state.set_global_table("UnitData");

    // ScenarioInfo — populated with real data if terrain loaded, else defaults.
    // The scenario loader may have already set ScenarioInfo (from _scenario.lua).
    // Only create a stub if it doesn't exist yet.
    lua_getglobal(L, "ScenarioInfo");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, "name");
        lua_pushstring(L, "OpenSupCom Test");
        lua_rawset(L, -3);
        lua_pushstring(L, "type");
        lua_pushstring(L, "skirmish");
        lua_rawset(L, -3);
        lua_pushstring(L, "size");
        lua_newtable(L);
        u32 w = 512, h = 512;
        if (sim.terrain()) {
            w = sim.terrain()->map_width();
            h = sim.terrain()->map_height();
        }
        lua_pushnumber(L, 1);
        lua_pushnumber(L, w);
        lua_settable(L, -3);
        lua_pushnumber(L, 2);
        lua_pushnumber(L, h);
        lua_settable(L, -3);
        lua_rawset(L, -3);
        lua_setglobal(L, "ScenarioInfo");
    } else {
        // ScenarioInfo already exists — update size if terrain is loaded
        if (sim.terrain()) {
            int si_idx = lua_gettop(L);
            lua_pushstring(L, "size");
            lua_gettable(L, si_idx);
            if (lua_istable(L, -1)) {
                int size_idx = lua_gettop(L);
                lua_pushnumber(L, 1);
                lua_pushnumber(L, sim.terrain()->map_width());
                lua_settable(L, size_idx);
                lua_pushnumber(L, 2);
                lua_pushnumber(L, sim.terrain()->map_height());
                lua_settable(L, size_idx);
            }
            lua_pop(L, 1); // size
        }
        lua_pop(L, 1); // ScenarioInfo
    }

    spdlog::info("Registered sim bindings");
}

} // namespace osc::lua
