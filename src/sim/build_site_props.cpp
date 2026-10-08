#include "sim/build_site_props.hpp"

#include "sim/blueprint_categories.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/prop.hpp"
#include "sim/unit.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <unordered_set>

extern "C" {
#include <lua.h>
}

namespace osc::sim {

namespace {

constexpr const char* kObstructsKey = "osc_obstructsbuilding_in_data";

// FA takes RECLAIMABLE props; FAF's patched engine OBSTRUCTSBUILDING
// (FA-Binary-Patches #16), which FAF's blueprints-props.lua gives wrecks.
bool data_has_obstructs_building(lua_State* L) {
    lua_pushstring(L, kObstructsKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isboolean(L, -1)) {
        const bool known = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return known;
    }
    lua_pop(L, 1);
    bool known = false;
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_istable(L, -1)) {
                std::unordered_set<std::string> categories;
                collect_blueprint_categories(L, lua_gettop(L), categories);
                known = categories.count("OBSTRUCTSBUILDING") > 0;
            }
            lua_pop(L, 1);
            if (known) {
                lua_pop(L, 1);
                break;
            }
        }
    }
    lua_pop(L, 1);
    lua_pushstring(L, kObstructsKey);
    lua_pushboolean(L, known ? 1 : 0);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return known;
}

bool push_blueprint(lua_State* L, const std::string& bp_id) {
    std::string key = bp_id;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    lua_pushstring(L, key.c_str());
    lua_rawget(L, -2);
    lua_remove(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool equal_nocase(const std::string& a, const char* b) {
    const size_t n = std::char_traits<char>::length(b);
    if (a.size() != n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string associated_bp(lua_State* L, const Entity& prop) {
    if (prop.lua_table_ref() < 0) {
        return {};
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, prop.lua_table_ref());
    lua_pushstring(L, "AssociatedBP");
    lua_gettable(L, -2);
    std::string id = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 2);
    return id;
}

bool names_rebuild_id(lua_State* L, const std::string& id) {
    bool named = false;
    lua_pushstring(L, "Economy");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "RebuildBonusIds");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            for (int i = 1;; ++i) {
                lua_rawgeti(L, -1, i);
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    break;
                }
                named = lua_type(L, -1) == LUA_TSTRING && equal_nocase(id, lua_tostring(L, -1));
                lua_pop(L, 1);
                if (named) {
                    break;
                }
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return named;
}

bool rebuild_bonus(lua_State* L, const Unit& builder, f32& bonus) {
    if (builder.lua_table_ref() < 0) {
        return false;
    }
    const int bp = lua_gettop(L);
    const int base = bp;
    lua_rawgeti(L, LUA_REGISTRYINDEX, builder.lua_table_ref());
    lua_pushstring(L, "GetRebuildBonus");
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, base);
        return false;
    }
    lua_insert(L, -2);
    lua_pushvalue(L, bp);
    for (int i = 0; i < 4; ++i) {
        lua_pushnil(L);
    }
    if (lua_pcall(L, 6, LUA_MULTRET, 0) != 0) {
        spdlog::warn("GetRebuildBonus error: {}", lua_tostring(L, -1));
        lua_settop(L, base);
        return false;
    }
    const bool one = lua_gettop(L) - base == 1;
    bonus = one ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
    lua_settop(L, base);
    return one;
}

} // namespace

BuildSiteProp find_build_site_prop(lua_State* L, const EntityRegistry& registry,
                                   const Unit& builder, const std::string& bp_id,
                                   const Vector3& site) {
    BuildSiteProp found;
    if (!L) {
        return found;
    }
    const int top = lua_gettop(L);
    if (!push_blueprint(L, bp_id)) {
        return found;
    }
    const auto [size_x, size_z] = blueprint_footprint(L, lua_gettop(L));
    const f32 x0 = std::nearbyint(site.x - size_x * 0.5f);
    const f32 z0 = std::nearbyint(site.z - size_z * 0.5f);
    const bool by_obstructs = data_has_obstructs_building(L);

    const Prop* nearest = nullptr;
    f32 nearest_d2 = std::numeric_limits<f32>::infinity();
    for (const u32 id : registry.collect_in_rect(x0, z0, x0 + size_x, z0 + size_z)) {
        const Entity* e = registry.find(id);
        if (!e || e->destroyed() || !e->is_prop()) {
            continue;
        }
        const auto& prop = static_cast<const Prop&>(*e);
        if (!(by_obstructs ? prop.obstructs_building : prop.reclaimable_category)) {
            continue;
        }
        const f32 dx = builder.position().x - prop.position().x;
        const f32 dy = builder.position().y - prop.position().y;
        const f32 dz = builder.position().z - prop.position().z;
        const f32 d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < nearest_d2) {
            nearest_d2 = d2;
            nearest = &prop;
        }
    }
    if (!nearest) {
        lua_settop(L, top);
        return found;
    }
    found.reclaim_id = nearest->entity_id();
    const std::string associated = associated_bp(L, *nearest);
    if (associated.empty() || !names_rebuild_id(L, associated)) {
        lua_settop(L, top);
        return found;
    }
    const f32 cx = site.x - nearest->position().x;
    const f32 cz = site.z - nearest->position().z;
    if (cx * cx + cz * cz >= 0.000001f) {
        lua_settop(L, top);
        return found;
    }
    found.reclaim_id = 0;
    f32 bonus = 0.0f;
    if (!rebuild_bonus(L, builder, bonus)) {
        spdlog::warn("Failed to get valid rebuild bonus from the script");
        lua_settop(L, top);
        return found;
    }
    found.rebuild = true;
    found.wreck_id = nearest->entity_id();
    found.bonus = bonus * nearest->fraction_complete();
    lua_settop(L, top);
    return found;
}

} // namespace osc::sim
