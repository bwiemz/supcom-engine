// Air staging (M206r): aircraft dock at an air staging platform to refuel and
// repair, as Moho's CUnitRefuel task and CUnitMotion::ProcessFuelLevels do
// (faf-re). Design: docs/plans/2026-09-26-m206r-air-staging-design.md.

#include "sim/unit.hpp"

#include "core/dmath.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/category_set.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"

#include <algorithm>
#include <cmath>

#include <lua.h>

namespace osc::sim {

namespace {

/// Ticks between a refuel's looks: CUnitRefuel returns 10 while it waits for
/// a slot or a full tank, and Moho's task thread then waits 10 - 1 ticks.
constexpr i32 kRefuelPollTicks = 9;
/// Full enough to leave (CUnitRefuel's kRefuelCompleteFuelRatio), and full
/// enough for ProcessFuelLevels to call the refuel done.
constexpr f32 kFullTank = 0.99f;
/// The cosine of the widest heading error at which the platform takes the
/// unit (kAttachFacingAlignment).
constexpr f32 kAttachAlignment = 0.95f;
/// ProcessFuelLevels' kFuelTickScale.
constexpr f32 kFuelTickScale = 0.1f;
/// Close enough over the bone to settle onto it.
constexpr f32 kOverBone = 0.5f;

Unit* live_unit(EntityRegistry& registry, u32 id) {
    Entity* e = registry.find(id);
    return e && !e->destroyed() && e->is_unit() ? static_cast<Unit*>(e) : nullptr;
}

/// A tick of a task's wait (Moho's "return n"): true while it still waits.
bool still_waiting(i32& wait) {
    if (wait <= 0) return false;
    --wait;
    return wait > 0;
}

bool refuels(const UnitCommand& cmd) {
    return cmd.type == CommandType::Dock || cmd.type == CommandType::TransportLoad;
}

/// The heading a unit docks at (TransportGetAttachFacing): its bone's
/// forward, flattened.
f32 bone_heading(const Unit& platform, i32 bone) {
    const Quaternion facing =
        quat_multiply(platform.orientation(), platform.bone_pose(bone).rotation);
    const Vector3 forward = quat_rotate(facing, Vector3{0.0f, 0.0f, 1.0f});
    if (forward.x * forward.x + forward.z * forward.z < 1e-6f)
        return quat_yaw(platform.orientation());
    return dmath::atan2(forward.x, forward.z);
}

/// `from` turned toward `to` by at most `step`, in [0, 2 pi).
f32 turn_toward(f32 from, f32 to, f32 step) {
    f32 diff = to - from;
    while (diff > 3.14159265f) diff -= 6.28318530f;
    while (diff < -3.14159265f) diff += 6.28318530f;
    f32 heading = std::abs(diff) <= step ? to : from + (diff > 0 ? step : -step);
    while (heading < 0) heading += 6.28318530f;
    while (heading >= 6.28318530f) heading -= 6.28318530f;
    return heading;
}

/// The army's brain hears `method` (a script callback, looked up through
/// its class).
void brain_hears(SimContext& ctx, i32 army, const char* method) {
    ArmyBrain* brain = ctx.sim ? ctx.sim->get_army(army) : nullptr;
    lua_State* L = ctx.L;
    if (!brain || !L || brain->lua_table_ref() < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, brain->lua_table_ref());
    lua_pushstring(L, method);
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return;
    }
    lua_pushvalue(L, -2);
    if (lua_pcall(L, 1, 0, 0) != 0) lua_pop(L, 1); // a script error is the script's
    lua_pop(L, 1);
}

} // namespace

bool Unit::is_staging_platform() const {
    static const CategoryName kAir{"AIRSTAGINGPLATFORM"};
    static const CategoryName kPod{"PODSTAGINGPLATFORM"};
    return has_category(kAir) || has_category(kPod);
}

bool Unit::docks_at(u32 platform_id) const {
    if (command_queue_.empty()) return false;
    const UnitCommand& head = command_queue_.front();
    return refuels(head) && head.target_id == platform_id && head.dock_phase == DockPhase::Approach;
}

