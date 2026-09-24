#include "sim/collision.hpp"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace osc::sim {

namespace {

f32 number_field(lua_State* L, int table, const char* key, f32 fallback) {
    lua_pushstring(L, key);
    lua_rawget(L, table);
    const f32 v = lua_type(L, -1) == LUA_TNUMBER ? static_cast<f32>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return v;
}

Vector3 to_local(const Vector3& p, const Vector3& position, const Quaternion& orientation) {
    const Quaternion inverse{-orientation.x, -orientation.y, -orientation.z, orientation.w};
    return quat_rotate(inverse, {p.x - position.x, p.y - position.y, p.z - position.z});
}

} // namespace

CollisionShape blueprint_collision_shape(lua_State* L, int bp_index) {
    CollisionShape shape;
    if (!L || !lua_istable(L, bp_index)) return shape;
    const f32 sx = number_field(L, bp_index, "SizeX", 0);
    const f32 sy = number_field(L, bp_index, "SizeY", 0);
    const f32 sz = number_field(L, bp_index, "SizeZ", 0);
    if (sx <= 0 || sy <= 0 || sz <= 0) return shape;
    shape.type = CollisionShapeType::BOX;
    shape.cx = number_field(L, bp_index, "CollisionOffsetX", 0);
    shape.cy = number_field(L, bp_index, "CollisionOffsetY", 0) + sy * 0.5f;
    shape.cz = number_field(L, bp_index, "CollisionOffsetZ", 0);
    shape.sx = sx * 0.5f;
    shape.sy = sy * 0.5f;
    shape.sz = sz * 0.5f;
    return shape;
}

f32 collision_reach(const CollisionShape& shape) {
    const f32 centre = std::sqrt(shape.cx * shape.cx + shape.cy * shape.cy + shape.cz * shape.cz);
    switch (shape.type) {
    case CollisionShapeType::SPHERE: return centre + shape.sx;
    case CollisionShapeType::BOX:
        return centre + std::sqrt(shape.sx * shape.sx + shape.sy * shape.sy + shape.sz * shape.sz);
    case CollisionShapeType::NONE: break;
    }
    return 0;
}

std::optional<f32> segment_enters(const CollisionShape& shape, const Vector3& position,
                                  const Quaternion& orientation, const Vector3& from,
                                  const Vector3& to, bool inside_counts) {
    if (shape.type == CollisionShapeType::NONE) return std::nullopt;
    // In the shape's own frame, centred on it.
    Vector3 a = to_local(from, position, orientation);
    Vector3 b = to_local(to, position, orientation);
    a = {a.x - shape.cx, a.y - shape.cy, a.z - shape.cz};
    b = {b.x - shape.cx, b.y - shape.cy, b.z - shape.cz};
    const Vector3 d{b.x - a.x, b.y - a.y, b.z - a.z};

    if (shape.type == CollisionShapeType::SPHERE) {
        const f32 r2 = shape.sx * shape.sx;
        const f32 c = a.x * a.x + a.y * a.y + a.z * a.z - r2;
        if (c <= 0) return inside_counts ? std::optional<f32>(0.0f) : std::nullopt;
        const f32 dd = d.x * d.x + d.y * d.y + d.z * d.z;
        if (dd <= 0) return std::nullopt;
        const f32 half_b = a.x * d.x + a.y * d.y + a.z * d.z;
        const f32 disc = half_b * half_b - dd * c;
        if (half_b >= 0 || disc < 0) return std::nullopt; // heading away, or passing by
        const f32 t = (-half_b - std::sqrt(disc)) / dd;
        if (t > 1.0f) return std::nullopt;
        return t;
    }

    // Box: the slabs' overlap along the segment.
    f32 enter = 0.0f;
    f32 leave = 1.0f;
    bool inside = true;
    const f32 starts[3] = {a.x, a.y, a.z};
    const f32 deltas[3] = {d.x, d.y, d.z};
    const f32 halves[3] = {shape.sx, shape.sy, shape.sz};
    for (int axis = 0; axis < 3; ++axis) {
        const f32 s = starts[axis];
        const f32 h = halves[axis];
        if (s < -h || s > h) inside = false;
        if (std::abs(deltas[axis]) < 1e-9f) {
            if (s < -h || s > h) return std::nullopt;
            continue;
        }
        f32 t0 = (-h - s) / deltas[axis];
        f32 t1 = (h - s) / deltas[axis];
        if (t0 > t1) std::swap(t0, t1);
        enter = std::max(enter, t0);
        leave = std::min(leave, t1);
        if (enter > leave) return std::nullopt;
    }
    if (inside) return inside_counts ? std::optional<f32>(0.0f) : std::nullopt;
    return enter;
}

Vector3 collision_centre(const Entity& e) {
    const CollisionShape& s = e.collision_shape();
    const Vector3& p = e.position();
    if (s.type == CollisionShapeType::NONE) return p;
    const Vector3 c = quat_rotate(e.orientation(), {s.cx, s.cy, s.cz});
    return {p.x + c.x, p.y + c.y, p.z + c.z};
}

f32 shape_distance(const CollisionShape& shape, const Vector3& position,
                   const Quaternion& orientation, const Vector3& point) {
    if (shape.type == CollisionShapeType::NONE) return std::numeric_limits<f32>::infinity();
    Vector3 p = to_local(point, position, orientation);
    p = {p.x - shape.cx, p.y - shape.cy, p.z - shape.cz};
    if (shape.type == CollisionShapeType::SPHERE)
        return std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z) - shape.sx;
    // Box: outside, the distance to it; inside, minus the depth.
    const f32 qx = std::abs(p.x) - shape.sx;
    const f32 qy = std::abs(p.y) - shape.sy;
    const f32 qz = std::abs(p.z) - shape.sz;
    const f32 ox = std::max(qx, 0.0f);
    const f32 oy = std::max(qy, 0.0f);
    const f32 oz = std::max(qz, 0.0f);
    return std::sqrt(ox * ox + oy * oy + oz * oz) + std::min(std::max({qx, qy, qz}), 0.0f);
}

} // namespace osc::sim
