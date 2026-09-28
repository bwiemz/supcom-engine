#include "sim/world_snapshot.hpp"

#include "sim/army_brain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/ieffect.hpp"
#include "sim/prop.hpp"
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
    static const CategoryName kCommand{"COMMAND"}, kEngineer{"ENGINEER"},
        kConstruction{"CONSTRUCTION"}, kStructure{"STRUCTURE"}, kAir{"AIR"}, kNaval{"NAVAL"},
        kLand{"LAND"};
    if (u.has_category(kCommand)) return IconClass::Commander;
    if (u.has_category(kEngineer) || u.has_category(kConstruction)) return IconClass::Engineer;
    if (u.has_category(kStructure)) return IconClass::Structure;
    if (u.has_category(kAir)) return IconClass::Air;
    if (u.has_category(kNaval)) return IconClass::Naval;
    if (u.has_category(kLand)) return IconClass::Land;
    return IconClass::Generic;
}

/// Where a beam meets bone `bone` of `e` (M214a): a unit's bone as posed; a
/// collision beam's start (bone 0) or far end (bone 1, what SetBeamFx ties
/// a beam's end to); else where the entity is.
Vector3 beam_point(const Entity& e, i32 bone) {
    if (e.is_collision_beam()) return bone == 1 ? e.beam_endpoint() : e.position();
    if (e.is_unit() && bone >= 0) return static_cast<const Unit&>(e).bone_world_position(bone);
    return e.position();
}

/// The way bone `bone` of `e` faces (its +Z): a unit's bone as posed, else
/// the entity's own facing.
Vector3 beam_axis(const Entity& e, i32 bone) {
    if (e.is_unit()) return static_cast<const Unit&>(e).bone_world_forward(bone);
    const Quaternion& q = e.orientation();
    return {2.0f * (q.x * q.z + q.w * q.y), 2.0f * (q.y * q.z - q.w * q.x),
            1.0f - 2.0f * (q.x * q.x + q.y * q.y)};
}

/// A beam effect's reach at capture (M214a).
void capture_beam(const SimState& sim, const IEffect& fx, EffectRecord& r) {
    const EntityRegistry& reg = sim.entity_registry();
    const Entity* from = fx.entity_id() ? reg.find(fx.entity_id()) : nullptr;
    if (!from || from->destroyed()) return;
    switch (fx.type()) {
    case EffectType::BEAM_ENTITY_TO_ENTITY: {
        const Entity* to = fx.target_entity_id() ? reg.find(fx.target_entity_id()) : nullptr;
        if (!to || to->destroyed()) return;
        r.beam = EffectRecord::BeamReach::Ends;
        r.beam_start = beam_point(*from, fx.bone_index());
        r.beam_end = beam_point(*to, fx.target_bone_index());
        return;
    }
    case EffectType::BEAM_EMITTER:
        // On a collision beam (a weapon's), from its start to its far end,
        // while it fires.
        if (from->is_collision_beam()) {
            if (!from->beam_enabled()) return;
            r.beam = EffectRecord::BeamReach::Ends;
            r.beam_start = beam_point(*from, 0);
            r.beam_end = beam_point(*from, 1);
            return;
        }
        [[fallthrough]];
    case EffectType::ATTACHED_EMITTER:
        r.beam = EffectRecord::BeamReach::Along;
        r.beam_start = beam_point(*from, fx.bone_index());
        r.beam_dir = beam_axis(*from, fx.bone_index());
        return;
    default: return;
    }
}

/// A trail's point at capture (M214b): its offset (OffsetEmitter, the
/// POSITION_* params) in its bone's frame, as Moho's
/// CEfxEmitter::InterpolatePosition places it; an entity that isn't a unit
/// has only its own frame.
void capture_anchor(const SimState& sim, const IEffect& fx, EffectRecord& r) {
    if (fx.type() != EffectType::TRAIL_EMITTER || !fx.entity_id()) return;
    const Entity* e = sim.entity_registry().find(fx.entity_id());
    if (!e || e->destroyed()) return;
    const Vector3 local{fx.offset_x(), fx.offset_y(), fx.offset_z()};
    r.anchored = true;
    if (e->is_unit()) {
        r.anchor = static_cast<const Unit&>(*e).bone_world_point(fx.bone_index(), local);
        return;
    }
    const Vector3 turned = quat_rotate(e->orientation(), local);
    r.anchor = {e->position().x + turned.x, e->position().y + turned.y, e->position().z + turned.z};
}

