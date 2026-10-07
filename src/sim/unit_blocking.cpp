#include "sim/unit_blocking.hpp"

#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"
#include "sim/steering.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace osc::sim {

namespace {

f32 dot(const Vector3& a, const Vector3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vector3 cross(const Vector3& a, const Vector3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// The box's projection onto `axis`: [centre - r, centre + r].
void project(const OrientedBox& b, const Vector3& axis, f32& lo, f32& hi) {
    const f32 c = dot(b.centre, axis);
    f32 r = 0.0f;
    for (int i = 0; i < 3; ++i) r += std::abs(dot(b.axis[i], axis)) * b.extent[i];
    lo = c - r;
    hi = c + r;
}

/// The box an entity's box shape makes in the world.
OrientedBox shape_box(const Entity& e) {
    const CollisionShape& s = e.collision_shape();
    OrientedBox b;
    b.centre = collision_centre(e);
    b.axis[0] = quat_rotate(e.orientation(), {1, 0, 0});
    b.axis[1] = quat_rotate(e.orientation(), {0, 1, 0});
    b.axis[2] = quat_rotate(e.orientation(), {0, 0, 1});
    b.extent[0] = s.sx;
    b.extent[1] = s.sy;
    b.extent[2] = s.sz;
    return b;
}

/// The transport its head order waits for, or 0 (Moho's
/// UNITSTATE_WaitingForTransport, and its focus).
u32 awaited_transport(const Unit& u) {
    const auto& q = u.command_queue();
    return !q.empty() && q.front().type == CommandType::TransportLoad ? q.front().target_id : 0;
}

bool ignores_structures(const Unit& u) {
    return (u.footprint().flags & blueprints::kFootprintIgnoreStructures) != 0;
}

/// Whether `blocks` says yes to a unit whose collision box (Moho's cached
/// one) touches the box from `lo` to `hi` (touching counts). Moho gathers
/// them all and tests each; only whether any blocks matters, so the first
/// answers.
template <typename F>
bool any_unit_touching(const SimState& sim, const Vector3& lo, const Vector3& hi, F&& blocks) {
    return sim.entity_registry().any_unit_collider(lo.x, lo.z, hi.x, hi.z, [&](const Entity& e) {
        const auto bounds = collision_bounds(e);
        if (!bounds) return false;
        const auto& [bmin, bmax] = *bounds;
        if (lo.x > bmax.x || bmin.x > hi.x || lo.y > bmax.y || bmin.y > hi.y || lo.z > bmax.z ||
            bmin.z > hi.z)
            return false;
        return blocks(static_cast<const Unit&>(e));
    });
}

const Unit* find_unit(const SimState& sim, u32 id) {
    const Entity* e = sim.entity_registry().find(id);
    return e && e->is_unit() && !e->destroyed() ? static_cast<const Unit*>(e) : nullptr;
}

} // namespace

bool boxes_overlap(const OrientedBox& a, const OrientedBox& b) {
    // The fifteen axes; a zero cross product projects both to [0, 0] and
    // passes, as Moho's does.
    const auto separated = [&](const Vector3& axis) {
        f32 alo = 0, ahi = 0, blo = 0, bhi = 0;
        project(a, axis, alo, ahi);
        project(b, axis, blo, bhi);
        return alo > bhi || blo > ahi;
    };
    for (const Vector3& axis : a.axis)
        if (separated(axis)) return false;
    for (const Vector3& axis : b.axis)
        if (separated(axis)) return false;
    for (const Vector3& u : a.axis)
        for (const Vector3& v : b.axis)
            if (separated(cross(u, v))) return false;
    return true;
}

bool box_sphere_overlap(const OrientedBox& box, const Vector3& centre, f32 radius) {
    const Vector3 d{centre.x - box.centre.x, centre.y - box.centre.y, centre.z - box.centre.z};
    f32 outside = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const f32 along = std::abs(dot(d, box.axis[i]));
        if (along > box.extent[i]) outside += (along - box.extent[i]) * (along - box.extent[i]);
    }
    return outside <= radius * radius;
}

bool shape_overlaps_box(const Entity& e, const OrientedBox& box) {
    const CollisionShape& s = e.collision_shape();
    switch (s.type) {
    case CollisionShapeType::BOX: return boxes_overlap(shape_box(e), box);
    case CollisionShapeType::SPHERE: return box_sphere_overlap(box, collision_centre(e), s.sx);
    default: return false;
    }
}

bool is_source_unit(i32 mode, const Unit& owner, const Unit& c) {
    if (&c == &owner || c.destroyed() || c.is_dying() || !c.is_mobile()) return true;
    // Planning, a unit on the move will be gone by the time it gets there.
    if (mode == 1 && c.moved_last_tick()) return true;
    if (c.has_unit_state("Attached")) return true;
    if (owner.layer() != c.layer()) return true;
    if (owner.has_category("NAVAL") && !c.has_category("NAVAL")) return true;
    if (c.has_category("AIR") && (c.layer() == "Air" || c.transport_capacity() > 0)) return true;
    if (ignores_structures(owner) && c.footprint().flags == 0) return true;
    const u32 awaited = awaited_transport(owner);
    if (awaited != 0 && awaited == awaited_transport(c)) return true;
    if (owner.has_unit_state("Upgrading") && c.creator_id() == owner.entity_id()) return true;
    if (mode == 2) return false;
    if (c.has_category("AIR") && c.army() == owner.army()) return true;
    if (awaited == 0 && awaited_transport(c) != 0) return false;
    // The owner minds what it doesn't outrank. FAF's exe calls
    // owner->IsHigherPriorityThan(candidate) here (0x0062F00E: ecx the owner,
    // eax the candidate); faf-re's text has them the other way round.
    return outranks(owner, c);
}

bool UnitBlockers::unit_blocked(u32 unit_id, path::Cell cell, i32 mode) const {
    const Unit* unit = find_unit(sim_, unit_id);
    if (!unit) return false;
    const blueprints::Footprint& fp = unit->footprint();
    const f32 span = std::max(fp.size_x, fp.size_z);
    const Vector3 lo{static_cast<f32>(cell.x), -1000.0f, static_cast<f32>(cell.z)};
    const Vector3 hi{lo.x + span, 1000.0f, lo.z + span};
    // The footprint's box, at the middle of the span's square.
    OrientedBox box;
    box.centre = {(lo.x + hi.x) * 0.5f, 0.0f, (lo.z + hi.z) * 0.5f};
    box.axis[0] = {1, 0, 0};
    box.axis[1] = {0, 1, 0};
    box.axis[2] = {0, 0, 1};
    box.extent[0] = static_cast<f32>(fp.size_x) * 0.5f;
    box.extent[1] = 1000.0f;
    box.extent[2] = static_cast<f32>(fp.size_z) * 0.5f;
    return any_unit_touching(sim_, lo, hi, [&](const Unit& c) {
        if (c.layer() == "Land") {
            if (ignores_structures(*unit) && !ignores_structures(c)) return false;
        } else if (c.layer() == "Air" || c.layer() == "Sub") {
            return false;
        }
        // Units in one formation don't block each other.
        if (same_formation_layer(*unit, c) || is_source_unit(mode, *unit, c)) return false;
        if (!c.is_mobile()) return true;
        if (std::max(c.footprint().size_x, c.footprint().size_z) <= 1) return true;
        return shape_overlaps_box(c, box);
    });
}

bool UnitBlockers::unit_blocked_at(u32 unit_id, f32 x, f32 z, i32 mode) const {
    const Unit* unit = find_unit(sim_, unit_id);
    if (!unit) return false;
    // SFootprint::ToCellPos: the corner, rounded half to even.
    const blueprints::Footprint& fp = unit->footprint();
    const path::Cell cell{static_cast<i16>(std::lrint(x - static_cast<f32>(fp.size_x) * 0.5f)),
                          static_cast<i16>(std::lrint(z - static_cast<f32>(fp.size_z) * 0.5f))};
    return unit_blocked(unit_id, cell, mode);
}

bool UnitBlockers::swept_blocked(u32 unit_id, const path::WorldPoint& from,
                                 const path::WorldPoint& to, i32 mode) const {
    const Unit* unit = find_unit(sim_, unit_id);
    if (!unit) return false;
    if (from.x == to.x && from.y == to.y && from.z == to.z) return false;
    // A box from centre to centre, 1.11 of the blueprint's SizeX wide.
    Vector3 dir{to.x - from.x, 0.0f, to.z - from.z};
    const f32 flat = std::sqrt(dir.x * dir.x + dir.z * dir.z);
    if (flat > 0.0f) dir = {dir.x / flat, 0.0f, dir.z / flat};
    else dir = {0.0f, 0.0f, 0.0f};
    OrientedBox box;
    box.centre = {(from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f, (from.z + to.z) * 0.5f};
    box.axis[0] = {dir.z, 0.0f, -dir.x}; // a quarter turn about Y
    box.axis[1] = {0, 1, 0};
    box.axis[2] = dir;
    box.extent[0] = unit->size_x() * 0.55555558f;
    box.extent[1] = 1000.0f;
    box.extent[2] = flat * 0.5f;
    // The box's world bounds, each axis spanning its turned half-extents.
    const auto reach = [&](f32 Vector3::* m) {
        f32 r = 0.0f;
        for (int i = 0; i < 3; ++i) r += std::abs(box.axis[i].*m) * box.extent[i];
        return r;
    };
    const Vector3 lo{box.centre.x - reach(&Vector3::x), box.centre.y - reach(&Vector3::y),
                     box.centre.z - reach(&Vector3::z)};
    const Vector3 hi{box.centre.x + reach(&Vector3::x), box.centre.y + reach(&Vector3::y),
                     box.centre.z + reach(&Vector3::z)};
    return any_unit_touching(sim_, lo, hi, [&](const Unit& c) {
        if (!shape_overlaps_box(c, box)) return false;
        if (c.layer() == "Air" || c.layer() == "Sub") return false;
        return !same_formation_layer(*unit, c) && !is_source_unit(mode, *unit, c);
    });
}

} // namespace osc::sim
