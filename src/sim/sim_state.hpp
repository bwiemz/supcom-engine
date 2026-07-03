#pragma once

#include "sim/armor_definition.hpp"
#include "sim/army_brain.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/economy_event.hpp"
#include "sim/entity_registry.hpp"
#include "sim/ieffect.hpp"
#include "sim/replay.hpp"
#include "sim/thread_manager.hpp"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace osc::blueprints {
class BlueprintStore;
}

namespace osc::map {
class Terrain;
class PathfindingGrid;
class Pathfinder;
class VisibilityGrid;
}

namespace osc::audio {
class SoundManager;
}

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::sim {

class AnimCache;
class BoneCache;
class SimState;
class Unit;

/// Victory condition (game mode) governing when an army is eliminated.
/// Maps to FA's lobby `Victory` option keys:
///   Demoralization = Assassination (lose all COMMAND/ACU units)
///   Domination     = Supremacy     (lose all structures + significant units)
///   Eradication    = Annihilation  (lose every unit)
///   Sandbox        = no win condition
enum class VictoryMode : i32 {
    Demoralization = 0,
    Domination = 1,
    Eradication = 2,
    Sandbox = 3,
};

/// Parse an FA `Victory` option string (or a friendly alias) into a mode.
/// Unknown values fall back to Demoralization (FA's default).
VictoryMode parse_victory_mode(const std::string& value);

/// Share condition (FA lobby `Share` option) governing what happens to a
/// defeated army's units.
enum class ShareMode : i32 {
    ShareUntilDeath = 0,  // FA default — the defeated army's units are destroyed
    FullShare = 1,        // units transfer to a surviving ally (else destroyed)
    CivilianDeserter = 2, // units transfer to a civilian army (else destroyed)
    // Killer-relative modes (TransferToKiller / Defectors) and PartialShare are
    // accepted but, lacking per-unit killer attribution, resolve to destruction.
    PartialShare = 3,
    TransferToKiller = 4,
    Defectors = 5,
};

/// Parse an FA `Share` option string into a ShareMode (default ShareUntilDeath).
ShareMode parse_share_mode(const std::string& value);

/// Fog-of-war mode (FA lobby `FogOfWar` option).
enum class FogMode : i32 {
    Explored = 0, // normal fog: unexplored hidden, explored dimmed, seen lit
    None = 1,     // no fog: the whole map and all units are always revealed
};

/// Parse an FA `FogOfWar` option string into a FogMode (default Explored).
FogMode parse_fog_mode(const std::string& value);

/// Camera shake event queued by ShakeCamera moho method.
struct CameraShakeEvent {
    f32 x = 0, z = 0;       // world position of shake source
    f32 radius = 30;         // max radius of effect
    f32 max_shake = 1;       // intensity at epicenter
    f32 min_shake = 0;       // intensity at edge of radius
    f32 duration = 0.5f;     // seconds
};

/// Per-army resource efficiency (pre-computed per tick).
struct ArmyEfficiency {
    f64 mass = 1.0;
    f64 energy = 1.0;
};

/// Cached blip data for dead-reckoning (last-known position when entity
/// leaves intel coverage, or entity destroyed while previously seen).
struct BlipSnapshot {
    Vector3 last_known_position;
    std::string blueprint_id;
    i32 entity_army = -1; // 0-based
    bool entity_dead = false;
};

/// Resource deposit (mass/hydrocarbon point on map).
struct ResourceDeposit {
    f32 x = 0, y = 0, z = 0;
    f32 size = 1.0f;
    enum Type : u8 { Mass = 0, Hydrocarbon = 1 } type = Mass;
};

/// Lightweight context passed to Unit::update() each tick.
struct SimContext {
    EntityRegistry& registry;
    lua_State* L;
    const map::Terrain* terrain;
    const map::Pathfinder* pathfinder;
    map::PathfindingGrid* pathfinding_grid; // non-const for obstacle marking
    const map::VisibilityGrid* visibility_grid;
    SimState* sim;
    static constexpr u32 MAX_EFFICIENCY_ARMIES = 16;
    std::array<ArmyEfficiency, MAX_EFFICIENCY_ARMIES> army_efficiency;
};

class SimState {
public:
    SimState(lua_State* L, blueprints::BlueprintStore* store);
    ~SimState();

    EntityRegistry& entity_registry() { return entity_registry_; }
    const EntityRegistry& entity_registry() const { return entity_registry_; }

    ThreadManager& thread_manager() { return thread_manager_; }

    blueprints::BlueprintStore* blueprint_store() { return blueprint_store_; }
    blueprints::BlueprintStore* blueprint_store() const { return blueprint_store_; }

    // Terrain & Pathfinding
    void set_terrain(std::unique_ptr<map::Terrain> terrain);
    map::Terrain* terrain() const { return terrain_.get(); }
    void build_pathfinding_grid();
    map::PathfindingGrid* pathfinding_grid() { return pathfinding_grid_.get(); }
    const map::Pathfinder* pathfinder() const { return pathfinder_.get(); }
    void build_visibility_grid();
    void build_spatial_grid();
    map::VisibilityGrid* visibility_grid() { return visibility_grid_.get(); }
    void add_temp_vision(u32 army, f32 x, f32 z, f32 radius, f32 lifetime_sec) {
        temp_visions_.push_back({army, x, z, radius, static_cast<i32>(lifetime_sec * 10.0f)});
    }

    // Audio
    void set_sound_manager(std::unique_ptr<audio::SoundManager> mgr);
    audio::SoundManager* sound_manager() { return sound_manager_.get(); }

    // Bones
    void set_bone_cache(std::unique_ptr<BoneCache> cache);
    BoneCache* bone_cache() { return bone_cache_.get(); }

    // Animation
    void set_anim_cache(std::unique_ptr<AnimCache> cache);
    AnimCache* anim_cache() { return anim_cache_.get(); }

    // Army/Brain management
    ArmyBrain& add_army(const std::string& name, const std::string& nickname);
    ArmyBrain* get_army(i32 index);
    ArmyBrain* get_army_by_name(const std::string& name);
    size_t army_count() const { return armies_.size(); }

    /// Iterate armies (for range-for or indexed access).
    ArmyBrain* army_at(size_t i) {
        return i < armies_.size() ? armies_[i].get() : nullptr;
    }
    const ArmyBrain* army_at(size_t i) const {
        return i < armies_.size() ? armies_[i].get() : nullptr;
    }

    // Alliance convenience
    void set_alliance(i32 army1, i32 army2, Alliance alliance);
    bool is_ally(i32 army1, i32 army2) const;
    bool is_enemy(i32 army1, i32 army2) const;
    bool is_neutral(i32 army1, i32 army2) const;

    // Armor definitions
    const ArmorDefinition& armor_definition() const { return armor_def_; }
    ArmorDefinition& armor_definition() { return armor_def_; }

    // Lua state access (for weapon/projectile updates)
    lua_State* lua_state() const { return L_; }

    // Tick loop
    void tick();
    u32 tick_count() const { return tick_count_; }
    f64 game_time() const { return game_time_; }

    // --- Command scheduling (deterministic / lockstep-ready) ---
    // All player/AI/network orders can be routed through the scheduler so they
    // apply on a fixed future tick, in a canonical order, identically on every
    // client. tick() dispatches the commands due for the current tick.
    CommandScheduler& command_scheduler() { return command_scheduler_; }
    const CommandScheduler& command_scheduler() const { return command_scheduler_; }

    /// Ticks of delay between submitting an order and it executing (0 = next
    /// tick). Multiplayer raises this to cover network round-trip.
    void set_command_delay(u32 ticks) { command_delay_ = ticks; }
    u32 command_delay() const { return command_delay_; }

    /// Submit an order for `unit_ids` from `source`, to execute after the
    /// command delay. Returns the tick it will run on.
    u32 schedule_command(u32 source, const std::vector<u32>& unit_ids,
                         const UnitCommand& command, bool clear_existing);

    /// Whether the next tick may run yet (always true in single-player; in
    /// lockstep, false until every peer has confirmed its command frame).
    bool ready_to_run_next_tick() const {
        return command_scheduler_.ready_to_run(tick_count_ + 1);
    }

    // --- Replay recording / playback ---
    // With recording on, every scheduled command is captured into a Replay that
    // (thanks to lockstep determinism) reproduces the match when re-fed into a
    // fresh sim.
    void set_recording(bool on) { recording_ = on; }
    bool recording() const { return recording_; }
    const Replay& recorded_replay() const { return recorded_replay_; }

    /// Re-submit a recorded command stream into this sim's scheduler. Commands
    /// carry their original exec ticks, so ticking the sim replays them in
    /// order. Also restores the recorded command delay / victory condition.
    void queue_replay(const Replay& replay);

    // Game end state
    bool game_ended() const { return game_ended_; }
    void set_game_ended(bool v) { game_ended_ = v; }
    const std::string& victory_condition() const { return victory_condition_; }
    void set_victory_condition(std::string mode);
    VictoryMode victory_mode() const { return victory_mode_; }
    bool sandbox_victory() const { return victory_mode_ == VictoryMode::Sandbox; }
    const std::string& share_condition() const { return share_condition_; }
    void set_share_condition(std::string mode);
    ShareMode share_mode() const { return share_mode_; }
    void set_fog_of_war(std::string mode);
    FogMode fog_mode() const { return fog_mode_; }

    /// Common Army: allied (same-team) armies pool their mass/energy each tick,
    /// so a teammate's reserves back up one that is stalling. Off by default.
    void set_common_army(bool on) { common_army_ = on; }
    bool common_army() const { return common_army_; }

    /// "No Rush" rule: for the first `seconds` of game time, units are confined
    /// within `radius` of their army's start position. seconds <= 0 disables it.
    void set_no_rush(f32 seconds, f32 radius);
    f32 no_rush_seconds() const { return no_rush_seconds_; }
    f32 no_rush_radius() const { return no_rush_radius_; }
    bool no_rush_active() const {
        return no_rush_seconds_ > 0.0f &&
               static_cast<f32>(game_time_) < no_rush_seconds_;
    }

    /// Check if player army (index 0) won, lost, or game still in progress.
    /// Returns: 0 = in progress, 1 = victory, 2 = defeat, 3 = draw.
    i32 player_result() const;

    /// Number of alliance-connected "teams" still in the game (non-civilian,
    /// non-defeated armies grouped by their alliance graph). Exposed for tests
    /// and diagnostics.
    i32 surviving_team_count() const;

    /// Deterministic checksum of the authoritative sim state (tick, army
    /// economies/states, and every entity's id/army/position/health). Two sims
    /// fed identical inputs on the same build produce identical checksums; a
    /// divergence signals a desync. This is the primitive lockstep multiplayer
    /// uses to detect out-of-sync clients, and validates single-player
    /// determinism / replay. Order-independent (entities are sorted by id).
    u32 compute_sync_checksum() const;

    static constexpr f64 SECONDS_PER_TICK = 0.1;

    /// Global sim generation — incremented each time a SimState is constructed.
    /// Used by entity handle safety to detect stale references across reloads.
    static u32 sim_generation() { return s_sim_generation_; }
    static void increment_sim_generation() { ++s_sim_generation_; }

    /// Monotonically increasing command ID for IsCommandsActive tracking.
    u32 next_command_id() { return ++next_command_id_; }

    // VFX / IEffect registry
    IEffectRegistry& effect_registry() { return effect_registry_; }
    const IEffectRegistry& effect_registry() const { return effect_registry_; }

    // Economy event registry
    EconomyEventRegistry& economy_events() { return economy_events_; }

    // Resource deposits
    void add_resource_deposit(const ResourceDeposit& d) { resource_deposits_.push_back(d); }
    const std::vector<ResourceDeposit>& resource_deposits() const { return resource_deposits_; }

    // Camera shake events (consumed by renderer each frame)
    void add_camera_shake(const CameraShakeEvent& e) { camera_shake_events_.push_back(e); }
    const std::vector<CameraShakeEvent>& camera_shake_events() const { return camera_shake_events_; }
    void clear_camera_shake_events() { camera_shake_events_.clear(); }

    // Death events (consumed by renderer for explosion VFX)
    struct DeathEvent {
        f32 x, y, z;    // world position
        f32 scale;       // explosion scale (from unit footprint)
        i32 army;        // army index (for army-colored flash)
    };
    void add_death_event(f32 x, f32 y, f32 z, f32 scale, i32 army) {
        death_events_.push_back({x, y, z, scale, army});
    }
    const std::vector<DeathEvent>& death_events() const { return death_events_; }
    void clear_death_events() { death_events_.clear(); }

    // Playable area bounds (set by SetPlayableRect Lua call)
    void set_playable_rect(f32 x0, f32 z0, f32 x1, f32 z1) {
        playable_x0_ = x0; playable_z0_ = z0;
        playable_x1_ = x1; playable_z1_ = z1;
        has_playable_rect_ = true;
    }
    bool has_playable_rect() const { return has_playable_rect_; }
    f32 playable_x0() const { return playable_x0_; }
    f32 playable_z0() const { return playable_z0_; }
    f32 playable_x1() const { return playable_x1_; }
    f32 playable_z1() const { return playable_z1_; }

    Vector3 clamp_to_playable(const Vector3& pos) const {
        if (!has_playable_rect_) return pos;
        Vector3 clamped = pos;
        if (clamped.x < playable_x0_) clamped.x = playable_x0_;
        if (clamped.x > playable_x1_) clamped.x = playable_x1_;
        if (clamped.z < playable_z0_) clamped.z = playable_z0_;
        if (clamped.z > playable_z1_) clamped.z = playable_z1_;
        return clamped;
    }

    bool is_valid_teleport_destination(const Unit& unit,
                                       const Vector3& destination) const;

private:
    void update_economies();
    /// Pool mass/energy across allied teams (Common Army). No-op unless enabled.
    void share_team_economy();
    /// Partition all non-civilian armies into alliance-connected teams.
    std::vector<std::vector<i32>> alliance_teams() const;
    void update_entities();
    void update_visibility();
    void dispatch_due_commands();
    void enforce_no_rush();
    /// Clamp a Move/Attack target to a unit's no-rush zone when the rule is
    /// active. Returns the (possibly clamped) position.
    Vector3 clamp_to_no_rush(const Unit& unit, const Vector3& target) const;
    void update_victory();
    /// Dispose of a just-defeated army's units per the active share condition.
    void dispose_defeated_army(i32 army);
    /// Recipient army index for unit transfer on defeat, or -1 to destroy.
    i32 find_share_recipient(i32 defeated_army) const;
    /// Count alliance-connected components among the given (alive) army indices.
    i32 count_alliance_components(const std::vector<i32>& army_indices) const;
    void tick_economy_events();
    void fire_on_intel_change(u32 entity_id, u32 army_idx,
                              const char* recon_type, bool val);

    // Stealth-aware intel helpers with pre-cached stealth flags to avoid
    // redundant per-army is_intel_enabled string lookups in inner loops.
    bool has_effective_radar_cached(const Entity* entity, u32 req_army,
                                    bool radar_stealth) const;
    bool has_effective_sonar_cached(const Entity* entity, u32 req_army,
                                    bool sonar_stealth) const;
    bool has_any_intel_cached(const Entity* entity, u32 req_army,
                              bool radar_stealth, bool sonar_stealth,
                              bool cloaked) const;

    lua_State* L_;
    EntityRegistry entity_registry_;
    ThreadManager thread_manager_;
    blueprints::BlueprintStore* blueprint_store_;
    std::unique_ptr<map::Terrain> terrain_;
    std::unique_ptr<map::PathfindingGrid> pathfinding_grid_;
    std::unique_ptr<map::Pathfinder> pathfinder_;
    std::unique_ptr<map::VisibilityGrid> visibility_grid_;
    std::unique_ptr<audio::SoundManager> sound_manager_;
    std::unique_ptr<BoneCache> bone_cache_;
    std::unique_ptr<AnimCache> anim_cache_;
    ArmorDefinition armor_def_;
    IEffectRegistry effect_registry_;
    EconomyEventRegistry economy_events_;
    std::vector<std::unique_ptr<ArmyBrain>> armies_;
    u32 tick_count_ = 0;
    f64 game_time_ = 0.0;
    CommandScheduler command_scheduler_;
    u32 command_delay_ = 0;
    Replay recorded_replay_;
    bool recording_ = false;

    // Temporary vision areas (scrying, Eye of Rhianne)
    struct TempVision {
        u32 army;
        f32 x, z, radius;
        i32 remaining_ticks;
    };
    std::vector<TempVision> temp_visions_;
    u32 next_command_id_ = 0;
    bool game_ended_ = false;
    std::string victory_condition_ = "demoralization";
    VictoryMode victory_mode_ = VictoryMode::Demoralization;
    std::string share_condition_ = "shareuntildeath";
    ShareMode share_mode_ = ShareMode::ShareUntilDeath;
    FogMode fog_mode_ = FogMode::Explored;
    f32 no_rush_seconds_ = 0.0f;   // 0 = No Rush disabled
    f32 no_rush_radius_ = 75.0f;   // default confinement radius (game units)
    bool common_army_ = false;     // pool allied economies when true
    std::vector<CameraShakeEvent> camera_shake_events_;
    std::vector<ResourceDeposit> resource_deposits_;
    std::vector<DeathEvent> death_events_;
    std::string build_ghost_bp_;
    f32 build_ghost_foot_x_ = 1.0f;
    f32 build_ghost_foot_z_ = 1.0f;
    f32 playable_x0_ = 0, playable_z0_ = 0;
    f32 playable_x1_ = 0, playable_z1_ = 0;
    bool has_playable_rect_ = false;

    static u32 s_sim_generation_;

    // Per-entity per-army previous visibility for OnIntelChange detection
    struct EntityVisSnapshot {
        bool vision = false;
        bool radar = false;
        bool sonar = false;
        bool omni = false;
    };
    static constexpr u32 MAX_VIS_ARMIES = 16;
    std::unordered_map<u32, std::array<EntityVisSnapshot, MAX_VIS_ARMIES>>
        prev_entity_vis_;

    // Dead-reckoning blip cache: per-entity per-army last-known data
    std::unordered_map<u32, std::array<BlipSnapshot, MAX_VIS_ARMIES>>
        blip_cache_;

public:
    // Build preview ghost (set by UI, consumed by renderer)
    void set_build_ghost(const std::string& bp_id, f32 foot_x, f32 foot_z) {
        build_ghost_bp_ = bp_id;
        build_ghost_foot_x_ = foot_x;
        build_ghost_foot_z_ = foot_z;
    }
    void clear_build_ghost() { build_ghost_bp_.clear(); }
    const std::string& build_ghost_bp() const { return build_ghost_bp_; }
    f32 build_ghost_foot_x() const { return build_ghost_foot_x_; }
    f32 build_ghost_foot_z() const { return build_ghost_foot_z_; }

    /// Look up cached blip snapshot for a specific entity+army pair.
    const BlipSnapshot* get_blip_snapshot(u32 entity_id, u32 army) const;

    /// Stealth-aware intel queries (check RadarStealth/SonarStealth).
    bool has_effective_radar(const Entity* entity, u32 req_army) const;
    bool has_effective_sonar(const Entity* entity, u32 req_army) const;
    bool has_any_intel(const Entity* entity, u32 req_army) const;
};

} // namespace osc::sim
