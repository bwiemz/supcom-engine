#pragma once

#include "blueprints/footprint.hpp"
#include "sim/occupancy.hpp"
#include "sim/category_set.hpp"
#include "sim/entity.hpp"
#include "sim/navigator.hpp"
#include "sim/pose.hpp"
#include "sim/transport_slots.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace osc::sim { class Manipulator; }
namespace osc::blueprints { class BlueprintStore; }

struct lua_State;

namespace osc::map {
class PathfindingGrid;
class Terrain;
}

namespace osc::sim {

struct SimContext;
class EntityRegistry;
class SimRandom;
class SimState;

struct IntelState {
    f32 radius = 0;
    bool enabled = false;
};

struct BuildQueueEntry {
    std::string blueprint_id;
    int count = 1;
};

struct UnitEconomy;
/// unit:GetResourceConsumed(): the share it obtains of what it asks for, at
/// its army's efficiencies (1 asking for nothing)
f64 resource_fraction(const UnitEconomy& econ, f64 mass_efficiency, f64 energy_efficiency);

struct UnitEconomy {
    f64 production_mass = 0.0;
    f64 production_energy = 0.0;
    f64 consumption_mass = 0.0;
    f64 consumption_energy = 0.0;
    bool production_active = false;
    bool consumption_active = false;
    /// The consumption flag as the unit's script last set it (Moho's
    /// ConsumptionActive): its OnConsumptionActive/InActive fire on a change
    /// of this, not of the drain the engine switches while building.
    bool script_consumption_active = false;
    bool maintenance_active = false;
    f64 energy_maintenance_override = -1.0; // negative = not set
    f64 storage_mass = 0.0;
    f64 storage_energy = 0.0;
    /// What the silo's missile under way asks per second (M206): the engine's
    /// own request, beside the consumption the unit's script sets.
    f64 silo_mass = 0.0;
    f64 silo_energy = 0.0;
    /// What its reclaim brings in per second, beside its own production
    f64 reclaim_mass = 0.0;
    f64 reclaim_energy = 0.0;
    /// What an aircraft docked at a staging platform asks per second for its
    /// repair there (M206r): the platform's RepairConsume*, only while the
    /// aircraft is damaged.
    f64 dock_repair_mass = 0.0;
    f64 dock_repair_energy = 0.0;

    /// Moho's mMaintainenceCost: a silo's missile under way asks through it
    /// too (CAiSiloBuildImpl).
    f64 mass_requested() const { return consumption_mass + silo_mass; }
    f64 energy_requested() const { return consumption_energy + silo_energy; }
    f64 mass_consumed(bool paused) const {
        return (consumption_active && !paused ? consumption_mass : 0.0) + silo_mass;
    }
    f64 energy_consumed(bool paused) const {
        return (consumption_active && !paused ? consumption_energy : 0.0) + silo_energy;
    }
};

/// An air staging platform's service (its blueprint's AI.RefuelingMultiplier,
/// RefuelingRepairAmount, RepairConsumeEnergy and RepairConsumeMass; M206r),
/// with Moho's defaults.
struct StagingRules {
    f32 refuel_multiplier = 1.0f;
    f32 repair_amount = 20.0f; ///< health a second
    f32 repair_energy = 2.0f;  ///< a tick
    f32 repair_mass = 0.5f;    ///< a tick
    f32 scan_radius = 300.0f;  ///< AI.StagingPlatformScanRadius: how far patrols look
};

/// A winged aircraft's attack runs, from its blueprint's Air table and
/// Physics.AttackElevation, with Moho's defaults (faf-re RUnitBlueprint.cpp;
/// docs/plans/2026-10-05-air-attack-runs-design.md).
struct AirCombatRules {
    bool winged = false;
    f32 min_airspeed = 0.0f;      ///< 0: MaxAirspeed
    f32 combat_turn_speed = 1.0f; ///< rad/s, in a combat turn
    f32 tight_turn_multiplier = 1.0f;
    f32 sustained_turn_threshold = 10.0f; ///< seconds of turning that force a break-off
    f32 engage_distance = 0.0f;
    f32 break_off_trigger = 0.0f;
    f32 break_off_distance = 0.0f;
    bool break_off_if_near_new_target = false;
    f32 random_break_off_distance_mult = 1.5f;
    f32 random_min_change_combat_state_time = 3.0f; ///< seconds
    f32 random_max_change_combat_state_time = 6.0f; ///< seconds
    f32 predict_ahead_for_bomb_drop = 0.0f;         ///< seconds
    f32 attack_elevation = 0.0f;                    ///< 0: Physics.Elevation
    f32 k_turn = 3.0f;
    f32 k_turn_damping = 3.0f;
    f32 k_move = 1.0f;
    f32 k_move_damping = 1.0f;
    /// A hovering aircraft's circling (Moho's CalcCirclingOrientation):
    /// HoverOverAttack ones never circle. The rest circle a target, or
    /// what they work on, at a radius that changes now and then.
    bool hover_over_attack = false;
    bool circling_dir_change = true;        ///< it may change direction at each change
    f32 circling_min_airspeed = 0.0f;       ///< MinAirspeed as given (0 missing): its step round
    f32 circling_turn_mult = 3.0f;          ///< its turn gain while circling
    f32 circling_radius_min = 0.6f;         ///< CirclingRadiusChangeMinRatio
    f32 circling_radius_max = 0.9f;         ///< CirclingRadiusChangeMaxRatio
    f32 circling_radius_vs_air_mult = 1.0f; ///< against a target in the air
    f32 circling_elevation_ratio = 0.25f;   ///< x attack_elevation: its height's wobble
    f32 circling_change_frequency = 2.0f;   ///< seconds, to twice that, between changes
    f32 bank_factor = 0.5f;                 ///< how far it leans into a change of speed
};

/// Its attack run under way: Moho's CUnitMotion combat state, and the
/// combat flight's yaw rate and velocity (the airframe's lag).
struct AirCombatState {
    u8 state = 0;         ///< EAirCombatState: 0 None .. 7 ReturnToMap
    u32 timeout_tick = 0; ///< until when a turn or break-off holds
    i32 sustained_turn_ticks = 0;
    f32 yaw_rate = 0.0f; ///< rad/s
    Vector3 velocity{};  ///< per second, horizontal
    bool flying = false; ///< the combat flight has the airframe
    /// Circling (a hovering aircraft's), drawn again at each timeout: the
    /// way round (Moho's -90 degree yaw of the tangent when set, +90 not), its
    /// height off AttackElevation, and its radius's ratio.
    bool circle_reverse = false;
    f32 circle_elevation = 0.0f;
    f32 circle_radius_ratio = 1.0f;
    /// Where it was when it began circling its work: Moho's motion target
    /// once it stops (CUnitMotion::Stop), which a reclaim's or capture's
    /// circle goes round.
    Vector3 circle_anchor{};
};

/// A blueprint's Economy.BuildTime, BuildCostMass and BuildCostEnergy (0
/// where missing): what building it, or repairing it, costs.
struct BuildEconomy {
    f64 time = 0.0;
    f64 mass = 0.0;
    f64 energy = 0.0;
};
BuildEconomy blueprint_build_economy(lua_State* L, const std::string& blueprint_id);

/// What an order did this tick (Unit::run_order, M193).
enum class OrderStep : u8 {
    Next, ///< finished or dropped (taken off the queue): run the next order now
    Hold, ///< goes on next tick
    Gone, ///< a script destroyed the unit: stop updating it
};

/// The heading that puts a target at `bearing` `attack_angle` radians off
/// the bow from `heading`, on the side it already lies (dead ahead, the
/// right): Moho's ApplyCommonMoveAttackAngle.
f32 attack_angle_heading(f32 bearing, f32 heading, f32 attack_angle);

class Unit : public Entity {
    friend struct StateIO; // snapshots (state_io.hpp)
public:
    bool is_unit() const override { return true; }

    const std::string& unit_id() const { return unit_id_; }
    void set_unit_id(const std::string& id) { unit_id_ = id; }

    i32 weapon_count() const {
        return static_cast<i32>(weapons_.size());
    }
    f32 build_rate() const { return build_rate_; }
    /// Economy.MaxBuildDistance: how far past the footprints it builds,
    /// reclaims and repairs (see sim/work_range.hpp).
    f32 max_build_distance() const { return max_build_distance_; }
    void set_max_build_distance(f32 d) { max_build_distance_ = d; }
    void set_build_rate(f32 r) { build_rate_ = r; }
    /// General.CapCost: what the unit counts against its army's unit cap
    /// (walls 0.1; drones, build bots and satellites 0).
    f32 cap_cost() const { return cap_cost_; }
    void set_cap_cost(f32 c) { cap_cost_ = c; }
    /// AI.GuardScanRadius: how far off its route a patrol takes on work,
    /// and how far from itself a guard takes on enemies.
    f32 guard_scan_radius() const { return guard_scan_radius_; }
    void set_guard_scan_radius(f32 r) { guard_scan_radius_ = r; }
    /// AI.GuardReturnRadius: how far from what it guards (or where its
    /// patrol broke off) a unit chases an enemy it has reached.
    f32 guard_return_radius() const { return guard_return_radius_; }
    void set_guard_return_radius(f32 r) { guard_return_radius_ = r; }
    /// AI.AttackAngle, degrees: a stopped unit turns its target this far
    /// off its bow (a broadside), from its slaved weapons or its attack.
    f32 attack_angle() const { return attack_angle_; }
    void set_attack_angle(f32 a) { attack_angle_ = a; }
    /// The way a parked attack asked it to face (zero: none), which it turns
    /// to with no slaved target (Moho's SetFacing, mFormationVec).
    const Vector3& attack_facing() const { return attack_facing_; }
    void set_attack_facing(const Vector3& v) { attack_facing_ = v; }
    /// It turned in place this tick (a move, to its motion events).
    bool turned_in_place() const { return turned_in_place_; }
    bool slaved_turning() const { return slaved_turning_; }
    /// Turn the hull toward its weapons' work, stopped (unit_facing.cpp).
    void face_weapons_work(f64 dt, const EntityRegistry& registry);
    /// Turn toward the heading `want` by at most `max_step` radians, onto it
    /// within that; false when already facing it.
    bool rotate_yaw_toward(f32 want, f32 max_step);
    /// AI.NeedUnpack: its weapon unpacks, holding the unit still, before it
    /// fires (retail's mobile artillery). A new order packs it up, and it
    /// looks for no targets while it moves (see begin_order).
    bool need_unpack() const { return need_unpack_; }
    void set_need_unpack(bool b) { need_unpack_ = b; }

    const std::string& layer() const { return layer_; }
    void set_layer(const std::string& l) { layer_ = l; }

    const std::string& armor_type() const { return armor_type_; }
    void set_armor_type(const std::string& t) { armor_type_ = t; }

    bool is_being_built() const { return is_being_built_; }
    void set_is_being_built(bool b) { is_being_built_ = b; }
    bool producing() const { return economy_.production_active && !is_being_built_ && !dying_; }

    f32 max_speed() const { return max_speed_; }
    void set_max_speed(f32 s) { max_speed_ = s; }

    // Navigation
    Navigator& navigator() { return navigator_; }
    const Navigator& navigator() const { return navigator_; }
    bool is_moving() const { return navigator_.is_moving(); }

    // Economy
    UnitEconomy& economy() { return economy_; }
    const UnitEconomy& economy() const { return economy_; }

    // Weapons
    void add_weapon(std::unique_ptr<Weapon> w);
    Weapon* get_weapon(i32 index);
    const std::vector<std::unique_ptr<Weapon>>& weapons() const {
        return weapons_;
    }

    // Categories (cached from the blueprint at creation time; see
    // collect_blueprint_categories)
    const std::unordered_set<std::string>& categories() const {
        return categories_;
    }
    /// The same categories as ids, for compiled category tests
    /// (lua::CategoryMatcher).
    const CategoryBits& category_bits() const { return category_bits_; }
    bool has_category(const std::string& cat) const {
        return categories_.count(cat) > 0;
    }
    bool has_category(CategoryName cat) const { return category_bits_.test(cat.id); }
    void add_category(std::string cat) {
        category_bits_.set(CategoryIds::intern(cat));
        categories_.insert(std::move(cat));
    }

