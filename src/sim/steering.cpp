#include "sim/steering.hpp"

#include "blueprints/footprint.hpp"
#include "core/dmath.hpp"
#include "map/pathfinding_grid.hpp"
#include "sim/collision.hpp"
#include "sim/entity_registry.hpp"
#include "sim/navigator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <ostream>
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
constexpr f32 kPi = 3.14159265358979f;

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
           u.max_speed() > 0 && !u.transport_id() && !u.parent_entity_id();
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
EntityId guarded(const Unit& u) {
    return u.guarded_unit_id();
}

/// The transport its head order waits for, or 0.
EntityId awaited_transport(const Unit& u) {
    const auto& q = u.command_queue();
    return !q.empty() && q.front().type == CommandType::TransportLoad ? q.front().target_id
                                                                      : EntityId();
}

/// Both in one formation (attacking or not), of one army: a formation is an
/// army's, and the guard key names only the guarded unit, which allies may
/// both guard.
bool same_formation(const Unit& a, const Unit& b) {
    const u64 f = formation_layer(a);
    return f != 0 && f == formation_layer(b) && a.army() == b.army();
}

bool attacking(const Unit& u) {
    return u.has_unit_state("Attacking");
}

/// Moho's func_IsSourceUnit, as steering asks it (mode 2): whether the
/// owner ignores `c` altogether.
bool ignored(const Unit& owner, const Unit& c) {
    if (&c == &owner || !steers(c) || !c.is_mobile()) return true;
    if (c.has_unit_state("Attached")) return true;
    if (c.layer() != owner.layer() || (owner.is_naval() && !c.is_naval())) return true;
    if (can_fly(c) && (c.is_air_unit() || c.transport_capacity() > 0)) return true;
    // One that ignores structures passes over a unit with no footprint flags at all
    // (func_IsSourceUnit, 0x0062EEA0).
    if ((owner.footprint().flags & blueprints::kFootprintIgnoreStructures) != 0 &&
        c.footprint().flags == 0)
        return true;
    if (owner.has_unit_state("Upgrading") && c.creator_id() == owner.entity_id()) return true;
    const EntityId awaited = awaited_transport(owner);
    return awaited && awaited == awaited_transport(c);
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
using PathCache = std::unordered_map<EntityId, std::vector<Vector3>>;

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
    const bool ignore_braking = same_formation_layer(rec, other);
    V2 rec_at{rec.position().x, rec.position().z};
    V2 other_at{other.position().x, other.position().z};
    // A unit with no path keeps a velocity of zero throughout (retail 0x00597800 reads one only
    // from a path: the unit's own at its first node, then its steps).
    V2 rec_vel{};
    V2 other_vel{};
    // Past the first node, a unit on Moho's spline has for velocity its step since the node
    // three back, read as one tick's (retail 0x0059E2D0 subtracts the nodes and
    // UnitsWillCollide scales by 10): three times its speed, so its braking lead is nine
    // times its own. The grid drive's sampled path keeps its true speed.
    const auto per = [](const Unit& u) {
        return u.navigator().has_spline() ? kTick : kStep * kTick;
    };
    const f32 rec_per = per(rec);
    const f32 other_per = per(other);
    for (int step = 0;; step += kStep) {
        if (nav.has_collision() && nav.collision().tick < now + step) return;
        if (!rec_path.empty()) {
            if (step >= static_cast<int>(rec_path.size())) return;
            const V2 p{rec_path[step].x, rec_path[step].z};
            rec_vel = step > 0 ? V2{(p.x - rec_at.x) / rec_per, (p.z - rec_at.z) / rec_per}
                               : V2{rec.velocity().x, rec.velocity().z};
            rec_at = p;
        }
        if (!other_path.empty()) {
            if (step >= static_cast<int>(other_path.size())) return;
            const V2 p{other_path[step].x, other_path[step].z};
            other_vel = step > 0
                            ? V2{(p.x - other_at.x) / other_per, (p.z - other_at.z) / other_per}
                            : V2{other.velocity().x, other.velocity().z};
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
                nav.set_collision(
                    {other.entity_id(), now + step, {other_at.x, 0.0f, other_at.z}, 1});
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

/// Where a sidestep to `p` ends, as Moho's func_TryBuildStructureAt(border 1)
/// finds it: `p` if the unit's footprint fits there, else the first place
/// that fits a cell at a time in square rings round it (900 tries), snapped
/// to its footprint's cells; none when `p` is within a cell of the playable
/// area's edge (the binary tests the first place's bounds throughout).
bool place(SimState& sim, const Unit& u, Vector3& p) {
    const OccupancyRect b = sim.move_bounds(u.army(), false);
    if (p.x - 1 < static_cast<f32>(b.x0) || p.z - 1 < static_cast<f32>(b.z0) ||
        p.x + 1 > static_cast<f32>(b.x1) || p.z + 1 > static_cast<f32>(b.z1))
        return false;
    const blueprints::Footprint& fp = u.footprint();
    const auto fits = [&](f32 x, f32 z) {
        if (sim.footprint_fits_at(fp, x, z) == 0) return false;
        const OccupancyRect r = footprint_rect(fp, x, z);
        p.x = static_cast<f32>(r.x0) + static_cast<f32>(fp.size_x) * 0.5f;
        p.z = static_cast<f32>(r.z0) + static_cast<f32>(fp.size_z) * 0.5f;
        return true;
    };
    const f32 x = p.x, z = p.z;
    if (fits(x, z)) return true;
    int tries = 0;
    for (int ring = 1; tries < 900; ++ring) {
        for (int i = -ring; i <= ring; ++i) {
            const int step = i == -ring || i == ring ? 1 : 2 * ring;
            for (int j = -ring; j <= ring; j += step) {
                ++tries;
                if (fits(x + static_cast<f32>(i), z + static_cast<f32>(j))) return true;
            }
        }
    }
    return false;
}

/// A line of the steering log (SimState::set_steer_log).
void steer_log(const SimState& sim, const Unit& u, const char* what) {
    if (std::ostream* out = sim.steer_log())
        *out << "STEER t=" << sim.tick_count() << ' ' << u.blueprint_id() << ' ' << what << " #"
             << EntityRegistry::moho_id(u.entity_id()) << '\n';
}

// ---- Moho pathing: CAiSteeringImpl on the unit's spline ----

/// Where a unit on Moho's spline will be: its nodes from the next (none without a spline).
void spline_path(const Unit& u, std::vector<Vector3>& out) {
    out.clear();
    const Navigator& nav = u.navigator();
    if (!nav.has_spline()) return;
    const auto& nodes = nav.spline().nodes();
    for (size_t k = nav.spline().current(); k < nodes.size(); ++k) out.push_back(nodes[k].position);
}

/// Where Moho judges a unit in a meeting: its spline's next node, else where it is
/// (ResolvePathProbePosition).
V2 probe_at(const Unit& u) {
    const Navigator& nav = u.navigator();
    if (nav.has_spline() && !nav.spline().empty()) {
        const Vector3& p = nav.spline().nodes()[nav.spline().current()].position;
        return {p.x, p.z};
    }
    return {u.position().x, u.position().z};
}

bool ignores_structures(const Unit& u) {
    return (u.footprint().flags & blueprints::kFootprintIgnoreStructures) != 0;
}

/// COGrid::ForAllEntitiesIterator's units for a sphere, as CollectCollisionCandidates asks
/// it: those whose collision shape the sphere meets, and above a radius of 3 any whose bounds
/// touch the box 0.707 of the radius about its centre (0x00721FB0).
std::vector<Entity*> units_meeting_sphere(const SimState& sim, const Vector3& c, f32 r) {
    constexpr f32 kReach = 32.0f; // past any unit's shape from its centre
    const f32 inner = r * 0.70700002f;
    std::vector<Entity*> out;
    for (Entity* e : sim.entity_registry().units_in_radius(c.x, c.z, r + kReach)) {
        const CollisionShape& shape = e->collision_shape();
        Vector3 lo, hi;
        bool meets = false;
        if (shape.type == CollisionShapeType::BOX) {
            const OrientedBox box = *collision_box(*e);
            Vector3 half{};
            for (int i = 0; i < 3; ++i) {
                half.x += std::abs(box.axis[i].x) * box.extent[i];
                half.y += std::abs(box.axis[i].y) * box.extent[i];
                half.z += std::abs(box.axis[i].z) * box.extent[i];
            }
            lo = {box.centre.x - half.x, box.centre.y - half.y, box.centre.z - half.z};
            hi = {box.centre.x + half.x, box.centre.y + half.y, box.centre.z + half.z};
            meets = box_sphere_overlap(box, c, r);
        } else if (shape.type == CollisionShapeType::SPHERE) {
            const Vector3 at = collision_centre(*e);
            lo = {at.x - shape.sx, at.y - shape.sx, at.z - shape.sx};
            hi = {at.x + shape.sx, at.y + shape.sx, at.z + shape.sx};
            const f32 dx = at.x - c.x, dy = at.y - c.y, dz = at.z - c.z;
            meets = dx * dx + dy * dy + dz * dz <= (r + shape.sx) * (r + shape.sx);
        } else {
            continue;
        }
        const bool in_inner = r > 3.0f && !(c.x - inner > hi.x) && !(lo.x > c.x + inner) &&
                              !(c.y - inner > hi.y) && !(lo.y > c.y + inner) &&
                              !(c.z - inner > hi.z) && !(lo.z > c.z + inner);
        if (in_inner || meets) out.push_back(e);
    }
    return out;
}

/// Moho's CheckCollisions on the units' splines: the units near `owner` it would
/// meet, predicted for whichever of the two yields (CollectCollisionCandidates: the
/// ones it steers round first, then those of its army that yield to it).
void check_moho(SimState& sim, Unit& owner) {
    Navigator& nav = owner.navigator();
    nav.clear_collision();
    if (owner.layer() == "Sub") return;
    const i32 now = static_cast<i32>(sim.tick_count());
    std::vector<Vector3> owner_path;
    spline_path(owner, owner_path);
    const f32 top = owner.max_speed();
    const f32 a = owner.drive().max_accel;
    const f32 nodes = nav.has_spline() ? static_cast<f32>(nav.spline().nodes().size()) : 0.0f;
    const f32 radius =
        nodes * top * kTick + (a > 0 ? top * top / (a * 2.0f) : 0.0f) + larger_size(owner);
    std::vector<Unit*> preferred;
    std::vector<Unit*> deferred;
    for (Entity* e : units_meeting_sphere(sim, owner.position(), radius)) {
        auto& c = static_cast<Unit&>(*e);
        if (ignored(owner, c)) continue;
        const Navigator& cn = c.navigator();
        if (cn.has_spline() && cn.spline().type() == path::PathType::Sidestep) continue;
        if (c.army() == owner.army() && !outranks(c, owner)) {
            deferred.push_back(&c);
            continue;
        }
        if (!cn.has_spline() && !can_fly(c)) continue;
        preferred.push_back(&c);
    }
    std::vector<Vector3> cand_path;
    for (Unit* c : preferred) {
        spline_path(*c, cand_path);
        predict(owner, owner_path, *c, cand_path, now); // the owner yields
    }
    for (Unit* c : deferred) {
        spline_path(*c, cand_path);
        predict(*c, cand_path, owner, owner_path, now); // it yields
    }
}

/// Moho's ResolvePossibleCollision (retail 0x0059DA00) at the tick of the meeting: no
/// threat (type 0), brake (4), or step aside (2), the other made to brake (5).
void resolve_moho(SimState& sim, Unit& owner) {
    Navigator& nav = owner.navigator();
    Navigator::Collision col = nav.collision();
    Entity* oe = sim.entity_registry().find(col.other);
    if (!oe || oe->destroyed() || !oe->is_unit()) return;
    auto& other = static_cast<Unit&>(*oe);
    const V2 owner_live{owner.position().x, owner.position().z};
    const V2 other_live{other.position().x, other.position().z};
    const V2 owner_vel{owner.velocity().x, owner.velocity().z};
    const V2 other_vel{other.velocity().x, other.velocity().z};
    const auto set = [&](u8 type, Vector3 avoid) {
        col.type = type;
        nav.set_collision(col);
        nav.set_avoid_target(avoid);
    };
    // A flier the engine leaves be; the owner's meeting is over.
    if (can_fly(other)) {
        set(0, owner.position());
        return;
    }
    const V2 owner_at = probe_at(owner);
    const V2 other_at = probe_at(other);
    steer_log(sim, owner, "resolve");
    // Velocities a tick apart in Moho's units: 1e-6 a tick squared is 1e-4 a second.
    const V2 dv{other_vel.x - owner_vel.x, other_vel.z - owner_vel.z};
    const bool threat = !zero(other_vel) &&
                        will_collide(owner, owner_at, owner_vel, other, other_at, other_vel,
                                     same_formation_layer(owner, other)) &&
                        dot(dv, dv) > 1e-4f &&
                        dot({other_live.x - owner_live.x, other_live.z - owner_live.z}, dv) < 0;
    if (!threat) {
        set(0, owner.position());
        return;
    }
    const f32 apart = larger_size(owner) + larger_size(other) + 0.5f;
    const bool formation = same_formation_layer(owner, other);
    const V2 away = normalized({owner_live.x - other_live.x, owner_live.z - other_live.z});
    const V2 heading = normalized(other_vel);
    // Not within the cone, one ahead going its way among them, it brakes: retail's
    // overtaking is its path navigator's, which moves the target cell aside before.
    if (!(dot(away, heading) > kCone)) {
        steer_log(sim, owner, "hold");
        set(4, owner.position());
        return;
    }
    steer_log(sim, owner, "sidestep");
    // The other brakes (MarkSecondarySteeringForRepath).
    if (!formation) {
        const bool marks = other.is_naval()
                               ? owner.is_naval()
                               : !ignores_structures(other) || ignores_structures(owner);
        if (marks) {
            Navigator& on = other.navigator();
            Navigator::Collision oc = on.collision();
            oc.type = 5;
            on.set_collision(oc);
            on.set_avoid_target(other.position());
            on.set_held_for(owner.entity_id());
        }
    }
    V2 base = owner_at;
    V2 dir;
    if (!zero(owner_vel) && dot(away, forward(owner)) < 0) {
        const V2 own = normalized(owner_vel);
        const V2 of = forward(other);
        V2 side{of.z, -of.x};
        if (!(dot(side, away) > 0)) side = {-side.x, -side.z};
        const f32 ways = dot(own, heading);
        if (ways > kCone) {
            dir = normalized({away.x + side.x, away.z + side.z});
        } else if (-kCone > ways) {
            dir = side;
            base = other_live;
        } else {
            dir = normalized({own.x + away.x, own.z + away.z});
        }
    } else {
        const f32 cross =
            (owner_live.z - other_live.z) * heading.x - heading.z * (owner_live.x - other_live.x);
        const f32 turn = (formation ? kPi * 0.25f : kPi * 0.5f) * (cross > 0 ? -1.0f : 1.0f);
        const f32 c = osc::dmath::cos(turn), s = osc::dmath::sin(turn);
        dir = {heading.x * c + heading.z * s, -heading.x * s + heading.z * c};
    }
    const Vector3 avoid{base.x + dir.x * apart, owner.position().y, base.z + dir.z * apart};
    // Only where it fits; the point kept is the one aimed at, not where the fit found room.
    Vector3 fits = avoid;
    if (!place(sim, owner, fits)) return;
    set(2, avoid);
}

/// Moho's unit half of CUnitMotion::ProcessSurfaceCollisionFromLastMove and
/// Sim::DoCollisionsFor (FAF 0x00597CD0): the units its box overlaps are pushed apart,
/// the push shared by their masses.
void collide_moho_units(SimState& sim, Unit& owner) {
    if (owner.is_air_unit() || owner.layer() == "Sub" || owner.is_dying() ||
        owner.is_being_built() || owner.parent_entity_id() || can_fly(owner) || owner.pushed() ||
        owner.is_naval() || owner.destroyed()) {
        return;
    }
    const Vector3& ov = owner.motion_velocity();
    if (!(ov.x * ov.x + ov.y * ov.y + ov.z * ov.z > 0.000001f) && !owner.surface_collision())
        return;
    const OrientedBox box =
        oriented_box(owner.position(), owner.orientation(),
                     {owner.size_x() * 0.5f, owner.size_y(), owner.size_z() * 0.5f});
    f32 reach_x = 0;
    f32 reach_z = 0;
    for (size_t i = 0; i < 3; ++i) {
        reach_x += std::abs(box.axis[i].x) * box.extent[i];
        reach_z += std::abs(box.axis[i].z) * box.extent[i];
    }
    struct Hit {
        EntityId id{};
        f32 depth;
    };
    std::vector<Hit> hits;
    sim.entity_registry().any_unit_collider(
        box.centre.x - reach_x, box.centre.z - reach_z, box.centre.x + reach_x,
        box.centre.z + reach_z, [&](const Entity& e) {
            if (&e == &owner || e.destroyed()) return false;
            const auto shape = collision_box(e);
            if (!shape) return false; // shortcut: a sphere's contact is left out
            if (const auto contact = box_contact(*shape, box))
                hits.push_back({e.entity_id(), contact->depth});
            return false;
        });
    const f32 owner_mass = owner.load_metric();
    const f32 speed = std::sqrt(ov.x * ov.x + ov.y * ov.y + ov.z * ov.z);
    const f32 approach = std::min(owner.max_speed() * 0.1f, speed);
    const u32 owner_order =
        owner.command_queue().empty() ? 0 : owner.command_queue().front().command_id;
    for (const Hit& hit : hits) {
        if (hit.depth < 0.001f) continue;
        Entity* e = sim.entity_registry().find(hit.id);
        if (!e || !e->is_unit()) continue;
        auto& c = static_cast<Unit&>(*e);
        if (ignored(owner, c) || c.is_air_unit()) continue;
        // Two on one order work together: none of them pushes.
        if (owner_order != 0 && !c.command_queue().empty() &&
            c.command_queue().front().command_id == owner_order)
            return;
        if (!c.is_mobile() || same_formation_layer(owner, c)) continue;
        V2 dir{owner.last_position().x - c.position().x, owner.last_position().z - c.position().z};
        if (dir.x * dir.x + dir.z * dir.z < 0.000001f)
            dir = {sim.random().range(-1.0f, 1.0f), sim.random().range(-1.0f, 1.0f)};
        dir = normalized(dir);
        const f32 push = std::max(larger_size(owner) * 0.5f, hit.depth) + approach;
        const f32 their = owner_mass / (c.load_metric() + owner_mass);
        const f32 mine = 1.0f - their;
        const auto allowed = [](const Unit& pushed, const Unit& against) {
            return pushed.footprint().flags == 0 ||
                   (ignores_structures(pushed) && ignores_structures(against));
        };
        if (mine > 0.1f && allowed(owner, c) && !owner.immobile())
            owner.add_impulse({dir.x * push * mine, 0, dir.z * push * mine},
                              owner.navigator().has_motion_node());
        if (their > 0.1f && allowed(c, owner) && !c.immobile())
            c.add_impulse({-dir.x * push * their, 0, -dir.z * push * their},
                          c.navigator().has_motion_node());
    }
}

/// Moho's CAiSteeringImpl::Execute on a unit's spline, in its army's phase: the spline
/// made and checked, its meetings resolved, and the next node handed to its motion.
void steer_moho(SimState& sim, Unit& u) {
    Navigator& nav = u.navigator();
    const i32 now = static_cast<i32>(sim.tick_count());
    const auto mode = [&] {
        return nav.through_target() ? path::PathType::TopSpeed : path::PathType::Normal;
    };
    const auto stop = [&] { steer_stop(sim, u); };
    // ProcessSplineMovement.
    const auto process = [&] {
        const Vector3* dest = nav.steer_destination();
        if (u.pushed()) {
            steer_log(sim, u, "pushed");
            // Pushed: off its spline till slower than a hundredth of its top speed a tick;
            // pushed off one, it asks its way afresh.
            if (nav.has_spline()) stop();
            const Vector3& v = u.motion_velocity();
            if (u.max_speed() * 0.01f > std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z)) {
                u.set_pushed(false);
                if (u.in_state_transition()) {
                    u.set_in_state_transition(false);
                    nav.moho_path().request_repath();
                    if (dest) {
                        nav.spline_generate(u, *dest, mode(), false);
                        check_moho(sim, u);
                    }
                }
            }
        } else if (nav.has_spline() && nav.spline().at_end()) {
            if (!dest || Navigator::unit_at(u, *dest)) {
                stop();
                return;
            }
            nav.spline_generate(u, *dest, mode(), true);
            check_moho(sim, u);
        }
        if (!nav.steer_paused()) {
            if (u.immobile()) {
                nav.set_steer_paused(true);
                stop();
                return;
            }
        } else if (!u.immobile()) {
            if (dest) {
                nav.spline_generate(u, *dest, mode(), false);
                check_moho(sim, u);
            }
            nav.set_steer_paused(false);
        }
        const Navigator::Collision col = nav.collision();
        if (col.type == 5) {
            nav.spline_brake(u, 4);
        } else if (col.type == 1 && now >= col.tick) {
            resolve_moho(sim, u);
            const Navigator::Collision after = nav.collision();
            if (after.type == 0) {
                if (dest) {
                    if (!nav.has_spline() || nav.spline().empty())
                        nav.spline_generate(u, *dest, mode(), false);
                    check_moho(sim, u);
                }
            } else if (after.type == 2) {
                const Vector3 to = nav.avoid_target();
                nav.spline_generate(u, to, path::PathType::Sidestep, false);
                nav.set_avoid_target({});
            } else if (after.type == 4) {
                nav.set_held_for(after.other);
                nav.spline_brake(u, 4);
            }
        }
        nav.advance_motion_node();
    };
    // DriveToNextWaypoint: on the spline it has, then afresh to a new waypoint.
    process();
    if (nav.take_waypoint_refresh()) {
        if (const Vector3* wp = nav.waypoint()) {
            nav.set_steer_destination(*wp);
            if (!Navigator::unit_at(u, *wp)) {
                nav.spline_generate(u, *wp, mode(), false);
                check_moho(sim, u);
                process();
            }
        }
    }
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
    steer_log(sim, owner, "resolve");
    const V2 owner_at{owner.position().x, owner.position().z};
    const V2 other_at{other.position().x, other.position().z};
    const V2 owner_vel{owner.velocity().x, owner.velocity().z};
    const V2 other_vel{other.velocity().x, other.velocity().z};
    // Not moving, not on course to meet, or not closing: nothing to do.
    const V2 rel_vel{owner_vel.x - other_vel.x, owner_vel.z - other_vel.z};
    const bool closing = !zero(other_vel) && !zero(rel_vel) &&
                         will_collide(owner, owner_at, owner_vel, other, other_at, other_vel,
                                      same_formation_layer(owner, other)) &&
                         dot({owner_at.x - other_at.x, owner_at.z - other_at.z}, rel_vel) < 0;
    if (!closing) {
        nav.set_next_check(now);
        return;
    }
    const V2 to_other = normalized({other_at.x - owner_at.x, other_at.z - owner_at.z});
    const V2 from_other{-to_other.x, -to_other.z};
    V2 heading = normalized(other_vel);
    if (zero(heading)) heading = forward(other);
    // Moho's cone (retail 0x0059DA00): it is within the other's forward cone,
    // the other coming at it. faf-re's decompile reads it the other way round.
    const bool coming = dot(from_other, heading) > kCone;
    // shortcut: retail overtakes by its path navigator moving the target aside, which the engine
    // lacks; until it does, one ahead going its way is stepped round here.
    const bool overtaking = dot(from_other, heading) < -kCone;
    // Crossing: it stops and lets the other pass. When the other yields to
    // it too (two of other armies, or a tie in rank, where Moho's timing
    // tells them apart), the one that wins goes first: two yielding to each
    // other would stop for ever.
    steer_log(sim, owner, coming ? "sidestep" : "hold");
    if (!coming && !overtaking) {
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
    // Coming at it (or ahead going its way): it steps aside, and the other
    // stops (unless both are in one formation, or only the other is naval).
    const bool formation = same_formation(owner, other);
    if (!formation && (!other.is_naval() || owner.is_naval()))
        other.navigator().hold(kHoldTicks, owner.entity_id());
    const f32 apart = larger_size(owner) + larger_size(other) + 0.5f;
    if (coming) {
        // Retail's step aside (0x0059DA00): driving at the other, it makes for
        // a point off the other's side it is on (meeting head-on, beside the
        // other itself), else it turns from the other's way, a right angle
        // (half that in a formation).
        V2 from = owner_at;
        V2 dir;
        if (!zero(owner_vel) && dot(from_other, forward(owner)) < 0) {
            const V2 own = normalized(owner_vel);
            const V2 of = forward(other);
            V2 side{of.z, -of.x};
            if (dot(side, from_other) <= 0) side = {-side.x, -side.z};
            const f32 ways = dot(own, heading);
            if (ways > kCone) {
                dir = normalized({from_other.x + side.x, from_other.z + side.z});
            } else if (ways < -kCone) {
                dir = side;
                from = other_at;
            } else {
                dir = normalized({own.x + from_other.x, own.z + from_other.z});
            }
        } else {
            const f32 cross =
                (owner_at.z - other_at.z) * heading.x - heading.z * (owner_at.x - other_at.x);
            const f32 turn = (formation ? kPi * 0.25f : kPi * 0.5f) * (cross > 0 ? -1.0f : 1.0f);
            const f32 c = osc::dmath::cos(turn), s = osc::dmath::sin(turn);
            dir = {heading.x * c + heading.z * s, -heading.x * s + heading.z * c};
        }
        Vector3 point{from.x + dir.x * apart, owner.position().y, from.z + dir.z * apart};
        // No place it fits near there: Moho leaves it on its way.
        if (zero(dir) || !place(sim, owner, point)) {
            nav.set_next_check(now);
            return;
        }
        nav.sidestep(point);
        return;
    }
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

u64 formation_layer(const Unit& u) {
    const auto& q = u.command_queue();
    if (q.empty()) return 0;
    const UnitCommand& head = q.front();
    // (Moho's GuardBusy, a guard off fighting for its unit: here its head
    // order is the fight, so it isn't in the guard formation.)
    if (head.type == CommandType::Guard && head.target_id && !u.has_category("ENGINEER")) {
        return (u64{1} << 32) | head.target_id.moho();
    }
    if (head.formed) return (u64{2} << 32) | head.command_id;
    return 0;
}

bool same_formation_layer(const Unit& a, const Unit& b) {
    return !attacking(a) && !attacking(b) && same_formation(a, b);
}

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
    if (awaited_transport(a) && !awaited_transport(b)) {
        return true;
    }
    if (guarded(a) == b.entity_id()) return false;
    if (guarded(b) == a.entity_id()) return true;
    if (moving(a) != moving(b)) return !moving(a);
    if (footprint(a) != footprint(b)) return footprint(a) > footprint(b);
    if (const Unit* line = more_in_line(b, a)) return line == &a;
    return a.entity_id() < b.entity_id();
}

void steer_units(SimState& sim, const std::vector<EntityId>& ids) {
    const i32 now = static_cast<i32>(sim.tick_count());
    PathCache cache;
    for (const EntityId id : ids) {
        Entity* e = sim.entity_registry().find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto& u = static_cast<Unit&>(*e);
        Navigator& nav = u.navigator();
        if (sim.moho_pathing() && nav.moho_active()) {
            if (steers(u)) steer_moho(sim, u);
            continue;
        }
        // Moho's steering ticks every unit, a still one too (CAiSteeringImpl::OnTick): one
        // pushed settles once slower than a hundredth of its top speed a tick
        // (ProcessSplineMovement), and meets what it runs into again.
        if (sim.moho_pathing() && steers(u) && u.pushed()) {
            const Vector3& v = u.motion_velocity();
            if (u.max_speed() * 0.01f > std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z)) {
                u.set_pushed(false);
                u.set_in_state_transition(false);
            }
            continue;
        }
        if (!steers(u) || !nav.is_moving() || nav.holding() || nav.sidestepping()) continue;
        if (nav.has_collision() && now >= nav.collision().tick) resolve(sim, u, now, cache);
        else if (now >= nav.next_check()) check(sim, u, now, cache);
    }
}

void collide_units(SimState& sim, Unit& u) {
    collide_moho_units(sim, u);
}

void steer_stop(SimState& sim, Unit& u) {
    Navigator& nav = u.navigator();
    if (!nav.has_spline()) return;
    const bool remaining = !nav.spline().empty();
    nav.spline_stop();
    if (remaining) check_moho(sim, u);
}

void steer_ground_units(SimState& sim) {
    std::vector<EntityId> ids;
    sim.entity_registry().for_each_unit([&](Entity& e) {
        if (auto& u = static_cast<Unit&>(e); steers(u) && moving(u)) {
            ids.push_back(u.entity_id());
        }
    });
    std::sort(ids.begin(), ids.end());
    steer_units(sim, ids);
}

} // namespace osc::sim
