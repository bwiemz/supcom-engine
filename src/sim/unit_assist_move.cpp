// The Transport order given to air transports with land units (Moho's
// UNITCOMMAND_AssistMove): the dispatch's arm for it
// (IAiCommandDispatchImpl), a transport's CUnitAssistMoveTask, and
// Unit::NeedsPickup. The pickup, the boarding and the drop are the transport
// orders' own (M206m, M206n).

#include "sim/unit.hpp"

#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace osc::sim {

namespace {

const CategoryName& transportation() {
    static const CategoryName name{"TRANSPORTATION"};
    return name;
}

/// MOBILE and LAND: the units the order's transports carry.
bool mobile_land(const Unit& u) {
    static const CategoryName kMobile{"MOBILE"};
    static const CategoryName kLand{"LAND"};
    return u.has_category(kMobile) && u.has_category(kLand);
}

f32 distance_xz(const Vector3& a, const Vector3& b) {
    const f32 dx = a.x - b.x;
    const f32 dz = a.z - b.z;
    return std::sqrt(dx * dx + dz * dz);
}

/// Its order is `cmd`'s: an AssistMove with its command id.
bool holds(const UnitCommand& c, const UnitCommand& cmd) {
    return c.type == CommandType::AssistMove && c.command_id == cmd.command_id;
}

} // namespace

OrderStep Unit::order_assist_move(UnitCommand& cmd, SimContext& ctx) {
    if (cmd.assist_phase == AssistPhase::Start) {
        // IAiCommandDispatchImpl's arm: the order's units split into
        // transports and mobile land units. Only with both does it do
        // anything: a transport runs CUnitAssistMoveTask, and anything else
        // drives to the goal, ready to board a transport that takes it
        // (NewMoveTask, its transport check set).
        bool transports = false;
        bool land = false;
        ctx.registry.for_each_unit([&](const Entity& e) {
            if (e.destroyed() || (transports && land)) return;
            const auto& u = static_cast<const Unit&>(e);
            if (std::none_of(u.command_queue_.begin(), u.command_queue_.end(),
                             [&](const UnitCommand& c) { return holds(c, cmd); })) {
                return;
            }
            if (u.has_category(transportation())) {
                transports = true;
            } else if (mobile_land(u)) {
                land = true;
            }
        });
        if (!transports || !land) {
            command_queue_.pop_front();
            return OrderStep::Next;
        }
        if (!has_category(transportation())) {
            cmd.assist_phase = AssistPhase::Walking;
        } else {
            // CUnitAssistMoveTask's constructor: UNITSTATE_AssistMoving, and
            // done at once without a transport's AI.
            set_unit_state("AssistMoving", true);
            if (!has_transport_ai()) {
                command_queue_.pop_front();
                end_assist_move(ctx);
                return destroyed() || !in_registry() ? OrderStep::Gone : OrderStep::Next;
            }
            cmd.assist_phase = AssistPhase::Preparing;
        }
    }
    if (cmd.assist_phase != AssistPhase::Walking) {
        return order_assist_transport(cmd, ctx);
    }
    // A land unit's: to the goal, unless a transport takes it (Moho's
    // AssignedTransportRef, which its move task hears of as
    // AINAVEVENT_ResumeTask): then it calls that transport, and the order
    // ends as it boards, or as the transport goes without it.
    if (cmd.assigned_id) {
        return order_call_transport(cmd, ctx);
    }
    return order_move(cmd, ctx);
}