bool Unit::settle_over(const Vector3& at, f32 heading, bool& glided, f64 dt, SimContext& ctx) {
    const f32 fdt = static_cast<f32>(dt);
    const auto gap = [&] {
        const f32 dx = at.x - position().x;
        const f32 dz = at.z - position().z;
        return std::sqrt(dx * dx + dz * dz);
    };
    if (!glided) {
        // The flight there, until the point is within the unit's turning
        // circle: a fast flier would circle it for good, never coming near
        // enough for the navigator to call it there.
        const f32 turning = current_airspeed_ / std::max(turn_rate_rad_ * turn_mult_, 0.1f);
        if (gap() > std::max(10.0f, turning)) {
            const Vector3 goal = navigator_.goal();
            if (!navigator_.is_moving() || std::abs(goal.x - at.x) > 1.0f ||
                std::abs(goal.z - at.z) > 1.0f)
                navigator_.set_goal(at, ctx.pathfinder, position(), layer_, naval_draft_, false);
            if (nav_update(dt, ctx.terrain)) return false;
        }
        navigator_.abort_move();
        glided = true;
    }
    // Settling: straight over the point, slowing to a hover, down to it and
    // turned the way it faces (Moho's move onto the land layer, at the
    // point's height and facing).
    const f32 dist = gap();
    const f32 speed = dist > kOverBone
                          ? std::max(current_airspeed_ - accel_rate_ * accel_mult_ * fdt,
                                     std::max(climb_rate_, 1.0f))
                          : 0.0f;
    current_airspeed_ = speed;
    Vector3 to = position();
    if (dist > kOverBone) {
        const f32 step = std::min(dist, speed * fdt);
        to.x += (at.x - to.x) / dist * step;
        to.z += (at.z - to.z) / dist * step;
    }
    const f32 ground = ctx.terrain ? ctx.terrain->get_terrain_height(to.x, to.z) : 0.0f;
    const f32 want_alt = at.y - ground;
    const f32 climb = climb_rate_ * fdt;
    current_altitude_ = current_altitude_ < want_alt
                            ? std::min(current_altitude_ + climb, want_alt)
                            : std::max(current_altitude_ - climb, want_alt);
    to.y = ground + current_altitude_;
    set_position(to);
    heading_ = turn_toward(heading_, heading, turn_rate_rad_ * turn_mult_ * fdt);
    pitch_ = 0.0f;
    bank_angle_ = 0.0f;
    set_orientation(euler_to_quat(heading_, 0.0f, 0.0f));
    return gap() <= kOverBone && current_altitude_ == want_alt;
}

