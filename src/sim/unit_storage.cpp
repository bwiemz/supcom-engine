// Carrier storage (M206q): the units a carrier keeps inside -- the aircraft
// it builds -- as Moho's CAiTransportImpl does (faf-re
// TransportAddToStorage, TransportRemoveFromStorage,
// TransportHasAvailableStorage, and the CUnitCarrierLaunch task).

#include "sim/unit.hpp"

#include "core/dmath.hpp"
#include "map/terrain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace osc::sim {

namespace {

/// Ticks between two launches. CUnitCarrierLaunch returns 3 after each,
/// and Moho's task thread then waits 3 - 1 ticks (CTaskThread: 0 runs on in
/// the same tick, 1 the next, n the n - 1th).
constexpr i32 kLaunchInterval = 2;

Unit* live_unit(EntityRegistry& registry, u32 id) {
    Entity* e = registry.find(id);
    return e && !e->destroyed() && e->is_unit() ? static_cast<Unit*>(e) : nullptr;
}

} // namespace

bool Unit::is_stored_unit(u32 id) const {
    return std::find(stored_ids_.begin(), stored_ids_.end(), id) != stored_ids_.end();
}

bool Unit::transport_has_available_storage() const {
    // Moho counts the places reserved for aircraft landing (M206s) as taken.
    return static_cast<i32>(stored_ids_.size() + storage_reserved_.size()) < storage_slots_;
}

std::optional<Unit::StoragePlace> Unit::reserve_storage(u32 unit_id) {
    // TransportReserveStorage: the next generic attach point, round robin.
    // Each pass round adds 3 ticks (modulo 50) to the wait of those after.
    const TransportSlots* slots = transport_slots();
    const std::vector<i32> points = slots ? slots->generic_bones() : std::vector<i32>{};
    if (points.empty()) return std::nullopt;
    if (std::find(storage_reserved_.begin(), storage_reserved_.end(), unit_id) ==
        storage_reserved_.end())
        storage_reserved_.push_back(unit_id);
    const i32 bone = points[next_generic_ % points.size()];
    StoragePlace place;
    place.point = bone_world_position(bone);
    const Quaternion facing = quat_multiply(orientation(), bone_pose(bone).rotation);
    const Vector3 forward = quat_rotate(facing, Vector3{0.0f, 0.0f, 1.0f});
    place.heading = forward.x * forward.x + forward.z * forward.z > 1e-6f
                        ? dmath::atan2(forward.x, forward.z)
                        : quat_yaw(orientation());
    place.height = place.point.y - position().y;
    place.delay = generic_overflow_;
    next_generic_ = (next_generic_ + 1) % static_cast<u32>(points.size());
    if (next_generic_ == 0) generic_overflow_ = (generic_overflow_ + 3) % 50;
    return place;
}

void Unit::clear_storage_reservation(u32 unit_id) {
    storage_reserved_.erase(
        std::remove(storage_reserved_.begin(), storage_reserved_.end(), unit_id),
        storage_reserved_.end());
}

void Unit::reset_storage_reservation() {
    next_generic_ = 0;
    launch_index_ = 0;
    generic_overflow_ = 0;
}

void Unit::add_to_storage(Unit& unit, EntityRegistry& registry, lua_State* L) {
    if (&unit == this || is_stored_unit(unit.entity_id())) return;
    // The script first (retail's hides a carried unit and makes it
    // untouchable), then it rides at the carrier's centre.
    const u32 id = unit.entity_id();
    unit.call_lua_method_with_entity(L, "OnAddToStorage", this);
    if (!live_unit(registry, id) || destroyed()) return;
    clear_storage_reservation(id); // landed (M206s)
    unit.transport_id_ = entity_id();
    unit.set_unit_state("Attached", true);
    unit.navigator_.abort_move();
    unit.ground_speed_ = 0;
    unit.hang_from(*this);
    unit.note_snap();
    stored_ids_.push_back(id);
}

