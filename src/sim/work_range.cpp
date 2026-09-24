#include "sim/work_range.hpp"

#include "sim/collision.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

extern "C" {
#include <lua.h>
}

namespace osc::sim {

f32 footprint_extent(const Entity& e) {
    // Whole cells, as Moho's byte-sized footprints.
    return std::floor(std::max(e.footprint_size_x(), e.footprint_size_z()));
}

f32 skirt_extent(const Unit& u) {
    return std::max({u.skirt_size_x(), u.skirt_size_z(), footprint_extent(u)});
}

std::pair<f32, f32> blueprint_skirt(lua_State* L, const std::string& bp_id) {
    std::pair<f32, f32> skirt{1.0f, 1.0f};
    if (!L || bp_id.empty()) return skirt;
    std::string key = bp_id;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const int top = lua_gettop(L);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, key.c_str());
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            const int bp = lua_gettop(L);
            const auto [fx, fz] = blueprint_footprint(L, bp);
            f32 sx = 0, sz = 0;
            lua_pushstring(L, "Physics");
            lua_rawget(L, bp);
            if (lua_istable(L, -1)) {
                const auto number = [&](const char* name) {
                    lua_pushstring(L, name);
                    lua_rawget(L, -2);
                    const f32 v = lua_type(L, -1) == LUA_TNUMBER
                                      ? static_cast<f32>(lua_tonumber(L, -1))
                                      : 0.0f;
                    lua_pop(L, 1);
                    return v;
                };
                sx = number("SkirtSizeX");
                sz = number("SkirtSizeZ");
            }
            skirt = {std::max({sx, fx, 1.0f}), std::max({sz, fz, 1.0f})};
        }
    }
    lua_settop(L, top);
    return skirt;
}

f32 work_gap(const Unit& worker, const Vector3& at, f32 target_extent) {
    const f32 dx = at.x - worker.position().x;
    const f32 dz = at.z - worker.position().z;
    return std::sqrt(dx * dx + dz * dz) - footprint_extent(worker) - target_extent;
}

Vector3 approach_point(const Unit& worker, const Vector3& at, f32 half_x, f32 half_z) {
    // Out from the target's centre toward the worker (straight down -z from
    // right on top of it), to where the worker's own cell clears it.
    f32 dx = worker.position().x - at.x;
    f32 dz = worker.position().z - at.z;
    const f32 len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-3f) {
        dx = 0;
        dz = -1;
    } else {
        dx /= len;
        dz /= len;
    }
    const f32 room = std::max(worker.footprint_size_x(), worker.footprint_size_z()) * 0.5f;
    constexpr f32 kUnbounded = std::numeric_limits<f32>::max();
    const f32 tx = std::abs(dx) > 1e-6f ? (half_x + room) / std::abs(dx) : kUnbounded;
    const f32 tz = std::abs(dz) > 1e-6f ? (half_z + room) / std::abs(dz) : kUnbounded;
    const f32 t = std::min(tx, tz);
    return {at.x + dx * t, at.y, at.z + dz * t};
}

} // namespace osc::sim
