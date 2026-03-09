#pragma once

#include "sim/armor_definition.hpp"
#include "sim/army_brain.hpp"
#include "sim/economy_event.hpp"
#include "sim/entity_registry.hpp"
#include "sim/ieffect.hpp"
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

    // Game end state
    bool game_ended() const { return game_ended_; }
    void set_game_ended(bool v) { game_ended_ = v; }

    /// Check if player army (index 0) won, lost, or game still in progress.
    /// Returns: 0 = in progress, 1 = victory, 2 = defeat, 3 = draw.
    i32 player_result() const;

    static constexpr f64 SECONDS_PER_TICK = 0.1;

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

private:
    void update_economies();
    void update_entities();
    void update_visibility();
    void tick_economy_events();
    void fire_on_intel_change(u32 entity_id, u32 army_idx,
                              const char* recon_type, bool val);

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
    u32 next_command_id_ = 0;
    bool game_ended_ = false;
    std::vector<CameraShakeEvent> camera_shake_events_;
    std::vector<ResourceDeposit> resource_deposits_;
    std::vector<DeathEvent> death_events_;
    std::string build_ghost_bp_;
    f32 build_ghost_foot_x_ = 1.0f;
    f32 build_ghost_foot_z_ = 1.0f;

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