OrderStep Unit::order_refuel(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    auto& registry = ctx.registry;
    // Scripts run on the way (the attach, the detach) may clear the queue (and
    // cmd with it) or replace it: the order goes only if it is still the head.
    const u32 order_id = cmd.command_id;
    const auto finish = [&] {
        set_unit_state("Refueling", false);
        navigator_.set_speed_through_goal(false);
        if (!command_queue_.empty() && &command_queue_.front() == &cmd &&
            command_queue_.front().command_id == order_id)
            command_queue_.pop_front();
        return OrderStep::Next;
    };
    const auto gone = [&] { return destroyed() || !in_registry(); };
    // Flying at the platform, through it rather than stopping on it (Moho's
    // SteerTowardPlatform with ForceSpeedThrough).
    const auto circle = [&](const Unit& pad) {
        const Vector3 at = pad.position();
        const Vector3 goal = navigator_.goal();
        if (!navigator_.is_moving() || std::abs(goal.x - at.x) > 1.0f ||
            std::abs(goal.z - at.z) > 1.0f) {
            navigator_.set_speed_through_goal(true);
            navigator_.set_goal(at, ctx.pathfinder, position(), layer_, naval_draft_, false);
        }
        nav_update(dt, ctx.terrain);
    };

    // The task ends with its platform: dead, busy with an order of its own,
    // or under water, which lets a docked unit go first. Otherwise a docked
    // unit stays aboard, its orders held, until the platform lets it go (an
    // unload), as Moho's CUnitRefuel leaves it.
    Unit* pad = live_unit(registry, cmd.target_id);
    if (!pad || pad->is_dying() || !pad->is_idle_state() || pad->diving() || pad->surfacing())
        return finish();
    if (pad->layer() == "Sub" || pad->layer() == "Seabed") {
        if (transport_id_ == pad->entity_id()) {
            pad->detach_cargo({entity_id()}, registry, ctx.L, ctx.terrain);
            if (gone()) return OrderStep::Gone;
        }
        return finish();
    }
    set_unit_state("Refueling", true);

    // A carrier (Moho's mIsCarrier, M206s): the unit lands and is stored,
    // refuels there, and is launched once full and repaired.
    static const CategoryName kCarrier{"CARRIER"};
    const bool carrier = pad->has_category(kCarrier);
    if (carrier && cmd.dock_phase == DockPhase::Reserve) {
        if (landing_.phase == LandPhase::None) {
            circle(*pad);
            if (still_waiting(cmd.dock_wait)) return OrderStep::Hold;
            if (!pad->transport_has_available_storage()) {
                cmd.dock_wait = kRefuelPollTicks;
                return OrderStep::Hold;
            }
            navigator_.set_speed_through_goal(false);
            navigator_.abort_move();
        }
        const LandStep step = carrier_land_step(*pad, true, dt, ctx);
        if (gone()) return OrderStep::Gone;
        if (command_queue_.empty() || &command_queue_.front() != &cmd ||
            command_queue_.front().command_id != order_id)
            return OrderStep::Next;
        if (step == LandStep::Failed) return finish();
        if (step == LandStep::Landing) return OrderStep::Hold;
        cmd.dock_phase = DockPhase::Docked;
        cmd.dock_wait = 0;
        return OrderStep::Hold;
    }
    if (carrier && cmd.dock_phase == DockPhase::Docked) {
        // Stored: every 9 ticks, once full and repaired, it is launched.
        if (transport_id_ != pad->entity_id()) return finish();
        if (still_waiting(cmd.dock_wait)) return OrderStep::Hold;
        const bool full = fuel_ratio_ < 0 || fuel_ratio_ > kFullTank;
        if (!full || health() < max_health()) {
            cmd.dock_wait = kRefuelPollTicks;
            return OrderStep::Hold;
        }
        pad->remove_from_storage(*this, registry, ctx.L, ctx.terrain);
        if (gone()) return OrderStep::Gone;
        if (command_queue_.empty() || &command_queue_.front() != &cmd ||
            command_queue_.front().command_id != order_id)
            return OrderStep::Next;
        cmd.dock_phase = DockPhase::Lift;
        cmd.dock_wait = 0;
        return OrderStep::Hold;
    }

    if (cmd.dock_phase == DockPhase::Reserve) {
        // Ask the platform for a slot of the unit's class (TransportAssignSlot),
        // heading for it meanwhile and asking again every 9 ticks.
        circle(*pad);
        if (still_waiting(cmd.dock_wait)) return OrderStep::Hold;
        TransportSlots* slots = pad->transport_slots();
        if (!air_class_ || !slots ||
            !slots->assign(entity_id(), transport_class_, transport_attach_bone())) {
            cmd.dock_wait = kRefuelPollTicks;
            return OrderStep::Hold;
        }
        navigator_.set_speed_through_goal(false);
        navigator_.abort_move();
        cmd.dock_phase = DockPhase::Approach;
        cmd.dock_wait = 0;
    }

    if (cmd.dock_phase == DockPhase::Approach) {
        // To the slot's bone, down to it, turned the way it faces; then the
        // platform takes the unit (TransportAttachUnit).
        const TransportSlots* slots = pad->built_transport_slots();
        const TransportSlots::Slot* slot = slots ? slots->slot_of(entity_id()) : nullptr;
        if (!slot) return finish();
        const f32 facing = bone_heading(*pad, slot->bone);
        const bool settled =
            settle_over(pad->bone_world_position(slot->bone), facing, cmd.started, dt, ctx);
        if (!settled || dmath::cos(heading_ - facing) <= kAttachAlignment) return OrderStep::Hold;
        attach_to_transport(pad, registry, ctx.L);
        if (gone()) return OrderStep::Gone;
        if (command_queue_.empty() || &command_queue_.front() != &cmd ||
            command_queue_.front().command_id != order_id)
            return OrderStep::Next;
        if (transport_id_ != pad->entity_id()) return finish();
        cmd.dock_phase = DockPhase::Docked;
        cmd.dock_wait = 0;
        return OrderStep::Hold;
    }

    if (cmd.dock_phase == DockPhase::Docked) {
        // Refuelling and repairing (tick_fuel); every 9 ticks, a unit with a
        // full tank and full health is let go, and climbs away.
        if (transport_id_ != pad->entity_id()) return finish();
        if (still_waiting(cmd.dock_wait)) return OrderStep::Hold;
        const bool full = fuel_ratio_ < 0 || fuel_ratio_ > kFullTank;
        if (!full || health() < max_health()) {
            cmd.dock_wait = kRefuelPollTicks;
            return OrderStep::Hold;
        }
        pad->detach_cargo({entity_id()}, registry, ctx.L, ctx.terrain);
        if (gone()) return OrderStep::Gone;
        if (command_queue_.empty() || &command_queue_.front() != &cmd) return OrderStep::Next;
        cmd.dock_phase = DockPhase::Lift;
        cmd.dock_wait = 0;
        return OrderStep::Hold;
    }

    // Lift: back up to its flying height, then done. With more orders queued,
    // it circles the platform while others given the same order still
    // refuel there, so they set off together.
    if (carrier && current_altitude_ < elevation_target_) {
        // Launched off a carrier at speed, it climbs as it flies back round
        // toward the carrier (Moho steers it at the platform as it ends).
        const Vector3 at = pad->position();
        const Vector3 goal = navigator_.goal();
        if (!navigator_.is_moving() || std::abs(goal.x - at.x) > 1.0f ||
            std::abs(goal.z - at.z) > 1.0f)
            navigator_.set_goal(at, ctx.pathfinder, position(), layer_, naval_draft_, false);
        nav_update(dt, ctx.terrain);
        return OrderStep::Hold;
    }
    if (!carrier && current_altitude_ != elevation_target_) {
        hold_altitude(dt, ctx.terrain, elevation_target_);
        return OrderStep::Hold;
    }
    set_unit_state("Refueling", false);
    if (command_queue_.size() > 1) {
        bool waiting_for_others = false;
        registry.for_each_unit([&](Entity& e) {
            if (waiting_for_others || e.destroyed() || !e.is_unit() || &e == this) return;
            const auto& other = static_cast<const Unit&>(e);
            waiting_for_others = !other.command_queue_.empty() &&
                                 other.command_queue_.front().command_id == order_id &&
                                 other.has_unit_state("Refueling");
        });
        if (waiting_for_others) {
            circle(*pad);
            if (still_waiting(cmd.dock_wait)) return OrderStep::Hold;
            cmd.dock_wait = kRefuelPollTicks;
            return OrderStep::Hold;
        }
    }
    return finish();
}