    /// A factory's rally orders (Moho's builder factory command queue,
    /// M206j): each unit it finishes takes a copy of them, after the
    /// roll-off move its script gives it. Only an immobile FACTORY keeps
    /// them.
    bool keeps_rally_orders() const { return !is_mobile() && has_category("FACTORY"); }
    const std::vector<UnitCommand>& rally_orders() const { return rally_orders_; }
    void add_rally_order(const UnitCommand& cmd) { rally_orders_.push_back(cmd); }
    void clear_rally_orders() { rally_orders_.clear(); }
    /// The rally orders, first given the blueprint's initial rally if there
    /// are none (Moho's BuilderSetUpInitialRally), as Moho keeps a factory's.
    /// It changes sim state: only the sim's own steps may call it.
    const std::vector<UnitCommand>& validated_rally_orders(lua_State* L, SimState* sim);
    /// Where the first rally order goes (unit:GetRallyPoint), without
    /// changing anything: the initial rally's point if there are none yet.
    /// False for a unit that keeps none. `L` is the sim's Lua state, whose
    /// blueprints it reads; the UI's unit objects ask it too.
    bool rally_point(lua_State* L, Vector3& out) const;

    // Build state (builder side) — tracks what this unit is constructing
    u32 build_target_id() const { return build_target_id_; }
    void set_build_target_id(u32 id) { build_target_id_ = id; }
    bool is_building() const { return build_target_id_ != 0; }
    /// This factory's build came from the queue of a factory it guards
    /// (M206h): the guard order runs it, and cancels it when it ends.
    bool factory_assist_build() const { return factory_assist_build_; }
    i32 assist_rolloff_wait() const { return assist_rolloff_wait_; }

    f64 build_time() const { return build_time_; }
    void set_build_time(f64 t) { build_time_ = t; }
    f64 build_cost_mass() const { return build_cost_mass_; }
    void set_build_cost_mass(f64 c) { build_cost_mass_ = c; }
    f64 build_cost_energy() const { return build_cost_energy_; }
    void set_build_cost_energy(f64 c) { build_cost_energy_ = c; }

    // Work progress (generic — used by build, upgrade, capture, etc.)
    f32 work_progress() const { return work_progress_; }
    void set_work_progress(f32 p) { work_progress_ = p; }

    // Pause state
    bool is_paused() const { return paused_; }
    void set_paused(bool p) { paused_ = p; }
    /// Pause or resume the unit's work, as unit:SetPaused does. Moho's paused
    /// unit keeps producing; its army pays for none of its work meanwhile.
    void pause(bool p) { paused_ = p; }

    // Shield back-reference (entity ID, set by _c_CreateShield)
    u32 shield_entity_id() const { return shield_entity_id_; }
    void set_shield_entity_id(u32 id) { shield_entity_id_ = id; }

    // State flags
    bool busy() const { return busy_; }
    /// Moho's IsIdleState: nothing queued, and not building, being built,
    /// repairing or capturing.
    bool is_idle_state() const {
        return command_queue_.empty() && !is_building() && !is_being_built() && !is_repairing() &&
               !is_capturing();
    }
    void set_busy(bool b) { busy_ = b; }
    /// Stunned for `seconds` (Moho's SetStunned: ten ticks a second, from
    /// now; a non-positive time ends it). A stunned unit's weapons don't
    /// fire (Moho's UnitWeapon::CanFire and Fire).
    void set_stunned(f64 seconds) {
        stun_ticks_ = seconds > 0 ? static_cast<i32>(std::min(seconds * 10.0, 1e9)) : 0;
    }
    bool is_stunned() const { return stun_ticks_ > 0; }
    i32 stun_ticks() const { return stun_ticks_; }

    bool block_command_queue() const { return block_command_queue_; }
    void set_block_command_queue(bool b) { block_command_queue_ = b; }

    i32 fire_state() const { return fire_state_; }
    void set_fire_state(i32 s) { fire_state_ = s; }

    // Script bits (9 toggles, bits 0-8)
    u16 script_bits() const { return script_bits_; }
    /// The tick it was made: a build template's order (Moho's mCreationTick)
    u32 creation_tick() const { return creation_tick_; }
    void set_creation_tick(u32 tick) { creation_tick_ = tick; }
    bool get_script_bit(i32 bit) const {
        return (bit >= 0 && bit <= 8) ? ((script_bits_ >> bit) & 1) != 0 : false;
    }
    void set_script_bit(i32 bit, bool value) {
        if (bit < 0 || bit > 8) return;
        if (value) script_bits_ |= static_cast<u16>(1u << bit);
        else       script_bits_ &= static_cast<u16>(~(1u << bit));
    }
    void toggle_script_bit(i32 bit) {
        if (bit >= 0 && bit <= 8)
            script_bits_ ^= static_cast<u16>(1u << bit);
    }

    // Toggle caps (which RULEUTC_* toggles this unit supports)
    bool has_toggle_cap(const std::string& cap) const {
        return toggle_caps_.count(cap) > 0;
    }
    void add_toggle_cap(const std::string& cap) { toggle_caps_.insert(cap); }
    void remove_toggle_cap(const std::string& cap) { toggle_caps_.erase(cap); }

    // Layer change with Lua OnLayerChange(new, old) callback
    void set_layer_with_callback(const std::string& new_layer, lua_State* L);

    /// Moho's horizontal motion states, raised to scripts as
    /// self:OnMotionHorzEventChange(new, old) when they change.
    enum class MotionHorz : u8 { Stopped, Cruise, TopSpeed, Stopping };
    MotionHorz motion_horz() const { return motion_horz_; }
    /// How it is turning, raised as self:OnMotionTurnEventChange(new, old).
    enum class MotionTurn : u8 { Straight, Turn, SharpTurn };
    MotionTurn motion_turn() const { return motion_turn_; }

    /// How a surface unit drives: its blueprint's Physics (M203).
    struct Drive {
        f32 max_accel = 0;           ///< MaxAcceleration, per second
        f32 max_brake = 0;           ///< MaxBrake (0: as MaxAcceleration)
        f32 turn_rate = 0;           ///< TurnRate, in radians per second
        f32 turn_radius = 0;         ///< TurnRadius (0: none)
        bool rotate_on_spot = false; ///< RotateOnSpot
        f32 rotate_threshold = 0.5f; ///< RotateOnSpotThreshold: slower than this it pivots
        f32 max_speed_reverse = 0;   ///< MaxSpeedReverse (0: it can't back up; absent: MaxSpeed)
        f32 backup_distance = 0;     ///< BackUpDistance: a goal behind it this near, it backs to
        /// TurnFacingRate, radians per second: how fast a hover's body turns
        /// to its work (none without one, as Moho's)
        f32 turn_facing_rate = 0;
    };
    const Drive& drive() const { return drive_; }
    void set_drive(const Drive& d) { drive_ = d; }
    /// Its speed along its heading, negative backing up.
    f32 ground_speed() const { return ground_speed_; }
    /// How far it keeps others off in plan view: the larger half-extent of
    /// its collision box (0 without one).
    f32 separation_radius() const;
    /// Pushed aside by a neighbour in the last separation pass (M203b).
    bool jostled() const { return jostled_; }
    void set_jostled(bool j) { jostled_ = j; }
    /// What the navigator drove it at this tick: the speed, the speed it was
    /// steering for (0 braking to a stop), its top speed, and how it turned.
    void note_drive(f32 speed, f32 target, f32 top, MotionTurn turn) {
        ground_speed_ = speed;
        target_speed_ = target;
        top_speed_ = top;
        motion_turn_next_ = turn;
        drove_ = true;
    }

    // Threat levels (cached from blueprint Defense at creation time)
    f32 surface_threat() const { return surface_threat_; }
    f32 air_threat() const { return air_threat_; }
    f32 sub_threat() const { return sub_threat_; }
    f32 economy_threat() const { return economy_threat_; }
    void set_surface_threat(f32 t) { surface_threat_ = t; }
    void set_air_threat(f32 t) { air_threat_ = t; }
    void set_sub_threat(f32 t) { sub_threat_ = t; }
    void set_economy_threat(f32 t) { economy_threat_ = t; }

    // Build queue (factory production queue)
    /// The unit's queue as FA's construction panel shows it, a run of one
    /// blueprint grouped with a count.
    std::vector<BuildQueueEntry> factory_queue() const;
    /// Remove up to `count` orders from the `index`-th (1-based) group of
    /// factory_queue(), newest first (DecreaseBuildCountInQueue). Removing
    /// the order in progress cancels it (cancel_factory_build).
    void decrease_build_count(int index, int count, EntityRegistry& registry, lua_State* L);
    /// Sim::RemoveCommandFromUnitQueue: order `id` off the queue, or the rally
    /// orders; the work of a head order stops as Stop stops it.
    void remove_command(u32 id, EntityRegistry& registry, lua_State* L);
    /// IncreaseBuildCountInQueue: `count` more of the index-th group of its
    /// factory queue (1-based, as factory_queue() groups it), after the
    /// group's last order. An index past the queue changes nothing.
    void increase_build_count(int index, int count);
    /// A factory's build under way is cancelled: the factory hears
    /// OnFailedToBuild, and the unit it was building is destroyed, as in Moho.
    void cancel_factory_build(EntityRegistry& registry, lua_State* L);
    /// True while a factory order is under way.
    bool building_factory_order() const {
        return build_target_id_ != 0 && !command_queue_.empty() &&
               command_queue_.front().type == CommandType::BuildFactory;
    }

    // Command queue
    /// A ferry takes this unit, waiting at its beacon (its head order a
    /// WaitForFerry): it boards that ferry.
    void board_ferry(u32 ferry_id) {
        if (!command_queue_.empty() && command_queue_.front().type == CommandType::WaitForFerry)
            command_queue_.front().assigned_id = ferry_id;
    }
    const std::deque<UnitCommand>& command_queue() const {
        return command_queue_;
    }
    /// The unit this one guards (GetGuardedUnit): its current order's
    /// target when that is a Guard, else 0. Fighting for its guard, it still
    /// guards (Moho keeps the guarded unit while the attack task runs).
    u32 guarded_unit_id() const {
        const UnitCommand* guard = guard_order();
        return guard ? guard->target_id : 0;
    }
    /// The Guard it is carrying out: the head order, or the one beneath a
    /// fight it broke off for (null: it guards nothing).
    const UnitCommand* guard_order() const {
        size_t i = 0;
        while (i < command_queue_.size() && command_queue_[i].from_guard) ++i;
        if (i == command_queue_.size() || command_queue_[i].type != CommandType::Guard)
            return nullptr;
        return &command_queue_[i];
    }
    void push_command(const UnitCommand& cmd, bool clear_existing);
    /// `cmd` at the back of the queue as it is (NotifyUpgrade's copy of an
    /// old unit's orders, a patrol's points in their order).
    void append_command(const UnitCommand& cmd) { command_queue_.push_back(cmd); }
    /// A unit guarding `from` (its current order a Guard of it, or a fight
    /// for that Guard) guards `to`.
    void retarget_guard(u32 from, u32 to) {
        for (UnitCommand& c : command_queue_) {
            if (c.from_guard) {
                if (c.leash_anchor_id == from) c.leash_anchor_id = to;
                continue;
            }
            if (c.type == CommandType::Guard && c.target_id == from) c.target_id = to;
            return;
        }
    }
    void clear_commands(const char* source = "?");
    std::vector<UnitCommand*> commands_with_id(u32 id) {
        std::vector<UnitCommand*> out;
        for (UnitCommand& c : command_queue_) {
            if (c.command_id == id) {
                out.push_back(&c);
            }
        }
        for (UnitCommand& c : rally_orders_) {
            if (c.command_id == id) {
                out.push_back(&c);
            }
        }
        return out;
    }

    /// Per-tick update, in phases: dying or carried (tick_lifecycle), the
    /// orders (tick_orders), coasting, layer changes and fuel
    /// (tick_after_orders), then the silo, regeneration, weapons and
    /// manipulators (tick_upkeep).
    void update(f64 dt, SimContext& ctx);

