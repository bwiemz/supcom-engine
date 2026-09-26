// Carrier storage (M206q): the units a carrier keeps inside -- the aircraft
// it builds -- as Moho's CAiTransportImpl does (faf-re
// TransportAddToStorage, TransportRemoveFromStorage,
// TransportHasAvailableStorage, and the CUnitCarrierLaunch task).

#include "sim/unit.hpp"

#include "core/dmath.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"

#include <algorithm>

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
    // Moho counts storage reservations too; units aren't reserved storage
    // here (landing on a carrier isn't modelled), so only the stored count.
    return static_cast<i32>(stored_ids_.size()) < storage_slots_;
}

void Unit::add_to_storage(Unit& unit, EntityRegistry& registry, lua_State* L) {
    if (&unit == this || is_stored_unit(unit.entity_id())) return;
    // The script first (retail's hides a carried unit and makes it
    // untouchable), then it rides at the carrier's centre.
    const u32 id = unit.entity_id();
    unit.call_lua_method_with_entity(L, "OnAddToStorage", this);
    if (!live_unit(registry, id) || destroyed()) return;
    unit.transport_id_ = entity_id();
    unit.set_unit_state("Attached", true);
    unit.navigator_.abort_move();
    unit.ground_speed_ = 0;
    unit.hang_from(*this);
    unit.note_snap();
    stored_ids_.push_back(id);
}

void Unit::remove_from_storage(Unit& unit, EntityRegistry& registry, lua_State* L) {
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
    if (unit.is_air_unit()) unit.current_airspeed_ = unit.max_airspeed_;
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
            remove_from_storage(*u, registry, ctx.L);
        if (destroyed() || !in_registry()) return OrderStep::Gone;
        if (command_queue_.empty() || &command_queue_.front() != head) return OrderStep::Next;
    }
    if (!cmd.launch_queue.empty()) return OrderStep::Hold;
    set_unit_state("TransportUnloading", false);
    command_queue_.pop_front();
    return OrderStep::Next;
}

} // namespace osc::sim