Unit::LandStep Unit::carrier_land_step(Unit& carrier, bool refuel, f64 dt, SimContext& ctx) {
    const u32 me = entity_id();
    const auto fail = [&] {
        abandon_landing(ctx.registry);
        return LandStep::Failed;
    };
    // A unit landing to refuel needs the carrier parked; one ordered aboard,
    // once under way, needs it still taking units in.
    if (refuel) {
        if (!carrier.is_idle_state() || carrier.diving() || carrier.surfacing()) return fail();
    } else if (landing_.phase != LandPhase::None && !carrier.has_unit_state("TransportLoading")) {
        return fail();
    }

    if (landing_.phase == LandPhase::None) {
        // A place in storage, and the point to come in from: 20 (and its
        // turn's wait) back from it, along the way it faces.
        if (!carrier.transport_has_available_storage()) {
            brain_hears(ctx, army(), "OnTransportFull");
            if (destroyed() || !in_registry()) return LandStep::Failed;
            return fail();
        }
        const std::optional<StoragePlace> place = carrier.reserve_storage(me);
        if (!place) return fail();
        landing_ = {};
        landing_.carrier = carrier.entity_id();
        landing_.place = *place;
        const f32 standoff = static_cast<f32>(place->delay) + 20.0f;
        landing_.approach = {place->point.x - dmath::sin(place->heading) * standoff, place->point.y,
                             place->point.z - dmath::cos(place->heading) * standoff};
        landing_.phase = LandPhase::Approach;
        set_unit_state("TransportLoading", true);
        navigator_.abort_move();
        return LandStep::Landing;
    }

    if (landing_.phase == LandPhase::Approach) {
        // In to the point it comes in from, down at the deck's height, facing
        // the way the place does.
        if (!settle_over(landing_.approach, landing_.place.heading, landing_.glided, dt, ctx))
            return LandStep::Landing;
        landing_.phase = LandPhase::Hold;
    }

    if (landing_.phase == LandPhase::Hold) {
        // A carrier under water is waited for (Moho looks again 4 ticks
        // later); then the deck's height is fixed, and the unit waits its turn.
        if (!landing_.deck_set) {
            if (still_waiting(landing_.wait)) return LandStep::Landing;
            if (carrier.layer() == "Sub" || carrier.layer() == "Seabed") {
                landing_.wait = 4;
                return LandStep::Landing;
            }
            landing_.place.point.y = carrier.position().y + landing_.place.height;
            landing_.deck_set = true;
            landing_.wait = landing_.place.delay > 1 ? landing_.place.delay - 1 : 0;
            return LandStep::Landing;
        }
        if (still_waiting(landing_.wait)) return LandStep::Landing;
        landing_.phase = LandPhase::Descend;
    }

    // Descend: onto the place; there the carrier stores it.
    if (!settle_over(landing_.place.point, landing_.place.heading, landing_.glided, dt, ctx))
        return LandStep::Landing;
    carrier.add_to_storage(*this, ctx.registry, ctx.L);
    if (destroyed() || !in_registry()) return LandStep::Failed;
    if (transport_id_ != carrier.entity_id()) return fail();
    landing_ = {};
    set_unit_state("TransportLoading", false);
    return LandStep::Stored;
}