    /// Lua callback helpers: call self:method() or self:method(entity)
    void call_lua_method(lua_State* L, const char* method_name);
    /// OnHealthChanged(new, old) when its health falls into another quarter,
    /// as Moho reports it: each value the quarter it is in, rounded down
    void report_health_band(lua_State* L);
    void call_lua_method_with_entity(lua_State* L, const char* method_name,
                                      Entity* arg_entity);

    /// Build helpers called from the order handlers (unit_orders.cpp).
    /// What start_build made of a build: its unit started; nothing, and the
    /// order goes; or nothing yet, the army being at its unit cap (an
    /// upgrade isn't held to it), and the builder tries again later.
    enum class BuildStart { Started, Failed, AtCap };
    BuildStart start_build(const UnitCommand& cmd, EntityRegistry& registry, lua_State* L);
    /// Works on the build under way; false once it has ended, and then
    /// `built` (if given) says whether the unit was finished or the build
    /// failed.
    bool progress_build(f64 dt, EntityRegistry& registry, lua_State* L,
                        map::PathfindingGrid* grid = nullptr, f32 efficiency = 1.0f,
                        bool* built = nullptr);
    void finish_build(EntityRegistry& registry, lua_State* L, bool success,
                      map::PathfindingGrid* grid = nullptr);

    /// Assist helpers (Guard command)
    void stop_assisting(lua_State* L = nullptr, EntityRegistry* registry = nullptr);
    void call_build_callback(lua_State* L, const char* method, Entity* target, const char* order);
    bool progress_build_assist(f64 dt, EntityRegistry& registry,
                                f32 efficiency = 1.0f);

    /// Reclaim helpers
    u32 reclaim_target_id() const { return reclaim_target_id_; }
    void set_reclaim_target_id(u32 id) { reclaim_target_id_ = id; }
    bool is_reclaiming() const { return reclaim_target_id_ != 0; }
    i32 reclaim_wait() const { return reclaim_wait_; }
    void set_reclaim_wait(i32 ticks) { reclaim_wait_ = ticks; }
    void begin_reclaim(u32 target_id, lua_State* L, EntityRegistry& registry);
    void stop_reclaiming(lua_State* L = nullptr, EntityRegistry* registry = nullptr);
    bool progress_reclaim(f64 dt, EntityRegistry& registry, lua_State* L);
    bool progress_reclaim_assist(f64 dt, EntityRegistry& registry);
    static bool reclaim_wears_down(const Entity& target);
    bool reclaim_arm_ready(const Entity& target) const;
    bool awaits_arm();
    bool arm_awaited() const { return arm_awaited_; }
    bool wear_down(Unit& target) const;
    u32 reclaim_into_wreck(u32 target_id, EntityRegistry& registry, lua_State* L);

    /// Repair helpers
    u32 repair_target_id() const { return repair_target_id_; }
    void set_repair_target_id(u32 id) { repair_target_id_ = id; }
    bool is_repairing() const { return repair_target_id_ != 0; }
    bool start_repair(const UnitCommand& cmd, EntityRegistry& registry, lua_State* L);
    bool progress_repair(f64 dt, EntityRegistry& registry, lua_State* L,
                          f32 efficiency = 1.0f);
    void stop_repairing(lua_State* L, EntityRegistry& registry);
    bool shield_needs_repair(EntityRegistry& registry, lua_State* L);

    /// Capture helpers
    u32 capture_target_id() const { return capture_target_id_; }
    void set_capture_target_id(u32 id) { capture_target_id_ = id; }
    bool is_capturing() const { return capture_target_id_ != 0; }
    bool is_being_captured() const { return being_captured_; }
    void set_being_captured(bool v) { being_captured_ = v; }
    bool capturable() const { return capturable_; }
    void set_capturable(bool c) { capturable_ = c; }
    bool start_capture(const UnitCommand& cmd, EntityRegistry& registry, lua_State* L);
    bool progress_capture(f64 dt, EntityRegistry& registry, lua_State* L,
                           f32 efficiency = 1.0f);
    void stop_capturing(lua_State* L, EntityRegistry& registry, bool failed);

    /// Enhancement helpers
    bool has_enhancement(const std::string& enh) const;
    void add_enhancement(const std::string& slot, const std::string& enh);
    void remove_enhancement(const std::string& enh);
    /// Slot -> enhancement, by slot name (scripts see them in this order).
    const std::map<std::string, std::string>& enhancements() const { return enhancements_; }
    bool is_enhancing() const { return enhancing_; }
    const std::string& enhance_name() const { return enhance_name_; }
    /// Reads the enhancement from the unit's own blueprint in `store` (not
    /// self.Blueprint, which only FAF's unit script sets).
    bool start_enhance(const UnitCommand& cmd, lua_State* L,
                       const blueprints::BlueprintStore* store);
    bool progress_enhance(f64 dt, lua_State* L, f32 efficiency = 1.0f);
    void finish_enhance(lua_State* L);
    void cancel_enhance(lua_State* L);

    // Veterancy level (0-5, set from Lua VeterancyComponent)
    u8 vet_level() const { return vet_level_; }
    void set_vet_level(u8 level) { vet_level_ = level; }
    const std::vector<std::pair<u32, f32>>& damage_contributions() const { return damage_contributions_; }
    void record_damage(u32 attacker_id, f32 amount);
    void clear_damage_contributions() { damage_contributions_.clear(); }

    // Stats/telemetry system
    void set_stat(const std::string& key, f64 value);
    f64 get_stat(const std::string& key, f64 default_val = 0) const;
    const std::unordered_map<std::string, f64>& stats() const { return stats_; }
    bool has_stat(const std::string& key) const;

    // Silo ammo system (nuke + tactical missile counters)
    i32 nuke_silo_ammo() const { return nuke_silo_ammo_; }
    i32 tactical_silo_ammo() const { return tactical_silo_ammo_; }
    void give_nuke_silo_ammo(i32 amount) { nuke_silo_ammo_ += amount; if (nuke_silo_ammo_ < 0) nuke_silo_ammo_ = 0; }
    void give_tactical_silo_ammo(i32 amount) { tactical_silo_ammo_ += amount; if (tactical_silo_ammo_ < 0) tactical_silo_ammo_ = 0; }
    void remove_nuke_silo_ammo(i32 amount) { nuke_silo_ammo_ -= amount; if (nuke_silo_ammo_ < 0) nuke_silo_ammo_ = 0; }
    void remove_tactical_silo_ammo(i32 amount) { tactical_silo_ammo_ -= amount; if (tactical_silo_ammo_ < 0) tactical_silo_ammo_ = 0; }
    /// The same, by kind: nukes, or tactical missiles (anti-nukes' too).
    i32 silo_ammo(bool nuke) const { return nuke ? nuke_silo_ammo_ : tactical_silo_ammo_; }
    void give_silo_ammo(bool nuke, i32 amount) {
        nuke ? give_nuke_silo_ammo(amount) : give_tactical_silo_ammo(amount);
    }
    void remove_silo_ammo(bool nuke, i32 amount) {
        nuke ? remove_nuke_silo_ammo(amount) : remove_tactical_silo_ammo(amount);
    }

    // Moho's silo (M206): it builds missiles for the unit's counted weapons,
    // one at a time, beside whatever else the unit is doing.
    /// The missile under way: for which weapon, how far along, and what its
    /// projectile blueprint's Economy says it costs.
    struct SiloBuild {
        i32 weapon = -1; ///< index into weapons(); -1: none under way
        bool nuke = false;
        f64 progress = 0; ///< 0 to 1
        f64 build_time = 0;
        f64 energy = 0;
        f64 mass = 0;
    };
    const SiloBuild& silo_build() const { return silo_build_; }
    bool silo_building() const { return silo_build_.weapon >= 0; }
    /// A build of a missile of that kind, ordered (IssueSiloBuildNuke/Tactical).
    void order_silo_build(bool nuke) { silo_orders_.push_back(nuke); }
    /// Builds of that kind ordered and not yet finished (the one under way
    /// included), as GetMissileInfo reports them.
    i32 silo_build_count(bool nuke) const;
    /// The first enabled counted weapon of that kind: the one the silo
    /// builds for. Null when there is none.
    Weapon* silo_weapon(bool nuke) const;
    /// That kind's storage: its silo weapon's MaxProjectileStorage (0 with none).
    i32 silo_max_storage(bool nuke) const;
    /// Per tick: start the next missile (an ordered build, else in auto mode
    /// one with room in its storage), advance the one under way at the
    /// unit's build rate as the economy allows, and store it when done. A
    /// paused unit's build waits. The script hears OnSiloBuildStart and
    /// OnSiloBuildEnd, and the unit is SiloBuildingAmmo between them.
    void update_silo(f64 dt, f32 efficiency, lua_State* L);
    /// StopSiloBuild: the missile under way is abandoned and the builds
    /// ordered are dropped.
    void stop_silo_build();
    /// GiveNukeSiloAmmo(blocks, true), FAF's: the missile under way, else the
    /// next one, has `blocks` of its 10 * BuildTime / build rate done.
    void set_silo_blocks(i32 blocks);
    i32 silo_blocks() const { return silo_blocks_; }
    /// An assisting engineer's build power, `rate`, on the missile under way.
    void assist_silo_build(f32 rate, f64 dt, f32 efficiency);

    /// The weapon a launch order uses: the first enabled one with ManualFire
    /// and CountedProjectile (a NukeWeapon for a nuke), OverCharge aside.
    Weapon* launch_weapon(bool nuke) const;
    /// Its OverChargeWeapon (switched off until an OverCharge order), if any.
    Weapon* overcharge_weapon() const;
    /// The launch or OverCharge order at the head of the queue, if `w` is
    /// the weapon it fires and the order has handed it its target (a launch
    /// within the weapon's range band, an OverCharge switched on); else null.
    const UnitCommand* launch_order_for(const Weapon& w) const;
    UnitCommand* launch_order_for(const Weapon& w);

    // Immobile flag (set during enhancement)
    bool immobile() const { return immobile_; }
    void set_immobile(bool b) { immobile_ = b; }

    // Unit states (generic string-based state tracking)
    bool has_unit_state(const std::string& state) const { return unit_states_.count(state) > 0; }
    const std::unordered_set<std::string>& unit_states() const { return unit_states_; }
    /// Moho's CheckAutoInitiate: an idle unit -- no orders, or only an
    /// attack whose target is gone -- may take an attack order of its
    /// weapons' own choosing.
    bool auto_initiate_allowed(const EntityRegistry& registry) const;
    /// A weapon with AutoInitiateAttackCommand picked `target`: the unit
    /// attacks it once its weapons are done this tick (the first pick wins).
    void request_auto_attack(u32 target) {
        if (auto_attack_target_ == 0) auto_attack_target_ = target;
    }
    void set_unit_state(const std::string& state, bool v) {
        if (v) unit_states_.insert(state);
        else   unit_states_.erase(state);
    }

    // Shield ratio (health bar, set by shield's UpdateShieldRatio)
    /// Its blueprint's lifebar (REntityBlueprint's LifeBarSize, LifeBarHeight
    /// and LifeBarOffset in world units, 0 for the console's default;
    /// LifeBarRender; Display.HideLifebars).
    struct LifeBar {
        f32 size = 0.0f;
        f32 height = 0.0f;
        f32 offset = 0.0f;
        bool render = true;
        bool hide = false;
    };
    const LifeBar& life_bar() const { return life_bar_; }
    void set_life_bar(const LifeBar& bar) { life_bar_ = bar; }
    f32 shield_ratio() const { return shield_ratio_; }
    void set_shield_ratio(f32 r) { shield_ratio_ = r; }

    // Transport system
    const std::vector<u32>& cargo_ids() const { return cargo_ids_; }
    void add_cargo(u32 id) { cargo_ids_.push_back(id); }
    void remove_cargo(u32 id);
    void clear_cargo() { cargo_ids_.clear(); }

    u32 transport_id() const { return transport_id_; }
    void set_transport_id(u32 id) { transport_id_ = id; }
    bool is_loaded() const { return transport_id_ != 0; }

    f32 speed_mult() const { return speed_mult_; }
    void set_speed_mult(f32 m) { speed_mult_ = m; }
    f32 effective_speed() const { return max_speed_ * speed_mult_; }

