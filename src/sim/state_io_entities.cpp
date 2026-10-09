// The entities' state, and what they own (M208c-b; see state_io.hpp). Each
// class's fields in declaration order; a comment names each one left out,
// and why.

#include "sim/state_io.hpp"

#include "sim/anim_cache.hpp"
#include "sim/bone_cache.hpp"
#include "sim/category_expr.hpp"
#include "sim/collision.hpp"
#include "sim/manipulator.hpp"
#include "sim/navigator.hpp"
#include "sim/projectile.hpp"
#include "sim/prop.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/transport_slots.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

#include <algorithm>

namespace osc::sim {

namespace {

// A snapshot is read only by the build that wrote it, so enums go as their
// values.
template <typename E> void enum8(StateWriter& w, E e) {
    w.u8v(static_cast<u8>(e));
}
template <typename E> E enum8(StateReader& r) {
    return static_cast<E>(r.u8v());
}

void save_ids(StateWriter& w, const std::vector<u32>& ids) {
    w.size(ids.size());
    for (u32 id : ids) w.u32v(id);
}
std::vector<u32> load_ids(StateReader& r) {
    std::vector<u32> ids(r.size(4));
    for (u32& id : ids) id = r.u32v();
    return ids;
}

template <typename Set> void save_strings(StateWriter& w, const Set& set) {
    std::vector<std::string> sorted(set.begin(), set.end());
    std::sort(sorted.begin(), sorted.end());
    w.size(sorted.size());
    for (const auto& s : sorted) w.str(s);
}
template <typename Set> Set load_strings(StateReader& r) {
    Set set;
    const size_t n = r.size(4);
    for (size_t i = 0; i < n; ++i) set.insert(r.str());
    return set;
}

template <typename Map> void save_str_f64(StateWriter& w, const Map& map) {
    std::vector<std::pair<std::string, f64>> sorted(map.begin(), map.end());
    std::sort(sorted.begin(), sorted.end());
    w.size(sorted.size());
    for (const auto& [k, v] : sorted) {
        w.str(k);
        w.f64v(v);
    }
}
template <typename Map> Map load_str_f64(StateReader& r) {
    Map map;
    const size_t n = r.size(12);
    for (size_t i = 0; i < n; ++i) {
        std::string k = r.str();
        map[k] = r.f64v();
    }
    return map;
}

void save_i32_set(StateWriter& w, const std::unordered_set<i32>& set) {
    std::vector<i32> sorted(set.begin(), set.end());
    std::sort(sorted.begin(), sorted.end());
    w.size(sorted.size());
    for (i32 v : sorted) w.i32v(v);
}
std::unordered_set<i32> load_i32_set(StateReader& r) {
    std::unordered_set<i32> set;
    const size_t n = r.size(4);
    for (size_t i = 0; i < n; ++i) set.insert(r.i32v());
    return set;
}

void save_i32s(StateWriter& w, const std::vector<i32>& v) {
    w.size(v.size());
    for (i32 x : v) w.i32v(x);
}
std::vector<i32> load_i32s(StateReader& r) {
    std::vector<i32> v(r.size(4));
    for (i32& x : v) x = r.i32v();
    return v;
}

void save_poses(StateWriter& w, const std::vector<BonePose>& poses) {
    w.size(poses.size());
    for (const BonePose& p : poses) {
        w.vec3(p.position);
        w.quat(p.rotation);
    }
}
std::vector<BonePose> load_poses(StateReader& r) {
    std::vector<BonePose> poses(r.size(28));
    for (BonePose& p : poses) {
        p.position = r.vec3();
        p.rotation = r.quat();
    }
    return poses;
}

void save_bytes(StateWriter& w, const std::vector<u8>& v) {
    w.size(v.size());
    if (!v.empty()) w.raw(v.data(), v.size());
}
std::vector<u8> load_bytes(StateReader& r) {
    std::vector<u8> v(r.size());
    if (!v.empty()) r.raw(v.data(), v.size());
    return v;
}

void save_shape(StateWriter& w, const CollisionShape& s) {
    enum8(w, s.type);
    w.f32v(s.cx);
    w.f32v(s.cy);
    w.f32v(s.cz);
    w.f32v(s.sx);
    w.f32v(s.sy);
    w.f32v(s.sz);
}
CollisionShape load_shape(StateReader& r) {
    CollisionShape s;
    s.type = enum8<CollisionShapeType>(r);
    s.cx = r.f32v();
    s.cy = r.f32v();
    s.cz = r.f32v();
    s.sx = r.f32v();
    s.sy = r.f32v();
    s.sz = r.f32v();
    return s;
}

// A manipulator's kind, in the stream.
enum class ManipKind : u8 {
    Rotate = 1,
    Anim,
    Slide,
    Aim,
    Slaver,
    CollisionDetector,
    FootPlant,
    Storage,
    Thrust,
    BoneEntity,
};

} // namespace

// ---------------------------------------------------------------- Entity

void StateIO::save(StateWriter& w, const Entity& e) {
    w.tag("ENTY");
    w.u32v(e.entity_id_);
    w.i32v(e.army_);
    w.vec3(e.position_);
    w.quat(e.orientation_);
    w.u32v(e.snap_serial_);
    w.u32v(e.followed_parent_snap_);
    w.f32v(e.health_);
    w.f32v(e.max_health_);
    w.f32v(e.regen_rate_);
    w.f32v(e.fraction_complete_);
    w.f32v(e.footprint_size_x_);
    w.f32v(e.footprint_size_z_);
    w.b(e.destroyed_);
    w.str(e.blueprint_id_);
    w.i32v(e.lua_table_ref_);
    w.size(e.ambient_sounds_.size());
    for (const auto& a : e.ambient_sounds_) {
        w.str(a.name);
        w.str(a.bank);
        w.str(a.cue);
        w.str(a.lod_cutoff);
    }
    // bone_data_: from the bone cache (load_unit / load_prop)
    w.b(e.do_not_target_);
    w.b(e.reclaimable_);
    w.str(e.custom_name_);
    w.str(e.strategic_underlay_);
    w.f32v(e.scale_x_);
    w.f32v(e.scale_y_);
    w.f32v(e.scale_z_);
    enum8(w, e.viz_allies_);
    enum8(w, e.viz_enemies_);
    enum8(w, e.viz_focus_player_);
    enum8(w, e.viz_neutrals_);
    save_shape(w, e.collision_shape_);
    save_shape(w, e.default_collision_shape_);
    w.str(e.mesh_override_); // (mesh_changes_ isn't: the renderer's, whose meshes a load remakes)
    w.b(e.unselectable_);
    w.b(e.is_wreckage_);
    w.u32v(e.parent_entity_id_);
    w.i32v(e.parent_bone_);
    w.i32v(e.attached_bone_);
    w.vec3(e.parent_offset_);
    w.size(e.children_.size());
    for (const ChildAttachment& c : e.children_) {
        w.u32v(c.entity_id);
        w.i32v(c.bone);
    }
    // grid_cell_x_, grid_cell_z_, registry_: the registry's, set as it takes the entity
    w.b(e.script_destroy_notified_);
    w.b(e.script_owns_death_);
    w.b(e.is_collision_beam_);
    w.b(e.beam_enabled_);
    w.vec3(e.beam_endpoint_);
    w.u32v(e.beam_launcher_id_);
    w.i32v(e.beam_fx_ref_);
    w.i32v(e.beam_setup_.weapon);
    w.i32v(e.beam_setup_.muzzle_bone);
    w.u32v(e.beam_setup_.check_interval);
    w.u32v(e.beam_setup_.check_clock);
    w.f32v(e.beam_setup_.length);
    w.f32v(e.beam_setup_.reached);
}

void StateIO::load(StateReader& r, Entity& e) {
    r.tag("ENTY");
    e.entity_id_ = r.u32v();
    e.army_ = r.i32v();
    e.position_ = r.vec3();
    e.orientation_ = r.quat();
    e.snap_serial_ = r.u32v();
    e.followed_parent_snap_ = r.u32v();
    e.health_ = r.f32v();
    e.max_health_ = r.f32v();
    e.regen_rate_ = r.f32v();
    e.fraction_complete_ = r.f32v();
    e.footprint_size_x_ = r.f32v();
    e.footprint_size_z_ = r.f32v();
    e.destroyed_ = r.b();
    e.blueprint_id_ = r.str();
    e.lua_table_ref_ = r.i32v();
    e.ambient_sounds_.resize(r.size(16));
    for (auto& a : e.ambient_sounds_) {
        a.name = r.str();
        a.bank = r.str();
        a.cue = r.str();
        a.lod_cutoff = r.str();
    }
    e.do_not_target_ = r.b();
    e.reclaimable_ = r.b();
    e.custom_name_ = r.str();
    e.strategic_underlay_ = r.str();
    e.scale_x_ = r.f32v();
    e.scale_y_ = r.f32v();
    e.scale_z_ = r.f32v();
    e.viz_allies_ = enum8<VizMode>(r);
    e.viz_enemies_ = enum8<VizMode>(r);
    e.viz_focus_player_ = enum8<VizMode>(r);
    e.viz_neutrals_ = enum8<VizMode>(r);
    e.collision_shape_ = load_shape(r);
    e.shape_reach_ = collision_reach(e.collision_shape_); // derived, not saved
    e.default_collision_shape_ = load_shape(r);
    e.mesh_override_ = r.str();
    e.unselectable_ = r.b();
    e.is_wreckage_ = r.b();
    e.parent_entity_id_ = r.u32v();
    e.parent_bone_ = r.i32v();
    e.attached_bone_ = r.i32v();
    e.parent_offset_ = r.vec3();
    e.children_.resize(r.size(8));
    for (ChildAttachment& c : e.children_) {
        c.entity_id = r.u32v();
        c.bone = r.i32v();
    }
    e.script_destroy_notified_ = r.b();
    e.script_owns_death_ = r.b();
    e.is_collision_beam_ = r.b();
    e.beam_enabled_ = r.b();
    e.beam_endpoint_ = r.vec3();
    e.beam_launcher_id_ = r.u32v();
    e.beam_fx_ref_ = r.i32v();
    e.beam_setup_.weapon = r.i32v();
    e.beam_setup_.muzzle_bone = r.i32v();
    e.beam_setup_.check_interval = r.u32v();
    e.beam_setup_.check_clock = r.u32v();
    e.beam_setup_.length = r.f32v();
    e.beam_setup_.reached = r.f32v();
}

// --------------------------------------------------------------- Commands

void StateIO::save(StateWriter& w, const UnitCommand& c) {
    enum8(w, c.type);
    w.vec3(c.target_pos);
    w.u32v(c.target_id);
    w.str(c.blueprint_id);
    w.u32v(c.command_id);
    w.str(c.formation);
    w.b(c.has_facing);
    w.f32v(c.facing);
    w.b(c.form_move);
    w.f32v(c.speed_cap);
    w.b(c.formed);
    save_ids(w, c.unload_ids);
    w.str(c.script_args);
    w.u32v(c.task_serial);
    w.b(c.factory);
    w.b(c.launched);
    w.b(c.started);
    w.b(c.approached);
    w.b(c.engaged);
    w.i32v(c.facing_clock);
    w.f32v(c.site_skirt_x);
    w.f32v(c.site_skirt_z);
    w.b(c.site_cleared);
    w.u32v(c.rebuild_wreck_id);
    w.f32v(c.rebuild_bonus);
    w.u32v(c.clearing_prop_id);
    w.b(c.clearing_approached);
    w.b(c.in_band);
    w.u32v(c.beacon_id);
    w.u32v(c.assigned_id);
    w.i32v(c.rolloff_wait);
    w.i32v(c.cap_wait);
    w.i32v(c.count);
    w.i32v(c.max_count);
    save_ids(w, c.launch_queue);
    w.i32v(c.launch_wait);
    enum8(w, c.dock_phase);
    w.i32v(c.dock_wait);
    w.b(c.from_patrol);
    w.i32v(c.patrol_scan);
    save_ids(w, c.patrol_claimed);
    w.vec3(c.patrol_from);
    w.b(c.begun);
    w.b(c.from_guard);
    w.b(c.guard_returning);
    w.b(c.leash_armed);
    w.u32v(c.leash_anchor_id);
    w.vec3(c.leash_anchor_pos);
}

void StateIO::load(StateReader& r, UnitCommand& c) {
    c.type = enum8<CommandType>(r);
    c.target_pos = r.vec3();
    c.target_id = r.u32v();
    c.blueprint_id = r.str();
    c.command_id = r.u32v();
    c.formation = r.str();
    c.has_facing = r.b();
    c.facing = r.f32v();
    c.form_move = r.b();
    c.speed_cap = r.f32v();
    c.formed = r.b();
    c.unload_ids = load_ids(r);
    c.script_args = r.str();
    c.task_serial = r.u32v();
    c.factory = r.b();
    c.launched = r.b();
    c.started = r.b();
    c.approached = r.b();
    c.engaged = r.b();
    c.facing_clock = r.i32v();
    c.site_skirt_x = r.f32v();
    c.site_skirt_z = r.f32v();
    c.site_cleared = r.b();
    c.rebuild_wreck_id = r.u32v();
    c.rebuild_bonus = r.f32v();
    c.clearing_prop_id = r.u32v();
    c.clearing_approached = r.b();
    c.in_band = r.b();
    c.beacon_id = r.u32v();
    c.assigned_id = r.u32v();
    c.rolloff_wait = r.i32v();
    c.cap_wait = r.i32v();
    c.count = r.i32v();
    c.max_count = r.i32v();
    c.launch_queue = load_ids(r);
    c.launch_wait = r.i32v();
    c.dock_phase = enum8<DockPhase>(r);
    c.dock_wait = r.i32v();
    c.from_patrol = r.b();
    c.patrol_scan = r.i32v();
    c.patrol_claimed = load_ids(r);
    c.patrol_from = r.vec3();
    c.begun = r.b();
    c.from_guard = r.b();
    c.guard_returning = r.b();
    c.leash_armed = r.b();
    c.leash_anchor_id = r.u32v();
    c.leash_anchor_pos = r.vec3();
}

// ------------------------------------------------------------- Navigator

void StateIO::save(StateWriter& w, const Navigator& n) {
    // sim_: set by the unit's load
    w.vec3(n.goal_);
    enum8(w, n.status_);
    w.b(n.speed_through_goal_);
    w.size(n.waypoints_.size());
    for (const Vector3& p : n.waypoints_) w.vec3(p);
    w.u64v(n.waypoint_index_);
    w.f32v(n.best_dist_);
    w.i32v(n.stalled_);
    w.u32v(n.collision_.other);
    w.i32v(n.collision_.tick);
    w.vec3(n.collision_.at);
    w.i32v(n.next_check_);
    w.b(n.sidestep_);
    w.u64v(n.sidestep_index_);
    w.i32v(n.hold_ticks_);
    w.u32v(n.held_for_);
    w.b(n.has_failed_request_);
    w.vec3(n.failed_goal_);
    w.vec3(n.failed_from_);
    w.i32v(n.suppressed_requests_);
    // Moho pathing (roadmap item 4c-2c)
    save(w, n.moho_);
    w.b(n.moho_pending_);
    w.b(n.moho_active_);
    w.b(n.through_target_);
    w.i32v(n.moho_waypoint_cell_.x);
    w.i32v(n.moho_waypoint_cell_.z);
    w.vec3(n.last_pos_);
    w.str(n.moho_layer_);
    w.f32v(n.moho_draft_);
    w.b(n.moho_amphibious_);
}

void StateIO::load(StateReader& r, Navigator& n, SimState& sim, i32 army) {
    n.goal_ = r.vec3();
    n.status_ = enum8<Navigator::Status>(r);
    n.speed_through_goal_ = r.b();
    n.waypoints_.resize(r.size(12));
    for (Vector3& p : n.waypoints_) p = r.vec3();
    n.waypoint_index_ = static_cast<size_t>(r.u64v());
    n.best_dist_ = r.f32v();
    n.stalled_ = r.i32v();
    n.collision_.other = r.u32v();
    n.collision_.tick = r.i32v();
    n.collision_.at = r.vec3();
    n.next_check_ = r.i32v();
    n.sidestep_ = r.b();
    n.sidestep_index_ = static_cast<size_t>(r.u64v());
    n.hold_ticks_ = r.i32v();
    n.held_for_ = r.u32v();
    n.has_failed_request_ = r.b();
    n.failed_goal_ = r.vec3();
    n.failed_from_ = r.vec3();
    n.suppressed_requests_ = r.i32v();
    load(r, n.moho_, sim, army);
    n.moho_pending_ = r.b();
    n.moho_active_ = r.b();
    n.through_target_ = r.b();
    n.moho_waypoint_cell_.x = r.i32v();
    n.moho_waypoint_cell_.z = r.i32v();
    n.last_pos_ = r.vec3();
    n.moho_layer_ = r.str();
    n.moho_draft_ = r.f32v();
    n.moho_amphibious_ = r.b();
}

// ------------------------------------------------------ Category rules

void StateIO::save(StateWriter& w, const CategoryExpr& c) {
    enum8(w, c.op_);
    w.str(c.name_);
    w.size(c.operands_.size());
    for (const CategoryExpr& o : c.operands_) save(w, o);
}

void StateIO::load(StateReader& r, CategoryExpr& c, int depth) {
    if (depth > 64) return r.fail("a category rule nested too deep");
    c.op_ = enum8<CategoryExpr::Op>(r);
    c.name_ = r.str();
    c.operands_.resize(r.size(5));
    for (CategoryExpr& o : c.operands_) load(r, o, depth + 1);
}

// ----------------------------------------------------------------- Weapon

void StateIO::save(StateWriter& w, const Weapon& wp) {
    w.tag("WEAP");
    w.str(wp.label);
    w.f32v(wp.max_range);
    w.f32v(wp.min_range);
    w.f32v(wp.rate_of_fire);
    w.f32v(wp.damage);
    w.f32v(wp.damage_radius);
    w.str(wp.damage_type);
    w.f32v(wp.muzzle_velocity);
    w.f32v(wp.projectile_lifetime);
    w.f32v(wp.projectile_lifetime_multiplier);
    enum8(w, wp.ballistic_arc);
    w.b(wp.lead_target);
    w.f32v(wp.muzzle_velocity_reduce_distance);
    w.f32v(wp.muzzle_velocity_random);
    w.b(wp.use_firing_solution);
    w.b(wp.projectile_physics.has_value());
    if (wp.projectile_physics) {
        w.b(wp.projectile_physics->track_target);
        w.b(wp.projectile_physics->use_gravity);
        w.f32v(wp.projectile_physics->max_speed);
        w.b(wp.projectile_physics->straight_down);
    }
    w.b(wp.fire_on_death);
    w.b(wp.dummy);
    w.b(wp.manual_fire);
    w.b(wp.counted_projectile);
    w.b(wp.nuke_weapon);
    w.i32v(wp.max_projectile_storage);
    w.b(wp.targets_projectiles);
    w.b(wp.overcharge);
    w.b(wp.beam);
    w.f32v(wp.max_beam_length);
    w.str(wp.muzzle_bone_name);
    w.f32v(wp.firing_randomness);
    w.u8v(wp.fire_target_layer_caps);
    w.f32v(wp.max_height_diff);
    w.f32v(wp.firing_tolerance);
    w.f32v(wp.tracking_radius);
    w.f32v(wp.heading_arc_center);
    w.f32v(wp.heading_arc_range);
    w.b(wp.slaved_to_body);
    w.f32v(wp.slaved_arc_range);
    w.str(wp.projectile_bp_id);
    w.str(wp.fire_control_label);
    w.b(wp.need_compute_bomb_drop);
    w.b(wp.auto_initiate_attack_command);
    w.f32v(wp.bomb_drop_threshold);
    w.size(wp.target_priorities.size());
    for (const CategoryExpr& c : wp.target_priorities) save(w, c);
    save(w, wp.restrict_disallow);
    save(w, wp.restrict_only_allow);
    w.b(wp.above_water_targets_only);
    w.b(wp.below_water_targets_only);
    w.b(wp.yaw_only_on_target);
    w.b(wp.above_water_fire_only);
    w.b(wp.always_recheck_target);
    w.u32v(wp.target_check_period);
    w.b(wp.cannot_attack_ground);
    w.i32v(wp.attack_ground_tries);
    w.i32v(wp.weapon_priorities_ref);
    w.i32v(wp.blueprint_ref);
    w.i32v(wp.lua_table_ref);
    w.b(wp.script_class);
    w.i32v(wp.weapon_index);
    w.u32v(wp.owner_entity_id);
    w.u32v(wp.target_entity_id);
    w.i32v(wp.aim_spot);
    w.u32v(wp.aim_spot_target);
    w.b(wp.has_ground_target);
    w.vec3(wp.ground_target);
    w.b(wp.ground_from_order);
    w.u32v(wp.shots_at_target);
    w.b(wp.last_order_point.has_value());
    w.vec3(wp.last_order_point.value_or(Vector3{}));
    w.b(wp.enabled);
    w.u32v(wp.fire_clock);
    w.u32v(wp.target_check_clock);
}

void StateIO::load(StateReader& r, Weapon& wp) {
    r.tag("WEAP");
    wp.label = r.str();
    wp.max_range = r.f32v();
    wp.min_range = r.f32v();
    wp.rate_of_fire = r.f32v();
    wp.damage = r.f32v();
    wp.damage_radius = r.f32v();
    wp.damage_type = r.str();
    wp.muzzle_velocity = r.f32v();
    wp.projectile_lifetime = r.f32v();
    wp.projectile_lifetime_multiplier = r.f32v();
    wp.ballistic_arc = enum8<Weapon::Arc>(r);
    wp.lead_target = r.b();
    wp.muzzle_velocity_reduce_distance = r.f32v();
    wp.muzzle_velocity_random = r.f32v();
    wp.use_firing_solution = r.b();
    wp.projectile_physics.reset();
    if (r.b()) {
        Weapon::ProjectilePhysics physics;
        physics.track_target = r.b();
        physics.use_gravity = r.b();
        physics.max_speed = r.f32v();
        physics.straight_down = r.b();
        wp.projectile_physics = physics;
    }
    wp.fire_on_death = r.b();
    wp.dummy = r.b();
    wp.manual_fire = r.b();
    wp.counted_projectile = r.b();
    wp.nuke_weapon = r.b();
    wp.max_projectile_storage = r.i32v();
    wp.targets_projectiles = r.b();
    wp.overcharge = r.b();
    wp.beam = r.b();
    wp.max_beam_length = r.f32v();
    wp.muzzle_bone_name = r.str();
    wp.firing_randomness = r.f32v();
    wp.fire_target_layer_caps = r.u8v();
    wp.max_height_diff = r.f32v();
    wp.firing_tolerance = r.f32v();
    wp.tracking_radius = r.f32v();
    wp.heading_arc_center = r.f32v();
    wp.heading_arc_range = r.f32v();
    wp.slaved_to_body = r.b();
    wp.slaved_arc_range = r.f32v();
    wp.projectile_bp_id = r.str();
    wp.fire_control_label = r.str();
    wp.need_compute_bomb_drop = r.b();
    wp.auto_initiate_attack_command = r.b();
    wp.bomb_drop_threshold = r.f32v();
    wp.target_priorities.resize(r.size(5));
    for (CategoryExpr& c : wp.target_priorities) load(r, c);
    load(r, wp.restrict_disallow);
    load(r, wp.restrict_only_allow);
    wp.above_water_targets_only = r.b();
    wp.below_water_targets_only = r.b();
    wp.yaw_only_on_target = r.b();
    wp.above_water_fire_only = r.b();
    wp.always_recheck_target = r.b();
    wp.target_check_period = r.u32v();
    wp.cannot_attack_ground = r.b();
    wp.attack_ground_tries = r.i32v();
    wp.weapon_priorities_ref = r.i32v();
    wp.blueprint_ref = r.i32v();
    wp.lua_table_ref = r.i32v();
    wp.script_class = r.b();
    wp.weapon_index = r.i32v();
    wp.owner_entity_id = r.u32v();
    wp.target_entity_id = r.u32v();
    wp.aim_spot = r.i32v();
    wp.aim_spot_target = r.u32v();
    wp.has_ground_target = r.b();
    wp.ground_target = r.vec3();
    wp.ground_from_order = r.b();
    wp.shots_at_target = r.u32v();
    const bool has_order_point = r.b();
    const Vector3 order_point = r.vec3();
    wp.last_order_point = has_order_point ? std::optional<Vector3>(order_point) : std::nullopt;
    wp.enabled = r.b();
    wp.fire_clock = r.u32v();
    wp.target_check_clock = r.u32v();
}

// ----------------------------------------------------------- Manipulators

void StateIO::save(StateWriter& w, const Manipulator& m) {
    w.tag("MANP");
    ManipKind kind{};
    if (dynamic_cast<const RotateManipulator*>(&m)) kind = ManipKind::Rotate;
    else if (dynamic_cast<const AnimManipulator*>(&m)) kind = ManipKind::Anim;
    else if (dynamic_cast<const SlideManipulator*>(&m)) kind = ManipKind::Slide;
    else if (dynamic_cast<const AimManipulator*>(&m)) kind = ManipKind::Aim;
    else if (dynamic_cast<const SlaverManipulator*>(&m)) kind = ManipKind::Slaver;
    else if (dynamic_cast<const CollisionDetectorManipulator*>(&m))
        kind = ManipKind::CollisionDetector;
    else if (dynamic_cast<const FootPlantManipulator*>(&m)) kind = ManipKind::FootPlant;
    else if (dynamic_cast<const StorageManipulator*>(&m)) kind = ManipKind::Storage;
    else if (dynamic_cast<const ThrustManipulator*>(&m)) kind = ManipKind::Thrust;
    else if (dynamic_cast<const BoneEntityManipulator*>(&m)) kind = ManipKind::BoneEntity;
    enum8(w, kind);
    // Waitable, then Manipulator (owner_: the unit loading it)
    w.i32v(m.waiting_thread_ref_);
    w.u64v(m.waiting_thread_serial_);
    w.i32v(m.bone_index_);
    w.i32v(m.precedence_);
    w.b(m.enabled_);
    w.b(m.destroyed_);
    w.i32v(m.lua_table_ref_);
    switch (kind) {
    case ManipKind::Rotate: {
        const auto& x = static_cast<const RotateManipulator&>(m);
        w.u8v(static_cast<u8>(x.axis_));
        w.f32v(x.current_angle_);
        w.f32v(x.goal_angle_);
        w.f32v(x.speed_);
        w.f32v(x.target_speed_);
        w.f32v(x.accel_);
        w.f32v(x.current_speed_);
        w.b(x.has_goal_);
        w.b(x.spin_down_);
        break;
    }
    case ManipKind::Anim: {
        const auto& x = static_cast<const AnimManipulator&>(m);
        w.str(x.current_anim_);
        w.f32v(x.rate_);
        w.f32v(x.fraction_);
        w.f32v(x.duration_);
        w.b(x.looping_);
        w.b(x.directional_);
        w.b(x.finished_);
        // sca_data_: from the animation cache, if the play found it
        w.b(x.sca_data_ != nullptr);
        save_i32s(w, x.sca_to_scm_map_);
        save_i32s(w, x.scm_to_sca_map_);
        save_i32_set(w, x.disabled_bones_);
        w.f32v(x.blend_time_);
        w.f32v(x.blend_remaining_);
        save_poses(w, x.last_);
        save_bytes(w, x.last_set_);
        save_poses(w, x.blend_from_);
        save_bytes(w, x.blend_from_set_);
        // frame_world_: scratch
        break;
    }
    case ManipKind::Slide: {
        const auto& x = static_cast<const SlideManipulator&>(m);
        w.vec3(x.current_);
        w.vec3(x.goal_);
        w.f32v(x.speed_);
        w.f32v(x.accel_);
        w.b(x.world_units_);
        break;
    }
    case ManipKind::Aim: {
        const auto& x = static_cast<const AimManipulator&>(m);
        w.i32v(x.yaw_bone_);
        w.i32v(x.pitch_bone_);
        w.i32v(x.muzzle_bone_);
        w.i32v(x.weapon_index_);
        w.str(x.label_);
        w.f32v(x.heading_);
        w.f32v(x.pitch_);
        w.f32v(x.yaw_min_);
        w.f32v(x.yaw_max_);
        w.f32v(x.yaw_speed_);
        w.f32v(x.pitch_min_);
        w.f32v(x.pitch_max_);
        w.f32v(x.pitch_speed_);
        w.f32v(x.reset_pose_time_);
        w.f32v(x.aim_heading_offset_);
        w.vec3(x.target_);
        w.b(x.elevation_.has_value());
        w.f32v(x.elevation_.value_or(0.0f));
        w.f32v(x.tolerance_);
        w.f32v(x.idle_time_);
        w.b(x.has_target_);
        w.b(x.on_target_);
        w.b(x.builder_arm_);
        w.b(x.yaw_only_on_target_);
        break;
    }
    case ManipKind::Slaver: {
        const auto& x = static_cast<const SlaverManipulator&>(m);
        w.i32v(x.slave_bone_);
        w.i32v(x.master_bone_);
        break;
    }
    case ManipKind::CollisionDetector: {
        const auto& x = static_cast<const CollisionDetectorManipulator&>(m);
        w.size(x.watched_.size());
        for (const auto& watch : x.watched_) {
            w.i32v(watch.bone);
            w.b(watch.below_foot_height);
            w.b(watch.below_surface);
        }
        w.b(x.terrain_check_);
        break;
    }
    case ManipKind::FootPlant: {
        const auto& x = static_cast<const FootPlantManipulator&>(m);
        w.i32v(x.foot_bone_);
        w.i32v(x.knee_bone_);
        w.i32v(x.hip_bone_);
        w.b(x.straight_legs_);
        w.f32v(x.max_foot_fall_);
        break;
    }
    case ManipKind::Storage: {
        const auto& x = static_cast<const StorageManipulator&>(m); // sim_: the loading sim
        w.b(x.mass_);
        w.vec3(x.empty_);
        w.vec3(x.full_);
        w.vec3(x.current_);
        break;
    }
    case ManipKind::Thrust: break;
    case ManipKind::BoneEntity: {
        const auto& x = static_cast<const BoneEntityManipulator&>(m); // sim_: the loading sim
        w.u32v(x.target_id_);
        w.i32v(x.target_bone_);
        break;
    }
    }
}

std::unique_ptr<Manipulator> StateIO::load_manipulator(StateReader& r, Unit& owner, SimState& sim) {
    r.tag("MANP");
    const auto kind = enum8<ManipKind>(r);
    const int waiting_ref = r.i32v();
    const u64 waiting_serial = r.u64v();
    const i32 bone = r.i32v();
    const i32 precedence = r.i32v();
    const bool enabled = r.b();
    const bool destroyed = r.b();
    const int table_ref = r.i32v();
    std::unique_ptr<Manipulator> m;
    switch (kind) {
    case ManipKind::Rotate: {
        auto x = std::make_unique<RotateManipulator>();
        x->axis_ = static_cast<char>(r.u8v());
        x->current_angle_ = r.f32v();
        x->goal_angle_ = r.f32v();
        x->speed_ = r.f32v();
        x->target_speed_ = r.f32v();
        x->accel_ = r.f32v();
        x->current_speed_ = r.f32v();
        x->has_goal_ = r.b();
        x->spin_down_ = r.b();
        m = std::move(x);
        break;
    }
    case ManipKind::Anim: {
        auto x = std::make_unique<AnimManipulator>();
        x->current_anim_ = r.str();
        x->rate_ = r.f32v();
        x->fraction_ = r.f32v();
        x->duration_ = r.f32v();
        x->looping_ = r.b();
        x->directional_ = r.b();
        x->finished_ = r.b();
        if (r.b()) {
            x->sca_data_ = sim.anim_cache() ? sim.anim_cache()->get(x->current_anim_) : nullptr;
            if (!x->sca_data_) r.fail("an animation that no longer loads: " + x->current_anim_);
        }
        x->sca_to_scm_map_ = load_i32s(r);
        x->scm_to_sca_map_ = load_i32s(r);
        x->disabled_bones_ = load_i32_set(r);
        x->blend_time_ = r.f32v();
        x->blend_remaining_ = r.f32v();
        x->last_ = load_poses(r);
        x->last_set_ = load_bytes(r);
        x->blend_from_ = load_poses(r);
        x->blend_from_set_ = load_bytes(r);
        m = std::move(x);
        break;
    }
    case ManipKind::Slide: {
        auto x = std::make_unique<SlideManipulator>();
        x->current_ = r.vec3();
        x->goal_ = r.vec3();
        x->speed_ = r.f32v();
        x->accel_ = r.f32v();
        x->world_units_ = r.b();
        m = std::move(x);
        break;
    }
    case ManipKind::Aim: {
        auto x = std::make_unique<AimManipulator>();
        x->yaw_bone_ = r.i32v();
        x->pitch_bone_ = r.i32v();
        x->muzzle_bone_ = r.i32v();
        x->weapon_index_ = r.i32v();
        x->label_ = r.str();
        x->heading_ = r.f32v();
        x->pitch_ = r.f32v();
        x->yaw_min_ = r.f32v();
        x->yaw_max_ = r.f32v();
        x->yaw_speed_ = r.f32v();
        x->pitch_min_ = r.f32v();
        x->pitch_max_ = r.f32v();
        x->pitch_speed_ = r.f32v();
        x->reset_pose_time_ = r.f32v();
        x->aim_heading_offset_ = r.f32v();
        x->target_ = r.vec3();
        const bool has_elevation = r.b();
        const f32 elevation = r.f32v();
        x->elevation_ = has_elevation ? std::optional<f32>(elevation) : std::nullopt;
        x->tolerance_ = r.f32v();
        x->idle_time_ = r.f32v();
        x->has_target_ = r.b();
        x->on_target_ = r.b();
        x->builder_arm_ = r.b();
        x->yaw_only_on_target_ = r.b();
        m = std::move(x);
        break;
    }
    case ManipKind::Slaver: {
        auto x = std::make_unique<SlaverManipulator>();
        x->slave_bone_ = r.i32v();
        x->master_bone_ = r.i32v();
        m = std::move(x);
        break;
    }
    case ManipKind::CollisionDetector: {
        auto x = std::make_unique<CollisionDetectorManipulator>();
        x->watched_.resize(r.size(6));
        for (auto& watch : x->watched_) {
            watch.bone = r.i32v();
            watch.below_foot_height = r.b();
            watch.below_surface = r.b();
        }
        x->terrain_check_ = r.b();
        m = std::move(x);
        break;
    }
    case ManipKind::FootPlant: {
        auto x = std::make_unique<FootPlantManipulator>();
        x->foot_bone_ = r.i32v();
        x->knee_bone_ = r.i32v();
        x->hip_bone_ = r.i32v();
        x->straight_legs_ = r.b();
        x->max_foot_fall_ = r.f32v();
        m = std::move(x);
        break;
    }
    case ManipKind::Storage: {
        const bool mass = r.b();
        const Vector3 empty = r.vec3();
        const Vector3 full = r.vec3();
        auto x = std::make_unique<StorageManipulator>(&sim, mass, empty, full);
        x->current_ = r.vec3();
        m = std::move(x);
        break;
    }
    case ManipKind::Thrust: m = std::make_unique<ThrustManipulator>(); break;
    case ManipKind::BoneEntity: {
        const u32 target = r.u32v();
        const i32 target_bone = r.i32v();
        m = std::make_unique<BoneEntityManipulator>(&sim, target, target_bone);
        break;
    }
    default: r.fail("a manipulator of an unknown kind"); return nullptr;
    }
    m->waiting_thread_ref_ = waiting_ref;
    m->waiting_thread_serial_ = waiting_serial;
    m->owner_ = &owner;
    m->bone_index_ = bone;
    m->precedence_ = precedence;
    m->enabled_ = enabled;
    m->destroyed_ = destroyed;
    m->lua_table_ref_ = table_ref;
    return m;
}

// ------------------------------------------------------------------- Unit

void StateIO::save(StateWriter& w, const Unit& u) {
    save(w, static_cast<const Entity&>(u));
    w.tag("UNIT");
    w.str(u.unit_id_);
    w.str(u.armor_type_);
    w.f32v(u.build_rate_);
    w.f32v(u.cap_cost_);
    w.f32v(u.max_build_distance_);
    w.f32v(u.guard_scan_radius_);
    w.b(u.need_unpack_);
    w.f32v(u.guard_return_radius_);
    w.f32v(u.attack_angle_);
    w.b(u.slaved_turning_);
    w.vec3(u.attack_facing_);
    w.b(u.turned_in_place_);
    w.str(u.layer_);
    w.str(u.motion_type_);
    w.f32v(u.layer_change_offset_);
    for (const blueprints::Footprint* fp : {&u.footprints_.main, &u.footprints_.alt}) {
        w.u8v(fp->size_x);
        w.u8v(fp->size_z);
        w.u8v(fp->caps);
        w.u8v(fp->flags);
        w.f32v(fp->max_slope);
        w.f32v(fp->min_water_depth);
        w.f32v(fp->max_water_depth);
    }
    w.i32v(u.footprints_.main_class);
    w.i32v(u.footprints_.alt_class);
    w.f32v(u.naval_draft_);
    w.u32v(u.jammer_blips_);
    w.f32v(u.jam_radius_min_);
    w.f32v(u.jam_radius_max_);
    w.b(u.is_being_built_);
    w.f32v(u.max_speed_);
    w.f32v(u.health_band_);
    save(w, u.navigator_);
    const UnitEconomy& ec = u.economy_;
    w.f64v(ec.production_mass);
    w.f64v(ec.production_energy);
    w.f64v(ec.consumption_mass);
    w.f64v(ec.consumption_energy);
    w.b(ec.production_active);
    w.b(ec.consumption_active);
    w.b(ec.script_consumption_active);
    w.b(ec.maintenance_active);
    w.f64v(ec.energy_maintenance_override);
    w.f64v(ec.storage_mass);
    w.f64v(ec.storage_energy);
    w.f64v(ec.reclaim_mass);
    w.f64v(ec.reclaim_energy);
    w.f64v(ec.silo_mass);
    w.f64v(ec.silo_energy);
    w.f64v(ec.dock_repair_mass);
    w.f64v(ec.dock_repair_energy);
    save_strings(w, u.categories_);
    // category_bits_: interned from categories_ (the ids are this process's)
    w.size(u.command_queue_.size());
    for (const UnitCommand& c : u.command_queue_) save(w, c);
    w.size(u.weapons_.size());
    for (const auto& wp : u.weapons_) save(w, *wp);
    w.size(u.rally_orders_.size());
    for (const UnitCommand& c : u.rally_orders_) save(w, c);
    w.u32v(u.build_target_id_);
    w.f64v(u.build_time_);
    w.f64v(u.build_cost_mass_);
    w.f64v(u.build_cost_energy_);
    w.f32v(u.work_progress_);
    w.u32v(u.reclaim_target_id_);
    w.f32v(u.reclaim_rate_);
    w.i32v(u.reclaim_wait_);
    w.b(u.builder_on_target_);
    w.b(u.arm_awaited_);
    w.u32v(u.repair_target_id_);
    w.f64v(u.repair_build_time_);
    w.f64v(u.repair_cost_mass_);
    w.f64v(u.repair_cost_energy_);
    w.u32v(u.capture_target_id_);
    w.f64v(u.capture_time_);
    w.f64v(u.capture_energy_cost_);
    w.b(u.capturable_);
    w.b(u.being_captured_);
    w.b(u.paused_);
    enum8(w, u.motion_horz_);
    enum8(w, u.motion_turn_);
    enum8(w, u.motion_turn_next_);
    const Unit::Drive& d = u.drive_;
    w.f32v(d.max_accel);
    w.f32v(d.max_brake);
    w.f32v(d.turn_rate);
    w.f32v(d.turn_radius);
    w.b(d.rotate_on_spot);
    w.f32v(d.rotate_threshold);
    w.f32v(d.max_speed_reverse);
    w.f32v(d.backup_distance);
    w.f32v(d.turn_facing_rate);
    const Unit::LifeBar& bar = u.life_bar_;
    w.f32v(bar.size);
    w.f32v(bar.height);
    w.f32v(bar.offset);
    w.b(bar.render);
    w.b(bar.hide);
    w.f32v(u.ground_speed_);
    w.f32v(u.target_speed_);
    w.f32v(u.top_speed_);
    w.b(u.drove_);
    w.b(u.jostled_);
    w.u32v(u.shield_entity_id_);
    w.b(u.busy_);
    w.i32v(u.stun_ticks_);
    w.b(u.block_command_queue_);
    w.i32v(u.fire_state_);
    w.u16v(u.script_bits_);
    w.u32v(u.creation_tick_);
    save_strings(w, u.toggle_caps_);
    w.f32v(u.surface_threat_);
    w.f32v(u.air_threat_);
    w.f32v(u.sub_threat_);
    w.f32v(u.economy_threat_);
    w.u32v(u.script_task_.serial);
    w.i32v(u.script_task_.object_ref);
    w.u32v(u.script_task_.wait);
    w.b(u.script_task_.suspended);
    w.u32v(u.next_task_serial_);
    w.i32v(u.script_task_result_);
    w.size(u.enhancements_.size());
    for (const auto& [slot, name] : u.enhancements_) {
        w.str(slot);
        w.str(name);
    }
    w.b(u.enhancing_);
    w.f64v(u.enhance_build_time_);
    w.str(u.enhance_name_);
    w.str(u.enhance_slot_);
    w.b(u.immobile_);
    w.b(u.factory_assist_build_);
    // auto_attack_target_ isn't saved: a weapon sets it and the unit uses it
    // in the same update.
    // air_stepped_ isn't saved: each update clears it before it is read.
    w.u32v(u.build_command_id_);
    w.b(u.build_released_with_order_);
    w.b(u.build_repairs_);
    w.i32v(u.assist_rolloff_wait_);
    save_strings(w, u.unit_states_);
    w.f32v(u.shield_ratio_);
    save_i32_set(w, u.hidden_bones_);
    // animated_bone_matrices_: the renderer's, from pose_
    {
        std::vector<std::pair<std::string, IntelState>> intel(u.intel_states_.begin(),
                                                              u.intel_states_.end());
        std::sort(intel.begin(), intel.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        w.size(intel.size());
        for (const auto& [name, st] : intel) {
            w.str(name);
            w.f32v(st.radius);
            w.b(st.enabled);
        }
    }
    w.size(u.manipulators_.size());
    for (const auto& m : u.manipulators_) save(w, *m);
    save_poses(w, u.pose_);
    // pose_order_, pose_locals_: update_pose's scratch
    save_ids(w, u.cargo_ids_);
    w.i32v(u.storage_slots_);
    save_ids(w, u.stored_ids_);
    w.u32v(u.launch_index_);
    save_ids(w, u.storage_reserved_);
    w.u32v(u.next_generic_);
    w.i32v(u.generic_overflow_);
    enum8(w, u.retrieve_phase_);
    save_ids(w, u.retrieve_ids_);
    w.i32v(u.retrieve_wait_);
    const Unit::CarrierLanding& l = u.landing_;
    w.u32v(l.carrier);
    enum8(w, l.phase);
    w.vec3(l.place.point);
    w.f32v(l.place.heading);
    w.f32v(l.place.height);
    w.i32v(l.place.delay);
    w.vec3(l.approach);
    w.i32v(l.wait);
    w.b(l.glided);
    w.b(l.deck_set);
    w.u32v(u.transport_id_);
    w.f32v(u.speed_mult_);
    w.i32v(u.transport_class_);
    w.i32v(u.transport_capacity_);
    const TransportLayout& tl = u.transport_layout_;
    w.i32v(tl.class_generic_up_to);
    w.i32v(tl.class2_attach_size);
    w.i32v(tl.class3_attach_size);
    w.i32v(tl.class4_attach_size);
    w.i32v(tl.class_s_attach_size);
    // transport_slots_: made lazily from the skeleton; its layout_ is the
    // unit's, its points (class1_, class2_, class3_, class4_, special_,
    // generic_, launch_) follow from the skeleton, its slots are saved
    w.b(u.transport_slots_ != nullptr);
    if (u.transport_slots_) {
        const auto& slots = u.transport_slots_->slots_;
        w.size(slots.size());
        for (const TransportSlots::Slot& s : slots) {
            w.u32v(s.unit_id);
            w.i32v(s.bone);
            w.i32v(s.unit_bone);
            save_i32s(w, s.bones);
        }
    }
    w.f32v(u.size_x_);
    w.f32v(u.size_y_);
    w.f32v(u.size_z_);
    w.f32v(u.average_density_);
    w.u8v(u.vet_level_);
    w.size(u.damage_contributions_.size());
    for (const auto& [id, dmg] : u.damage_contributions_) {
        w.u32v(id);
        w.f32v(dmg);
    }
    save_str_f64(w, u.stats_);
    w.i32v(u.nuke_silo_ammo_);
    w.i32v(u.tactical_silo_ammo_);
    w.size(u.silo_orders_.size());
    for (bool nuke : u.silo_orders_) w.b(nuke);
    const Unit::SiloBuild& sb = u.silo_build_;
    w.i32v(sb.weapon);
    w.b(sb.nuke);
    w.f64v(sb.progress);
    w.f64v(sb.build_time);
    w.f64v(sb.energy);
    w.f64v(sb.mass);
    w.i32v(u.silo_blocks_);
    w.b(u.assisting_silo_);
    w.b(u.teleporting_);
    w.u32v(u.teleport_snap_);
    w.b(u.overcharge_armed_);
    enum8(w, u.pickup_phase_);
    save_ids(w, u.pickup_ids_);
    w.vec3(u.pickup_center_);
    w.quat(u.pickup_facing_);
    w.i32v(u.pickup_ticks_);
    w.f32v(u.transport_hover_height_);
    // Idle aircraft (item 6)
    w.f32v(u.auto_land_time_);
    w.f32v(u.start_turn_distance_);
    w.u32v(u.idle_landing_.idle_since);
    w.b(u.idle_landing_.descending);
    w.vec3(u.idle_landing_.target);
    w.str(u.idle_landing_.layer);
    w.i32v(u.idle_landing_.reserved.x0);
    w.i32v(u.idle_landing_.reserved.z0);
    w.i32v(u.idle_landing_.reserved.x1);
    w.i32v(u.idle_landing_.reserved.z1);
    w.i32v(u.beam_up_ticks_);
    w.vec3(u.beam_from_);
    w.quat(u.beam_from_orientation_);
    enum8(w, u.ferry_phase_);
    w.i32v(u.ferry_index_);
    w.b(u.ferry_leg_set_);
    w.u32v(u.ferry_for_);
    w.size(u.adjacent_unit_ids_.size());
    for (u32 id : u.adjacent_unit_ids_) w.u32v(id);
    w.f32v(u.skirt_size_x_);
    w.f32v(u.skirt_size_z_);
    w.f32v(u.skirt_offset_x_);
    w.f32v(u.skirt_offset_z_);
    w.f32v(u.accel_mult_);
    w.f32v(u.turn_mult_);
    w.f32v(u.break_off_distance_mult_);
    w.f32v(u.break_off_trigger_mult_);
    w.f32v(u.fuel_ratio_);
    w.f32v(u.fuel_use_time_);
    w.f32v(u.fuel_recharge_rate_);
    w.b(u.refuel_started_);
    w.b(u.dock_repair_asked_);
    w.b(u.air_class_);
    w.i32v(u.docking_slots_);
    const StagingRules& st = u.staging_rules_;
    w.f32v(st.refuel_multiplier);
    w.f32v(st.repair_amount);
    w.f32v(st.repair_energy);
    w.f32v(st.repair_mass);
    w.f32v(st.scan_radius);
    const AirCombatRules& ar = u.air_combat_rules_;
    w.b(ar.winged);
    for (const f32 v : {ar.min_airspeed, ar.combat_turn_speed, ar.tight_turn_multiplier,
                        ar.sustained_turn_threshold, ar.engage_distance, ar.break_off_trigger,
                        ar.break_off_distance})
        w.f32v(v);
    w.b(ar.break_off_if_near_new_target);
    for (const f32 v :
         {ar.random_break_off_distance_mult, ar.random_min_change_combat_state_time,
          ar.random_max_change_combat_state_time, ar.predict_ahead_for_bomb_drop,
          ar.attack_elevation, ar.k_turn, ar.k_turn_damping, ar.k_move, ar.k_move_damping})
        w.f32v(v);
    w.b(ar.hover_over_attack);
    w.b(ar.circling_dir_change);
    for (const f32 v : {ar.circling_min_airspeed, ar.circling_turn_mult, ar.circling_radius_min,
                        ar.circling_radius_max, ar.circling_radius_vs_air_mult,
                        ar.circling_elevation_ratio, ar.circling_change_frequency, ar.bank_factor})
        w.f32v(v);
    const AirCombatState& ac = u.air_combat_;
    w.u8v(ac.state);
    w.u32v(ac.timeout_tick);
    w.i32v(ac.sustained_turn_ticks);
    w.f32v(ac.yaw_rate);
    w.vec3(ac.velocity);
    w.b(ac.flying);
    w.b(ac.circle_reverse);
    w.f32v(ac.circle_elevation);
    w.f32v(ac.circle_radius_ratio);
    w.vec3(ac.circle_anchor);
    w.f32v(u.heading_);
    w.f32v(u.pitch_);
    w.f32v(u.bank_angle_);
    w.f32v(u.current_airspeed_);
    w.f32v(u.current_altitude_);
    w.f32v(u.max_airspeed_);
    w.f32v(u.turn_rate_rad_);
    w.f32v(u.accel_rate_);
    w.f32v(u.climb_rate_);
    w.f32v(u.elevation_target_);
    w.b(u.fly_in_water_);
    enum8(w, u.vert_motion_);
    w.f32v(u.sub_elevation_);
    w.f32v(u.dive_surface_speed_);
    w.str(u.vert_event_);
    w.b(u.crashing_);
    w.b(u.crash_impacted_);
    w.vec3(u.velocity_);
    w.f32v(u.crash_velocity_y_);
    w.f32v(u.crash_spin_rate_);
    w.u32v(u.creator_id_);
    w.vec3(u.tick_position_);
    w.b(u.tick_position_set_);
    w.b(u.moved_last_tick_);
    w.b(u.auto_overcharge_);
    w.b(u.overcharge_paused_);
    w.b(u.cloaked_);
    w.b(u.radar_stealth_);
    w.b(u.sonar_stealth_);
    w.b(u.auto_mode_);
    w.b(u.repeat_queue_);
    w.b(u.auto_surface_mode_);
    w.u32v(u.focus_entity_id_);
    w.b(u.can_take_damage_);
    w.b(u.can_be_killed_);
    w.u32v(u.last_attacker_id_);
    save_strings(w, u.command_caps_);
    save_strings(w, u.original_command_caps_);
    save(w, u.build_restriction_);
    w.i32v(u.selection_priority_);
    w.f32v(u.elevation_override_);
    w.b(u.dying_);
    w.b(u.transferred_);
    w.size(u.on_unit_built_callbacks_.size());
    for (const Unit::UnitBuiltCallback& cb : u.on_unit_built_callbacks_) {
        w.i32v(cb.func_ref);
        w.i32v(cb.cat_ref);
    }
}

void StateIO::load(StateReader& r, Unit& u, SimState& sim) {
    load(r, static_cast<Entity&>(u));
    r.tag("UNIT");
    u.unit_id_ = r.str();
    u.armor_type_ = r.str();
    u.build_rate_ = r.f32v();
    u.cap_cost_ = r.f32v();
    u.max_build_distance_ = r.f32v();
    u.guard_scan_radius_ = r.f32v();
    u.need_unpack_ = r.b();
    u.guard_return_radius_ = r.f32v();
    u.attack_angle_ = r.f32v();
    u.slaved_turning_ = r.b();
    u.attack_facing_ = r.vec3();
    u.turned_in_place_ = r.b();
    u.layer_ = r.str();
    u.motion_type_ = r.str();
    u.layer_change_offset_ = r.f32v();
    for (blueprints::Footprint* fp : {&u.footprints_.main, &u.footprints_.alt}) {
        fp->size_x = r.u8v();
        fp->size_z = r.u8v();
        fp->caps = r.u8v();
        fp->flags = r.u8v();
        fp->max_slope = r.f32v();
        fp->min_water_depth = r.f32v();
        fp->max_water_depth = r.f32v();
    }
    u.footprints_.main_class = r.i32v();
    u.footprints_.alt_class = r.i32v();
    u.naval_draft_ = r.f32v();
    u.jammer_blips_ = r.u32v();
    u.jam_radius_min_ = r.f32v();
    u.jam_radius_max_ = r.f32v();
    u.is_being_built_ = r.b();
    u.max_speed_ = r.f32v();
    u.health_band_ = r.f32v();
    load(r, u.navigator_, sim, u.army());
    u.navigator_.set_sim_state(&sim);
    UnitEconomy& ec = u.economy_;
    ec.production_mass = r.f64v();
    ec.production_energy = r.f64v();
    ec.consumption_mass = r.f64v();
    ec.consumption_energy = r.f64v();
    ec.production_active = r.b();
    ec.consumption_active = r.b();
    ec.script_consumption_active = r.b();
    ec.maintenance_active = r.b();
    ec.energy_maintenance_override = r.f64v();
    ec.storage_mass = r.f64v();
    ec.storage_energy = r.f64v();
    ec.reclaim_mass = r.f64v();
    ec.reclaim_energy = r.f64v();
    ec.silo_mass = r.f64v();
    ec.silo_energy = r.f64v();
    ec.dock_repair_mass = r.f64v();
    ec.dock_repair_energy = r.f64v();
    u.categories_ = load_strings<std::unordered_set<std::string>>(r);
    u.category_bits_ = {};
    for (const std::string& c : u.categories_) u.category_bits_.set(CategoryIds::intern(c));
    u.command_queue_.resize(r.size(8));
    for (UnitCommand& c : u.command_queue_) load(r, c);
    u.weapons_.resize(r.size(8));
    for (auto& wp : u.weapons_) {
        wp = std::make_unique<Weapon>();
        load(r, *wp);
    }
    u.rally_orders_.resize(r.size(8));
    for (UnitCommand& c : u.rally_orders_) load(r, c);
    u.build_target_id_ = r.u32v();
    u.build_time_ = r.f64v();
    u.build_cost_mass_ = r.f64v();
    u.build_cost_energy_ = r.f64v();
    u.work_progress_ = r.f32v();
    u.reclaim_target_id_ = r.u32v();
    u.reclaim_rate_ = r.f32v();
    u.reclaim_wait_ = r.i32v();
    u.builder_on_target_ = r.b();
    u.arm_awaited_ = r.b();
    u.repair_target_id_ = r.u32v();
    u.repair_build_time_ = r.f64v();
    u.repair_cost_mass_ = r.f64v();
    u.repair_cost_energy_ = r.f64v();
    u.capture_target_id_ = r.u32v();
    u.capture_time_ = r.f64v();
    u.capture_energy_cost_ = r.f64v();
    u.capturable_ = r.b();
    u.being_captured_ = r.b();
    u.paused_ = r.b();
    u.motion_horz_ = enum8<Unit::MotionHorz>(r);
    u.motion_turn_ = enum8<Unit::MotionTurn>(r);
    u.motion_turn_next_ = enum8<Unit::MotionTurn>(r);
    Unit::Drive& d = u.drive_;
    d.max_accel = r.f32v();
    d.max_brake = r.f32v();
    d.turn_rate = r.f32v();
    d.turn_radius = r.f32v();
    d.rotate_on_spot = r.b();
    d.rotate_threshold = r.f32v();
    d.max_speed_reverse = r.f32v();
    d.backup_distance = r.f32v();
    d.turn_facing_rate = r.f32v();
    Unit::LifeBar& bar = u.life_bar_;
    bar.size = r.f32v();
    bar.height = r.f32v();
    bar.offset = r.f32v();
    bar.render = r.b();
    bar.hide = r.b();
    u.ground_speed_ = r.f32v();
    u.target_speed_ = r.f32v();
    u.top_speed_ = r.f32v();
    u.drove_ = r.b();
    u.jostled_ = r.b();
    u.shield_entity_id_ = r.u32v();
    u.busy_ = r.b();
    u.stun_ticks_ = r.i32v();
    u.block_command_queue_ = r.b();
    u.fire_state_ = r.i32v();
    u.script_bits_ = r.u16v();
    u.creation_tick_ = r.u32v();
    u.toggle_caps_ = load_strings<std::unordered_set<std::string>>(r);
    u.surface_threat_ = r.f32v();
    u.air_threat_ = r.f32v();
    u.sub_threat_ = r.f32v();
    u.economy_threat_ = r.f32v();
    u.script_task_.serial = r.u32v();
    u.script_task_.object_ref = r.i32v();
    u.script_task_.wait = r.u32v();
    u.script_task_.suspended = r.b();
    u.next_task_serial_ = r.u32v();
    u.script_task_result_ = r.i32v();
    u.enhancements_.clear();
    const size_t enhancements = r.size(8);
    for (size_t i = 0; i < enhancements; ++i) {
        std::string slot = r.str();
        u.enhancements_[slot] = r.str();
    }
    u.enhancing_ = r.b();
    u.enhance_build_time_ = r.f64v();
    u.enhance_name_ = r.str();
    u.enhance_slot_ = r.str();
    u.immobile_ = r.b();
    u.factory_assist_build_ = r.b();
    u.build_command_id_ = r.u32v();
    u.build_released_with_order_ = r.b();
    u.build_repairs_ = r.b();
    u.assist_rolloff_wait_ = r.i32v();
    u.unit_states_ = load_strings<std::unordered_set<std::string>>(r);
    u.shield_ratio_ = r.f32v();
    u.hidden_bones_ = load_i32_set(r);
    u.intel_states_.clear();
    const size_t intel = r.size(9);
    for (size_t i = 0; i < intel; ++i) {
        std::string name = r.str();
        IntelState st;
        st.radius = r.f32v();
        st.enabled = r.b();
        u.intel_states_[name] = st;
    }
    u.manipulators_.clear();
    const size_t manipulators = r.size(8);
    for (size_t i = 0; i < manipulators && r.ok(); ++i) {
        auto m = load_manipulator(r, u, sim);
        if (m) u.manipulators_.push_back(std::move(m));
    }
    u.pose_ = load_poses(r);
    u.cargo_ids_ = load_ids(r);
    u.storage_slots_ = r.i32v();
    u.stored_ids_ = load_ids(r);
    u.launch_index_ = r.u32v();
    u.storage_reserved_ = load_ids(r);
    u.next_generic_ = r.u32v();
    u.generic_overflow_ = r.i32v();
    u.retrieve_phase_ = enum8<Unit::RetrievePhase>(r);
    u.retrieve_ids_ = load_ids(r);
    u.retrieve_wait_ = r.i32v();
    Unit::CarrierLanding& l = u.landing_;
    l.carrier = r.u32v();
    l.phase = enum8<Unit::LandPhase>(r);
    l.place.point = r.vec3();
    l.place.heading = r.f32v();
    l.place.height = r.f32v();
    l.place.delay = r.i32v();
    l.approach = r.vec3();
    l.wait = r.i32v();
    l.glided = r.b();
    l.deck_set = r.b();
    u.transport_id_ = r.u32v();
    u.speed_mult_ = r.f32v();
    u.transport_class_ = r.i32v();
    u.transport_capacity_ = r.i32v();
    TransportLayout& tl = u.transport_layout_;
    tl.class_generic_up_to = r.i32v();
    tl.class2_attach_size = r.i32v();
    tl.class3_attach_size = r.i32v();
    tl.class4_attach_size = r.i32v();
    tl.class_s_attach_size = r.i32v();

    // Its skeleton, as creation gave it (the bone cache, by blueprint)
    if (auto* bones = sim.bone_cache())
        u.set_bone_data(bones->get(u.blueprint_id(), sim.lua_state()));
    u.animated_bone_matrices_.clear();
    u.init_animated_bones();
    u.transport_slots_.reset();
    if (r.b()) {
        if (!u.bone_data()) {
            r.fail("a transport's slots, and no skeleton: " + u.blueprint_id());
            return;
        }
        u.transport_slots_ = std::make_unique<TransportSlots>(*u.bone_data(), u.transport_layout_);
        auto& slots = u.transport_slots_->slots_;
        slots.resize(r.size(16));
        for (TransportSlots::Slot& s : slots) {
            s.unit_id = r.u32v();
            s.bone = r.i32v();
            s.unit_bone = r.i32v();
            s.bones = load_i32s(r);
        }
    }
    u.size_x_ = r.f32v();
    u.size_y_ = r.f32v();
    u.size_z_ = r.f32v();
    u.average_density_ = r.f32v();
    u.vet_level_ = r.u8v();
    u.damage_contributions_.resize(r.size(8));
    for (auto& [id, dmg] : u.damage_contributions_) {
        id = r.u32v();
        dmg = r.f32v();
    }
    u.stats_ = load_str_f64<std::unordered_map<std::string, f64>>(r);
    u.nuke_silo_ammo_ = r.i32v();
    u.tactical_silo_ammo_ = r.i32v();
    u.silo_orders_.resize(r.size(1));
    for (bool& nuke : u.silo_orders_) nuke = r.b();
    Unit::SiloBuild& sb = u.silo_build_;
    sb.weapon = r.i32v();
    sb.nuke = r.b();
    sb.progress = r.f64v();
    sb.build_time = r.f64v();
    sb.energy = r.f64v();
    sb.mass = r.f64v();
    u.silo_blocks_ = r.i32v();
    u.assisting_silo_ = r.b();
    u.teleporting_ = r.b();
    u.teleport_snap_ = r.u32v();
    u.overcharge_armed_ = r.b();
    u.pickup_phase_ = enum8<Unit::PickupPhase>(r);
    u.pickup_ids_ = load_ids(r);
    u.pickup_center_ = r.vec3();
    u.pickup_facing_ = r.quat();
    u.pickup_ticks_ = r.i32v();
    u.transport_hover_height_ = r.f32v();
    u.auto_land_time_ = r.f32v();
    u.start_turn_distance_ = r.f32v();
    u.idle_landing_.idle_since = r.u32v();
    u.idle_landing_.descending = r.b();
    u.idle_landing_.target = r.vec3();
    u.idle_landing_.layer = r.str();
    u.idle_landing_.reserved.x0 = r.i32v();
    u.idle_landing_.reserved.z0 = r.i32v();
    u.idle_landing_.reserved.x1 = r.i32v();
    u.idle_landing_.reserved.z1 = r.i32v();
    u.beam_up_ticks_ = r.i32v();
    u.beam_from_ = r.vec3();
    u.beam_from_orientation_ = r.quat();
    u.ferry_phase_ = enum8<Unit::FerryPhase>(r);
    u.ferry_index_ = r.i32v();
    u.ferry_leg_set_ = r.b();
    u.ferry_for_ = r.u32v();
    u.adjacent_unit_ids_.clear();
    const size_t adjacent = r.size(4);
    for (size_t i = 0; i < adjacent; ++i) u.adjacent_unit_ids_.insert(r.u32v());
    u.skirt_size_x_ = r.f32v();
    u.skirt_size_z_ = r.f32v();
    u.skirt_offset_x_ = r.f32v();
    u.skirt_offset_z_ = r.f32v();
    u.accel_mult_ = r.f32v();
    u.turn_mult_ = r.f32v();
    u.break_off_distance_mult_ = r.f32v();
    u.break_off_trigger_mult_ = r.f32v();
    u.fuel_ratio_ = r.f32v();
    u.fuel_use_time_ = r.f32v();
    u.fuel_recharge_rate_ = r.f32v();
    u.refuel_started_ = r.b();
    u.dock_repair_asked_ = r.b();
    u.air_class_ = r.b();
    u.docking_slots_ = r.i32v();
    StagingRules& st = u.staging_rules_;
    st.refuel_multiplier = r.f32v();
    st.repair_amount = r.f32v();
    st.repair_energy = r.f32v();
    st.repair_mass = r.f32v();
    st.scan_radius = r.f32v();
    AirCombatRules& ar = u.air_combat_rules_;
    ar.winged = r.b();
    for (f32* v : {&ar.min_airspeed, &ar.combat_turn_speed, &ar.tight_turn_multiplier,
                   &ar.sustained_turn_threshold, &ar.engage_distance, &ar.break_off_trigger,
                   &ar.break_off_distance})
        *v = r.f32v();
    ar.break_off_if_near_new_target = r.b();
    for (f32* v :
         {&ar.random_break_off_distance_mult, &ar.random_min_change_combat_state_time,
          &ar.random_max_change_combat_state_time, &ar.predict_ahead_for_bomb_drop,
          &ar.attack_elevation, &ar.k_turn, &ar.k_turn_damping, &ar.k_move, &ar.k_move_damping})
        *v = r.f32v();
    ar.hover_over_attack = r.b();
    ar.circling_dir_change = r.b();
    for (f32* v : {&ar.circling_min_airspeed, &ar.circling_turn_mult, &ar.circling_radius_min,
                   &ar.circling_radius_max, &ar.circling_radius_vs_air_mult,
                   &ar.circling_elevation_ratio, &ar.circling_change_frequency, &ar.bank_factor})
        *v = r.f32v();
    AirCombatState& ac = u.air_combat_;
    ac.state = r.u8v();
    ac.timeout_tick = r.u32v();
    ac.sustained_turn_ticks = r.i32v();
    ac.yaw_rate = r.f32v();
    ac.velocity = r.vec3();
    ac.flying = r.b();
    ac.circle_reverse = r.b();
    ac.circle_elevation = r.f32v();
    ac.circle_radius_ratio = r.f32v();
    ac.circle_anchor = r.vec3();
    u.heading_ = r.f32v();
    u.pitch_ = r.f32v();
    u.bank_angle_ = r.f32v();
    u.current_airspeed_ = r.f32v();
    u.current_altitude_ = r.f32v();
    u.max_airspeed_ = r.f32v();
    u.turn_rate_rad_ = r.f32v();
    u.accel_rate_ = r.f32v();
    u.climb_rate_ = r.f32v();
    u.elevation_target_ = r.f32v();
    u.fly_in_water_ = r.b();
    u.vert_motion_ = enum8<Unit::VertMotion>(r);
    u.sub_elevation_ = r.f32v();
    u.dive_surface_speed_ = r.f32v();
    u.vert_event_ = r.str();
    u.crashing_ = r.b();
    u.crash_impacted_ = r.b();
    u.velocity_ = r.vec3();
    u.crash_velocity_y_ = r.f32v();
    u.crash_spin_rate_ = r.f32v();
    u.creator_id_ = r.u32v();
    u.tick_position_ = r.vec3();
    u.tick_position_set_ = r.b();
    u.moved_last_tick_ = r.b();
    u.auto_overcharge_ = r.b();
    u.overcharge_paused_ = r.b();
    u.cloaked_ = r.b();
    u.radar_stealth_ = r.b();
    u.sonar_stealth_ = r.b();
    u.auto_mode_ = r.b();
    u.repeat_queue_ = r.b();
    u.auto_surface_mode_ = r.b();
    u.focus_entity_id_ = r.u32v();
    u.can_take_damage_ = r.b();
    u.can_be_killed_ = r.b();
    u.last_attacker_id_ = r.u32v();
    u.command_caps_ = load_strings<std::unordered_set<std::string>>(r);
    u.original_command_caps_ = load_strings<std::unordered_set<std::string>>(r);
    load(r, u.build_restriction_);
    u.selection_priority_ = r.i32v();
    u.elevation_override_ = r.f32v();
    u.dying_ = r.b();
    u.transferred_ = r.b();
    u.on_unit_built_callbacks_.resize(r.size(8));
    for (Unit::UnitBuiltCallback& cb : u.on_unit_built_callbacks_) {
        cb.func_ref = r.i32v();
        cb.cat_ref = r.i32v();
    }
    u.refresh_bone_matrices();
}

// ---------------------------------------------------- Projectile, Prop, Shield

void StateIO::save(StateWriter& w, const Projectile& p) {
    save(w, static_cast<const Entity&>(p));
    w.tag("PROJ");
    w.size(p.shooters.size());
    for (const auto& [unit, weapon] : p.shooters) {
        w.u32v(unit);
        w.i32v(weapon);
    }
    w.vec3(p.velocity);
    w.u32v(p.target_entity_id);
    w.i32v(p.target_point);
    w.vec3(p.target_position);
    w.b(p.has_target_position);
    w.u32v(p.launcher_id);
    w.f32v(p.damage_amount);
    w.f32v(p.damage_radius);
    w.str(p.damage_type);
    w.f32v(p.lifetime);
    w.f32v(p.max_speed);
    w.f32v(p.acceleration);
    w.f32v(p.ballistic_accel);
    w.f32v(p.turn_rate);
    w.b(p.tracking);
    w.b(p.lead_target);
    w.f32v(p.max_zig_zag);
    w.f32v(p.zig_zag_freq);
    w.vec3(p.zig_zag_offset);
    w.u32v(p.zig_zag_next_tick);
    w.b(p.keep_last_aim);
    w.f32v(p.detonate_above_height);
    w.f32v(p.detonate_below_height);
    w.b(p.destroy_on_water);
    w.b(p.stay_upright);
    w.b(p.velocity_align);
    w.vec3(p.angular_velocity);
    w.vec3(p.scale_velocity);
    w.b(p.collision_enabled);
    w.b(p.collide_surface);
    w.b(p.collide_entity);
    w.b(p.stay_underwater);
    w.b(p.impacted);
    w.b(p.in_water);
    w.b(p.burst_armed);
    save_ids(w, p.passed);
    // info_: the sim's blueprint info, by blueprint (load_sim_state)
}

void StateIO::load(StateReader& r, Projectile& p) {
    load(r, static_cast<Entity&>(p));
    r.tag("PROJ");
    p.shooters.resize(r.size(8));
    for (auto& [unit, weapon] : p.shooters) {
        unit = r.u32v();
        weapon = r.i32v();
    }
    p.velocity = r.vec3();
    p.target_entity_id = r.u32v();
    p.target_point = r.i32v();
    p.target_position = r.vec3();
    p.has_target_position = r.b();
    p.launcher_id = r.u32v();
    p.damage_amount = r.f32v();
    p.damage_radius = r.f32v();
    p.damage_type = r.str();
    p.lifetime = r.f32v();
    p.max_speed = r.f32v();
    p.acceleration = r.f32v();
    p.ballistic_accel = r.f32v();
    p.turn_rate = r.f32v();
    p.tracking = r.b();
    p.lead_target = r.b();
    p.max_zig_zag = r.f32v();
    p.zig_zag_freq = r.f32v();
    p.zig_zag_offset = r.vec3();
    p.zig_zag_next_tick = r.u32v();
    p.keep_last_aim = r.b();
    p.detonate_above_height = r.f32v();
    p.detonate_below_height = r.f32v();
    p.destroy_on_water = r.b();
    p.stay_upright = r.b();
    p.velocity_align = r.b();
    p.angular_velocity = r.vec3();
    p.scale_velocity = r.vec3();
    p.collision_enabled = r.b();
    p.collide_surface = r.b();
    p.collide_entity = r.b();
    p.stay_underwater = r.b();
    p.impacted = r.b();
    p.in_water = r.b();
    p.burst_armed = r.b();
    p.passed = load_ids(r);
}

void StateIO::save(StateWriter& w, const Prop& p) {
    save(w, static_cast<const Entity&>(p));
    w.tag("PROP");
    // untargetable, reclaimable_category, obstructs_building, reclaim_mass_max,
    // reclaim_energy_max: its blueprint's (read_prop_blueprint)
    w.f32v(p.sink_rate);
    w.size(p.pose.size());
    for (const auto& m : p.pose)
        for (f32 v : m) w.f32v(v);
}

void StateIO::load(StateReader& r, Prop& p) {
    load(r, static_cast<Entity&>(p));
    r.tag("PROP");
    p.sink_rate = r.f32v();
    p.pose.resize(r.size(64));
    for (auto& m : p.pose)
        for (f32& v : m) v = r.f32v();
}

void StateIO::save(StateWriter& w, const Shield& s) {
    save(w, static_cast<const Entity&>(s));
    w.tag("SHLD");
    w.u32v(s.owner_id);
    w.b(s.is_on);
    w.f32v(s.size);
    w.str(s.shield_type);
}

void StateIO::load(StateReader& r, Shield& s) {
    load(r, static_cast<Entity&>(s));
    r.tag("SHLD");
    s.owner_id = r.u32v();
    s.is_on = r.b();
    s.size = r.f32v();
    s.shield_type = r.str();
}

} // namespace osc::sim