OrderStep Unit::order_assist_transport(UnitCommand& cmd, SimContext& ctx) {
    lua_State* L = ctx.L;
    const EntityId me = entity_id();
    // Scripts run on the way (the pickup's, the drop's) may clear the queue
    // (and cmd with it) or replace it: the order goes on only while it is
    // still the head.
    const u32 order_id = cmd.command_id;
    const auto still_head = [&] {
        return !command_queue_.empty() && &command_queue_.front() == &cmd &&
               command_queue_.front().command_id == order_id;
    };
    for (;;) {
        switch (cmd.assist_phase) {
        case AssistPhase::Start:
        case AssistPhase::Walking: return OrderStep::Hold; // (not a transport's)
        case AssistPhase::Preparing:
            // TaskTick's TASKSTATE_Preparing: it looks 2 ticks on (it returns 3).
            cmd.task_wait = 2;
            cmd.assist_phase = AssistPhase::Waiting;
            return OrderStep::Hold;
        case AssistPhase::Waiting: {
            if (cmd.task_wait > 0) {
                --cmd.task_wait;
                if (cmd.task_wait > 0) return OrderStep::Hold;
            }
            // CUnitAssistMoveTask::Wait: the units to pick up, and a load
            // (CUnitLoadUnits) for them.
            bool pathfinding = false;
            std::vector<Unit*> pickups = assist_pickups(cmd, ctx, pathfinding);
            if (!pickups.empty()) {
                // The load's constructor: TransportLoading, OnStartTransportLoading.
                set_unit_state("TransportLoading", true);
                call_lua_method(L, "OnStartTransportLoading");
                if (destroyed() || !in_registry()) return OrderStep::Gone;
                if (!still_head()) {
                    finish_pickup(false, L);
                    return destroyed() || !in_registry() ? OrderStep::Gone : OrderStep::Next;
                }
                load_requested_.clear();
                for (const Unit* u : pickups) {
                    load_requested_.push_back(u->entity_id());
                }
                cmd.assist_phase = AssistPhase::LoadPreparing;
                return OrderStep::Hold;
            }
            // One still waiting for its path may need a lift: look again.
            if (pathfinding) {
                cmd.assist_phase = AssistPhase::Preparing;
                return OrderStep::Hold;
            }
            // None left to pick up: set down what it carries, else go to the
            // goal (TASKSTATE_Starting, TASKSTATE_Processing).
            if (cargo_ids_.empty()) {
                cmd.assist_phase = AssistPhase::Finishing;
                // Its own pickups' stops are not the move's failure.
                if (aborted_command_ == order_id) aborted_command_ = 0;
            } else {
                cmd.assist_phase = AssistPhase::Unloading;
            }
            return OrderStep::Hold;
        }
        case AssistPhase::LoadPreparing: {
            // CUnitLoadUnits::DoTask, on the load's first tick.
            std::vector<Unit*> pickups;
            for (const EntityId id : std::exchange(load_requested_, {})) {
                Entity* e = ctx.registry.find(id);
                if (e && e->is_unit()) {
                    pickups.push_back(static_cast<Unit*>(e));
                }
            }
            std::erase_if(pickups, [](const Unit* u) {
                if (u->destroyed() || u->is_dying() || u->transport_id() || u->parent_entity_id() ||
                    u->awaited_transport()) {
                    return true;
                }
                const auto& q = u->command_queue_;
                return !q.empty() && q.front().type == CommandType::WaitForFerry &&
                       q.front().assigned_id;
            });
            if (!begin_pickup(pickups, false, L)) {
                return OrderStep::Gone;
            }
            if (!still_head()) {
                finish_pickup(false, L);
                return destroyed() || !in_registry() ? OrderStep::Gone : OrderStep::Next;
            }
            // Each taken unit's AssignedTransportRef.
            for (const EntityId id : pickup_ids_) {
                Entity* e = ctx.registry.find(id);
                if (!e || e->destroyed() || !e->is_unit()) {
                    continue;
                }
                auto& q = static_cast<Unit*>(e)->command_queue_;
                if (!q.empty() && q.front().type == CommandType::AssistMove) {
                    q.front().assigned_id = me;
                }
            }
            if (pickup_ids_.empty()) {
                finish_pickup(false, L);
                cmd.assist_phase = AssistPhase::Waiting;
                return destroyed() || !in_registry() ? OrderStep::Gone : OrderStep::Hold;
            }
            // TASKSTATE_Waiting in the same tick (NewMoveTask).
            cmd.assist_phase = AssistPhase::Loading;
            continue;
        }
        case AssistPhase::Loading: {
            const PickupStep step = pickup_step(ctx);
            if (step == PickupStep::Gone) return OrderStep::Gone;
            if (step == PickupStep::Running) return OrderStep::Hold;
            if (!still_head()) return OrderStep::Next;
            // The load is over: the task looks again in the same tick.
            cmd.assist_phase = AssistPhase::Waiting;
            cmd.task_wait = 0;
            continue;
        }
        case AssistPhase::Unloading: {
            // CUnitAssistMoveTask::Start: the drop is where the order goes
            // (the mean of its carried units' places for the order, when any
            // still hold it, which is the same place: the order has no
            // formation). Then CUnitUnloadUnits sets down all of it.
            const UnloadStep step = unload_at(cmd.target_pos, {}, ctx);
            if (step == UnloadStep::Gone) return OrderStep::Gone;
            if (step == UnloadStep::Holding) return OrderStep::Hold;
            if (!still_head()) return OrderStep::Next;
            cmd.assist_phase = AssistPhase::Preparing;
            continue;
        }
        case AssistPhase::Finishing: {
            // IssueMoveTaskAndComplete: to the goal, and the order is done
            // there.
            const OrderStep step = order_move(cmd, ctx);
            if (step != OrderStep::Next) return step;
            end_assist_move(ctx);
            return destroyed() || !in_registry() ? OrderStep::Gone : OrderStep::Next;
        }
        }
    }
}

