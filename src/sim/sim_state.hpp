#pragma once

#include "sim/army_brain.hpp"
#include "sim/entity_registry.hpp"
#include "sim/thread_manager.hpp"

#include <array>
#include <memory>
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

namespace osc::sim {

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

    // Terrain & Pathfinding
    void set_terrain(std::unique_ptr<map::Terrain> terrain);
    map::Terrain* terrain() const { return terrain_.get(); }
    void build_pathfinding_grid();
    map::PathfindingGrid* pathfinding_grid() { return pathfinding_grid_.get(); }
    const map::Pathfinder* pathfinder() const { return pathfinder_.get(); }
    void build_visibility_grid();
    map::VisibilityGrid* visibility_grid() { return visibility_grid_.get(); }

    // Army/Brain management
    ArmyBrain& add_army(const std::string& name, const std::string& nickname);
    ArmyBrain* get_army(i32 index);
    ArmyBrain* get_army_by_name(const std::string& name);
    size_t army_count() const { return armies_.size(); }

    /// Iterate armies (for range-for or indexed access).
    ArmyBrain* army_at(size_t i) {
        return i < armies_.size() ? armies_[i].get() : nullptr;
    }

    // Alliance convenience
    void set_alliance(i32 army1, i32 army2, Alliance alliance);
    bool is_ally(i32 army1, i32 army2) const;
    bool is_enemy(i32 army1, i32 army2) const;
    bool is_neutral(i32 army1, i32 army2) const;

    // Lua state access (for weapon/projectile updates)
    lua_State* lua_state() const { return L_; }

    // Tick loop
    void tick();
    u32 tick_count() const { return tick_count_; }
    f64 game_time() const { return game_time_; }

    static constexpr f64 SECONDS_PER_TICK = 0.1;

    /// Monotonically increasing command ID for IsCommandsActive tracking.
    u32 next_command_id() { return ++next_command_id_; }

private:
    void update_economies();
    void update_entities();
    void update_visibility();
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
    std::vector<std::unique_ptr<ArmyBrain>> armies_;
    u32 tick_count_ = 0;
    f64 game_time_ = 0.0;
    u32 next_command_id_ = 0;

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
    /// Look up cached blip snapshot for a specific entity+army pair.
    const BlipSnapshot* get_blip_snapshot(u32 entity_id, u32 army) const;

    /// Mark entity as dead in all army blip cache entries.
    void mark_entity_dead_in_cache(u32 entity_id);

    /// Stealth-aware intel queries (check RadarStealth/SonarStealth).
    bool has_effective_radar(const Entity* entity, u32 req_army) const;
    bool has_effective_sonar(const Entity* entity, u32 req_army) const;
    bool has_any_intel(const Entity* entity, u32 req_army) const;
};

} // namespace osc::sim
