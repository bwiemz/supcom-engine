#include "sim/entity_registry.hpp"
#include "sim/collision.hpp"
#include "sim/entity.hpp"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace osc::sim {

EntityRegistry::EntityRegistry() = default;
EntityRegistry::~EntityRegistry() = default;

u32 EntityRegistry::register_entity(std::unique_ptr<Entity> entity) {
    u32 id = next_id_++;
    entity->set_entity_id(id);
    entity->set_registry(this);
    order_.push_back({id, entity.get()});
    if (entity->is_unit()) unit_order_.push_back({id, entity.get()});
    entities_[id] = std::move(entity);

    notify_collision_shape_changed(*entities_[id]);
    if (grid_initialized_) {
        auto* e = entities_[id].get();
        i32 cx, cz;
        world_to_cell(e->position().x, e->position().z, cx, cz);
        grid_insert(id, cx, cz);
        e->set_grid_cell(cx, cz);
    }

    return id;
}

void EntityRegistry::unregister_entity(u32 id) {
    auto it = entities_.find(id);
    if (it == entities_.end()) return;
    // Take the entity out before the hook: the hook runs scripts (OnDestroy),
    // which may create or remove entities and so rehash the map.
    std::unique_ptr<Entity> entity = std::move(it->second);
    entities_.erase(it);
    const auto clear_slot = [id](std::vector<Slot>& order) {
        auto slot = std::lower_bound(order.begin(), order.end(), id,
                                     [](const Slot& s, u32 v) { return s.id < v; });
        if (slot == order.end() || slot->id != id || !slot->entity) return false;
        slot->entity = nullptr;
        return true;
    };
    if (clear_slot(order_)) ++removed_slots_;
    large_colliders_.erase(id);
    if (entity->is_unit()) clear_slot(unit_order_);
    if (grid_initialized_) {
        i32 cx = entity->grid_cell_x();
        i32 cz = entity->grid_cell_z();
        if (cx >= 0) grid_remove(id, cx, cz);
    }
    entity->set_registry(nullptr);
    Entity& ref = *entity;
    graveyard_.push_back(std::move(entity)); // freed by collect_garbage()
    if (unregister_hook_) unregister_hook_(ref);
}

Entity* EntityRegistry::find(u32 id) const {
    auto it = entities_.find(id);
    return it != entities_.end() ? it->second.get() : nullptr;
}

void EntityRegistry::collect_garbage() {
    graveyard_.clear();
    compact();
}

void EntityRegistry::compact() {
    // Not under a walk: it holds its place by index.
    if (walking_ > 0 || removed_slots_ == 0) return;
    const auto empty = [](const Slot& s) { return s.entity == nullptr; };
    std::erase_if(order_, empty);
    std::erase_if(unit_order_, empty);
    removed_slots_ = 0;
}

// --- Spatial hash grid ---

void EntityRegistry::init_spatial_grid(u32 map_width, u32 map_height) {
    // Reset all entity grid cells so stale coordinates are not reused on re-init
    for_each([](Entity& e) { e.set_grid_cell(-1, -1); });
    grid_cells_.clear();

    grid_width_ = (map_width + CELL_SIZE - 1) / CELL_SIZE;
    grid_height_ = (map_height + CELL_SIZE - 1) / CELL_SIZE;
    if (grid_width_ == 0) grid_width_ = 1;
    if (grid_height_ == 0) grid_height_ = 1;
    grid_cells_.resize(static_cast<size_t>(grid_width_) * grid_height_);
    grid_initialized_ = true;

    // Retroactively insert all existing entities (e.g. props created before grid init)
    for_each([this](Entity& e) {
        i32 cx, cz;
        world_to_cell(e.position().x, e.position().z, cx, cz);
        grid_insert(e.entity_id(), cx, cz);
        e.set_grid_cell(cx, cz);
    });

    spdlog::info("Spatial hash grid: {}x{} cells (cell_size={}u, {} entities indexed)",
                 grid_width_, grid_height_, CELL_SIZE, entities_.size());
}

void EntityRegistry::world_to_cell(f32 wx, f32 wz, i32& cx, i32& cz) const {
    cx = static_cast<i32>(std::floor(wx / static_cast<f32>(CELL_SIZE)));
    cz = static_cast<i32>(std::floor(wz / static_cast<f32>(CELL_SIZE)));
    cx = std::clamp(cx, 0, static_cast<i32>(grid_width_) - 1);
    cz = std::clamp(cz, 0, static_cast<i32>(grid_height_) - 1);
}

size_t EntityRegistry::cell_index(i32 cx, i32 cz) const {
    return static_cast<size_t>(cz) * grid_width_ + static_cast<size_t>(cx);
}

void EntityRegistry::grid_insert(u32 entity_id, i32 cx, i32 cz) {
    grid_cells_[cell_index(cx, cz)].push_back(entity_id);
}

void EntityRegistry::grid_remove(u32 entity_id, i32 cx, i32 cz) {
    auto& cell = grid_cells_[cell_index(cx, cz)];
    auto it = std::find(cell.begin(), cell.end(), entity_id);
    if (it != cell.end()) {
        *it = cell.back(); // swap-and-pop for O(1) removal
        cell.pop_back();
    }
}

