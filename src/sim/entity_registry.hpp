#pragma once

#include "core/types.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace osc::sim {

class Entity;

class EntityRegistry {
public:
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

private:
    std::unordered_map<u32, std::unique_ptr<Entity>> entities_;
    u32 next_id_ = 1;
};

} // namespace osc::sim
