#include "sim/steering.hpp"

#include "core/dmath.hpp"
#include "map/pathfinding_grid.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace osc::sim {

namespace {

// Moho's CAiPathSpline and CAiSteeringImpl (faf-re).
constexpr int kNodes = 20;    ///< a path ahead: one node a tick (kDefaultPathNodeLimit)
constexpr int kStep = 3;      ///< predictions every third node
constexpr int kHoldTicks = 5; ///< a mode 4 path ends five still nodes on
/// Checks every half path ahead. Moho also checks at each of its
/// navigator's waypoints, which lie close together; the engine's A*
/// waypoints are few, and a 20-tick check can miss a meeting just past its
/// reach until the units touch.
constexpr int kCheckEvery = kNodes / 2;
constexpr f32 kCone = 0.707f; ///< ahead and going its way: within 45 degrees
constexpr f32 kTick = 0.1f;

struct V2 {
    f32 x = 0, z = 0;
};

f32 dot(V2 a, V2 b) {
    return a.x * b.x + a.z * b.z;
}
V2 normalized(V2 v) {
    const f32 l = std::sqrt(v.x * v.x + v.z * v.z);
    return l > 1e-6f ? V2{v.x / l, v.z / l} : V2{};
}
bool zero(V2 v) {
    return v.x * v.x + v.z * v.z <= 1e-6f;
}

/// Its facing, flattened (heading 0 is +z, pi/2 is +x).
V2 forward(const Unit& u) {
    const f32 yaw = quat_yaw(u.orientation());
    return {osc::dmath::sin(yaw), osc::dmath::cos(yaw)};
}

/// A unit the pass steers: on the ground (or the water), able to move, not
/// carried, not dying or unfinished.
bool steers(const Unit& u) {
    return !u.destroyed() && !u.is_dying() && !u.is_being_built() && !u.is_air_unit() &&
           u.max_speed() > 0 && u.transport_id() == 0 && u.parent_entity_id() == 0;
}

bool can_fly(const Unit& u) {
    return u.motion_type() == "RULEUMT_Air";
}
bool moving(const Unit& u) {
    return u.navigator().busy();
}
f32 larger_size(const Unit& u) {
    return std::max(u.size_x(), u.size_z());
}
f32 footprint(const Unit& u) {
    return std::max(u.footprint_size_x(), u.footprint_size_z());
}
f32 accel(const Unit& u) {
    const f32 a = u.drive().max_accel * u.accel_mult();
    return a > 0 ? a : u.max_speed();
}

/// The unit its head order guards (or fights for, guarding), or 0.
u32 guarded(const Unit& u) {
    return u.guarded_unit_id();
}

/// The transport its head order waits for, or 0.
u32 awaited_transport(const Unit& u) {
    const auto& q = u.command_queue();
    return !q.empty() && q.front().type == CommandType::TransportLoad ? q.front().target_id : 0;
}

/// The formation move it is in (its head order's id), or 0.
u32 formation_move(const Unit& u) {
    const auto& q = u.command_queue();
    return !q.empty() && !q.front().formation.empty() ? q.front().command_id : 0;
}

bool same_formation(const Unit& a, const Unit& b) {
    const u32 f = formation_move(a);
    return f != 0 && f == formation_move(b) && a.army() == b.army();
}

bool attacking(const Unit& u) {
    return u.has_unit_state("Attacking");
}

/// Moho's func_IsSourceUnit, as steering asks it (mode 2): whether the
/// owner ignores `c` altogether.
bool ignored(const Unit& owner, const Unit& c) {
    if (&c == &owner || !steers(c) || !c.is_mobile()) return true;
    if (c.layer() != owner.layer() || (owner.is_naval() && !c.is_naval())) return true;
    if (can_fly(c) && (c.is_air_unit() || c.transport_capacity() > 0)) return true;
    const u32 awaited = awaited_transport(owner);
    return awaited != 0 && awaited == awaited_transport(c);
}

/// Moho's func_UnitMoreInLineToOther(a1, a2): the one that faces the other
/// less, or null when neither faces the other.
const Unit* more_in_line(const Unit& a1, const Unit& a2) {
    const V2 a1_to_a2 =
        normalized({a2.position().x - a1.position().x, a2.position().z - a1.position().z});
    const V2 a2_to_a1{-a1_to_a2.x, -a1_to_a2.z};
    const f32 a2_align = dot(a2_to_a1, forward(a2));
    const f32 a1_align = dot(a1_to_a2, forward(a1));
    if (a2_align <= 0 && a1_align <= 0) return nullptr;
    return a2_align <= a1_align ? &a2 : &a1;
}

/// Moho's braking lead (sub_596930's helper): how far ahead of it a unit
/// going `v` a second would stop.
f32 braking_lead(V2 v, const Unit& u, bool ignore_braking) {
    if (ignore_braking) return 0;
    const f32 speed_sq = dot(v, v);
    const f32 a = u.drive().max_accel;
    return a > 0 ? speed_sq / (a * 2.0f) : 0.0f;
}

/// A unit's box in plan view, led by its braking distance: Moho's
/// BuildCollisionObb2D, centred the whole lead ahead of the unit and
/// (SizeZ + lead) long. So a unit braking further than its own length has
/// a box that no longer covers its rear; that is Moho's placement, kept.
struct Box {
    V2 center, axis0, axis1;
    f32 extent0 = 0, extent1 = 0;
};

Box box_of(const Unit& u, V2 at, f32 lead) {
    const V2 f = forward(u);
    Box b;
    b.center = {at.x + f.x * lead, at.z + f.z * lead};
    b.axis1 = f;
    b.axis0 = {f.z, -f.x};
    b.extent0 = (u.size_x() + u.size_z()) * 0.25f;
    b.extent1 = (u.size_z() + lead) * 0.5f;
    return b;
}

bool overlap_on(const Box& a, const Box& b, V2 axis) {
    const V2 n = normalized(axis);
    if (zero(n)) return true;
    const f32 dist = std::abs(dot({b.center.x - a.center.x, b.center.z - a.center.z}, n));
    const f32 ra = std::abs(dot(a.axis0, n)) * a.extent0 + std::abs(dot(a.axis1, n)) * a.extent1;
    const f32 rb = std::abs(dot(b.axis0, n)) * b.extent0 + std::abs(dot(b.axis1, n)) * b.extent1;
    return dist <= ra + rb;
}

/// Moho's func_UnitsWillCollide: the two units' boxes, each led by its
/// braking distance at the given velocity, overlap.
bool will_collide(const Unit& a, V2 a_at, V2 a_vel, const Unit& b, V2 b_at, V2 b_vel,
                  bool ignore_braking) {
    const f32 a_lead = braking_lead(a_vel, a, ignore_braking);
    const f32 b_lead = braking_lead(b_vel, b, ignore_braking);
    const f32 reach = a.size_z() + a_lead + b.size_z() + b_lead;
    const f32 dx = a_at.x - b_at.x, dz = a_at.z - b_at.z;
    if (dx * dx + dz * dz > reach * reach) return false;
    const Box ba = box_of(a, a_at, a_lead);
    const Box bb = box_of(b, b_at, b_lead);
    return overlap_on(ba, bb, ba.axis0) && overlap_on(ba, bb, ba.axis1) &&
           overlap_on(ba, bb, bb.axis0) && overlap_on(ba, bb, bb.axis1);
}

/// Where the unit expects to be each of the next ticks.
void path_ahead(const Unit& u, std::vector<Vector3>& out) {
    u.navigator().path_ahead(u.position(), u.ground_speed(), u.max_speed(), accel(u), kNodes, out);
}

/// Each unit's path ahead, made once a tick: the pass runs before anything
/// moves, and a unit meets many others. A resolution that changes a unit's
/// path drops its entry. (Looked up by id only; nothing walks it.)
using PathCache = std::unordered_map<u32, std::vector<Vector3>>;

const std::vector<Vector3>& cached_path(PathCache& cache, const Unit& u) {
    auto [it, fresh] = cache.try_emplace(u.entity_id());
    if (fresh) path_ahead(u, it->second);
    return it->second;
}

/// Moho's PredictCollisionForSteerings: walk `rec`'s path and `other`'s
/// together, every third node, and record in `rec`'s navigator the first
/// meeting nearer than the one it holds.
void predict(Unit& rec, const std::vector<Vector3>& rec_path, const Unit& other,
             const std::vector<Vector3>& other_path, i32 now) {
    if (rec_path.empty() && other_path.empty()) return;
    Navigator& nav = rec.navigator();
    if (nav.collision().other == other.entity_id()) nav.clear_collision();
    const bool ignore_braking = !attacking(rec) && !attacking(other) && same_formation(rec, other);
    V2 rec_at{rec.position().x, rec.position().z};
    V2 other_at{other.position().x, other.position().z};
    V2 rec_vel{rec.velocity().x, rec.velocity().z};
    V2 other_vel{other.velocity().x, other.velocity().z};
    for (int step = 0;; step += kStep) {
        if (nav.has_collision() && nav.collision().tick < now + step) return;
        if (!rec_path.empty()) {
            if (step >= static_cast<int>(rec_path.size())) return;
            const V2 p{rec_path[step].x, rec_path[step].z};
            if (step > 0)
                rec_vel = {(p.x - rec_at.x) / (kStep * kTick), (p.z - rec_at.z) / (kStep * kTick)};
            rec_at = p;
        }
        if (!other_path.empty()) {
            if (step >= static_cast<int>(other_path.size())) return;
            const V2 p{other_path[step].x, other_path[step].z};
            if (step > 0)
                other_vel = {(p.x - other_at.x) / (kStep * kTick),
                             (p.z - other_at.z) / (kStep * kTick)};
            other_at = p;
        }
        if (will_collide(rec, rec_at, rec_vel, other, other_at, other_vel, ignore_braking)) {
            const f32 held =
                nav.has_collision()
                    ? (rec_at.x - nav.collision().at.x) * (rec_at.x - nav.collision().at.x) +
                          (rec_at.z - nav.collision().at.z) * (rec_at.z - nav.collision().at.z)
                    : 9999.0f;
            const f32 fresh = (rec_at.x - other_at.x) * (rec_at.x - other_at.x) +
                              (rec_at.z - other_at.z) * (rec_at.z - other_at.z);
            if (held > fresh)
                nav.set_collision({other.entity_id(), now + step, {other_at.x, 0.0f, other_at.z}});
            return;
        }
        if (rec_path.empty() && other_path.empty()) return;
    }
}

/// Moho's CheckCollisions: the units near `owner` it would meet, each
/// predicted for whichever of the two yields.
void check(SimState& sim, Unit& owner, i32 now, PathCache& cache) {
    Navigator& nav = owner.navigator();
    nav.clear_collision();
    const std::vector<Vector3>& owner_path = cached_path(cache, owner);
    nav.set_next_check(now + kCheckEvery);
    if (owner.layer() == "Sub") return; // Moho checks no submerged owner
    const f32 a = accel(owner);
    const f32 radius = static_cast<f32>(owner_path.size()) * owner.max_speed() * kTick +
                       owner.max_speed() * owner.max_speed() / (a * 2.0f) + larger_size(owner);
    for (Entity* e :
         sim.entity_registry().units_in_radius(owner.position().x, owner.position().z, radius)) {
        auto& c = static_cast<Unit&>(*e);
        if (ignored(owner, c)) continue;
        if (c.navigator().sidestepping()) continue; // a PT_2 path under way
        const std::vector<Vector3>& cand_path = cached_path(cache, c);
        if (c.army() == owner.army() && !outranks(c, owner)) {
            predict(c, cand_path, owner, owner_path, now); // it yields
        } else if (!cand_path.empty() || can_fly(c)) {
            predict(owner, owner_path, c, cand_path, now); // the owner yields
        }
    }
}

/// Which of two units that yield to each other goes first: the one that
/// outranks the other without being outranked back, else the lower id.
bool wins(const Unit& a, const Unit& b) {
    const bool ab = outranks(a, b), ba = outranks(b, a);
    if (ab != ba) return ab;
    return a.entity_id() < b.entity_id();
}

/// Moho's ResolvePossibleCollision, at the tick of the meeting.
void resolve(SimState& sim, Unit& owner, i32 now, PathCache& cache) {
    Navigator& nav = owner.navigator();
    const Navigator::Collision col = nav.collision();
    nav.clear_collision();
    // Whatever it decides changes the paths ahead of the two.
    cache.erase(owner.entity_id());
    cache.erase(col.other);
    Entity* oe = sim.entity_registry().find(col.other);
    if (!oe || oe->destroyed() || !oe->is_unit()) {
        nav.set_next_check(now);
        return;
    }
    auto& other = static_cast<Unit&>(*oe);
    // A flier (on the ground) Moho stops; nothing else changes. The engine
    // doesn't stop it.
    if (can_fly(other)) return;
    const V2 owner_at{owner.position().x, owner.position().z};
    const V2 other_at{other.position().x, other.position().z};
    const V2 owner_vel{owner.velocity().x, owner.velocity().z};
    const V2 other_vel{other.velocity().x, other.velocity().z};
    // Not moving, not on course to meet, or not closing: nothing to do.
    const V2 rel_vel{owner_vel.x - other_vel.x, owner_vel.z - other_vel.z};
    const bool closing =
        !zero(other_vel) && !zero(rel_vel) &&
        will_collide(owner, owner_at, owner_vel, other, other_at, other_vel,
                     !attacking(owner) && !attacking(other) && same_formation(owner, other)) &&
        dot({owner_at.x - other_at.x, owner_at.z - other_at.z}, rel_vel) < 0;
    if (!closing) {
        nav.set_next_check(now);
        return;
    }
    const V2 to_other = normalized({other_at.x - owner_at.x, other_at.z - owner_at.z});
    V2 heading = normalized(other_vel);
    if (zero(heading)) heading = forward(other);
    // Crossing or head-on: it stops and lets the other pass. When the
    // other yields to it too (two of other armies, or a tie in rank, where
    // Moho's timing tells them apart), the one that wins goes first: two
    // yielding to each other would stop for ever.
    if (dot(to_other, heading) <= kCone) {
        const Navigator& on = other.navigator();
        const bool other_yields =
            on.held_for() == owner.entity_id() || on.collision().other == owner.entity_id();
        if (other_yields && wins(owner, other)) {
            nav.set_next_check(now + kStep);
            return;
        }
        nav.hold(kHoldTicks, other.entity_id());
        return;
    }
    // Ahead and going its way: it steps aside to overtake, and the other
    // stops (unless both are in one formation, or only the other is naval).
    const bool formation = same_formation(owner, other);
    if (!formation && (!other.is_naval() || owner.is_naval()))
        other.navigator().hold(kHoldTicks, owner.entity_id());
    const f32 apart = larger_size(owner) + larger_size(other) + 0.5f;
    V2 own_heading = moving(owner) && !zero(owner_vel) ? normalized(owner_vel) : forward(owner);
    if (zero(own_heading)) own_heading = forward(owner);
    V2 base = heading;
    if (!zero(owner_vel) && dot(own_heading, heading) > kCone)
        base = normalized({heading.x + own_heading.x, heading.z + own_heading.z});
    if (zero(base)) base = heading;
    V2 lateral = normalized({-base.z, base.x});
    if (zero(lateral)) lateral = normalized({-to_other.z, to_other.x});
    const f32 cross = to_other.x * base.z - to_other.z * base.x;
    if (cross > 0) lateral = {-lateral.x, -lateral.z};
    if (formation) lateral = {-lateral.x, -lateral.z};
    V2 away = normalized({base.x + lateral.x, base.z + lateral.z});
    if (zero(away)) away = {-to_other.x, -to_other.z};
    Vector3 point{owner_at.x + away.x * apart, owner.position().y, owner_at.z + away.z * apart};
    // Somewhere it can drive, or it stops instead.
    if (const auto* grid = sim.pathfinding_grid()) {
        u32 gx = 0, gz = 0;
        grid->world_to_grid(point.x, point.z, gx, gz);
        if (!grid->is_passable_for(gx, gz, owner.layer(), owner.naval_draft(),
                                   owner.is_amphibious() || owner.is_hover())) {
            nav.hold(kHoldTicks, other.entity_id());
            return;
        }
    }
    point = sim.clamp_to_playable(point, owner.army());
    nav.sidestep(point);
}

} // namespace

