#include "sim/sim_state.hpp"

#include "map/terrain.hpp"
#include "sim/fnv.hpp"
#include "sim/manipulator.hpp"
#include "sim/path_search.hpp"
#include "sim/path_tables.hpp"
#include "sim/platoon.hpp"
#include "sim/projectile.hpp"
#include "sim/shield.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <bit>
#include <string_view>
#include <utility>
#include <vector>

namespace osc::sim {

namespace {

/// A vector, x then y then z.
void mix_vec(Fnv& f, const Vector3& v) {
    f.mix_f32(v.x);
    f.mix_f32(v.y);
    f.mix_f32(v.z);
}

/// A string, its length then its bytes.
void mix_str(Fnv& f, std::string_view str) {
    f.mix(static_cast<u64>(str.size()));
    for (const char ch : str) f.mix(static_cast<u8>(ch));
}

/// An army: its state, its economy, its platoons and its influence map.
void mix_army(Fnv& armies, const ArmyBrain& a) {
    armies.mix(static_cast<u64>(a.state()));
    const auto& econ = a.economy();
    for (const auto* res : {&econ.mass, &econ.energy}) {
        armies.mix_f32(static_cast<f32>(res->stored));
        armies.mix_f32(static_cast<f32>(res->max_storage));
        armies.mix_f32(static_cast<f32>(res->income));
        armies.mix_f32(static_cast<f32>(res->requested));
    }
    // Its platoons (their unique names, DisbandOnIdle and units decide
    // which the next tick's cleanup destroys, and so which AI threads
    // run on).
    for (size_t i = 0; i < a.platoon_count(); ++i) {
        const Platoon* p = a.platoon_at(i);
        if (!p || p->destroyed()) continue;
        armies.mix(p->platoon_id());
        mix_str(armies, p->unique_name());
        armies.mix(p->disband_on_idle() ? 1u : 0u);
        armies.mix(static_cast<u64>(p->unit_ids().size()));
        for (const u32 id : p->unit_ids()) armies.mix(id);
    }
    // Its influence map's entries (M207b), as Moho mixes their strengths.
    if (const InfluenceMap* map = a.influence_map()) {
        armies.mix(static_cast<u64>(map->entry_count()));
        map->for_each_entry([&](const InfluenceMap::EntryView& v) {
            armies.mix(v.id);
            armies.mix_f32(v.strength);
        });
    }
}

void mix_quat(Fnv& f, const Quaternion& q) {
    f.mix_f32(q.x);
    f.mix_f32(q.y);
    f.mix_f32(q.z);
    f.mix_f32(q.w);
}

bool same_place(const Vector3& p, const Quaternion& q, const Vector3& p2, const Quaternion& q2) {
    return p.x == p2.x && p.y == p2.y && p.z == p2.z && q.x == q2.x && q.y == q2.y && q.z == q2.z &&
           q.w == q2.w;
}

/// What every entity has: its id, army, committed place and facing, the pending and last ones
/// where they differ from it, health and completion.
void mix_entity(Fnv& entities, const Entity& e) {
    entities.mix(e.entity_id());
    entities.mix(static_cast<u64>(static_cast<u32>(e.army())));
    entities.mix(e.destroyed() ? 1u : 0u);
    mix_vec(entities, e.position());
    mix_quat(entities, e.orientation());
    const bool pending =
        !same_place(e.pending_position(), e.pending_orientation(), e.position(), e.orientation());
    const bool moved =
        !same_place(e.last_position(), e.last_orientation(), e.position(), e.orientation());
    entities.mix((pending ? 2u : 0u) | (moved ? 1u : 0u));
    if (pending) {
        mix_vec(entities, e.pending_position());
        mix_quat(entities, e.pending_orientation());
    }
    if (moved) {
        mix_vec(entities, e.last_position());
        mix_quat(entities, e.last_orientation());
    }
    entities.mix_f32(e.health());
    entities.mix_f32(e.fraction_complete());
}

/// A projectile's flight: its velocity, its target and where it homes,
/// its lifetime, whether it has hit and whether it tracks.
void mix_projectile(Fnv& projectiles, const Projectile& p) {
    projectiles.mix(p.entity_id());
    mix_vec(projectiles, p.velocity);
    projectiles.mix(p.target_entity_id);
    // The target point it homes on, unless the centre.
    if (p.target_point != -1)
        projectiles.mix(0x5450543a00000000ull | static_cast<u32>(p.target_point)); // "TPT:"
    mix_vec(projectiles, p.target_position);
    projectiles.mix(p.lifetime_end);
    projectiles.mix((p.impacted ? 1u : 0u) | (p.tracking ? 2u : 0u));
}

/// What mix_unit_script_state sorts, kept unit to unit.
struct UnitScratch {
    std::vector<std::pair<std::string_view, f64>> stats;
    std::vector<std::string_view> names;
};

/// A unit's player settings (pause, fire state, modes), and its factory's
/// assist and sacrifice work.
void mix_unit_settings(Fnv& units, const Unit& u) {
    // The player's settings: a pause or fire state that differs between
    // peers is a desync before it moves anything.
    units.mix(u.entity_id());
    units.mix(u.script_bits());
    units.mix(static_cast<u64>(static_cast<u32>(u.fire_state())));
    units.mix((u.is_paused() ? 1u : 0u) | (u.auto_mode() ? 2u : 0u) | (u.repeat_queue() ? 4u : 0u) |
              (u.auto_surface_mode() ? 8u : 0u) | (u.is_dying() ? 16u : 0u) |
              (u.is_being_built() ? 32u : 0u) | (u.factory_assist_build() ? 64u : 0u));
    // An assist build's roll-off, only while under way.
    if (u.assist_rolloff_wait() != 0)
        units.mix(0x524f4c4c00000000ull | static_cast<u32>(u.assist_rolloff_wait())); // "ROLL"
    if (!u.assist_pending_bp().empty()) {
        units.mix(0x50454e4400000000ull); // "PEND"
        mix_str(units, u.assist_pending_bp());
    }
    if (u.sacrifice_order() != 0) {
        units.mix(0x5341435200000000ull | u.sacrifice_order()); // "SACR"
    }
}

/// A unit's flight: its attack runs and circling, a carrier's storage and
/// landings, an idle aircraft's landing, its last vertical motion event.
void mix_unit_flight(Fnv& units, const Unit& u) {
    // A winged aircraft's attack run, only while under way.
    if (const AirCombatState& ac = u.air_combat(); ac.flying || ac.state != 0) {
        units.mix(0x4149524300000000ull | ac.state); // "AIRC"
        units.mix(ac.timeout_tick);
        units.mix(static_cast<u64>(static_cast<u32>(ac.sustained_turn_ticks)));
        if (ac.flying) {
            mix_vec(units, u.air_velocity());
        }
        // A hovering aircraft's circle, once drawn, and where it is
        // drawn about.
        const Vector3& around = ac.circle_anchor;
        if (ac.circle_reverse || ac.circle_elevation != 0.0f || ac.circle_radius_ratio != 1.0f ||
            around.x != 0 || around.y != 0 || around.z != 0) {
            units.mix(0x4349524300000000ull | (ac.circle_reverse ? 1u : 0u)); // "CIRC"
            units.mix_f32(ac.circle_elevation);
            units.mix_f32(ac.circle_radius_ratio);
            mix_vec(units, around);
        }
    }
    // What a carrier keeps inside (M206q), only when it keeps something.
    if (!u.stored_ids().empty()) {
        units.mix(0x53544f5200000000ull | u.stored_ids().size()); // "STOR"
        for (u32 id : u.stored_ids()) units.mix(id);
    }
    // Landing on carriers (M206s), only while under way: a carrier's
    // reservations and retrieve, a unit's landing.
    if (!u.storage_reserved_ids().empty() || u.next_storage_point() != 0 ||
        u.storage_overflow() != 0 || u.retrieve_phase() != 0 || u.landing_phase() != 0) {
        units.mix(0x4c414e4400000000ull); // "LAND"
        units.mix(static_cast<u64>(u.storage_reserved_ids().size()));
        for (u32 id : u.storage_reserved_ids()) units.mix(id);
        units.mix(u.next_storage_point());
        units.mix(static_cast<u64>(static_cast<u32>(u.storage_overflow())));
        units.mix(u.retrieve_phase());
        for (u32 id : u.retrieve_ids()) units.mix(id);
        units.mix(static_cast<u64>(static_cast<u32>(u.retrieve_wait())));
        units.mix(u.landing_phase());
        units.mix(static_cast<u64>(static_cast<u32>(u.landing_wait())));
    }
    // An idle aircraft's landing (AutoLandTime), only once its orders
    // ran out: since when, whether it comes down, where, and the place
    // it reserved.
    if (const Unit::IdleLanding& il = u.idle_landing();
        il.idle_since != 0 || il.descending || il.target.x != 0 || il.target.y != 0 ||
        il.target.z != 0 || !il.layer.empty() || il.reserved.x1 != il.reserved.x0 ||
        il.reserved.z1 != il.reserved.z0 || il.reserved.x0 != 0 || il.reserved.z0 != 0) {
        units.mix(0x49444c4500000000ull | (il.descending ? 1u : 0u)); // "IDLE"
        units.mix(il.idle_since);
        mix_vec(units, il.target);
        mix_str(units, il.layer);
        units.mix(path::pack_cell({il.reserved.x0, il.reserved.z0}));
        units.mix(path::pack_cell({il.reserved.x1, il.reserved.z1}));
    }
    // Its last vertical motion event (OnMotionVertEventChange tells the
    // next against it), only off Top.
    if (u.vert_event() != "Top") {
        units.mix(0x5645525400000000ull); // "VERT"
        mix_str(units, u.vert_event());
    }
}

/// What a unit's scripts set: its stun, hull facing, statistics, named
/// states, flags, multipliers, veterancy, enhancements and collision
/// detectors.
void mix_unit_script_state(Fnv& units, const Unit& u, UnitScratch& scratch) {
    // A stun, only while it lasts.
    if (u.stun_ticks() > 0)
        units.mix(0x5354554e00000000ull | static_cast<u32>(u.stun_ticks())); // "STUN"
    // The hull's facing work (CalcMoveCommon), only while set: a slaved
    // turn's hysteresis, a parked attack's facing, a turn in place.
    if (u.slaved_turning() || u.turned_in_place() || u.attack_facing().x != 0 ||
        u.attack_facing().y != 0 || u.attack_facing().z != 0) {
        units.mix(0x4641434500000000ull | (u.slaved_turning() ? 1u : 0u) | // "FACE"
                  (u.turned_in_place() ? 2u : 0u));
        mix_vec(units, u.attack_facing());
    }
    // Its statistics (KILLS and the rest its scripts keep), by name.
    if (!u.stats().empty()) {
        scratch.stats.assign(u.stats().begin(), u.stats().end());
        std::sort(scratch.stats.begin(), scratch.stats.end());
        units.mix(0x5354415400000000ull | scratch.stats.size()); // "STAT"
        for (const auto& [name, value] : scratch.stats) {
            mix_str(units, name);
            units.mix(std::bit_cast<u64>(value));
        }
    }
    // Its named states (Moving, Busy...), by name.
    if (!u.unit_states().empty()) {
        scratch.names.assign(u.unit_states().begin(), u.unit_states().end());
        std::sort(scratch.names.begin(), scratch.names.end());
        units.mix(0x5553544100000000ull | scratch.names.size()); // "USTA"
        for (const auto name : scratch.names) mix_str(units, name);
    }
    // Flags its scripts set, only those off their defaults.
    const u64 flags = (u.busy() ? 1u : 0u) | (u.block_command_queue() ? 2u : 0u) |
                      (u.can_take_damage() ? 0u : 4u) | (u.can_be_killed() ? 0u : 8u) |
                      (u.is_crashing() ? 16u : 0u) | (u.teleporting() ? 32u : 0u) |
                      (u.jostled() ? 64u : 0u);
    if (flags != 0) units.mix(0x464c414700000000ull | flags); // "FLAG"
    // Speed, acceleration and turn multipliers, only off 1.
    if (u.speed_mult() != 1.0f || u.accel_mult() != 1.0f || u.turn_mult() != 1.0f) {
        units.mix(0x4d554c5400000000ull); // "MULT"
        units.mix_f32(u.speed_mult());
        units.mix_f32(u.accel_mult());
        units.mix_f32(u.turn_mult());
    }
    // Its veterancy and last attacker, only once set; its enhancements.
    if (u.vet_level() != 0 || u.last_attacker_id() != 0) {
        units.mix(0x5645544c00000000ull | u.vet_level()); // "VETL"
        units.mix(u.last_attacker_id());
    }
    if (!u.enhancements().empty()) {
        units.mix(0x454e484300000000ull | u.enhancements().size()); // "ENHC"
        for (const auto& [slot, name] : u.enhancements()) {
            mix_str(units, slot);
            mix_str(units, name);
        }
    }
    // Its collision detectors' bones: each contact told once (OnAnimCollision).
    for (const auto& m : u.manipulators()) {
        const auto* detector = dynamic_cast<const CollisionDetectorManipulator*>(m.get());
        if (!detector || detector->is_destroyed()) continue;
        units.mix(0x434f4c4400000000ull | (detector->enabled() ? 1u : 0u) | // "COLD"
                  (detector->terrain_check() ? 2u : 0u));
        for (const auto& watch : detector->watched()) {
            units.mix(static_cast<u64>(static_cast<u32>(watch.bone)) << 2 |
                      (watch.below_foot_height ? 1u : 0u) | (watch.below_surface ? 2u : 0u));
        }
    }
}

/// A unit's layer and depth, its transport and cargo, its work (build,
/// reclaim, repair, capture, silos) and its economy.
void mix_unit_work(Fnv& units, const Unit& u) {
    mix_str(units, u.layer());
    // A sub's depth and dive (M206o), only when under or on its way, so
    // other units hash as before.
    if (u.sub_elevation() != 0.0f || u.diving() || u.surfacing()) {
        units.mix_f32(u.sub_elevation());
        units.mix((u.diving() ? 1u : 0u) | (u.surfacing() ? 2u : 0u));
    }
    units.mix(u.transport_id());
    units.mix(static_cast<u64>(u.cargo_ids().size()));
    // Who holds which of a transport's attach points (M206l).
    if (const auto* slots = u.built_transport_slots()) {
        for (const auto& slot : slots->slots()) {
            units.mix(slot.unit_id);
            units.mix(static_cast<u64>(static_cast<u32>(slot.bone)));
        }
    }
    units.mix(u.build_target_id());
    units.mix(u.reclaim_target_id());
    if (u.reclaim_wait() != 0) {
        units.mix(0x52574149u); // "RWAI"
        units.mix(static_cast<u64>(static_cast<u32>(u.reclaim_wait())));
    }
    if (!u.builder_on_target() || u.arm_awaited()) {
        units.mix(0x41524d54u); // "ARMT"
        units.mix((u.builder_on_target() ? 1u : 0u) | (u.arm_awaited() ? 2u : 0u));
    }
    units.mix(u.repair_target_id());
    units.mix(u.capture_target_id());
    units.mix_f32(u.work_progress());
    units.mix(static_cast<u64>(static_cast<u32>(u.nuke_silo_ammo())));
    units.mix(static_cast<u64>(static_cast<u32>(u.tactical_silo_ammo())));
    units.mix(static_cast<u64>(static_cast<u32>(u.silo_build().weapon)));
    units.mix_f32(static_cast<f32>(u.silo_build().progress));
    if (u.silo_blocks() != 0) {
        units.mix(0x53424c4bu); // "SBLK"
        units.mix(static_cast<u64>(static_cast<u32>(u.silo_blocks())));
    }
    const auto& econ = u.economy();
    units.mix_f32(static_cast<f32>(econ.consumption_mass));
    units.mix_f32(static_cast<f32>(econ.consumption_energy));
    units.mix_f32(static_cast<f32>(econ.production_mass));
    units.mix_f32(static_cast<f32>(econ.production_energy));
    // Docked at a staging platform (M206r), only then: its tank and its
    // repair's ask.
    if (u.refuel_started() || econ.dock_repair_energy != 0 || econ.dock_repair_mass != 0) {
        units.mix(0x4655454c00000000ull); // "FUEL"
        units.mix_f32(u.fuel_ratio());
        units.mix_f32(static_cast<f32>(econ.dock_repair_energy));
        units.mix_f32(static_cast<f32>(econ.dock_repair_mass));
    }
}

/// One order in a unit's queue, with the progress it keeps. `need_unpack`:
/// the unit packs up before it acts (AI.NeedUnpack).
void mix_command(Fnv& orders, const UnitCommand& cmd, bool need_unpack) {
    orders.mix(static_cast<u64>(cmd.type));
    orders.mix(cmd.target_id);
    mix_vec(orders, cmd.target_pos);
    orders.mix(cmd.command_id);
    mix_str(orders, cmd.blueprint_id);
    orders.mix((cmd.launched ? 1u : 0u) | (cmd.started ? 2u : 0u) | (cmd.approached ? 4u : 0u) |
               (cmd.in_band ? 8u : 0u));
    orders.mix(cmd.beacon_id);
    orders.mix(cmd.assigned_id);
    // A factory rolling its unit off, only then (as the cargo below).
    if (cmd.rolloff_wait != 0) {
        orders.mix(0x524f4c4cu); // "ROLL"
        orders.mix(static_cast<u64>(static_cast<u32>(cmd.rolloff_wait)));
    }
    // A factory build raised past one unit, only then: what it still
    // makes, and what it goes round with.
    if (cmd.count != 1 || cmd.max_count != 1) {
        orders.mix(0x434e5453u); // "CNTS"
        orders.mix(static_cast<u64>(static_cast<u32>(cmd.count)) << 32 |
                   static_cast<u32>(cmd.max_count));
    }
    // A task waiting out its army's unit cap or a pause, only then.
    if (cmd.task_wait != 0) {
        orders.mix(0x43415057u); // "CAPW"
        orders.mix(static_cast<u64>(static_cast<u32>(cmd.task_wait)));
    }
    if (cmd.sacrifice_wait != 0) {
        orders.mix(0x53414357u); // "SACW"
        orders.mix(static_cast<u64>(static_cast<u32>(cmd.sacrifice_wait)));
    }
    // A refuel under way (M206r), only once it has a slot or waits.
    if (cmd.dock_phase != DockPhase::Reserve || cmd.dock_wait != 0) {
        orders.mix(0x444f434bu); // "DOCK"
        orders.mix(static_cast<u64>(cmd.dock_phase));
        orders.mix(static_cast<u64>(static_cast<u32>(cmd.dock_wait)));
    }
    // An order a NeedUnpack unit has begun (its pack-up done), only
    // then: other units' orders hash as before.
    if (cmd.begun && need_unpack) orders.mix(0x4245474eu); // "BEGN"
    // A guard's fight and its way home, only then.
    if (cmd.from_guard || cmd.guard_returning) {
        orders.mix(0x47415244u); // "GARD"
        orders.mix((cmd.from_guard ? 1u : 0u) | (cmd.guard_returning ? 2u : 0u));
    }
    // A guard's or patrol's fight's leash, only once it has one.
    const Vector3& anchor = cmd.leash_anchor_pos;
    if (cmd.leash_armed || cmd.leash_anchor_id != 0 || anchor.x != 0 || anchor.y != 0 ||
        anchor.z != 0) {
        orders.mix(0x4c455348u); // "LESH"
        orders.mix(cmd.leash_armed ? 1u : 0u);
        orders.mix(cmd.leash_anchor_id);
        mix_vec(orders, cmd.leash_anchor_pos);
    }
    if (cmd.from_patrol || !cmd.patrol_claimed.empty()) {
        orders.mix(0x5054524cu); // "PTRL"
        orders.mix(cmd.from_patrol ? 1u : 0u);
        orders.mix(static_cast<u64>(cmd.patrol_claimed.size()));
        for (u32 id : cmd.patrol_claimed) {
            orders.mix(id);
        }
    }
    const Vector3& from = cmd.patrol_from;
    if (from.x != 0 || from.y != 0 || from.z != 0) {
        orders.mix(0x5046524du); // "PFRM"
        mix_vec(orders, from);
    }
    // A carrier's launch under way (M206q), only then.
    if (cmd.launch_wait >= 0) {
        orders.mix(0x4c4e4348u); // "LNCH"
        orders.mix(static_cast<u64>(static_cast<u32>(cmd.launch_wait)));
        orders.mix(static_cast<u64>(cmd.launch_queue.size()));
        for (u32 id : cmd.launch_queue) orders.mix(id);
    }
    // A formation's slot, pace and facing, only for a formation order.
    if (cmd.formed || cmd.speed_cap != 0 || cmd.has_facing || !cmd.formation.empty()) {
        orders.mix(0x464f524du | (cmd.formed ? 1ull << 32 : 0u) | // "FORM"
                   (cmd.has_facing ? 2ull << 32 : 0u));
        orders.mix_f32(cmd.speed_cap);
        orders.mix_f32(cmd.facing);
        mix_str(orders, cmd.formation);
    }
    // A FormAttack, and its slot until it is dropped, only then.
    if (cmd.form_attack) {
        orders.mix(0x46415454u | (cmd.has_slot ? 1ull << 32 : 0u)); // "FATT"
        if (cmd.has_slot) mix_vec(orders, cmd.slot_offset);
    }
    if (cmd.target_point >= 0) {
        orders.mix(0x54504e54u); // "TPNT"
        orders.mix(static_cast<u64>(static_cast<u32>(cmd.target_point)));
    }
    // A winged attack within reach, flying its runs, only then.
    if (cmd.engaged) orders.mix(0x454e4744u); // "ENGD"
    // A patrol's or guard's wait before it next looks about, only
    // while it waits.
    if (cmd.patrol_scan != 0) {
        orders.mix(0x5053434eu); // "PSCN"
        orders.mix(static_cast<u64>(static_cast<u32>(cmd.patrol_scan)));
    }
    // An attack's facing clock (its SetFacing every 9 ticks), only while
    // it runs.
    if (cmd.facing_clock != 0) {
        orders.mix(0x46434c4bu); // "FCLK"
        orders.mix(static_cast<u64>(static_cast<u32>(cmd.facing_clock)));
    }
    // A specific unload's cargo; only when set, so other orders hash
    // as before it existed.
    if (!cmd.unload_ids.empty()) {
        orders.mix(static_cast<u64>(cmd.unload_ids.size()));
        for (u32 id : cmd.unload_ids) orders.mix(id);
    }
}

/// A unit's orders: its queue, a transport's pickup, a factory's rally
/// orders.
void mix_orders(Fnv& orders, const Unit& u) {
    orders.mix(u.entity_id());
    orders.mix(static_cast<u64>(u.command_queue().size()));
    for (const UnitCommand& cmd : u.command_queue()) mix_command(orders, cmd, u.need_unpack());
    // A transport's pickup and a unit's beam up (M206m), only while
    // under way.
    if (u.pickup_running() || u.beam_up_ticks() > 0) {
        orders.mix(0x5049434bu); // "PICK"
        orders.mix((u.pickup_running() ? 1u : 0u) | (u.pickup_ready() ? 2u : 0u));
        orders.mix(static_cast<u64>(static_cast<u32>(u.pickup_ticks())));
        orders.mix(static_cast<u64>(u.pickup_ids().size()));
        for (const u32 id : u.pickup_ids()) orders.mix(id);
        orders.mix(static_cast<u64>(static_cast<u32>(u.beam_up_ticks())));
    }
    // A factory's rally orders (M206j), only where it has some.
    if (!u.rally_orders().empty()) {
        orders.mix(0x52414c4cu); // "RALL": apart from the queue above
        orders.mix(static_cast<u64>(u.rally_orders().size()));
        for (const UnitCommand& cmd : u.rally_orders()) {
            orders.mix(static_cast<u64>(cmd.type));
            orders.mix(cmd.target_id);
            mix_vec(orders, cmd.target_pos);
            orders.mix(cmd.command_id);
        }
    }
}

/// A unit's movement: its navigator, velocity, heading and height, and
/// Moho pathing's state when it is on.
void mix_navigation(Fnv& navigation, const Unit& u, bool moho_pathing) {
    navigation.mix(u.entity_id());
    navigation.mix(static_cast<u64>(u.navigator().status()));
    mix_vec(navigation, u.navigator().goal());
    mix_vec(navigation, u.velocity());
    navigation.mix_f32(u.heading());
    navigation.mix_f32(u.elevation_target());
    // Moho pathing's (--moho-pathing), only with it on: its navigator and finder.
    if (moho_pathing) {
        navigation.mix(0x4d4f484f00000000ull); // "MOHO"
        u.navigator().fingerprint_moho(navigation);
    }
}

/// A weapon: its target, ground target, clocks, the values its scripts
/// may change, and its aim spot.
void mix_weapon(Fnv& weapons, const Weapon& w) {
    weapons.mix(w.target_entity_id);
    weapons.mix((w.has_ground_target ? 1u : 0u) | (w.enabled ? 2u : 0u));
    if (w.has_ground_target) mix_vec(weapons, w.ground_target);
    weapons.mix(w.fire_clock);
    // When it next looks for a target.
    weapons.mix(w.target_check_clock);
    // A ground attack's order and shots (AttackGroundTries), only
    // while it has them.
    if (w.ground_from_order || w.shots_at_target != 0 || w.last_order_point) {
        weapons.mix(0x47524e44u | (w.ground_from_order ? 1ull << 32 : 0u) | // "GRND"
                    (w.last_order_point ? 2ull << 32 : 0u));
        weapons.mix(w.shots_at_target);
        if (w.last_order_point) mix_vec(weapons, *w.last_order_point);
    }
    // What its scripts may change (ChangeMaxRadius, ChangeRateOfFire,
    // ChangeDamage, SetFireTargetLayerCaps...).
    weapons.mix_f32(w.max_range);
    weapons.mix_f32(w.min_range);
    weapons.mix_f32(w.rate_of_fire);
    weapons.mix_f32(w.damage);
    weapons.mix_f32(w.damage_radius);
    weapons.mix(w.fire_target_layer_caps);
    // Its aim spot, unless it is the centre of its target now: off
    // that, where it shoots, and whether it draws a new one.
    if (w.aim_spot != -1 || w.aim_spot_target != w.target_entity_id) {
        weapons.mix(0x41494d5300000000ull); // "AIMS"
        weapons.mix(static_cast<u32>(w.aim_spot));
        weapons.mix(w.aim_spot_target);
    }
}

} // namespace

std::array<u64, SimState::ChecksumParts::kCount> SimState::ChecksumParts::values() const {
    return {rng,     armies,      entities, units,          orders,  navigation,
            weapons, projectiles, shields,  economy_events, threads, terrain};
}

u32 SimState::ChecksumParts::total() const {
    Fnv f;
    for (const u64 part : values()) f.mix(part);
    return static_cast<u32>(f.h ^ (f.h >> 32));
}

SimState::ChecksumParts SimState::checksum_parts() const {
    ChecksumParts parts;
    parts.rng = sim_random_.digest();

    Fnv armies;
    armies.mix(tick_count_);
    for (const auto& a : armies_) mix_army(armies, *a);
    // The jammers' fakes each army holds, only where any: their places
    // about their jammers and which it knows fake (a known fake adds no
    // threat to its influence map).
    if (!jam_offsets_.empty()) {
        armies.mix(0x4a414d5300000000ull | jam_offsets_.size()); // "JAMS"
        for (const auto& [key, offsets] : jam_offsets_) {
            armies.mix(key);
            for (const Vector3& o : offsets) mix_vec(armies, o);
            const auto known = jam_known_.find(key);
            armies.mix(known == jam_known_.end() ? 0 : known->second);
            const auto sensed = jam_sensed_.find(key);
            armies.mix(sensed == jam_sensed_.end() ? 0 : sensed->second);
        }
    }
    parts.armies = armies.h;

    // The registry walks in id order.
    Fnv entities, units, orders, navigation, weapons, projectiles, shields;
    UnitScratch scratch;
    entity_registry_.for_each([&](const Entity& e) {
        mix_entity(entities, e);
        if (e.is_projectile()) mix_projectile(projectiles, static_cast<const Projectile&>(e));
        if (e.is_shield()) {
            shields.mix(e.entity_id());
            shields.mix(static_cast<const Shield&>(e).is_on ? 1u : 0u);
        }
        if (!e.is_unit()) return;
        const auto& u = static_cast<const Unit&>(e);
        mix_unit_settings(units, u);
        mix_unit_flight(units, u);
        mix_unit_script_state(units, u, scratch);
        mix_unit_work(units, u);
        mix_orders(orders, u);
        mix_navigation(navigation, u, moho_pathing_);
        weapons.mix(e.entity_id());
        for (const auto& w : u.weapons()) mix_weapon(weapons, *w);
    });
    // Each army's searches, waiting and in flight, and each footprint
    // class's dirty clusters: Moho pathing's, only with it on (the tables'
    // background work runs either way, but only its searches read them).
    if (moho_pathing_ && path_tables_) {
        navigation.mix(0x5041544800000000ull); // "PATH"
        for (const auto& a : armies_) a->path_queue().fingerprint(navigation);
        path_tables_->fingerprint(navigation);
    }
    parts.entities = entities.h;
    parts.units = units.h;
    parts.orders = orders.h;
    parts.navigation = navigation.h;
    parts.weapons = weapons.h;
    parts.projectiles = projectiles.h;
    parts.shields = shields.h;

    Fnv events;
    economy_events_.for_each([&](const EconomyEvent& evt) {
        events.mix(evt.unit_id());
        events.mix_f32(static_cast<f32>(evt.progress()));
        events.mix((evt.is_done() ? 1u : 0u) | (evt.is_cancelled() ? 2u : 0u));
    });
    parts.economy_events = events.h;

    Fnv threads;
    thread_manager_.for_each_live([&](u64 serial, i32 wake) {
        threads.mix(serial);
        threads.mix(static_cast<u64>(static_cast<u32>(wake)));
    });
    parts.threads = threads.h;

    // The ground: two games whose heights differ anywhere (a flattening, a
    // load that restored it otherwise) differ here, away from any unit.
    parts.terrain = terrain_ ? terrain_->heightmap().digest() : 0;
    return parts;
}

} // namespace osc::sim