void Unit::abandon_landing(EntityRegistry& registry) {
    if (landing_.phase != LandPhase::None)
        if (Unit* carrier = live_unit(registry, landing_.carrier))
            carrier->clear_storage_reservation(entity_id());
    landing_ = {};
    set_unit_state("TransportLoading", false);
}

OrderStep Unit::order_carrier_landing(UnitCommand& cmd, f64 dt, SimContext& ctx) {
    // Ordered aboard a carrier (Moho's CUnitCarrierLand from a load order):
    // the carrier takes its own share of the order (its retrieve; each load
    // order its own, as each is its own Moho command), and the unit lands
    // and is stored.
    const u32 order_id = cmd.command_id;
    const auto finish = [&] {
        abandon_landing(ctx.registry);
        if (!command_queue_.empty() && &command_queue_.front() == &cmd &&
            command_queue_.front().command_id == order_id)
            command_queue_.pop_front();
        return OrderStep::Next;
    };
    Unit* carrier = live_unit(ctx.registry, cmd.target_id);
    if (!carrier || carrier->is_dying() || transport_id_ != 0) return finish();
    const u32 cid = carrier->entity_id();
    if (std::none_of(carrier->command_queue_.begin(), carrier->command_queue_.end(),
                     [&](const UnitCommand& c) {
                         return c.type == CommandType::TransportLoad && c.target_id == cid &&
                                c.command_id == cmd.command_id;
                     })) {
        UnitCommand load;
        load.type = CommandType::TransportLoad;
        load.target_id = cid;
        load.target_pos = carrier->position();
        load.command_id = cmd.command_id;
        carrier->command_queue_.push_back(load);
    }
    const LandStep step = carrier_land_step(*carrier, false, dt, ctx);
    if (destroyed() || !in_registry()) return OrderStep::Gone;
    if (command_queue_.empty() || &command_queue_.front() != &cmd ||
        command_queue_.front().command_id != order_id)
        return OrderStep::Next;
    if (step == LandStep::Landing) return OrderStep::Hold;
    command_queue_.pop_front(); // stored, or given up
    return OrderStep::Next;
}

