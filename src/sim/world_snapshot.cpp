#include "sim/world_snapshot.hpp"

#include "sim/army_brain.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace osc::sim {

namespace {

Vector3 lerp(const Vector3& a, const Vector3& b, f32 t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

/// Normalized lerp along the shorter arc (q and -q are the same rotation).
Quaternion nlerp(const Quaternion& a, const Quaternion& b, f32 t) {
    const f32 dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    const f32 s = dot < 0.0f ? -1.0f : 1.0f;
    Quaternion r{a.x + (s * b.x - a.x) * t, a.y + (s * b.y - a.y) * t,
                 a.z + (s * b.z - a.z) * t, a.w + (s * b.w - a.w) * t};
    const f32 len = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (len > 1e-8f) {
        r.x /= len;
        r.y /= len;
        r.z /= len;
        r.w /= len;
    }
    return r;
}

IconClass icon_class(const Unit& u) {
    if (u.has_category("COMMAND")) return IconClass::Commander;
    if (u.has_category("ENGINEER") || u.has_category("CONSTRUCTION")) return IconClass::Engineer;
    if (u.has_category("STRUCTURE")) return IconClass::Structure;
    if (u.has_category("AIR")) return IconClass::Air;
    if (u.has_category("NAVAL")) return IconClass::Naval;
    if (u.has_category("LAND")) return IconClass::Land;
    return IconClass::Generic;
}

void capture_unit(const Unit& u, EntityRecord& r, WorldSnapshot& out) {
    r.unit_id = u.unit_id();
    r.icon = icon_class(u);
    r.footprint_size_x = u.footprint_size_x();
    r.is_being_built = u.is_being_built();
    r.build_target_id = u.build_target_id();
    r.reclaim_target_id = u.reclaim_target_id();
    r.repair_target_id = u.repair_target_id();
    r.capture_target_id = u.capture_target_id();
    r.work_progress = u.work_progress();
    r.vet_level = u.vet_level();
    r.cargo_count = static_cast<u32>(u.cargo_ids().size());
    r.nuke_silo_ammo = u.nuke_silo_ammo();
    r.tactical_silo_ammo = u.tactical_silo_ammo();
    r.weapon_count = u.weapon_count();

    const auto& pose = u.animated_bone_matrices();
    if (!pose.empty()) {
        r.bone_offset = static_cast<u32>(out.bones.size());
        r.bone_count = static_cast<u32>(pose.size());
        out.bones.insert(out.bones.end(), pose.begin(), pose.end());
    }

    r.command_offset = static_cast<u32>(out.commands.size());
    for (const auto& c : u.command_queue())
        out.commands.push_back({c.type, c.target_id, c.target_pos});
    r.command_count = static_cast<u32>(out.commands.size()) - r.command_offset;

    // Only what can be drawn: a ring needs the intel on and a radius.
    r.intel_offset = static_cast<u32>(out.intel.size());
    for (const auto& [type, state] : u.intel_states())
        if (state.enabled && state.radius >= 1.0f) out.intel.push_back({type, state.radius});
    r.intel_count = static_cast<u32>(out.intel.size()) - r.intel_offset;

    r.adjacent_offset = static_cast<u32>(out.adjacent.size());
    out.adjacent.insert(out.adjacent.end(), u.adjacent_unit_ids().begin(),
                        u.adjacent_unit_ids().end());
    r.adjacent_count = static_cast<u32>(out.adjacent.size()) - r.adjacent_offset;
}

} // namespace

const EntityRecord* WorldSnapshot::find(u32 id) const {
    auto it = std::lower_bound(entities.begin(), entities.end(), id,
                               [](const EntityRecord& e, u32 v) { return e.id < v; });
    return it != entities.end() && it->id == id ? &*it : nullptr;
}

void WorldSnapshot::clear() {
    tick = 0;
    entities.clear();
    bones.clear();
    commands.clear();
    intel.clear();
    adjacent.clear();
    effects.clear();
    armies.clear();
    visibility.reset();
    player_result = 0;
}

void capture_world(const SimState& sim, WorldSnapshot& out) {
    out.tick = sim.tick_count();
    out.entities.clear();
    out.bones.clear();
    out.commands.clear();
    out.intel.clear();
    out.adjacent.clear();
    out.effects.clear();
    out.armies.clear();

    sim.entity_registry().for_each([&](const Entity& e) {
        if (e.destroyed()) return;
        EntityRecord& r = out.entities.emplace_back();
        r.id = e.entity_id();
        r.position = e.position();
        r.orientation = e.orientation();
        r.beam_end = e.beam_endpoint();
        r.snap_serial = e.snap_serial();
        r.is_unit = e.is_unit();
        r.is_prop = e.is_prop();
        r.is_projectile = e.is_projectile();
        r.is_shield = e.is_shield();
        r.is_collision_beam = e.is_collision_beam();
        r.beam_enabled = e.beam_enabled();
        r.is_wreckage = e.is_wreckage();
        r.army = e.army();
        r.blueprint_id = e.blueprint_id();
        r.mesh_override = e.mesh_override();
        r.scale_x = e.scale_x();
        r.scale_y = e.scale_y();
        r.scale_z = e.scale_z();
        r.fraction_complete = e.fraction_complete();
        r.health = e.health();
        r.max_health = e.max_health();
        r.custom_name = e.custom_name();
        if (e.is_unit()) capture_unit(static_cast<const Unit&>(e), r, out);
        if (e.is_shield()) {
            const auto& s = static_cast<const Shield&>(e);
            r.shield_owner_id = s.owner_id;
            r.shield_on = s.is_on;
            r.shield_size = s.size;
        }
    });
    // The registry walks in id order, which find()'s binary search relies on.
    assert(
        std::is_sorted(out.entities.begin(), out.entities.end(),
                       [](const EntityRecord& a, const EntityRecord& b) { return a.id < b.id; }));

    for (const auto& fx : sim.effect_registry().all()) {
        if (!fx || fx->destroyed()) continue;
        EffectRecord& r = out.effects.emplace_back();
        r.id = fx->id();
        r.type = fx->type();
        r.blueprint_path = fx->blueprint_path();
        r.entity_id = fx->entity_id();
        r.target_entity_id = fx->target_entity_id();
        r.offset_x = fx->offset_x();
        r.offset_y = fx->offset_y();
        r.offset_z = fx->offset_z();
        r.scale = fx->scale();
        r.army = fx->army();
        r.light_size = fx->light_size();
        r.thickness = static_cast<f32>(fx->get_param("THICKNESS"));
        r.length = static_cast<f32>(fx->get_param("LENGTH"));
    }

    for (size_t i = 0; i < sim.army_count(); ++i) {
        ArmyRecord& a = out.armies.emplace_back();
        const ArmyBrain* brain = sim.army_at(i);
        if (!brain) continue;
        a.valid = true;
        a.has_color = brain->has_color();
        a.r = brain->color_r();
        a.g = brain->color_g();
        a.b = brain->color_b();
        const auto& econ = brain->economy();
        a.mass = {econ.mass.stored, econ.mass.max_storage, econ.mass.income, econ.mass.requested};
        a.energy = {econ.energy.stored, econ.energy.max_storage, econ.energy.income,
                    econ.energy.requested};
        a.mass_efficiency = brain->mass_efficiency();
        a.energy_efficiency = brain->energy_efficiency();
    }

    if (const auto* grid = sim.visibility_grid()) out.visibility = *grid;
    else out.visibility.reset();
    out.player_result = sim.player_result();
}

std::vector<std::string> world_blueprints(const SimState& sim) {
    std::vector<std::string> ids;
    sim.entity_registry().for_each([&](const Entity& e) {
        if (e.destroyed() || e.blueprint_id().empty()) return;
        if (e.is_unit() || e.is_prop() || e.is_projectile()) ids.push_back(e.blueprint_id());
    });
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

void WorldHistory::capture(const SimState& sim) {
    cur_ = 1 - cur_;
    capture_world(sim, snaps_[cur_]);
    ++captured_;
    // The sim forgets its events once the tick is over: keep them until
    // the renderer shows them.
    for (const auto& d : sim.death_events())
        events_.deaths.push_back({d.x, d.y, d.z, d.scale, d.army});
    for (const auto& s : sim.camera_shake_events())
        events_.shakes.push_back({s.x, s.z, s.radius, s.max_shake, s.min_shake});
}

void WorldHistory::clear() {
    for (auto& s : snaps_) s.clear();
    captured_ = 0;
    events_.clear();
}

FrameView::Pair FrameView::lookup(u32 id) const {
    if (!cur_) return {};
    const EntityRecord* to = cur_->find(id);
    if (!to) return {};
    const EntityRecord* from = prev_ ? prev_->find(id) : nullptr;
    if (from && from->snap_serial != to->snap_serial) from = nullptr;
    return {from, to};
}

FrameView::Pair FrameView::pair_for(const EntityRecord& e) const {
    const EntityRecord* from = prev_ ? prev_->find(e.id) : nullptr;
    if (from && from->snap_serial != e.snap_serial) from = nullptr;
    return {from, &e};
}

Vector3 FrameView::position(const EntityRecord& e) const {
    const Pair p = pair_for(e);
    return p.from ? lerp(p.from->position, e.position, alpha_) : e.position;
}

Quaternion FrameView::orientation(const EntityRecord& e) const {
    const Pair p = pair_for(e);
    return p.from ? nlerp(p.from->orientation, e.orientation, alpha_) : e.orientation;
}

Vector3 FrameView::beam_end(const EntityRecord& e) const {
    const Pair p = pair_for(e);
    return p.from ? lerp(p.from->beam_end, e.beam_end, alpha_) : e.beam_end;
}

Vector3 FrameView::position(const Entity& e) const {
    const Pair p = lookup(e.entity_id());
    if (!p.to) return e.position();
    return position(*p.to);
}

Quaternion FrameView::orientation(const Entity& e) const {
    const Pair p = lookup(e.entity_id());
    if (!p.to) return e.orientation();
    return orientation(*p.to);
}

Vector3 FrameView::beam_end(const Entity& e) const {
    const Pair p = lookup(e.entity_id());
    if (!p.to) return e.beam_endpoint();
    return beam_end(*p.to);
}

bool FrameView::bones(u32 id, std::vector<BoneMatrix>& out) const {
    const Pair p = lookup(id);
    if (!p.to || p.to->bone_count == 0) return false;
    const auto to = cur_->bones_of(*p.to);
    out.assign(to.begin(), to.end());
    // A changed bone count is a different skeleton: nothing to blend from.
    if (!p.from || p.from->bone_count != p.to->bone_count) return true;
    const auto from = prev_->bones_of(*p.from);
    for (size_t b = 0; b < out.size(); ++b)
        for (size_t i = 0; i < 16; ++i)
            out[b][i] = from[b][i] + (to[b][i] - from[b][i]) * alpha_;
    return true;
}

} // namespace osc::sim
