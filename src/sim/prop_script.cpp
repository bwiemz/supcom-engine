#include "sim/prop_script.hpp"

#include "core/test_status.hpp"
#include "sim/blueprint_categories.hpp"
#include "sim/bone_cache.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/prop.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace osc::sim {

namespace {

/// Push /lua/sim/Prop.lua's Prop class (loaded once), or nil.
void push_generic_prop_class(lua_State* L) {
    constexpr const char* kKey = "__osc_generic_prop_class";
    lua_pushstring(L, kKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_isnil(L, -1)) {
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_pushnil(L);
        }
        return;
    }
    lua_pop(L, 1);
    const int top = lua_gettop(L);
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    bool found = false;
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, "/lua/sim/Prop.lua");
        if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
            lua_pushstring(L, "Prop");
            lua_rawget(L, -2);
            found = lua_istable(L, -1);
        }
    }
    if (found) {
        lua_pushstring(L, kKey);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
        lua_replace(L, top + 1);
        lua_settop(L, top + 1);
        return;
    }
    lua_settop(L, top);
    lua_pushstring(L, kKey);
    lua_pushboolean(L, 0); // not there (tests without FA's scripts): don't retry
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushnil(L);
}

/// Push the class a prop of `bp_id` is an instance of.
void push_prop_class(lua_State* L, const std::string& bp_id) {
    if (!bp_id.empty()) {
        push_blueprint_script_class(L, bp_id, "_prop.bp", "__osc_prop_script_classes", "Prop",
                                    /*warn_default_missing=*/false);
        if (lua_istable(L, -1)) return;
        lua_pop(L, 1);
    }
    push_generic_prop_class(L);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    // Without FA's scripts: the engine's methods alone.
    lua_pushstring(L, "moho");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "prop_methods");
        lua_rawget(L, -2);
        lua_remove(L, -2);
    }
}

std::string lowercase(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

void read_prop_blueprint(lua_State* L, Prop& prop) {
    if (!L) {
        return;
    }
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, lowercase(prop.blueprint_id()).c_str());
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            std::unordered_set<std::string> categories;
            collect_blueprint_categories(L, lua_gettop(L), categories);
            prop.untargetable = categories.count("UNTARGETABLE") > 0;
            prop.reclaimable_category = categories.count("RECLAIMABLE") > 0;
            prop.obstructs_building = categories.count("OBSTRUCTSBUILDING") > 0;
            lua_pushstring(L, "Economy");
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                const auto number = [&](const char* key) {
                    lua_pushstring(L, key);
                    lua_rawget(L, -2);
                    const f32 v = lua_type(L, -1) == LUA_TNUMBER
                                      ? static_cast<f32>(lua_tonumber(L, -1))
                                      : 0.0f;
                    lua_pop(L, 1);
                    return v;
                };
                prop.reclaim_mass_max = number("ReclaimMassMax");
                prop.reclaim_energy_max = number("ReclaimEnergyMax");
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

void create_prop_object(lua_State* L, SimState& sim, Prop& prop, bool push) {
    if (!L) return;
    read_prop_blueprint(L, prop);
    const std::string bp_id = lowercase(prop.blueprint_id());
    if (!prop.bone_data() && !bp_id.empty()) {
        if (auto* bones = sim.bone_cache()) prop.set_bone_data(bones->get(bp_id, L));
    }

    const int top = lua_gettop(L);
    lua_newtable(L);
    const int obj = lua_gettop(L);
    push_prop_class(L, bp_id);
    if (lua_istable(L, -1)) {
        lua_setmetatable(L, obj);
    } else {
        lua_pop(L, 1);
    }
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &prop);
    lua_rawset(L, obj);
    lua_pushstring(L, "_c_sim_gen");
    lua_pushnumber(L, static_cast<lua_Number>(SimState::sim_generation()));
    lua_rawset(L, obj);
    lua_pushstring(L, "EntityId");
    push_entity_id(L, prop.entity_id());
    lua_rawset(L, obj);
    // self.Blueprint, as FAF's scripts read it (retail calls GetBlueprint);
    // and the collision box it gives, until a script sets another.
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, bp_id.c_str());
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "Blueprint");
            lua_pushvalue(L, -2);
            lua_rawset(L, obj);
            prop.set_default_collision_shape(blueprint_collision_shape(L, lua_gettop(L)));
            const auto [sx, sz] = blueprint_footprint(L, lua_gettop(L));
            prop.set_footprint_size(sx, sz);
            // One worth reclaiming stands on its footprint's ground, with
            // its blueprint's Footprint.OccupancyCaps (Moho's Prop: 88 of
            // retail's, buses, ruins and the like, have LAND|SEABED; trees and
            // rocks none).
            lua_pushstring(L, "Footprint");
            lua_rawget(L, -2);
            const u8 caps = lua_istable(L, -1) ? [&] {
                lua_pushstring(L, "OccupancyCaps");
                lua_rawget(L, -2);
                const auto c = static_cast<u8>(lua_tonumber(L, -1));
                lua_pop(L, 1);
                return c;
            }()
                                               : u8{0};
            lua_pop(L, 1);
            if (caps != 0 && (prop.reclaim_mass_max > 0 || prop.reclaim_energy_max > 0)) {
                blueprints::Footprint fp;
                fp.size_x = static_cast<u8>(std::clamp(std::lround(sx), 0L, 255L));
                fp.size_z = static_cast<u8>(std::clamp(std::lround(sz), 0L, 255L));
                fp.caps = caps;
                GroundOccupant ground;
                ground.caps = caps;
                ground.rects.push_back(footprint_rect(fp, prop.position().x, prop.position().z));
                sim.occupy_ground(prop.entity_id(), std::move(ground));
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_pushvalue(L, obj);
    prop.set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));

    // Prop.OnCreate: health, reclaim values from the blueprint, the cache
    // position; trees and wrecks add their own.
    lua_pushstring(L, "OnCreate");
    lua_gettable(L, obj);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, obj);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            const char* err = lua_tostring(L, -1);
            const std::string message =
                "Prop " + bp_id + " OnCreate error: " + (err ? err : "(unknown)");
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
    }
    lua_settop(L, push ? obj : top);
}

Prop* spawn_prop(lua_State* L, SimState& sim, const std::string& bp_id, const Vector3& position,
                 const Quaternion& orientation, bool push) {
    auto made = std::make_unique<Prop>();
    made->set_blueprint_id(bp_id);
    made->set_position(position);
    made->set_orientation(orientation);
    made->set_fraction_complete(1.0f);
    const u32 id = sim.entity_registry().register_entity(std::move(made));
    auto* prop = static_cast<Prop*>(sim.entity_registry().find(id));
    if (!prop) {
        if (push) lua_pushnil(L);
        return nullptr;
    }
    create_prop_object(L, sim, *prop, push);
    return prop;
}

void create_map_prop_objects(lua_State* L, SimState& sim) {
    if (!L) return;
    // Collect first: OnCreate may create or destroy entities.
    std::vector<u32> ids;
    sim.entity_registry().for_each([&](const Entity& e) {
        if (e.is_prop() && !e.destroyed() && e.lua_table_ref() < 0) ids.push_back(e.entity_id());
    });
    for (const u32 id : ids) {
        Entity* e = sim.entity_registry().find(id);
        if (e && e->is_prop() && !e->destroyed() && e->lua_table_ref() < 0)
            create_prop_object(L, sim, static_cast<Prop&>(*e), false);
    }
    spdlog::info("Map props: {} given script objects", ids.size());
}

} // namespace osc::sim