void Unit::remove_from_storage(Unit& unit, EntityRegistry& registry, lua_State* L,
                               const map::Terrain* terrain) {
    if (!is_stored_unit(unit.entity_id())) return;
    const u32 id = unit.entity_id();
    unit.call_lua_method_with_entity(L, "OnRemoveFromStorage", this);
    forget_stored(id);
    if (!live_unit(registry, id)) return;
    unit.transport_id_ = 0;
    unit.set_unit_state("Attached", false);

    // Out at the next launch bone, facing as it does (else at the carrier).
    Vector3 at = position();
    Quaternion facing = orientation();
    if (const TransportSlots* slots = transport_slots()) {
        const std::vector<i32> bones = slots->launch_bones();
        if (!bones.empty()) {
            launch_index_ = (launch_index_ + 1) % static_cast<u32>(bones.size());
            const i32 bone = bones[launch_index_];
            at = bone_world_position(bone);
            facing = quat_multiply(orientation(), bone_pose(bone).rotation);
        }
    }
    unit.set_position(at);
    unit.set_orientation(facing);
    unit.note_snap();
    // Moving off at once along its facing, at its top speed.
    const Vector3 forward = quat_rotate(facing, Vector3{0.0f, 0.0f, 1.0f});
    if (forward.x * forward.x + forward.z * forward.z > 1e-6f)
        unit.set_heading(dmath::atan2(forward.x, forward.z));
    if (unit.is_air_unit()) {
        unit.current_airspeed_ = unit.max_airspeed_;
        // It flies on from the height it left at: the air navigator holds an
        // aircraft's height over its air floor.
        if (terrain) unit.current_altitude_ = at.y - unit.air_floor(terrain, at.x, at.z);
    }
}

void Unit::forget_stored(u32 id) {
    stored_ids_.erase(std::remove(stored_ids_.begin(), stored_ids_.end(), id), stored_ids_.end());
}

bool Unit::launches_on_unload() const {
    return !stored_ids_.empty() && has_category("CARRIER") &&
           (has_category("AIRSTAGINGPLATFORM") || has_category("PODSTAGINGPLATFORM"));
}

OrderStep Unit::order_carrier_launch(UnitCommand& cmd, SimContext& ctx) {
    auto& registry = ctx.registry;
    // CUnitCarrierLaunch: the units to launch -- those the order names that
    // are stored, else all of them -- are ordered to its point, then leave
    // one every few ticks. (Moho gives them a ground Guard there; units here
    // have no ground guard, so they move to it.)
    if (cmd.launch_wait < 0) {
        for (u32 id : stored_ids_)
            if (cmd.unload_ids.empty() ||
                std::find(cmd.unload_ids.begin(), cmd.unload_ids.end(), id) != cmd.unload_ids.end())
                cmd.launch_queue.push_back(id);
        UnitCommand move;
        move.type = CommandType::Move;
        move.target_pos = cmd.target_pos;
        for (u32 id : cmd.launch_queue)
            if (Unit* u = live_unit(registry, id)) u->push_command(move, true);
        // The first leaves the next tick: Moho's task gets ready (returning
        // 1), then orders the units and launches at once (returning 0).
        cmd.launch_wait = 2;
    }
    set_unit_state("TransportUnloading", !cmd.launch_queue.empty());
    if (!cmd.launch_queue.empty() && --cmd.launch_wait <= 0) {
        const u32 id = cmd.launch_queue.front();
        cmd.launch_queue.erase(cmd.launch_queue.begin());
        cmd.launch_wait = kLaunchInterval;
        // Scripts run: the order (and cmd with it) may be gone afterwards.
        const UnitCommand* head = &cmd;
        if (Unit* u = live_unit(registry, id); u && is_stored_unit(id))
            remove_from_storage(*u, registry, ctx.L, ctx.terrain);
        if (destroyed() || !in_registry()) return OrderStep::Gone;
        if (command_queue_.empty() || &command_queue_.front() != head) return OrderStep::Next;
    }
    if (!cmd.launch_queue.empty()) return OrderStep::Hold;
    set_unit_state("TransportUnloading", false);
    command_queue_.pop_front();
    return OrderStep::Next;
}