    // Movement multipliers
    f32 accel_mult() const { return accel_mult_; }
    void set_accel_mult(f32 m) { accel_mult_ = m; }
    f32 turn_mult() const { return turn_mult_; }
    void set_turn_mult(f32 m) { turn_mult_ = m; }
    f32 break_off_distance_mult() const { return break_off_distance_mult_; }
    void set_break_off_distance_mult(f32 m) { break_off_distance_mult_ = m; }
    f32 break_off_trigger_mult() const { return break_off_trigger_mult_; }
    void set_break_off_trigger_mult(f32 m) { break_off_trigger_mult_ = m; }
    void reset_speed_and_accel() { speed_mult_ = 1.0f; accel_mult_ = 1.0f; turn_mult_ = 1.0f; }

    // Fuel system
    f32 fuel_ratio() const { return fuel_ratio_; }
    void set_fuel_ratio(f32 r) { fuel_ratio_ = r; }
    f32 fuel_use_time() const { return fuel_use_time_; }
    void set_fuel_use_time(f32 t) { fuel_use_time_ = t; }
    /// Physics.FuelRechargeRate: how fast it refuels (M206r).
    void set_fuel_recharge_rate(f32 r) { fuel_recharge_rate_ = r; }
    /// Docked at a staging platform, refuelling or repairing (M206r).
    bool refuel_started() const { return refuel_started_; }

    // Air movement (populated from blueprint Air subtable)
    f32 heading() const { return heading_; }
    void set_heading(f32 h) { heading_ = h; }
    f32 pitch_angle() const { return pitch_; }
    void set_pitch_angle(f32 p) { pitch_ = p; }
    f32 bank_angle() const { return bank_angle_; }
    void set_bank_angle(f32 b) { bank_angle_ = b; }
    f32 current_airspeed() const { return current_airspeed_; }
    void set_current_airspeed(f32 s) { current_airspeed_ = s; }
    f32 current_altitude() const { return current_altitude_; }
    void set_current_altitude(f32 a) { current_altitude_ = a; }
    f32 max_airspeed() const { return max_airspeed_; }
    void set_max_airspeed(f32 s) { max_airspeed_ = s; }
    f32 turn_rate_rad() const { return turn_rate_rad_; }
    void set_turn_rate_rad(f32 r) { turn_rate_rad_ = r; }
    f32 accel_rate() const { return accel_rate_; }
    void set_accel_rate(f32 r) { accel_rate_ = r; }
    f32 climb_rate() const { return climb_rate_; }
    void set_climb_rate(f32 r) { climb_rate_ = r; }
    f32 elevation_target() const { return elevation_target_; }
    void set_elevation_target(f32 e) { elevation_target_ = e; }
    void set_dive_surface_speed(f32 s) { dive_surface_speed_ = s; }
    /// How far under the water's surface a sub is (M206o): 0 at the surface,
    /// down to its Physics.Elevation when dived.
    f32 sub_elevation() const { return sub_elevation_; }
    /// Whether it is on its way down or up (Moho's MovingDown/MovingUp).
    bool diving() const { return vert_motion_ == VertMotion::Down; }
    bool surfacing() const { return vert_motion_ == VertMotion::Up; }
    /// Its vertical motion event: Top, Down, Bottom or Up.
    const std::string& vert_event() const { return vert_event_; }
    /// A new vertical motion event, told to the script (when `L` is given)
    /// as OnMotionVertEventChange(new, old).
    void set_vert_event(const char* event, lua_State* L);
    bool is_air_unit() const { return layer_ == "Air"; }
    /// An aircraft, flying or landed (MotionType Air).
    bool can_fly() const { return motion_type_ == "RULEUMT_Air"; }
    /// Air.FlyInWater: an aircraft that may fly under the water's surface.
    void set_fly_in_water(bool b) { fly_in_water_ = b; }
    /// What an aircraft holds its height over at (x, z): the ground, or the
    /// water's surface above it unless it flies in water (Moho's CUnitMotion
    /// samples max(terrain, water) for fliers).
    f32 air_floor(const map::Terrain* terrain, f32 x, f32 z) const;

    // Motion type (from blueprint Physics.MotionType)
    const std::string& motion_type() const { return motion_type_; }
    void set_motion_type(const std::string& mt) { motion_type_ = mt; }
    /// Its footprints as Moho resolves them from its blueprint (roadmap item
    /// 4): a mobile unit's is the footprint class nearest its size for its
    /// motion type's caps, a structure's its own with the layers it may be
    /// built on. The alt one is for its AltMotionType.
    const blueprints::Footprint& footprint() const { return footprints_.main; }
    const blueprints::Footprint& alt_footprint() const { return footprints_.alt; }
    /// The footprint class it paths as (-1: none, as a structure or a flier).
    i32 footprint_class() const { return footprints_.main_class; }
    i32 alt_footprint_class() const { return footprints_.alt_class; }
    void set_footprints(const blueprints::UnitFootprints& f) { footprints_ = f; }
    f32 naval_draft() const { return naval_draft_; }
    void set_naval_draft(f32 d) { naval_draft_ = d; }
    bool is_amphibious() const {
        return motion_type_ == "RULEUMT_Amphibious" || motion_type_ == "RULEUMT_AmphibiousFloating";
    }
    bool is_hover() const { return motion_type_ == "RULEUMT_Hover"; }
    /// Under water it walks the seabed, on the terrain (Moho's
    /// UpdateCurrentLayer: Amphibious and Land units). Hover and floating
    /// units, and ships, ride the surface.
    bool walks_seabed() const {
        return motion_type_ == "RULEUMT_Amphibious" || motion_type_ == "RULEUMT_Land";
    }
    /// The height it stands at on the ground at (x, z): the terrain, under
    /// the water too, for one that walks the seabed; else the surface.
    f32 ground_y(const map::Terrain* terrain, f32 x, f32 z) const;
    /// Physics.LayerChangeOffsetHeight: how far above (+) or below (-) the
    /// water's surface the ground must lie for it to count as under water.
    f32 layer_change_offset() const { return layer_change_offset_; }
    void set_layer_change_offset(f32 h) { layer_change_offset_ = h; }
    /// Moho's CUnitMotion::UpdateCurrentLayer, each tick it moves: the layer
    /// it is on by the ground under it and the water over that.
    void update_current_layer(const map::Terrain* terrain, lua_State* L);
    /// Moho's Unit::IsMobile: a blueprint that moves (a structure's
    /// MotionType is RULEUMT_None).
    bool is_mobile() const { return !motion_type_.empty() && motion_type_ != "RULEUMT_None"; }
    bool is_naval() const {
        return motion_type_ == "RULEUMT_Water" || motion_type_ == "RULEUMT_SurfacingSub";
    }

    /// How far it moved over its last tick, per second (SimState sets it):
    /// weapons that LeadTarget aim ahead by it.
    const Vector3& velocity() const { return velocity_; }
    void set_velocity(const Vector3& v) { velocity_ = v; }

    // A killed aircraft's fall (see begin_dying).
    bool is_crashing() const { return crashing_; }
    bool teleporting() const { return teleporting_; }
    bool crash_impacted() const { return crash_impacted_; }
    f32 crash_velocity_y() const { return crash_velocity_y_; }
    f32 crash_spin_rate() const { return crash_spin_rate_; }
    /// The landing a falling aircraft made, taken once (SimState then calls
    /// its script's OnImpact).
    bool take_crash_impact();

    // Misc flags
    u32 creator_id() const { return creator_id_; }
    /// Whether it moved over the last tick: its position as this tick began
    /// against the last's (Moho's committed transform against the one
    /// before). A new unit hasn't.
    bool moved_last_tick() const { return moved_last_tick_; }
    /// Where it stood as this tick began (the next tick's moved_last_tick).
    const Vector3& tick_position() const { return tick_position_; }
    /// Note where it stands as a tick begins (SimState::tick).
    void note_tick_position() {
        const Vector3& p = position();
        moved_last_tick_ =
            tick_position_set_ &&
            (p.x != tick_position_.x || p.y != tick_position_.y || p.z != tick_position_.z);
        tick_position_ = p;
        tick_position_set_ = true;
    }
    void set_creator_id(u32 id) { creator_id_ = id; }
    bool auto_overcharge() const { return auto_overcharge_; }
    void set_auto_overcharge(bool b) { auto_overcharge_ = b; }
    bool overcharge_paused() const { return overcharge_paused_; }
    void set_overcharge_paused(bool b) { overcharge_paused_ = b; }
    bool is_cloaked() const { return cloaked_; }
    void set_cloaked(bool v) { cloaked_ = v; }
    bool has_radar_stealth() const { return radar_stealth_; }
    void set_radar_stealth(bool v) { radar_stealth_ = v; }
    bool has_sonar_stealth() const { return sonar_stealth_; }
    void set_sonar_stealth(bool v) { sonar_stealth_ = v; }
    bool auto_mode() const { return auto_mode_; }
    void set_auto_mode(bool v) { auto_mode_ = v; }
    /// Factory repeat-build flag (UserUnit:IsRepeatQueue / SetRepeatQueue):
    /// a finished build order goes to the back of the queue
    /// (order_build_in_place), and one taken from a guarded factory goes to
    /// the back of that factory's (order_guard).
    bool repeat_queue() const { return repeat_queue_; }
    void set_repeat_queue(bool v) { repeat_queue_ = v; }
    /// Submarine auto-surface flag (SetAutoSurfaceMode). Stored; submarines
    /// do not surface by themselves yet.
    bool auto_surface_mode() const { return auto_surface_mode_; }
    void set_auto_surface_mode(bool v) { auto_surface_mode_ = v; }
    u32 focus_entity_id() const { return focus_entity_id_; }
    void set_focus_entity_id(u32 id) { focus_entity_id_ = id; }

    /// Dead: Moho's Kill hands the death to the unit's script, whose death
    /// thread plays it out (animation, death weapon, wreck) and destroys the
    /// unit. Until then the unit is dead (IsDead): no orders, weapons,
    /// economy or targeting, only its manipulators run. One killed in flight
    /// falls until it lands.
    bool is_dying() const { return dying_; }
    void begin_dying();
    void tick_dying(f32 dt, const map::Terrain* terrain);

    // Damage/kill flags
    bool can_take_damage() const { return can_take_damage_; }
    void set_can_take_damage(bool b) { can_take_damage_ = b; }
    bool can_be_killed() const { return can_be_killed_; }
    void set_can_be_killed(bool b) { can_be_killed_ = b; }
    u32 last_attacker_id() const { return last_attacker_id_; }
    void set_last_attacker_id(u32 id) { last_attacker_id_ = id; }

    // Command caps (RULEUCC_* command capabilities)
    void add_command_cap(const std::string& cap) { command_caps_.insert(cap); }
    void remove_command_cap(const std::string& cap) { command_caps_.erase(cap); }
    void restore_command_caps() { command_caps_ = original_command_caps_; }
    void snapshot_command_caps() { original_command_caps_ = command_caps_; }
    bool has_command_cap(const std::string& cap) const { return command_caps_.count(cap) > 0; }
    /// General.SelectionPriority, 1 without one
    int selection_priority() const { return selection_priority_; }
    void set_selection_priority(int p) { selection_priority_ = std::max(p, 1); }

    // Build restrictions
    /// What it may not build grows by a category (Unit:AddBuildRestriction);
    /// remove_build_restriction takes one off it
    void add_build_restriction(CategoryExpr category) {
        build_restriction_ =
            build_restriction_.empty()
                ? std::move(category)
                : CategoryExpr::combine(CategoryExpr::Op::Union, std::move(build_restriction_),
                                        std::move(category));
    }
    void remove_build_restriction(CategoryExpr category) {
        if (build_restriction_.empty()) return;
        build_restriction_ = CategoryExpr::combine(
            CategoryExpr::Op::Difference, std::move(build_restriction_), std::move(category));
    }
    void restore_build_restrictions() { build_restriction_ = {}; }
    /// What it may not build (empty: no restriction)
    const CategoryExpr& build_restriction() const { return build_restriction_; }

