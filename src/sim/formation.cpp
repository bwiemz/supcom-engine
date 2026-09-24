#include "sim/formation.hpp"

#include "core/dmath.hpp"
#include "core/test_status.hpp"
#include "map/terrain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/unit.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace osc::sim {

namespace {

struct Slot {
    f32 x = 0, z = 0; ///< in the world
    int category = 0; ///< index of its category in the result table
};

/// Push /lua/formations.lua's function `name`, or nothing (false).
bool push_formation_function(lua_State* L, const std::string& name) {
    const int top = lua_gettop(L);
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return false;
    }
    lua_pushstring(L, "/lua/formations.lua");
    if (lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
        lua_settop(L, top);
        return false;
    }
    lua_pushstring(L, name.c_str());
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return false;
    }
    lua_remove(L, -2); // the module
    return true;
}

} // namespace

std::vector<FormationSlot> plan_formation(lua_State* L, const EntityRegistry& registry,
                                          const map::Terrain* terrain, std::vector<u32> unit_ids,
                                          const std::string& formation, const Vector3& target,
                                          std::optional<f32> facing) {
    std::vector<FormationSlot> out;
    if (!L) return out;
    std::sort(unit_ids.begin(), unit_ids.end());
    unit_ids.erase(std::unique(unit_ids.begin(), unit_ids.end()), unit_ids.end());
    std::vector<const Unit*> units;
    for (const u32 id : unit_ids) {
        const Entity* e = registry.find(id);
        if (e && !e->destroyed() && e->is_unit() && e->lua_table_ref() >= 0)
            units.push_back(static_cast<const Unit*>(e));
    }
    if (units.size() < 2) return out;

    const int top = lua_gettop(L);
    if (!push_formation_function(L, formation)) return out;
    lua_newtable(L);
    for (size_t i = 0; i < units.size(); ++i) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, units[i]->lua_table_ref());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    if (lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
        if (lua_isstring(L, -1)) {
            const std::string message = "Formation " + formation + " error: " + lua_tostring(L, -1);
            spdlog::warn("{}", message);
            if (test_status::count_lua_failures()) test_status::record_failure(message);
        }
        lua_settop(L, top);
        return out;
    }
    const int slots_table = lua_gettop(L);

    // Facing: as ordered, else from the group's centre toward the target.
    f32 cx = 0, cz = 0;
    f32 widest = 1.0f;
    for (const Unit* u : units) {
        cx += u->position().x;
        cz += u->position().z;
        widest = std::max({widest, u->footprint_size_x(), u->footprint_size_z()});
    }
    cx /= static_cast<f32>(units.size());
    cz /= static_cast<f32>(units.size());
    f32 heading = 0;
    if (facing) {
        heading = *facing;
    } else if (const f32 dx = target.x - cx, dz = target.z - cz; dx * dx + dz * dz > 1e-4f) {
        heading = osc::dmath::atan2(dx, dz);
    }
    const f32 fx = osc::dmath::sin(heading), fz = osc::dmath::cos(heading); // forward
    const f32 rx = -fz, rz = fx;                                            // across (x)
    const f32 scale = widest + 2.0f;

    std::vector<Slot> slots;
    const int n = luaL_getn(L, slots_table);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, slots_table, i);
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 1);
            const auto sx = static_cast<f32>(lua_tonumber(L, -1));
            lua_rawgeti(L, -2, 2);
            const auto sy = static_cast<f32>(lua_tonumber(L, -1));
            lua_pop(L, 2);
            slots.push_back({target.x + (rx * sx + fx * sy) * scale,
                             target.z + (rz * sx + fz * sy) * scale, i});
        }
        lua_pop(L, 1);
    }

    // Each slot in turn takes the nearest unassigned unit of its category.
    // Retail's formation scripts make one slot per unit, category by category.
    std::map<std::pair<const void*, std::string>, bool> fits; // (category, blueprint)
    const auto matches = [&](int slot_index, const Unit& u) {
        lua_rawgeti(L, slots_table, slot_index);
        lua_rawgeti(L, -1, 3);
        const void* category = lua_topointer(L, -1);
        const auto key = std::make_pair(category, u.blueprint_id());
        if (const auto it = fits.find(key); it != fits.end()) {
            lua_pop(L, 2);
            return it->second;
        }
        bool fit = true; // a slot without a category takes anyone
        if (!lua_isnil(L, -1)) {
            lua_pushstring(L, "EntityCategoryContains");
            lua_rawget(L, LUA_GLOBALSINDEX);
            lua_pushvalue(L, -2);
            lua_rawgeti(L, LUA_REGISTRYINDEX, u.lua_table_ref());
            fit = lua_pcall(L, 2, 1, 0) == 0 && lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
        fits.emplace(key, fit);
        return fit;
    };
    std::vector<bool> taken(units.size(), false);
    for (const Slot& slot : slots) {
        size_t best = units.size();
        f32 best_d2 = std::numeric_limits<f32>::max();
        for (size_t i = 0; i < units.size(); ++i) {
            if (taken[i]) continue;
            const f32 dx = units[i]->position().x - slot.x;
            const f32 dz = units[i]->position().z - slot.z;
            const f32 d2 = dx * dx + dz * dz;
            if (d2 >= best_d2 || !matches(slot.category, *units[i])) continue;
            best = i;
            best_d2 = d2;
        }
        if (best == units.size()) continue;
        taken[best] = true;
        const f32 y = terrain ? terrain->get_surface_height(slot.x, slot.z) : target.y;
        out.push_back({units[best]->entity_id(), {slot.x, y, slot.z}});
    }
    for (size_t i = 0; i < units.size(); ++i)
        if (!taken[i]) out.push_back({units[i]->entity_id(), target});
    lua_settop(L, top);
    std::sort(out.begin(), out.end(),
              [](const FormationSlot& a, const FormationSlot& b) { return a.unit_id < b.unit_id; });
    return out;
}

} // namespace osc::sim
