// What a patrol takes on along its route, as Moho's CUnitPatrolTask does
// (faf-re): FindTarget, EvaluatePatrolReclaimAttack, RecomputePatrolSearchBox.

#include "sim/unit.hpp"

#include "sim/army_brain.hpp"
#include "sim/category_set.hpp"
#include "sim/entity_registry.hpp"
#include "sim/prop.hpp"
#include "sim/sim_state.hpp"
#include "sim/weapon.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace osc::sim {

namespace {

/// Moho's RecomputePatrolSearchBox.
struct PatrolBox {
    Vector3 center;
    f32 along_x = 0;
    f32 along_z = 1;
    f32 half_width = 0;
    f32 half_length = 0;

    bool contains(const Vector3& p) const {
        const f32 dx = p.x - center.x;
        const f32 dz = p.z - center.z;
        return std::fabs(dx * along_z - dz * along_x) <= half_width &&
               std::fabs(dx * along_x + dz * along_z) <= half_length &&
               std::fabs(p.y - center.y) <= 100.0f;
    }

    /// Every entity in the box's bounds, in id order.
    std::vector<u32> collect(const EntityRegistry& registry) const {
        const f32 reach_x = std::fabs(along_z) * half_width + std::fabs(along_x) * half_length;
        const f32 reach_z = std::fabs(along_x) * half_width + std::fabs(along_z) * half_length;
        return registry.collect_in_rect(center.x - reach_x, center.z - reach_z, center.x + reach_x,
                                        center.z + reach_z);
    }
    /// The live units in its bounds, in id order (from the grid of units).
    std::vector<Entity*> units(const EntityRegistry& registry) const {
        const f32 reach_x = std::fabs(along_z) * half_width + std::fabs(along_x) * half_length;
        const f32 reach_z = std::fabs(along_x) * half_width + std::fabs(along_z) * half_length;
        return registry.units_in_rect(center.x - reach_x, center.z - reach_z, center.x + reach_x,
                                      center.z + reach_z);
    }
};

PatrolBox patrol_box(const Unit& unit, const UnitCommand& leg) {
    // The box runs to the leg from the queue's tail when that is another
    // patrol point, else from the unit. Moho reads only the tail, whatever
    // the leg: an attack-move with a patrol queued after it measures from
    // that patrol's point too (faf-re RecomputePatrolSearchBox).
    const auto& queue = unit.command_queue();
    const UnitCommand& last = queue.back();
    const Vector3 to = leg.target_pos;
    const Vector3 from =
        &last != &leg && last.type == CommandType::Patrol ? last.target_pos : unit.position();
    PatrolBox box;
    const f32 dx = to.x - from.x;
    const f32 dz = to.z - from.z;
    const f32 length = std::sqrt(dx * dx + dz * dz);
    if (length >= 0.001f) {
        box.along_x = dx / length;
        box.along_z = dz / length;
    }
    box.center = {(to.x + from.x) * 0.5f, from.y, (to.z + from.z) * 0.5f};
    box.half_width = unit.guard_scan_radius();
    box.half_length = unit.guard_scan_radius() + length * 0.5f;
    return box;
}

bool off_map(const SimState& sim, const Vector3& p, f32 margin) {
    return sim.has_playable_rect() &&
           (p.x < sim.playable_x0() + margin || p.x > sim.playable_x1() - margin ||
            p.z < sim.playable_z0() + margin || p.z > sim.playable_z1() - margin);
}

bool outside_no_rush(const SimState& sim, const ArmyBrain* brain, const Vector3& p) {
    if (!brain || !sim.no_rush_active()) {
        return false;
    }
    const f32 dx = brain->start_position().x - p.x;
    const f32 dz = brain->start_position().z - p.z;
    return std::sqrt(dx * dx + dz * dz) > sim.no_rush_radius();
}

bool nearly_full(const ResourceState& r) {
    return r.stored > r.max_storage * 0.75;
}

} // namespace

const Weapon* Unit::scan_weapon() const {
    for (const auto& w : weapons_) {
        if (w->weapon_index == 0) {
            return w->dummy || w->fire_on_death || w->manual_fire ? nullptr : w.get();
        }
    }
    return nullptr;
}

bool Unit::target_exempt(const Unit& enemy, const EntityRegistry& registry) const {
    // Moho's CAiAttackerImpl::IsTargetExempt: what its own queue reclaims or
    // captures, and what its army's engineers are capturing.
    for (const UnitCommand& c : command_queue_) {
        if ((c.type == CommandType::Reclaim || c.type == CommandType::Capture) &&
            c.target_id == enemy.entity_id()) {
            return true;
        }
    }
    if (!enemy.is_being_captured()) {
        return false;
    }
    static const CategoryName kEngineer{"ENGINEER"};
    bool capturing = false;
    registry.for_each_unit([&](const Entity& e) {
        if (capturing || e.destroyed() || e.army() != army()) {
            return;
        }
        const auto& u = static_cast<const Unit&>(e);
        capturing = !u.is_dying() && u.capture_target_id() == enemy.entity_id() &&
                    u.has_category(kEngineer);
    });
    return capturing;
}