    // Elevation override
    f32 elevation_override() const { return elevation_override_; }
    void set_elevation_override(f32 e) { elevation_override_ = e; }
    bool has_elevation_override() const { return elevation_override_ >= 0; }
    void clear_elevation_override() { elevation_override_ = -1.0f; }

    i32 transport_class() const { return transport_class_; }
    void set_transport_class(i32 c) { transport_class_ = c; }
    /// Transport.AirClass: only these dock at a staging platform (Moho's
    /// TransportValidateType).
    void set_air_class(bool air_class) { air_class_ = air_class; }
    bool air_class() const { return air_class_; }
    /// A staging platform's refuelling and repair (M206r).
    void set_staging_rules(const StagingRules& rules) { staging_rules_ = rules; }
    const AirCombatRules& air_combat_rules() const { return air_combat_rules_; }
    void set_air_combat_rules(const AirCombatRules& rules) { air_combat_rules_ = rules; }
    AirCombatState& air_combat() { return air_combat_; }
    const AirCombatState& air_combat() const { return air_combat_; }
    const StagingRules& staging_rules() const { return staging_rules_; }
    /// Transport.DockingSlots: how many aircraft the UI's Dock sends to it.
    void set_docking_slots(i32 n) { docking_slots_ = n; }
    i32 docking_slots() const { return docking_slots_; }
    /// An air or pod staging platform (Moho's TransportIsAirStagingPlatform).
    bool is_staging_platform() const;
    /// Whether the unit's head order is a refuel at that platform that holds
    /// a slot there (M206r): the platform keeps the slot while it comes.
    bool docks_at(u32 platform_id) const;
    i32 transport_capacity() const { return transport_capacity_; }
    void set_transport_capacity(i32 c) { transport_capacity_ = c; }
    void set_transport_layout(const TransportLayout& layout) { transport_layout_ = layout; }
    /// The blueprint's SizeY: a carried unit with no AttachPoint bone hangs
    /// by its centre, half of it up, where entities attached to its bone -1
    /// sit (bone_world_transform).
    void set_size_y(f32 size_y) { size_y_ = size_y; }
    f32 size_y() const { return size_y_; }
    void set_size_xz(f32 size_x, f32 size_z) {
        size_x_ = size_x;
        size_z_ = size_z;
    }
    /// The blueprint's SizeX and SizeZ: steering's boxes (M203c).
    f32 size_x() const { return size_x_; }
    f32 size_z() const { return size_z_; }
    void set_average_density(f32 d) { average_density_ = d; }
    /// Size x density: a transport picks up the largest first (M206m).
    f32 load_metric() const { return size_x_ * size_y_ * size_z_ * average_density_; }

    /// The transport's attach points and who holds them (M206l), built from
    /// its skeleton the first time they are asked for; null without one.
    TransportSlots* transport_slots();
    /// The slots, if they have been built.
    const TransportSlots* built_transport_slots() const { return transport_slots_.get(); }
    /// Whether this transport has room for `cargo` now: a free slot of its
    /// class, or, for a transport without attach points, fewer units aboard
    /// than its Class1Capacity (0: no limit).
    bool transport_has_space_for(const Unit& cargo);
    /// The bone a carried unit hangs by: its AttachPoint bone, else its root
    /// for a flier, else -1 (its centre).
    i32 transport_attach_bone() const;
    void set_transport_hover_height(f32 h) { transport_hover_height_ = h; }

    // Idle aircraft (roadmap item 6; Moho's CUnitMotion landing phase).
    /// Air.AutoLandTime (seconds idle before it lands; 0 or less, never) and
    /// Air.StartTurnDistance (how near the place it must be).
    void set_auto_land(f32 seconds, f32 start_turn_distance) {
        auto_land_time_ = seconds;
        start_turn_distance_ = start_turn_distance;
    }
    /// Air.StartTurnDistance: also a circle's radius before its ratio when
    /// a hovering aircraft circles its work.
    f32 start_turn_distance() const { return start_turn_distance_; }
    /// An aircraft landing, or landed, of its own accord.
    struct IdleLanding {
        u32 idle_since = 0;       ///< Moho's mPreparationTick: when its orders ran out (0 with one)
        bool descending = false;  ///< in its landing phase
        Vector3 target{};         ///< where it comes down
        std::string layer;        ///< Land or Water, its layer down there
        OccupancyRect reserved{}; ///< the place it reserved (none when empty)
    };
    const IdleLanding& idle_landing() const { return idle_landing_; }
    /// For tests: its landing as given (no reservation taken or freed).
    void set_idle_landing(const IdleLanding& l) { idle_landing_ = l; }
    /// Its reservation goes (landed, taken off, or gone).
    void free_landing_reservation(SimState& sim);
    /// A transport's pickup (M206m): the units given slots, not yet aboard;
    /// whether it is at their centre; the ticks it has waited there.
    const std::vector<u32>& pickup_ids() const { return pickup_ids_; }
    bool pickup_ready() const { return pickup_phase_ == PickupPhase::Waiting; }
    bool pickup_running() const { return pickup_phase_ != PickupPhase::None; }
    i32 pickup_ticks() const { return pickup_ticks_; }
    /// Ticks left of a unit's beam up into its transport (0: not beaming).
    i32 beam_up_ticks() const { return beam_up_ticks_; }
    /// Whether the unit's head order is a load onto that transport.
    bool calls_transport(u32 transport_id) const;

    void attach_to_transport(Unit* transport, EntityRegistry& registry, lua_State* L);

    // Carrier storage (M206q; Moho's CAiTransportImpl storage): units a
    // carrier keeps inside -- the aircraft it builds -- beside its cargo
    // slots. A stored unit rides at the carrier's centre, its transport set
    // to the carrier, so it neither moves nor fights.
    void set_storage_slots(i32 n) { storage_slots_ = n; }
    i32 storage_slots() const { return storage_slots_; }
    const std::vector<u32>& stored_ids() const { return stored_ids_; }
    bool is_stored_unit(u32 id) const;
    /// Whether another unit fits in storage (Transport.StorageSlots).
    bool transport_has_available_storage() const;
    /// Store `unit`: its script hears OnAddToStorage(carrier) first.
    void add_to_storage(Unit& unit, EntityRegistry& registry, lua_State* L);
    /// Take `unit` out of storage: its script hears OnRemoveFromStorage(carrier),
    /// and it is set at the carrier's next launch bone, facing as the bone
    /// does (else at the carrier).
    void remove_from_storage(Unit& unit, EntityRegistry& registry, lua_State* L,
                             const map::Terrain* terrain = nullptr);
    /// Forget a stored unit that is gone.
    void forget_stored(u32 id);
    /// A place in a carrier's storage an aircraft lands at (M206s; Moho's
    /// TransportReserveStorage): the attach point it comes down onto, the
    /// way the point faces, its height over the carrier, and the ticks the
    /// aircraft waits its turn (0, then 3 more for each pass round).
    struct StoragePlace {
        Vector3 point{};
        f32 heading = 0;
        f32 height = 0;
        i32 delay = 0;
    };
    /// Reserve the carrier's next generic attach point, round robin, for
    /// `unit_id`; nothing without any. Reserved places count as taken.
    std::optional<StoragePlace> reserve_storage(u32 unit_id);
    void clear_storage_reservation(u32 unit_id);
    /// The round robin starts over (Moho's TransportResetReservation).
    void reset_storage_reservation();
    const std::vector<u32>& storage_reserved_ids() const { return storage_reserved_; }
    /// The round robin's next point and wait (for the checksum).
    u32 next_storage_point() const { return next_generic_; }
    i32 storage_overflow() const { return generic_overflow_; }
    /// A carrier's retrieve under way: its phase (0: none), the units it
    /// waits for, its wait (for the checksum).
    u32 retrieve_phase() const { return static_cast<u32>(retrieve_phase_); }
    const std::vector<u32>& retrieve_ids() const { return retrieve_ids_; }
    i32 retrieve_wait() const { return retrieve_wait_; }
    /// A landing under way (for the checksum): phase (0: none) and wait.
    u32 landing_phase() const { return static_cast<u32>(landing_.phase); }
    i32 landing_wait() const { return landing_.wait; }
    /// A landing's place and the point it comes in from.
    const StoragePlace& landing_place() const { return landing_.place; }
    const Vector3& landing_approach() const { return landing_.approach; }
    /// Whether this unit is landing on that carrier (M206s).
    bool lands_on(u32 carrier_id) const {
        return landing_.phase != LandPhase::None && landing_.carrier == carrier_id;
    }
    /// Whether an unload order launches this carrier's stored units rather
    /// than dropping cargo: a CARRIER that is an air (or pod) staging
    /// platform, with something stored (Moho's dispatch of TransportUnload).
    bool launches_on_unload() const;
    /// Drop the cargo (all of it, or those of `ids` still aboard, in cargo
    /// order): each is set down where it hung, level, and on the ground when
    /// `terrain` is given (M206n).
    void detach_all_cargo(EntityRegistry& registry, lua_State* L,
                          const map::Terrain* terrain = nullptr);
    void detach_cargo(std::vector<u32> ids, EntityRegistry& registry, lua_State* L,
                      const map::Terrain* terrain = nullptr);
    /// Whether the unit's footprint fits the ground where it is (Moho's
    /// SFootprint::FitsAt): every cell under it passable for it.
    bool footprint_fits(const map::PathfindingGrid& grid) const;

    // Bone visibility (per-unit, ShowBone/HideBone)
    bool is_bone_hidden(i32 idx) const { return hidden_bones_.count(idx) > 0; }
    void show_bone(i32 idx) { hidden_bones_.erase(idx); }
    void hide_bone(i32 idx) { hidden_bones_.insert(idx); }
    /// The hidden bones, as a mask of the first 64 (the renderer's most).
    u64 hidden_bone_mask() const {
        u64 mask = 0;
        for (const i32 i : hidden_bones_)
            if (i >= 0 && i < 64) mask |= u64{1} << i;
        return mask;
    }

    // Animated bone matrices (for GPU skinning)
    const std::vector<std::array<f32, 16>>& animated_bone_matrices() const {
        return animated_bone_matrices_;
    }
    std::vector<std::array<f32, 16>>& animated_bone_matrices() {
        return animated_bone_matrices_;
    }
    u32 animated_bone_count() const {
        return static_cast<u32>(animated_bone_matrices_.size());
    }
    void init_animated_bones();

    // Manipulator system
    Manipulator* add_manipulator(std::unique_ptr<Manipulator> m);
    void remove_manipulator(Manipulator* m);
    void tick_manipulators(f32 dt, lua_State* L);
    /// Moho's CCollisionManipulator::ManipulatorUpdate, once the pose has
    /// moved: each enabled collision detector's watched bones that crossed
    /// their line this tick, told to the script as
    /// OnAnimCollision / OnAnimTerrainCollision / OnNotAnimTerrainCollision
    /// (bone name, x, y, z): the bone in the unit's frame, or in the world
    /// for the terrain's two.
    void check_collision_detectors(lua_State* L);
    /// Turn the builder arms to `at`, or back with none; Moho's mobile build
    /// task calls OnPrepareArmToBuild as an arm takes its site
    void aim_builder_arms(const Vector3* at, lua_State* L);
    /// Moho's IAiBuilder on-target latch: what its builder arms last said,
    /// true for a unit without one.
    bool builder_on_target() const { return builder_on_target_; }
    void set_builder_on_target(bool b) { builder_on_target_ = b; }
    void destroy_all_manipulators();
    const std::vector<std::unique_ptr<Manipulator>>& manipulators() const { return manipulators_; }