OrderStep Unit::order_carrier_retrieve(UnitCommand& cmd, SimContext& ctx) {
    auto& registry = ctx.registry;
    const u32 me = entity_id();
    const u32 order_id = cmd.command_id;
    const auto ends = [&](bool complete) {
        end_retrieve(complete, ctx);
        if (destroyed() || !in_registry()) return OrderStep::Gone;
        if (!command_queue_.empty() && &command_queue_.front() == &cmd &&
            command_queue_.front().command_id == order_id)
            command_queue_.pop_front();
        return OrderStep::Next;
    };
    if (retrieve_phase_ == RetrievePhase::None) {
        // The units ordered aboard (the order's other units): the army's with
        // this load order onto this carrier that aren't aboard something.
        retrieve_ids_.clear();
        registry.for_each_unit([&](Entity& e) {
            if (e.destroyed() || !e.is_unit() || e.army() != army() || e.entity_id() == me) return;
            const auto& u = static_cast<const Unit&>(e);
            if (u.transport_id() != 0) return;
            if (std::any_of(u.command_queue_.begin(), u.command_queue_.end(),
                            [&](const UnitCommand& c) {
                                return c.type == CommandType::TransportLoad && c.target_id == me &&
                                       c.command_id == order_id;
                            }))
                retrieve_ids_.push_back(u.entity_id());
        });
        call_lua_method(ctx.L, "OnStartTransportLoading");
        if (destroyed() || !in_registry()) return OrderStep::Gone;
        if (command_queue_.empty() || &command_queue_.front() != &cmd) return OrderStep::Next;
        set_unit_state("TransportLoading", true);
        retrieve_phase_ = RetrievePhase::Gather;
        return OrderStep::Hold;
    }
    // Diving ends it, once under way.
    if (retrieve_phase_ == RetrievePhase::Watch && diving()) return ends(false);
    if (retrieve_phase_ == RetrievePhase::Gather) {
        // Until each of them has this order in hand; then a submerged carrier
        // surfaces, and any other stops.
        for (const u32 id : retrieve_ids_) {
            const Entity* e = registry.find(id);
            const auto* u =
                e && !e->destroyed() && e->is_unit() ? static_cast<const Unit*>(e) : nullptr;
            if (!u || u->is_dying() || u->is_being_built() || u->transport_id() != 0) continue;
            if (u->command_queue_.empty() || u->command_queue_.front().command_id != order_id)
                return OrderStep::Hold;
        }
        if (motion_type_ == "RULEUMT_SurfacingSub" && layer_ == "Sub") {
            start_surfacing(ctx.L);
            if (destroyed() || !in_registry()) return OrderStep::Gone;
            retrieve_phase_ = RetrievePhase::Surface;
            return OrderStep::Hold;
        }
        navigator_.abort_move();
        retrieve_phase_ = RetrievePhase::Watch;
        return OrderStep::Hold;
    }
    if (retrieve_phase_ == RetrievePhase::Surface) {
        if (layer_ == "Water" || layer_ == "Air") {
            reset_storage_reservation();
            retrieve_phase_ = RetrievePhase::Watch;
        }
        return OrderStep::Hold;
    }
    // Watch: every 9 ticks, those no longer landing here are let go; with
    // none left, it is done.
    if (retrieve_wait_ > 0) {
        --retrieve_wait_;
        if (retrieve_wait_ > 0) return OrderStep::Hold;
    }
    std::erase_if(retrieve_ids_, [&](u32 id) {
        const Entity* e = registry.find(id);
        const auto* u =
            e && !e->destroyed() && e->is_unit() ? static_cast<const Unit*>(e) : nullptr;
        return !u || u->is_dying() || !u->has_unit_state("TransportLoading") || !u->lands_on(me);
    });
    if (retrieve_ids_.empty()) return ends(true);
    retrieve_wait_ = 9;
    return OrderStep::Hold;
}

void Unit::end_retrieve(bool complete, SimContext& ctx) {
    retrieve_phase_ = RetrievePhase::None;
    retrieve_wait_ = 0;
    const std::vector<u32> left = std::exchange(retrieve_ids_, {});
    call_lua_method(ctx.L, "OnStopTransportLoading");
    if (destroyed() || !in_registry()) return;
    reset_storage_reservation();
    set_unit_state("TransportLoading", false);
    if (complete) return;
    for (const u32 id : left)
        if (Unit* u = live_unit(ctx.registry, id); u && !u->is_dying()) u->navigator_.abort_move();
}

} // namespace osc::sim