void Unit::tick_docked(f64 dt, SimContext& ctx, Unit& platform) {
    const u32 platform_id = platform.entity_id();
    if (!command_queue_.empty()) {
        UnitCommand& head = command_queue_.front();
        if (refuels(head) && head.target_id == platform_id &&
            head.dock_phase == DockPhase::Docked) {
            order_refuel(head, dt, ctx);
            if (destroyed() || !in_registry()) return;
        }
    }
    // Still aboard, it refuels; let go this tick, it flies.
    Unit* pad = transport_id_ == platform_id ? live_unit(ctx.registry, platform_id) : nullptr;
    tick_fuel(dt, ctx, pad);
}

bool Unit::tick_fuel(f64 dt, SimContext& ctx, Unit* platform) {
    // The repair asks for resources only while the unit is docked and damaged.
    if (!platform) {
        dock_repair_asked_ = false;
        economy_.dock_repair_mass = 0;
        economy_.dock_repair_energy = 0;
    }
    if (fuel_ratio_ < 0 || fuel_use_time_ <= 0) return true;
    auto* L = ctx.L;
    const f32 before = fuel_ratio_;
    if (!platform) {
        // Flying burns a FuelUseTime's worth over its tank.
        refuel_started_ = false;
        if (!is_air_unit()) return true;
        fuel_ratio_ = std::max(before - 1.0f / (fuel_use_time_ * 10.0f), 0.0f);
        if (fuel_ratio_ == 0.0f && before > 0.0f) {
            call_lua_method(L, "OnRunOutOfFuel");
            if (destroyed() || dying_) return false;
        }
        return true;
    }

    const StagingRules& pad = platform->staging_rules_;
    const bool needs_repair = health() < max_health();
    if (!refuel_started_ && before < 1.0f) {
        refuel_started_ = true;
        call_lua_method(L, "OnStartRefueling");
        if (destroyed() || dying_) return false;
    }
    if (needs_repair) {
        // Moho asks the army for the platform's RepairConsume* and heals
        // RefuelingRepairAmount / 10 whenever the request is met: each tick
        // the economy can afford it, the first tick only asking. Here the
        // ask runs per second, and the heal goes at the army's efficiency.
        if (dock_repair_asked_) {
            f32 eff = 1.0f;
            if (army() >= 0 && static_cast<u32>(army()) < SimContext::MAX_EFFICIENCY_ARMIES) {
                const auto& ae = ctx.army_efficiency[static_cast<u32>(army())];
                eff = static_cast<f32>(std::min(ae.mass, ae.energy));
            }
            set_health(std::min(max_health(), health() + pad.repair_amount * kFuelTickScale * eff));
        }
        refuel_started_ = true;
        dock_repair_asked_ = true;
        economy_.dock_repair_energy = static_cast<f64>(pad.repair_energy) * 10.0;
        economy_.dock_repair_mass = static_cast<f64>(pad.repair_mass) * 10.0;
    } else {
        dock_repair_asked_ = false;
        economy_.dock_repair_energy = 0;
        economy_.dock_repair_mass = 0;
    }
    if (refuel_started_ && before > kFullTank && !needs_repair) refuel_started_ = false;
    fuel_ratio_ = std::min(before + fuel_recharge_rate_ / fuel_use_time_ * kFuelTickScale *
                                        pad.refuel_multiplier,
                           1.0f);
    if (before == 0.0f && fuel_ratio_ > 0.0f) {
        call_lua_method(L, "OnGotFuel");
        if (destroyed() || dying_) return false;
    }
    return true;
}

} // namespace osc::sim