/// Each army's recon of `u` (M215d): the sim's (cloak and stealth counted),
/// less what the unit's layer hides from a sense, as Moho's GetNewReconFor
/// asks radar only above the water and sonar only in or under it.
void capture_recon(const SimState& sim, const Unit& u, EntityRecord& r) {
    const auto* recon = sim.entity_recon(u.entity_id());
    if (!recon) return;
    const std::string& layer = u.layer();
    const bool under = layer == "Sub" || layer == "Seabed";
    const bool wet = under || layer == "Water";
    static_assert(SimState::MAX_VIS_ARMIES <= 32, "an army's recon is a bit of a u32");
    for (u32 a = 0; a < SimState::MAX_VIS_ARMIES; ++a) {
        const SimState::EntityVisSnapshot& f = (*recon)[a];
        if (f.vision) r.los_now |= 1u << a;
        if (f.vision || f.omni || (f.radar && !under) || (f.sonar && wet)) r.detected |= 1u << a;
    }
}

void capture_unit(const Unit& u, EntityRecord& r, WorldSnapshot& out) {
    r.unit_id = u.unit_id();
    r.icon = icon_class(u);
    r.is_mobile = u.is_mobile();
    r.footprint_size_x = u.footprint_size_x();
    r.footprint_size_z = u.footprint_size_z();
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

    r.auto_mode = u.auto_mode();
    r.repeat_queue = u.repeat_queue();
    r.overcharge_paused = u.overcharge_paused();
    r.auto_surface = u.auto_surface_mode();
    r.stunned = u.is_stunned();
    r.is_dying = u.is_dying();
    r.fuel_ratio = u.fuel_ratio();
    r.shield_ratio = u.shield_ratio();
    r.build_rate = u.build_rate();
    r.creator_id = u.creator_id();
    const auto& econ = u.economy();
    r.mass_produced = static_cast<f32>(econ.production_active ? econ.production_mass : 0.0);
    r.energy_produced = static_cast<f32>(econ.production_active ? econ.production_energy : 0.0);
    r.mass_consumed = static_cast<f32>(econ.consumption_active ? econ.consumption_mass : 0.0);
    r.energy_consumed = static_cast<f32>(econ.consumption_active ? econ.consumption_energy : 0.0);
    r.mass_requested = static_cast<f32>(econ.consumption_mass);
    r.energy_requested = static_cast<f32>(econ.consumption_energy);
    r.nuke_silo_max = u.silo_max_storage(true);
    r.tactical_silo_max = u.silo_max_storage(false);
    r.nuke_silo_builds = u.silo_build_count(true);
    r.tactical_silo_builds = u.silo_build_count(false);

    r.hidden_bones = u.hidden_bone_mask();
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
    r.rally_offset = static_cast<u32>(out.commands.size());
    for (const auto& c : u.rally_orders())
        out.commands.push_back({c.type, c.target_id, c.target_pos});
    r.rally_count = static_cast<u32>(out.commands.size()) - r.rally_offset;

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
        if (e.is_unit()) {
            capture_unit(static_cast<const Unit&>(e), r, out);
            capture_recon(sim, static_cast<const Unit&>(e), r);
        }
        if (e.is_prop()) {
            const auto& pose = static_cast<const Prop&>(e).pose; // TryCopyPose
            if (!pose.empty()) {
                r.bone_offset = static_cast<u32>(out.bones.size());
                r.bone_count = static_cast<u32>(pose.size());
                out.bones.insert(out.bones.end(), pose.begin(), pose.end());
            }
        }
        if (e.is_shield()) {
            const auto& s = static_cast<const Shield&>(e);
            r.shield_owner_id = s.owner_id;
            // Retail's shield is up while it has its mesh (shield.lua's
            // CreateShieldMesh and RemoveShield); its TurnOn is a script
            // state, never the engine's (M215b).
            r.shield_on = s.is_on || !e.mesh_override().empty();
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
        capture_beam(sim, *fx, r);
        capture_anchor(sim, *fx, r);
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
        for (i32 j = 0; j < static_cast<i32>(sim.army_count()) && j < 32; ++j)
            if (j != static_cast<i32>(i) && brain->is_ally(j)) a.allies |= 1u << j;
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