    /// A bone's model-space transform in the unit's pose: the bind pose as
    /// its manipulators leave it (animators, aim controllers, rotators,
    /// sliders, in precedence order), recomputed each tick after they move.
    BonePose bone_pose(i32 bone) const;
    /// A bone's world position in the sim pose (the unit's position if the
    /// bone doesn't exist).
    Vector3 bone_world_position(i32 bone) const;
    /// The way a bone faces in the world (its +Z, as a muzzle fires): the
    /// unit's facing if the bone doesn't exist.
    Vector3 bone_world_forward(i32 bone) const;
    /// Where `local`, a point in a bone's frame (in world units), is in the
    /// world as the sim poses it: an effect's offset from its bone (the
    /// unit's own frame if the bone doesn't exist).
    Vector3 bone_world_point(i32 bone, const Vector3& local) const;
    /// A bone's rotation in the world as the sim poses it (the unit's own if
    /// the bone doesn't exist).
    Quaternion bone_world_rotation(i32 bone) const;
    /// Where an entity attached to a bone of ours sits before its parent
    /// offset (Moho's Unit::GetBoneWorldTransform, M211k): the bone's world
    /// place and turn as the sim poses it; for bone -1, or one we haven't,
    /// our centre, half our height up.
    BonePose bone_world_transform(i32 bone) const;
    /// Its blueprint's AI.TargetBones: the points weapons aim at on it
    /// (Moho's target points). How many it has.
    i32 target_point_count() const;
    /// Where target point `index` is in the world (Moho's
    /// Unit::GetTargetPoint): its bone, the last for an index past them;
    /// for -1, or one with no bone in the mesh, its centre, half its
    /// height up.
    Vector3 target_point(i32 index) const;
    /// One of its target points at random (Moho's Unit::PickTargetPoint):
    /// -1, drawing nothing, with none.
    i32 pick_target_point(SimRandom& rng) const;
    /// One of its target points above `water` (`above`), or below it, at
    /// random: Moho's PickTargetPointAboveWater / BelowWater. `out` is -1
    /// with none. Returns whether there is one; with no target points,
    /// whether the unit itself is above the water (at or below it). With
    /// no `rng` it only answers, drawing nothing.
    bool pick_target_point_by_water(SimRandom* rng, f32 water, bool above, i32& out) const;
    /// Free every manipulator, first detaching their Lua tables (see
    /// Manipulator::lua_table_ref). Called when the unit leaves the sim.
    /// A Script order's Lua task (Moho's CUnitScriptTask, M206w): whether one
    /// runs, and its end -- OnDestroy runs and its object goes -- when its
    /// order is gone, done, or the unit dies or is destroyed.
    bool has_script_task() const { return script_task_.object_ref >= 0; }
    u32 script_task_serial() const { return script_task_.serial; }
    void end_script_task(lua_State* L);
    /// The last AI result a task of the unit's gave (SetAIResult; 0 unknown).
    i32 script_task_result() const { return script_task_result_; }
    void set_script_task_result(i32 result) { script_task_result_ = result; }

    void release_manipulators(lua_State* L);
    /// Detach the weapons' Lua tables (null _c_object and _c_unit) and drop
    /// every Lua ref the weapons and the on-given callbacks hold. Idempotent;
    /// runs however the unit leaves the sim (entity_Destroy or C++ removal),
    /// each table's script OnDestroy first unless `run_on_destroy` is false.
    void release_weapon_scripts(lua_State* L, bool run_on_destroy = true);
    /// Hand a teleport to the script (OnTeleportUnit(self, location,
    /// orientation)); false if the unit's class has no handler.
    bool call_on_teleport_unit(lua_State* L, const Vector3& location);

    // Intel system (per-type enabled/disabled + radius)
    bool is_intel_enabled(const std::string& type) const;
    /// Its jammer (Intel.JammerBlips and JamRadius; M215e): the fake blips
    /// it gives each enemy army that senses it while its Jammer intel is on,
    /// and how far from it they fall.
    u32 jammer_blips() const { return jammer_blips_; }
    f32 jam_radius_min() const { return jam_radius_min_; }
    f32 jam_radius_max() const { return jam_radius_max_; }
    void set_jammer(u32 blips, f32 radius_min, f32 radius_max) {
        jammer_blips_ = blips;
        jam_radius_min_ = radius_min;
        jam_radius_max_ = radius_max;
    }
    f32 get_intel_radius(const std::string& type) const;
    /// InitIntel: give the unit this intel, a new handle, off until
    /// EnableIntel (Moho).
    void init_intel(const std::string& type, f32 radius);
    /// Register intel the unit has (from its blueprint), switched off until
    /// the script enables it. Leaves intel the unit already has untouched.
    void add_intel(const std::string& type, f32 radius);
    /// EnableIntel: a no-op for intel the unit doesn't have, as in Moho.
    void enable_intel(const std::string& type);
    void disable_intel(const std::string& type);
    void set_intel_radius(const std::string& type, f32 radius);
    const std::unordered_map<std::string, IntelState>& intel_states() const { return intel_states_; }

    // Adjacency system
    /// Adjacent structures in id order: their callbacks fire in this order.
    const std::set<u32>& adjacent_unit_ids() const { return adjacent_unit_ids_; }
    void add_adjacent(u32 id) { adjacent_unit_ids_.insert(id); }
    void remove_adjacent(u32 id) { adjacent_unit_ids_.erase(id); }
    void clear_adjacents() { adjacent_unit_ids_.clear(); }

    f32 skirt_size_x() const { return skirt_size_x_; }
    f32 skirt_size_z() const { return skirt_size_z_; }
    f32 skirt_offset_x() const { return skirt_offset_x_; }
    f32 skirt_offset_z() const { return skirt_offset_z_; }
    void set_skirt(f32 sx, f32 sz, f32 ox, f32 oz) {
        skirt_size_x_ = sx; skirt_size_z_ = sz;
        skirt_offset_x_ = ox; skirt_offset_z_ = oz;
    }

    void fire_adjacency_callbacks(EntityRegistry& registry, lua_State* L);

    /// Whether this unit's place has been taken by a replacement of another
    /// army (ChangeUnitArmy, M206v): its Destroy is a hand-over, not a loss.
    bool transferred() const { return transferred_; }
    void set_transferred() { transferred_ = true; }

    // OnUnitBuilt callback system (for factory production notification)
    struct UnitBuiltCallback {
        int func_ref;
        int cat_ref;
    };
    void add_on_unit_built_callback(int func_ref, int cat_ref) {
        on_unit_built_callbacks_.push_back({func_ref, cat_ref});
    }
    void fire_on_unit_built_callbacks(lua_State* L, Entity* built_unit);
    void clear_on_unit_built_callbacks(lua_State* L);

private:
    void call_on_reclaimed(u32 target_id, EntityRegistry& registry, lua_State* L);
    void run_on_reclaimed(Entity& target, lua_State* L);
    /// Move along the navigator's path, no faster than `speed_cap` if set (a
    /// formation keeping its slowest unit's pace).
    bool nav_update(f64 dt, const map::Terrain* terrain, f32 speed_cap = 0);
    /// Walk toward work out of reach (the goal set when the order sent the
    /// unit): nav_update, first asking again for a path the pathfinder put
    /// off (the navigator keeps a throttled request without retrying it).
    /// True until the unit gets there.
    bool approach_update(f64 dt, SimContext& ctx);
    /// An order that walked toward its target lets the navigator go when it
    /// ends before it got there (its target gone, taken or whole): Moho's
    /// task ends its move with it, so the unit isn't left Moving.
    void end_approach(const UnitCommand& cmd) {
        if (cmd.approached) navigator_.abort_move();
    }
    /// An order ending before its tick's move (its target gone) lets go of
    /// the move it was making, if any: the head order's is the only one.
    void release_navigator() {
        if (navigator_.busy()) navigator_.abort_move();
    }

    // update's phases. Each but the last says whether the tick goes on.
    /// Dying (the death animation), or carried (following the transport):
    /// nothing else this tick.
    bool tick_lifecycle(f64 dt, SimContext& ctx);
    /// Run orders from the head of the queue until one holds (unit_orders.cpp).
    bool tick_orders(f64 dt, SimContext& ctx, f32 econ_eff);
    /// An order's first run (on a patrol, each leg's): what Moho's task
    /// does as it is made. A Move, Patrol, Guard or Attack drops the weapons'
    /// targets of an immobile NeedUnpack unit, so its weapon packs up and
    /// the unit can go.
    void begin_order(UnitCommand& cmd, lua_State* L);
    /// Coasting, amphibious layer changes, air separation, fuel.
    bool tick_after_orders(f64 dt, SimContext& ctx);
    /// A silo assist ended, regeneration, the silo, motion events, weapons,
    /// manipulators. Runs while paused too.
    void tick_upkeep(f64 dt, SimContext& ctx, f32 econ_eff, bool was_assisting_silo);