bool outranks(const Unit& a, const Unit& b) {
    if (a.immobile() || a.has_unit_state("Upgrading")) return true;
    if (b.immobile() || b.has_unit_state("Upgrading")) return false;
    if (a.is_naval() != b.is_naval()) return a.is_naval();
    // One that paths through structures goes first.
    const bool a_ignores = (a.footprint().flags & blueprints::kFootprintIgnoreStructures) != 0;
    const bool b_ignores = (b.footprint().flags & blueprints::kFootprintIgnoreStructures) != 0;
    if (a_ignores != b_ignores) return a_ignores;
    const bool a_grounded_flier = can_fly(a) && !a.is_air_unit();
    const bool b_grounded_flier = can_fly(b) && !b.is_air_unit();
    if (a_grounded_flier) return true;
    if (b_grounded_flier) return false;
    if (awaited_transport(a) != 0 && awaited_transport(b) == 0) return true;
    if (guarded(a) == b.entity_id()) return false;
    if (guarded(b) == a.entity_id()) return true;
    if (moving(a) != moving(b)) return !moving(a);
    if (footprint(a) != footprint(b)) return footprint(a) > footprint(b);
    if (const Unit* line = more_in_line(b, a)) return line == &a;
    return a.entity_id() < b.entity_id();
}

void steer_ground_units(SimState& sim) {
    const i32 now = static_cast<i32>(sim.tick_count());
    std::vector<u32> ids;
    sim.entity_registry().for_each_unit([&](Entity& e) {
        if (auto& u = static_cast<Unit&>(e); steers(u) && moving(u)) ids.push_back(u.entity_id());
    });
    std::sort(ids.begin(), ids.end());
    PathCache cache;
    for (const u32 id : ids) {
        Entity* e = sim.entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto& u = static_cast<Unit&>(*e);
        Navigator& nav = u.navigator();
        if (!steers(u) || !nav.is_moving() || nav.holding() || nav.sidestepping()) continue;
        if (nav.has_collision() && now >= nav.collision().tick) resolve(sim, u, now, cache);
        else if (now >= nav.next_check()) check(sim, u, now, cache);
    }
}

} // namespace osc::sim