std::vector<Unit*> Unit::assist_pickups(const UnitCommand& cmd, SimContext& ctx,
                                        bool& pathfinding) {
    // CUnitAssistMoveTask::GetEntitiesAlreadyAtLoc.
    pathfinding = false;
    std::vector<Unit*> army_land; // the army's mobile land units (CArmyImpl::GetUnits)
    ctx.registry.for_each_unit([&](Entity& e) {
        if (e.destroyed() || e.army() != army()) return;
        auto& u = static_cast<Unit&>(e);
        if (mobile_land(u)) army_land.push_back(&u);
    });
    // The one that most needs a lift: farthest from its goal, less a third
    // of its distance from the transport, of those of this order that would
    // get there sooner carried.
    Vector3 best_at{0.0f, 0.0f, 0.0f};
    f32 best = 0.0f;
    for (Unit* u : army_land) {
        if (u->is_dying() || u->is_being_built() || !u->is_mobile()) continue;
        const auto& q = u->command_queue_;
        // Its order dispatched (Moho's navigator has the goal from then on).
        if (q.empty() || !holds(q.front(), cmd) || q.front().assist_phase != AssistPhase::Walking)
            continue;
        if (u->transport_id() || q.front().assigned_id || u->has_unit_state("TransportLoading"))
            continue;
        if (!transport_can_carry(*u) || !transport_has_space_for(*u)) continue;
        if (u->navigator_.moho_path().pathfinding()) {
            pathfinding = true;
            continue;
        }
        if (!u->needs_pickup(*this)) continue;
        const f32 score = distance_xz(u->navigator_.goal_pos(*u), u->position()) -
                          distance_xz(position(), u->position()) * 0.33f;
        if (score > best) {
            best = score;
            best_at = u->position();
        }
    }
    // Every one of the army's mobile land units within its GuardScanRadius
    // of that one, whatever its order (0x005F7EE2: the binary walks the
    // army's whole set again here, not the ones that passed above).
    std::vector<Unit*> out;
    if (best_at.x == 0.0f && best_at.y == 0.0f && best_at.z == 0.0f) return out;
    for (Unit* u : army_land) {
        if (guard_scan_radius_ > distance_xz(best_at, u->position())) out.push_back(u);
    }
    return out;
}

bool Unit::needs_pickup(const Unit& transport) const {
    // Moho's Unit::NeedsPickup (0x005F0E80; the Steam exe's 0x005F7950): its
    // time to drive to its goal against the transport's to come for it and
    // carry it there.
    if (navigator_.moho_path().goal_problem()) {
        return true;
    }
    const f32 transport_speed =
        transport.can_fly() ? transport.max_airspeed() : transport.max_speed();
    if (transport_speed < 0.001f) return false;
    const f32 speed = can_fly() ? max_airspeed() : max_speed();
    if (speed < 0.001f) return false;
    const f32 inv_speed = 1.0f / speed;
    const f32 to_goal = distance_xz(navigator_.goal_pos(*this), position());
    const f32 goal_time = inv_speed * to_goal;
    const f32 to_transport = distance_xz(transport.position(), position());
    f32 pickup_time = (to_transport + to_goal) / transport_speed + 5.0f;
    if (transport.layer() == "Air" || to_transport > guard_scan_radius_) {
        pickup_time += 4.0f;
    } else {
        pickup_time = inv_speed * to_transport + pickup_time;
    }
    return goal_time > pickup_time;
}

void Unit::end_assist_move(SimContext& ctx) {
    // ~CUnitAssistMoveTask: UNITSTATE_AssistMoving goes, and what it still
    // carries loses its orders.
    set_unit_state("AssistMoving", false);
    const std::vector<EntityId> aboard = cargo_ids_;
    for (const EntityId id : aboard) {
        Entity* e = ctx.registry.find(id);
        if (!e || e->destroyed() || !e->is_unit()) continue;
        auto& cargo = static_cast<Unit&>(*e);
        if (cargo.is_dying()) continue;
        cargo.clear_commands(ctx);
        if (destroyed() || !in_registry()) return;
    }
}

} // namespace osc::sim
