#pragma once

#include "core/types.hpp"
#include "sim/sim_random.hpp"

#include <algorithm>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace osc::sim {

class Entity;

class EntityRegistry {
public:
    static constexpr u32 CELL_SIZE = 32;

    EntityRegistry();
    ~EntityRegistry();

    /// Register an entity and assign it a unique ID. Returns the ID.
    u32 register_entity(std::unique_ptr<Entity> entity);

    /// Remove an entity by ID. Every removal path (Lua Destroy, reclaim,
    /// sacrifice, projectile impact, ...) goes through here, so the
    /// unregister hook sees each entity exactly once, while still valid.
    void unregister_entity(u32 id);

    /// Called for each entity just before it is removed. SimState uses it to
    /// release what the entity owns outside the registry (its Lua table's
    /// pointer back to C++, a structure's pathfinding footprint).
    using UnregisterHook = std::function<void(Entity&)>;
    void set_unregister_hook(UnregisterHook hook) { unregister_hook_ = std::move(hook); }

    /// Look up an entity by ID. Returns nullptr if not found.
    Entity* find(u32 id) const;

    /// Number of active entities.
    size_t count() const { return entities_.size(); }

    /// Free the entities unregistered since the last call. An unregistered
    /// entity leaves every lookup immediately but its memory lives until
    /// here: scripts routinely destroy a unit from inside one of its own
    /// callbacks (a structure finishing its upgrade replaces itself), and the
    /// C++ frame that ran the callback is still inside that unit's update.
    /// SimState calls this at the end of each tick.
    void collect_garbage();

    /// Initialize spatial hash grid. Must be called after map dimensions are known.
    /// If not called, collect_in_radius/collect_in_rect fall back to O(N) scan.
    void init_spatial_grid(u32 map_width, u32 map_height);

    /// Notify the registry that an entity's position has changed.
    void notify_position_changed(Entity& entity);

    /// Collect entity IDs within radius of a point (2D distance, ignoring Y),
    /// in ascending id order.
    std::vector<u32> collect_in_radius(f32 x, f32 z, f32 radius) const;

    /// Collect entity IDs within an axis-aligned rectangle (2D, ignoring Y),
    /// in ascending id order.
    std::vector<u32> collect_in_rect(f32 x0, f32 z0, f32 x1, f32 z1) const;

    /// Iterate all entities in id order: the same order on every platform,
    /// as lockstep needs (a hash map's order is the standard library's).
    /// Entities created during the walk are visited by the next walk;
    /// entities removed during it are skipped.
    template <typename F>
    void for_each(F&& fn) const {
        const Walking guard(walking_);
        const size_t n = order_.size();
        for (size_t i = 0; i < n; ++i)
            if (Entity* e = order_[i].entity) fn(*e);
    }

    u32 grid_width() const { return grid_width_; }
    u32 grid_height() const { return grid_height_; }
    bool grid_initialized() const { return grid_initialized_; }

    /// Deterministic sim RNG used by sim code (e.g. weapon firing randomness).
    /// SimState points this at its own SimRandom so every client shares one
    /// seeded stream; a bare registry (unit tests) falls back to its own.
    void set_sim_random(SimRandom* r) { sim_random_ = r ? r : &default_random_; }
    SimRandom& sim_random() { return *sim_random_; }

private:
    std::unordered_map<u32, std::unique_ptr<Entity>> entities_; ///< lookup only
    /// Every live entity in id order (ids only grow, so a new one appends).
    /// A removed entity leaves a null slot until compact(), so a walk in
    /// progress keeps its place.
    struct Slot {
        u32 id;
        Entity* entity;
    };
    std::vector<Slot> order_;
    size_t removed_slots_ = 0;
    /// Walks in progress (a walk's callback may start another).
    mutable u32 walking_ = 0;
    struct Walking {
        u32& depth;
        explicit Walking(u32& d) : depth(d) { ++depth; }
        ~Walking() { --depth; }
        Walking(const Walking&) = delete;
        Walking& operator=(const Walking&) = delete;
    };
    void compact();
    std::vector<std::unique_ptr<Entity>> graveyard_; ///< see collect_garbage()
    UnregisterHook unregister_hook_;
    u32 next_id_ = 1;

    SimRandom default_random_;
    SimRandom* sim_random_ = &default_random_;

    // Spatial hash grid
    bool grid_initialized_ = false;
    u32 grid_width_ = 0;
    u32 grid_height_ = 0;
    std::vector<std::vector<u32>> grid_cells_;

    void world_to_cell(f32 wx, f32 wz, i32& cx, i32& cz) const;
    size_t cell_index(i32 cx, i32 cz) const;
    void grid_insert(u32 id, i32 cx, i32 cz);
    void grid_remove(u32 id, i32 cx, i32 cz);
};

} // namespace osc::sim
