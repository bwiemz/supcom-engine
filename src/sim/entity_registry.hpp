#pragma once

#include "core/types.hpp"

#include <algorithm>
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

    /// Remove an entity by ID.
    void unregister_entity(u32 id);

    /// Look up an entity by ID. Returns nullptr if not found.
    Entity* find(u32 id) const;

    /// Number of active entities.
    size_t count() const { return entities_.size(); }

    /// Initialize spatial hash grid. Must be called after map dimensions are known.
    /// If not called, collect_in_radius/collect_in_rect fall back to O(N) scan.
    void init_spatial_grid(u32 map_width, u32 map_height);

    /// Notify the registry that an entity's position has changed.
    void notify_position_changed(Entity& entity);

    /// Collect entity IDs within radius of a point (2D distance, ignoring Y).
    std::vector<u32> collect_in_radius(f32 x, f32 z, f32 radius) const;

    /// Collect entity IDs within an axis-aligned rectangle (2D, ignoring Y).
    std::vector<u32> collect_in_rect(f32 x0, f32 z0, f32 x1, f32 z1) const;

    /// Iterate all entities.
    template <typename F>
    void for_each(F&& fn) const {
        for (const auto& [id, e] : entities_)
            fn(*e);
    }

    u32 grid_width() const { return grid_width_; }
    u32 grid_height() const { return grid_height_; }
    bool grid_initialized() const { return grid_initialized_; }

private:
    std::unordered_map<u32, std::unique_ptr<Entity>> entities_;
    u32 next_id_ = 1;

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