    // The order handlers (unit_orders.cpp), one per kind of order.
    OrderStep run_order(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff);
    OrderStep order_stop();
    OrderStep order_move(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// Close to the best weapon's range of the target, and stay on it.
    OrderStep order_attack(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// An Attack with no target unit: at a point on the ground.
    OrderStep order_attack_ground(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// Reach the site, start the structure, build it.
    OrderStep order_build_mobile(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff);
    /// A factory's build, or an upgrade: started where the unit stands.
    OrderStep order_build_in_place(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff);
    /// A finished factory build order's end: to the back of a repeating
    /// queue, else out of it.
    OrderStep end_factory_build_order(UnitCommand& cmd);
    /// A sacrifice's donation to `target` (Moho's CUnitSacrificeTask).
    void donate_sacrifice(Unit& target, lua_State* L);
    /// Whether a factory whose unit is built still holds for the roll-off,
    /// counting `wait` down (see the definition).
    bool holds_for_rolloff(i32& wait) const;
    /// Whether a build its army's unit cap stopped still waits, counting
    /// its cap_wait down (it tries again once that runs out).
    static bool waits_out_unit_cap(UnitCommand& cmd);
    /// A build its army's unit cap stopped: the order `command_id` (if
    /// still at the head -- the brain's scripts ran) waits kCapRetryTicks.
    OrderStep hold_for_unit_cap(u32 command_id);
    /// Go to the point, then queue it again at the back.
    OrderStep order_patrol(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// An enemy a patrol on leg `cmd` breaks off to attack (Moho's
    /// CUnitPatrolTask::FindTarget), or null.
    Entity* find_patrol_target(const UnitCommand& cmd, SimContext& ctx);
    /// Whether a guard of `guarded` (null: of a point) takes on enemies.
    bool guard_reacts(const Unit* guarded) const;
    /// A guard's fight (Moho's guard task, GetBestEnemy and its Starting
    /// state): going home after one, or, every 6 ticks, an enemy within its
    /// GuardScanRadius taken on as an Attack ahead of the guard. The step,
    /// or nothing while the guard goes on as before. `ref` is what it
    /// guards: the guarded unit's position, or the point.
    std::optional<OrderStep> guard_engage(UnitCommand& cmd, const Unit* guarded, const Vector3& ref,
                                          f64 dt, SimContext& ctx);
    /// A Guard of a point: go there, and take on enemies near it.
    OrderStep order_guard_point(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// Head for `goal`, setting a new path only when it moved by a unit.
    void walk_to(const Vector3& goal, f64 dt, SimContext& ctx);
    /// Moho's FindBestEnemy with the unit's primary weapon: of `candidates`
    /// (in id order), the enemy within `range` (XZ) of the unit it takes on
    /// first, or null (unit_patrol.cpp).
    Entity* best_enemy(const std::vector<Entity*>& candidates, f32 range, SimContext& ctx);
    /// The weapon a guard or patrol looks for enemies with (Moho's
    /// GetPrimaryWeapon: weapon 0, unless it fires only on death, by hand,
    /// or not at all), or null.
    const Weapon* scan_weapon() const;
    /// An enemy its scans leave alone (Moho's IsTargetExempt): the target of
    /// a reclaim or capture in its queue, or a unit its army's engineers are
    /// capturing.
    bool target_exempt(const Unit& enemy, const EntityRegistry& registry) const;
    /// What a patrolling PATROLHELPER breaks off to reclaim or repair
    /// (Moho's EvaluatePatrolReclaimAttack), or null.
    Entity* find_patrol_work(const UnitCommand& cmd, SimContext& ctx);
    OrderStep order_reclaim(UnitCommand& cmd, f64 dt, SimContext& ctx);
    OrderStep reclaim_work(UnitCommand& cmd, f64 dt, SimContext& ctx);
    OrderStep order_repair(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff);
    OrderStep order_capture(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff);
    /// A repair of a unit under construction (Moho's repair task builds it):
    /// it builds alongside any builder, and completes it if it gets there
    /// first.
    OrderStep order_repair_construction(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff,
                                        Unit& target);
    /// Help with what the guarded unit works on, or follow it. Never ends.
    OrderStep order_guard(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff);
    /// A guard order ends: a factory's assisted build (M206h) is cancelled,
    /// as a factory's build is when its order goes; an assist just stops.
    void end_guard_build(EntityRegistry& registry, lua_State* L);
    /// The unit this factory just finished takes the rally orders of
    /// `rally_id` (itself, or the factory it built the unit for); Moho's
    /// CFactoryBuildTask::InheritQueuedCommandsTo.
    void hand_over_rally_orders(u32 built_id, u32 rally_id, SimContext& ctx);
    /// A submarine dives or surfaces.
    OrderStep order_dive(lua_State* L);
    /// Set off down (Water to Sub) or up (Sub to Water): the layer changes
    /// when the sub gets there (Moho's SetNewTargetLayer).
    void start_dive(lua_State* L);
    void start_surfacing(lua_State* L);
    /// A tick of a dive or surfacing, stationary or not (Moho's
    /// HandleDivingAndSurfacing), and a sub held at its depth.
    void tick_dive(const map::Terrain* terrain, lua_State* L);
    /// An idle aircraft's landing (Moho's CUnitMotion landing phase):
    /// AutoLandTime after its orders ran out, near where it is, it finds a
    /// place (PrepareMove), reserves it and comes down onto it.
    void tick_idle_landing(f64 dt, SimContext& ctx);
    /// A hovering aircraft at work circles it (Moho's ComputeAirControl: its
    /// focus while Building, Repairing, Reclaiming or Capturing).
    bool circles_its_work() const;
    /// Its circling this tick: round what it builds or repairs, or round
    /// where it stopped to reclaim or capture.
    void tick_work_circling(f64 dt, SimContext& ctx);
    /// A landed aircraft given an order goes back to the air.
    void take_off(SimContext& ctx);
    OrderStep order_enhance(UnitCommand& cmd, f64 dt, SimContext& ctx, f32 econ_eff);
    /// A Script order (M206w): its task made at the front of the queue, then
    /// its TaskTick each tick it asks for, its status deciding what follows.
    OrderStep order_script(UnitCommand& cmd, SimContext& ctx);
    /// Make `cmd`'s task: its class, its object, OnCreate(args). False if
    /// the scripts destroyed the unit.
    bool start_script_task(UnitCommand& cmd, lua_State* L);
    /// One TaskTick: its status (an error, or no number: done).
    int tick_script_task(lua_State* L);
    /// A load order (M206m, Moho's shared TransportLoadUnits): the transport
    /// it targets runs the pickup, and the units it carries call it.
    OrderStep order_transport_load(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// The transport's side (CUnitLoadUnits): slots for the units calling
    /// it, a flight to their centre, a low hover while they board.
    OrderStep order_transport_pickup(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// A unit's side (CUnitCallTransport): to its place, then a beam up.
    OrderStep order_call_transport(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// End the pickup: slots of units that never came are given up.
    void finish_pickup(bool completed, lua_State* L);
    /// A transport at its drop (M206n, Moho's CUnitUnloadUnits): it comes
    /// down to its hover height, then sets down the cargo (`ids`, or all of
    /// it) whose footprint fits the ground under it; the rest stays aboard.
    /// True once it has set them down.
    bool unload_step(f64 dt, SimContext& ctx, const std::vector<u32>& ids);
    /// A beam up cut short: back down on the ground, level, and the script
    /// told it has stopped.
    void abandon_beam_up(const map::Terrain* terrain, lua_State* L);
    /// Hold still at `altitude` over the ground, climbing or sinking to it.
    void hold_altitude(f64 dt, const map::Terrain* terrain, f32 altitude);
    /// A transport down to its TransportHoverHeight as to a landing (Moho's
    /// ShouldHoverInsteadOfLand): MovingDown, then the Hover event. True there.
    bool hover_low(f64 dt, SimContext& ctx);
    /// A transport flies to the point and drops all its cargo.
    OrderStep order_transport_unload(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// An aircraft's refuel at a staging platform (M206r, Moho's CUnitRefuel):
    /// a slot, the flight to its bone, docking, and the climb away once full.
    OrderStep order_refuel(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// A tick of a unit attached to a staging platform: its refuel order, if
    /// that is still its head, and its fuel and repair.
    void tick_docked(f64 dt, SimContext& ctx, Unit& platform);
    /// A patrolling aircraft that needs fuel or repair (Moho's
    /// Unit::FindPlatform): the first of its army's idle staging platforms
    /// in reach with room for it; null when it needs none or finds none.
    Unit* find_platform(SimContext& ctx);
    /// Fuel (Moho's CUnitMotion::ProcessFuelLevels): it refuels and repairs
    /// docked at `platform`, and burns in flight. False if a script killed it.
    bool tick_fuel(f64 dt, SimContext& ctx, Unit* platform);
    /// An aircraft ordered aboard a carrier (M206s): it lands, and is stored.
    OrderStep order_carrier_landing(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// A carrier's share of a load order onto it (Moho's CUnitCarrierRetrieve):
    /// it stops (surfacing, if under water) and waits while its aircraft land.
    OrderStep order_carrier_retrieve(UnitCommand& cmd, SimContext& ctx);
    /// The retrieve is over: the script hears it, the reservations start over,
    /// and, cut short, the aircraft still coming stop.
    void end_retrieve(bool complete, SimContext& ctx);
    /// A tick of a landing on `carrier` (Moho's CUnitCarrierLand): reserve a
    /// place, fly in, wait its turn, come down, be stored. `refuel`: landing
    /// to refuel, which needs the carrier idle rather than loading.
    enum class LandStep : u8 { Landing, Stored, Failed };
    LandStep carrier_land_step(Unit& carrier, bool refuel, f64 dt, SimContext& ctx);
    /// A landing given up: its place and its loading state go.
    void abandon_landing(EntityRegistry& registry);
    /// Fly to `at` until it is within the unit's turning circle, then glide
    /// over it, coming down to `y` turned to `heading` (M206r's approach).
    /// True once settled there.
    bool settle_over(const Vector3& at, f32 heading, bool& glided, f64 dt, SimContext& ctx);
    /// A staging platform's unload (M206r): its aircraft go from where they
    /// sit to the drop point.
    OrderStep order_staging_release(UnitCommand& cmd, SimContext& ctx);
    /// A carrier's unload: its stored units are launched (M206q).
    OrderStep order_carrier_launch(UnitCommand& cmd, SimContext& ctx);
    /// A nuke, a tactical missile or an OverCharge, by its weapon.
    OrderStep order_launch(UnitCommand& cmd, f64 dt, SimContext& ctx);
    OrderStep order_sacrifice(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// Handed to the script, and held until the warp.
    OrderStep order_teleport(UnitCommand& cmd, lua_State* L);
    /// A ferry's cycle along its route (the leading Ferry orders).
    OrderStep order_ferry(UnitCommand& cmd, f64 dt, SimContext& ctx);
    /// Wait at a beacon until a ferry takes the unit, then board it.
    OrderStep order_wait_for_ferry(UnitCommand& cmd, f64 dt, SimContext& ctx);

    std::string unit_id_;
    std::string armor_type_ = "Default";
    f32 build_rate_ = 1.0f;
    f32 cap_cost_ = 1.0f;           // Moho's RUnitBlueprint default
    f32 max_build_distance_ = 5.0f; // Moho's RUnitBlueprint default
    f32 guard_scan_radius_ = 25.0f;
    f32 attack_angle_ = 0.0f;      // AI.AttackAngle, degrees
    bool slaved_turning_ = false;  // turning to a slaved target (Moho's hysteresis)
    Vector3 attack_facing_;        // a parked attack's facing; zero: none
    bool turned_in_place_ = false; // this tick
    f32 guard_return_radius_ = 50.0f;
    bool need_unpack_ = false;
    std::string layer_ = "Land";
    std::string motion_type_;       // raw MotionType from blueprint
    f32 layer_change_offset_ = -0.1f; // Physics.LayerChangeOffsetHeight (Moho's default)
    blueprints::UnitFootprints footprints_;
    f32 naval_draft_ = 0;           // abs(Physics.Elevation) for naval units
    u32 jammer_blips_ = 0;          // Intel.JammerBlips
    f32 jam_radius_min_ = 0, jam_radius_max_ = 0; // Intel.JamRadius
    bool is_being_built_ = false;
    f32 max_speed_ = 0;
    Navigator navigator_;
    UnitEconomy economy_;
    std::unordered_set<std::string> categories_;
    CategoryBits category_bits_; // categories_, as ids
    std::deque<UnitCommand> command_queue_;
    std::vector<std::unique_ptr<Weapon>> weapons_;
    std::vector<UnitCommand> rally_orders_; // see rally_orders()
    u32 build_target_id_ = 0;     // entity ID of unit being built
    f64 build_time_ = 0;          // target's Economy.BuildTime
    f64 build_cost_mass_ = 0;     // target's Economy.BuildCostMass
    f64 build_cost_energy_ = 0;   // target's Economy.BuildCostEnergy
    f32 work_progress_ = 0.0f;
    u32 reclaim_target_id_ = 0;   // entity ID being reclaimed
    f32 reclaim_rate_ = 0;        // fraction_complete decrease per second
    i32 reclaim_wait_ = 0;
    bool builder_on_target_ = true;
    bool arm_awaited_ = false;
    u32 repair_target_id_ = 0;    // entity ID of unit being repaired
    f64 repair_build_time_ = 0;   // target's Economy.BuildTime
    f64 repair_cost_mass_ = 0;    // target's Economy.BuildCostMass
    f64 repair_cost_energy_ = 0;  // target's Economy.BuildCostEnergy
    u32 capture_target_id_ = 0;   // entity ID of unit being captured
    f64 capture_time_ = 0;        // total seconds to capture
    f64 capture_energy_cost_ = 0; // total energy drain
    bool capturable_ = true;      // can this unit be captured?
    bool being_captured_ = false; // is this unit currently being captured?
    bool paused_ = false;
    MotionHorz motion_horz_ = MotionHorz::Stopped;
    void update_motion_horz(lua_State* L);
    MotionTurn motion_turn_ = MotionTurn::Straight;
    MotionTurn motion_turn_next_ = MotionTurn::Straight;
    void update_motion_turn(lua_State* L);
    Drive drive_;
    f32 ground_speed_ = 0;
    f32 target_speed_ = 0;
    f32 top_speed_ = 0;
    bool drove_ = false; ///< the navigator drove it this tick
    bool jostled_ = false;
    /// With no navigator driving it, it brakes to a stop along its heading.
    void coast(f64 dt, const map::Terrain* terrain);
    u32 shield_entity_id_ = 0;       // entity ID of shield (set by _c_CreateShield)
    bool busy_ = false;
    i32 stun_ticks_ = 0; ///< ticks of stun left (set_stunned)
    bool block_command_queue_ = false;
    i32 fire_state_ = 0;         // 0=ReturnFire, 1=HoldFire, 2=HoldGround
    u16 script_bits_ = 0;        // 9 toggle bits (0-8)
    u32 creation_tick_ = 0;      // the tick it was made (Moho's mCreationTick)
    std::unordered_set<std::string> toggle_caps_; // RULEUTC_* toggle capabilities
    f32 surface_threat_ = 0;
    f32 air_threat_ = 0;
    f32 sub_threat_ = 0;
    f32 economy_threat_ = 0;
    /// The running Script order's task (M206w).
    struct ScriptTaskRun {
        u32 serial = 0;         ///< its order's task_serial
        int object_ref = -2;    ///< its Lua object (LUA_NOREF: none runs)
        u32 wait = 0;           ///< ticks to skip before its next TaskTick
        bool suspended = false; ///< asleep until its order goes (Suspend)
    };
    ScriptTaskRun script_task_;
    u32 next_task_serial_ = 0;
    i32 script_task_result_ = 0;
    // Enhancement system
    std::map<std::string, std::string> enhancements_; // slot → enh name
    bool enhancing_ = false;
    f64 enhance_build_time_ = 0;
    std::string enhance_name_;
    std::string enhance_slot_; // blueprint Slot of enhance_name_, "" if none
    bool immobile_ = false;
    bool factory_assist_build_ = false;           // see factory_assist_build()
    u32 auto_attack_target_ = 0;                  // request_auto_attack's, used the same tick
    /// The order a build (build_target_id_) is for, and whether the builder
    /// lets it go once that order is no longer the head (M206u): a mobile
    /// build, a repair's or a guard's help. A factory's build is cancelled
    /// by its own paths (stop_unit, end_guard_build), an upgrade by its own.
    u32 build_command_id_ = 0;
    bool build_released_with_order_ = false;
    i32 assist_rolloff_wait_ = 0; ///< an assist build's roll-off (holds_for_rolloff)
    std::unordered_set<std::string> unit_states_; // generic string-based states
    // Shield health ratio (0-1); 0 until a shield sets it, as in Moho's
    // SSTIUnitVariableData (the UI shows a shield bar above 0).
    f32 shield_ratio_ = 0.0f;
    LifeBar life_bar_;
    // Bone visibility
    std::unordered_set<i32> hidden_bones_;
    // Animated bone matrices (identity = no deformation)
    std::vector<std::array<f32, 16>> animated_bone_matrices_;
    // Intel system
    std::unordered_map<std::string, IntelState> intel_states_;
    // Manipulator system
    f32 health_band_ = 1.0f;
    std::vector<std::unique_ptr<Manipulator>> manipulators_;
    std::vector<BonePose> pose_; // empty: the bind pose
    // Reused each tick by update_pose (no allocation once sized).
    std::vector<Manipulator*> pose_order_;
    PoseLocals pose_locals_;
    void update_pose();
    /// The renderer's matrices from pose_ (update_pose's end, and a load's).
    void refresh_bone_matrices();
    // Transport system
    std::vector<u32> cargo_ids_;      // entity IDs of units loaded on this transport
    i32 storage_slots_ = 0;           // Transport.StorageSlots (M206q)
    std::vector<u32> stored_ids_;     // stored units, in the order they were stored
    u32 launch_index_ = 0;            // the launch bone used last
    // Landing on carriers (M206s): the units with a place reserved, the
    // next generic point, and the queue's wait for the next pass round.
    std::vector<u32> storage_reserved_;
    u32 next_generic_ = 0;
    i32 generic_overflow_ = 0;
    // A carrier taking in aircraft ordered aboard (Moho's
    // CUnitCarrierRetrieve): those it waits for, and where it is.
    enum class RetrievePhase : u8 { None, Gather, Surface, Watch };
    RetrievePhase retrieve_phase_ = RetrievePhase::None;
    std::vector<u32> retrieve_ids_;
    i32 retrieve_wait_ = 0;
    // An aircraft landing on a carrier (Moho's CUnitCarrierLand): its place,
    // the point it comes in from, and where it is.
    enum class LandPhase : u8 { None, Approach, Hold, Descend };
    struct CarrierLanding {
        u32 carrier = 0;
        LandPhase phase = LandPhase::None;
        StoragePlace place{};
        Vector3 approach{};
        i32 wait = 0;
        bool glided = false;   // came near enough to glide the rest
        bool deck_set = false; // its turn and the deck's height fixed
    };
    CarrierLanding landing_;
    u32 transport_id_ = 0;           // entity ID of transport this unit is on (0 = not loaded)
    f32 speed_mult_ = 1.0f;          // speed multiplier (reduced when carrying cargo)
    i32 transport_class_ = 1;        // cargo TransportClass (1=small, 2=medium, 3=large)
    i32 transport_capacity_ = 0;     // transport Class1Capacity (max small slots)
    TransportLayout transport_layout_;
    std::unique_ptr<TransportSlots> transport_slots_;
    // The blueprint's size and AverageDensity (Moho's defaults 1 and 0.49):
    // a transport picks up its largest units first.
    f32 size_x_ = 1.0f, size_y_ = 1.0f, size_z_ = 1.0f;
    f32 average_density_ = 0.49f;
    /// Hang from the transport's bone that holds our slot (or sit at its
    /// origin without one): our AttachPoint bone, or centre, on it, facing
    /// as it does.
    void hang_from(const Unit& transport);
    /// Give up slots whose unit is no longer aboard (it died, was taken off
    /// the cargo list, or left), as Moho's TransportUnreserveUnattachedSpots.
    void release_stale_slots(const EntityRegistry& registry);
    // Veterancy
    u8 vet_level_ = 0;
    std::vector<std::pair<u32, f32>> damage_contributions_; // (attacker_id, cumulative_damage)
    // Stats/telemetry
    std::unordered_map<std::string, f64> stats_;
    // Silo ammo counters
    i32 nuke_silo_ammo_ = 0;
    i32 tactical_silo_ammo_ = 0;
    std::deque<bool> silo_orders_; // builds ordered, oldest first (true: a nuke)
    SiloBuild silo_build_;
    i32 silo_blocks_ = 0;
    bool assisting_silo_ = false; // this tick, a Guard lent a silo its build power
    // Orders handed to the script (M206d): a teleport charging (and the snap
    // count its warp will change), an OverCharge weapon switched on.
    bool teleporting_ = false;
    u32 teleport_snap_ = 0;
    bool overcharge_armed_ = false;
    // A transport's pickup (M206m): where it is in the order (holding for
    // its units' orders, flying to them, coming down to its hover height,
    // waiting while they board), the units it has given slots to that aren't
    // aboard yet, their centre, and the ticks it has waited there.
    enum class PickupPhase : u8 { None, Holding, Flying, Landing, Waiting };
    PickupPhase pickup_phase_ = PickupPhase::None;
    std::vector<u32> pickup_ids_;
    Vector3 pickup_center_{};
    Quaternion pickup_facing_{};
    i32 pickup_ticks_ = 0;
    f32 transport_hover_height_ = 0.0f; // Air.TransportHoverHeight
    f32 auto_land_time_ = 0.0f;         // Air.AutoLandTime
    f32 start_turn_distance_ = 0.0f;    // Air.StartTurnDistance
    IdleLanding idle_landing_;
    // A unit beaming up into its transport (M206m): ticks left of the 10,
    // and where it started.
    i32 beam_up_ticks_ = 0;
    Vector3 beam_from_{};
    Quaternion beam_from_orientation_{};
    // A ferry's cycle (M206f, Moho's CUnitFerryTask): loading at its beacon,
    // flying out along its route, unloading at its end, flying back; which
    // route point it heads for, and whether that leg's path is asked for.
    enum class FerryPhase : u8 { Load, Out, Unload, Back };
    FerryPhase ferry_phase_ = FerryPhase::Load;
    i32 ferry_index_ = 0;
    bool ferry_leg_set_ = false;
    u32 ferry_for_ = 0; // the beacon the cycle runs from: a new route starts afresh
    /// The beacon a ferry route loads at: the one a transport of its army
    /// already keeps there, else a new one from AI.BeaconName. 0 without one.
    u32 ferry_beacon(SimContext& ctx, UnitCommand& head);
    /// Fly a ferry leg toward `to`; true while under way.
    bool ferry_fly(f64 dt, SimContext& ctx, const Vector3& to);
    /// A teleport or an OverCharge whose order went unfinished: the script
    /// hears OnFailedTeleport, or the weapon OnDisableWeapon.
    void settle_interrupted_orders(lua_State* L);
    /// Take the missile under way off the silo: its request, the unit
    /// state. The script hears nothing: it was not finished (the Yolona
    /// Oss's tells a cancel by the state going without OnSiloBuildEnd).
    void abandon_silo_build();
    /// A finished missile: taken off the silo, and OnSiloBuildEnd.
    void end_silo_build(lua_State* L);
    f64 silo_blocks_progress(i32 blocks) const;
    // Adjacency system
    std::set<u32> adjacent_unit_ids_;
    int selection_priority_ = 1;
    f32 skirt_size_x_ = 0;
    f32 skirt_size_z_ = 0;
    f32 skirt_offset_x_ = 0;
    f32 skirt_offset_z_ = 0;
    // Movement multipliers
    f32 accel_mult_ = 1.0f;
    f32 turn_mult_ = 1.0f;
    f32 break_off_distance_mult_ = 1.0f;
    f32 break_off_trigger_mult_ = 1.0f;
    // Fuel system
    f32 fuel_ratio_ = -1.0f;     // -1 = no fuel system (sentinel)
    f32 fuel_use_time_ = 0.0f;   // seconds of flight time
    f32 fuel_recharge_rate_ = 0.0f; // Physics.FuelRechargeRate
    /// Docked, it has heard OnStartRefueling (Moho's mHasDoneCallback).
    bool refuel_started_ = false;
    /// Docked and damaged, it asked its army for its repair last tick: the
    /// heal comes from the next tick (Moho's economy request).
    bool dock_repair_asked_ = false;
    bool air_class_ = false;     // Transport.AirClass
    i32 docking_slots_ = 0;      // Transport.DockingSlots
    StagingRules staging_rules_; // a staging platform's service
    AirCombatRules air_combat_rules_; // a winged aircraft's attack runs
    AirCombatState air_combat_;       // its attack run under way
    // Air movement state
    f32 heading_ = 0;            // yaw in radians
    f32 pitch_ = 0;              // pitch in radians (visual only for dive/climb)
    f32 bank_angle_ = 0;         // roll in radians (visual banking on turns)
    f32 current_airspeed_ = 0;   // current speed (ramps toward max_airspeed_)
    f32 current_altitude_ = 0;   // actual Y offset above terrain
    f32 max_airspeed_ = 0;       // from blueprint Air.MaxAirspeed (fallback: max_speed_)
    f32 turn_rate_rad_ = 0;      // yaw rate rad/s, from Air.TurnSpeed (rad/s)
    f32 accel_rate_ = 0;         // from Air.AccelerateRate (fallback: max_airspeed * 0.5)
    f32 climb_rate_ = 5.0f;      // vertical speed limit (units/sec)
    f32 elevation_target_ = 18.0f; // target altitude above its air floor, from Physics.Elevation
    bool fly_in_water_ = false;    // Air.FlyInWater
    // Diving and surfacing (M206o).
    enum class VertMotion : u8 { None, Down, Up };
    VertMotion vert_motion_ = VertMotion::None;
    f32 sub_elevation_ = 0.0f;
    f32 dive_surface_speed_ = 1.0f; // Physics.DiveSurfaceSpeed
    std::string vert_event_ = "Top";
    // Air crash state
    bool crashing_ = false;
    bool crash_impacted_ = false; // set on landing, taken by SimState
    Vector3 velocity_{};
    f32 crash_velocity_y_ = 0;
    f32 crash_spin_rate_ = 0;
    // Misc flags
    u32 creator_id_ = 0;
    Vector3 tick_position_{};        // where it stood as this tick began
    bool tick_position_set_ = false; // (none before its first tick)
    bool moved_last_tick_ = false;
    bool auto_overcharge_ = false;
    bool overcharge_paused_ = false;
    bool cloaked_ = false;
    bool radar_stealth_ = false;
    bool sonar_stealth_ = false;
    bool auto_mode_ = false;
    bool repeat_queue_ = false;
    bool auto_surface_mode_ = false;
    u32 focus_entity_id_ = 0;
    // Damage/kill flags
    bool can_take_damage_ = true;
    bool can_be_killed_ = true;
    u32 last_attacker_id_ = 0;
    // Command caps
    std::unordered_set<std::string> command_caps_;
    std::unordered_set<std::string> original_command_caps_;
    // Build restrictions
    CategoryExpr build_restriction_;
    // Elevation override
    f32 elevation_override_ = -1.0f; // -1 = no override (sentinel)
    bool dying_ = false;             ///< killed; see begin_dying
    bool transferred_ = false;       ///< replaced; see set_transferred
    // OnUnitBuilt callbacks (function + category filter)
    std::vector<UnitBuiltCallback> on_unit_built_callbacks_;
    // Build queue (factory production queue)
};

/// Whether a reclaim may start on `target` (Moho's Sim.cpp UNITCOMMAND_Reclaim
/// check and CUnitReclaimTask).
bool reclaim_target_valid(const Entity& target);

} // namespace osc::sim