void EntityRegistry::notify_position_changed(Entity& entity) {
    if (!grid_initialized_) return;

    i32 new_cx, new_cz;
    world_to_cell(entity.position().x, entity.position().z, new_cx, new_cz);

    i32 old_cx = entity.grid_cell_x();
    i32 old_cz = entity.grid_cell_z();

    if (old_cx == new_cx && old_cz == new_cz) return; // same cell, no update

    if (old_cx >= 0) grid_remove(entity.entity_id(), old_cx, old_cz);
    grid_insert(entity.entity_id(), new_cx, new_cz);
    entity.set_grid_cell(new_cx, new_cz);
}

std::vector<u32> EntityRegistry::collect_in_radius(f32 x, f32 z,
                                                    f32 radius) const {
    std::vector<u32> result;
    f32 r2 = radius * radius;

    if (!grid_initialized_) {
        // Fallback to O(N) scan (already in id order)
        for_each([&](const Entity& e) {
            if (e.destroyed()) return;
            f32 dx = e.position().x - x;
            f32 dz = e.position().z - z;
            if (dx * dx + dz * dz <= r2) result.push_back(e.entity_id());
        });
        return result;
    }

    // Compute cell range covering the bounding box of the circle
    i32 cx_min, cz_min, cx_max, cz_max;
    world_to_cell(x - radius, z - radius, cx_min, cz_min);
    world_to_cell(x + radius, z + radius, cx_max, cz_max);

    for (i32 cz = cz_min; cz <= cz_max; ++cz) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            const auto& cell = grid_cells_[cell_index(cx, cz)];
            for (u32 id : cell) {
                auto* e = find(id);
                if (!e || e->destroyed()) continue;
                f32 dx = e->position().x - x;
                f32 dz = e->position().z - z;
                if (dx * dx + dz * dz <= r2)
                    result.push_back(id);
            }
        }
    }
    // A cell lists ids in the order they moved in; callers get a canonical order.
    std::sort(result.begin(), result.end());
    return result;
}

std::vector<u32> EntityRegistry::collect_in_rect(f32 x0, f32 z0,
                                                  f32 x1, f32 z1) const {
    std::vector<u32> result;
    if (x0 > x1) std::swap(x0, x1);
    if (z0 > z1) std::swap(z0, z1);

    if (!grid_initialized_) {
        // Fallback to O(N) scan (already in id order)
        for_each([&](const Entity& e) {
            if (e.destroyed()) return;
            f32 ex = e.position().x;
            f32 ez = e.position().z;
            if (ex >= x0 && ex <= x1 && ez >= z0 && ez <= z1) result.push_back(e.entity_id());
        });
        return result;
    }

    i32 cx_min, cz_min, cx_max, cz_max;
    world_to_cell(x0, z0, cx_min, cz_min);
    world_to_cell(x1, z1, cx_max, cz_max);

    for (i32 cz = cz_min; cz <= cz_max; ++cz) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            const auto& cell = grid_cells_[cell_index(cx, cz)];
            for (u32 id : cell) {
                auto* e = find(id);
                if (!e || e->destroyed()) continue;
                f32 ex = e->position().x;
                f32 ez = e->position().z;
                if (ex >= x0 && ex <= x1 && ez >= z0 && ez <= z1)
                    result.push_back(id);
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

void EntityRegistry::notify_collision_shape_changed(const Entity& entity) {
    if (collision_reach(entity.collision_shape()) > COLLIDER_REACH)
        large_colliders_.insert(entity.entity_id());
    else large_colliders_.erase(entity.entity_id());
}

void EntityRegistry::collect_colliders(f32 x0, f32 z0, f32 x1, f32 z1,
                                       std::vector<u32>& out) const {
    out.clear();
    if (x0 > x1) std::swap(x0, x1);
    if (z0 > z1) std::swap(z0, z1);
    const auto consider = [&](const Entity& e, f32 reach) {
        if (e.destroyed() || e.collision_shape().type == CollisionShapeType::NONE) return;
        const Vector3& p = e.position();
        if (p.x >= x0 - reach && p.x <= x1 + reach && p.z >= z0 - reach && p.z <= z1 + reach)
            out.push_back(e.entity_id());
    };
    if (!grid_initialized_) {
        for_each([&](const Entity& e) { consider(e, COLLIDER_REACH); });
    } else {
        i32 cx_min, cz_min, cx_max, cz_max;
        world_to_cell(x0 - COLLIDER_REACH, z0 - COLLIDER_REACH, cx_min, cz_min);
        world_to_cell(x1 + COLLIDER_REACH, z1 + COLLIDER_REACH, cx_max, cz_max);
        for (i32 cz = cz_min; cz <= cz_max; ++cz)
            for (i32 cx = cx_min; cx <= cx_max; ++cx)
                for (u32 id : grid_cells_[cell_index(cx, cz)])
                    if (const Entity* e = find(id)) consider(*e, COLLIDER_REACH);
    }
    for (u32 id : large_colliders_)
        if (const Entity* e = find(id)) consider(*e, collision_reach(e->collision_shape()));
    // A cell lists ids in the order they moved in, and a large shape may be
    // in a cell too: one canonical list.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

} // namespace osc::sim