Entity* Unit::best_enemy(const std::vector<Entity*>& candidates, f32 range, SimContext& ctx) {
    const SimState* sim = ctx.sim;
    if (!sim || is_being_built() || army() < 0) {
        return nullptr;
    }
    const Weapon* weapon = scan_weapon();
    if (!weapon) {
        return nullptr;
    }
    static const CategoryName kBenign{"BENIGN"};
    Entity* best = nullptr;
    int best_priority = 0;
    f32 best_dist2 = 0;
    for (Entity* e : candidates) {
        if (!e || e->destroyed() || !e->is_unit()) {
            continue;
        }
        const auto& enemy = static_cast<const Unit&>(*e);
        if (enemy.is_dying() || !sim->is_enemy(army(), enemy.army())) {
            continue;
        }
        const f32 dx = enemy.position().x - position().x;
        const f32 dz = enemy.position().z - position().z;
        const f32 dist2 = dx * dx + dz * dz;
        if (dist2 > range * range) {
            continue;
        }
        if (sim->recon_of(enemy, static_cast<u32>(army())) == 0 || enemy.has_category(kBenign)) {
            continue;
        }
        const bool air = enemy.is_air_unit();
        if ((enemy.parent_entity_id() != 0 && (air || enemy.is_being_built())) ||
            (air && enemy.is_being_built()) || (!air && off_map(*sim, enemy.position(), 0))) {
            continue;
        }
        if (target_exempt(enemy, ctx.registry)) {
            continue;
        }
        if (!weapon->can_pick(*this, enemy, sim)) {
            continue;
        }
        // A unit that unpacks to fire takes on only what it can hit from
        // where it stands (FindBestEnemy: a NeedUnpack unit's candidate needs
        // an Available firing solution).
        if (need_unpack_ && !weapon->can_target(*this, enemy, sim)) {
            continue;
        }
        const int priority = weapon->priority_of(enemy);
        if (priority < 0) {
            continue;
        }
        if (!best || priority < best_priority ||
            (priority == best_priority && dist2 < best_dist2)) {
            best = e;
            best_priority = priority;
            best_dist2 = dist2;
        }
    }
    return best;
}

Entity* Unit::find_patrol_target(const UnitCommand& cmd, SimContext& ctx) {
    const PatrolBox box = patrol_box(*this, cmd);
    std::vector<Entity*> candidates;
    for (Entity* e : box.units(ctx.registry))
        if (box.contains(e->position())) candidates.push_back(e);
    return best_enemy(candidates, guard_scan_radius(), ctx);
}

Entity* Unit::find_patrol_work(const UnitCommand& cmd, SimContext& ctx) {
    static const CategoryName kPatrolHelper{"PATROLHELPER"};
    static const CategoryName kCommand{"COMMAND"};
    static const CategoryName kSacu{"SACU_BEHAVIOR"};
    static const CategoryName kReclaim{"RECLAIM"};
    static const CategoryName kReclaimable{"RECLAIMABLE"};
    const SimState* sim = ctx.sim;
    if (!sim || !has_category(kPatrolHelper)) {
        return nullptr;
    }
    // In formation (an attack-move always is: Moho makes its patrol task so)
    // a commander or support commander leaves the helping to the others.
    const bool in_formation = cmd.type == CommandType::AggressiveMove || !cmd.formation.empty();
    if (in_formation && (has_category(kCommand) || has_category(kSacu))) {
        return nullptr;
    }
    const ArmyBrain* brain = army() >= 0 ? sim->army_at(static_cast<size_t>(army())) : nullptr;
    const bool energy_full = brain && nearly_full(brain->economy().energy);
    const bool mass_full = brain && nearly_full(brain->economy().mass);
    const bool reclaims = has_category(kReclaim);
    const PatrolBox box = patrol_box(*this, cmd);
    Entity* best = nullptr;
    f32 best_dist2 = 0;
    for (const u32 id : box.collect(ctx.registry)) {
        Entity* e = ctx.registry.find(id);
        if (!e || e->destroyed() || !box.contains(e->position())) {
            continue;
        }
        f32 weight = 1.0f;
        if (!e->is_unit() || static_cast<const Unit&>(*e).is_dying()) {
            if (!e->is_prop() || !reclaims) {
                continue;
            }
            const auto& prop = static_cast<const Prop&>(*e);
            if (!prop.reclaimable_category) {
                continue;
            }
            if ((energy_full || mass_full) && (energy_full || prop.reclaim_energy_max <= 0) &&
                (mass_full || prop.reclaim_mass_max <= 0)) {
                continue;
            }
            const auto& claimed = cmd.patrol_claimed;
            if (std::find(claimed.begin(), claimed.end(), id) != claimed.end()) {
                continue;
            }
        } else {
            const auto& other = static_cast<const Unit&>(*e);
            if (&other == this || other.velocity().x != 0 || other.velocity().y != 0 ||
                other.velocity().z != 0 || other.is_air_unit() ||
                sim->is_neutral(army(), other.army())) {
                continue;
            }
            if (sim->is_enemy(army(), other.army())) {
                if (!other.has_category(kReclaimable)) {
                    continue;
                }
                weight = 0.5f;
            } else {
                if (!energy_full || !mass_full || other.health() >= other.max_health() * 0.9f ||
                    other.has_unit_state("BeingReclaimed")) {
                    continue;
                }
                weight = 2.0f;
            }
        }
        if (off_map(*sim, e->position(), 1.0f) || outside_no_rush(*sim, brain, e->position())) {
            continue;
        }
        const f32 dx = position().x - e->position().x;
        const f32 dy = position().y - e->position().y;
        const f32 dz = position().z - e->position().z;
        const f32 dist2 = (dx * dx + dy * dy + dz * dz) * weight;
        if (!best || dist2 < best_dist2) {
            best = e;
            best_dist2 = dist2;
        }
    }
    return best;
}

} // namespace osc::sim
