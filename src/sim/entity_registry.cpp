#include "sim/entity_registry.hpp"
#include "sim/collision.hpp"
#include "sim/entity.hpp"

#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace osc::sim {

EntityRegistry::EntityRegistry() = default;
EntityRegistry::~EntityRegistry() = default;

EntityId EntityRegistry::reserve_id(const Entity& e) {
    const u32 source = e.army() >= 0 ? static_cast<u32>(e.army()) & 0xFFu : 0xFFu;
    u32 bits = source << 20;
    if (e.is_projectile()) bits = (source | 0x100u) << 20;
    else if (e.is_prop()) bits = 0x2FFu << 20;
    else if (e.is_shield()) bits = (source | 0x400u) << 20;
    else if (!e.is_unit()) bits = (source | 0x500u) << 20;
    IdPool& pool = id_pools_[bits];
    for (;;) {
        u32 serial = 0;
        if (pool.released.empty()) {
            serial = pool.next++;
        } else {
            serial = *pool.released.begin();
            pool.released.erase(pool.released.begin());
        }
        const u32 moho = bits | serial;
        // Past 2^20 serials Moho's id runs into another pool's, whose entity may be alive.
        if (live_by_moho_.contains(moho)) {
            continue;
        }
        std::vector<u32>& generations = id_pools_[moho & 0xFFF00000u].generations;
        const u32 index = moho & 0xFFFFFu;
        if (index >= generations.size()) {
            generations.resize(index + 1);
        }
        u32& generation = generations[index];
        generation = generation % EntityId::kMaxGeneration + 1;
        return EntityId::of(moho, generation);
    }
}

EntityId EntityRegistry::live_id(u32 moho) const {
    const auto it = live_by_moho_.find(moho);
    return it == live_by_moho_.end() ? EntityId() : it->second;
}

void EntityRegistry::update_id_pools() {
    for (auto& [bits, pool] : id_pools_) {
        if (pool.held.size() == kIdHeldBeats) {
            const std::vector<u32>& oldest = pool.held.front();
            if (!oldest.empty()) {
                pool.released.insert(oldest.begin(), oldest.end());
                auto top = pool.released.lower_bound(pool.next);
                u32 next = pool.next;
                while (top != pool.released.begin() && *std::prev(top) == next - 1) {
                    --top;
                    --next;
                }
                if (next != pool.next) {
                    pool.released.erase(top, pool.released.end());
                    pool.next = next;
                }
            }
            pool.held.pop_front();
        }
        pool.held.emplace_back();
    }
}

std::array<u32, 16> EntityRegistry::family_counts() const {
    std::array<u32, 16> counts{};
    for (const Slot& slot : order_)
        if (slot.entity) ++counts[slot.id.moho() >> 28];
    return counts;
}

void EntityRegistry::index_slots() {
    slot_of_.clear();
    for (size_t i = 0; i < order_.size(); ++i)
        if (order_[i].entity) slot_of_[order_[i].id] = {i, kNoSlot};
    for (size_t i = 0; i < unit_order_.size(); ++i)
        if (unit_order_[i].entity) slot_of_[unit_order_[i].id].second = i;
}

EntityId EntityRegistry::register_entity(std::unique_ptr<Entity> entity) {
    const EntityId id = reserve_id(*entity);
    entity->set_entity_id(id);
    entity->place_now();
    entity->set_registry(this);
    order_.push_back({id, entity.get()});
    size_t unit_slot = kNoSlot;
    if (entity->is_unit()) {
        unit_slot = unit_order_.size();
        unit_order_.push_back({id, entity.get()});
    }
    slot_of_[id] = {order_.size() - 1, unit_slot};
    live_.insert(entity.get());
    walk_.push_back(id);
    Entity* const made = entity.get();
    entities_[id] = std::move(entity);
    live_by_moho_[id.moho()] = id;
    ++live_count_;

    notify_collision_shape_changed(*made);
    if (grid_initialized_) {
        auto* e = made;
        i32 cx, cz;
        world_to_cell(e->position().x, e->position().z, cx, cz);
        grid_insert(*e, cx, cz);
        e->set_grid_cell(cx, cz);
        if (e->is_prop()) {
            prop_cell_update(*e);
        }
    }

    return id;
}

void EntityRegistry::unregister_entity(EntityId id) {
    const auto found = entities_.find(id);
    if (found == entities_.end() || !found->second) return;
    // Take the entity out before the hook: the hook runs scripts (OnDestroy),
    // which may create or remove entities and so grow the table.
    std::unique_ptr<Entity> entity = std::move(found->second);
    entities_.erase(found);
    --live_count_;
    const u32 moho = id.moho();
    live_by_moho_.erase(moho);
    id_pools_[moho & 0xFFF00000u].held.back().push_back(moho & 0xFFFFFu);
    if (const auto slot = slot_of_.find(id); slot != slot_of_.end()) {
        if (order_[slot->second.first].entity) {
            order_[slot->second.first].entity = nullptr;
            ++removed_slots_;
        }
        if (slot->second.second != kNoSlot) unit_order_[slot->second.second].entity = nullptr;
        slot_of_.erase(slot);
    }
    large_colliders_.erase(id);
    if (grid_initialized_) {
        i32 cx = entity->grid_cell_x();
        i32 cz = entity->grid_cell_z();
        if (cx >= 0) grid_remove(*entity, cx, cz);
        prop_cell_remove(*entity);
    }
    entity->set_registry(nullptr);
    Entity& ref = *entity;
    graveyard_.push_back(std::move(entity)); // freed by collect_garbage()
    if (unregister_hook_) unregister_hook_(ref);
    // Held until its OnDestroy (in the hook) has run, as a script's own
    // Destroy() runs it: a handle to it still resolves there (per review).
    live_.erase(&ref);
}

void EntityRegistry::destroy(Entity& entity) {
    if (entity.destroyed() || find(entity.entity_id()) != &entity) {
        return;
    }
    entity.mark_destroyed();
    deletion_queue_.push_back(entity.entity_id());
}

void EntityRegistry::wake(Entity& entity) {
    if (entity.walk() != Entity::Walk::Parked || find(entity.entity_id()) != &entity) {
        return;
    }
    entity.set_walk(Entity::Walk::Stepping);
    walk_.push_back(entity.entity_id());
}

EntityId EntityRegistry::take_destroyed() {
    if (deletion_queue_.empty()) {
        return EntityId();
    }
    const EntityId id = deletion_queue_.front();
    deletion_queue_.pop_front();
    return id;
}

void EntityRegistry::purge_destroyed() {
    while (const EntityId id = take_destroyed()) {
        unregister_entity(id);
    }
}

Entity* EntityRegistry::find(EntityId id) const {
    const auto it = entities_.find(id);
    return it == entities_.end() ? nullptr : it->second.get();
}

void EntityRegistry::collect_garbage(const std::function<void(Entity&)>& release) {
    std::vector<std::unique_ptr<Entity>> dead;
    dead.swap(graveyard_);
    for (const auto& e : dead) cells_.unlink(*e);
    if (release)
        for (const auto& e : dead) release(*e);
    dead.clear();
    compact();
}

void EntityRegistry::compact() {
    // Not under a walk: it holds its place by index.
    if (walking_ > 0 || removed_slots_ == 0) return;
    const auto empty = [](const Slot& s) { return s.entity == nullptr; };
    std::erase_if(order_, empty);
    std::erase_if(unit_order_, empty);
    removed_slots_ = 0;
    index_slots();
}

// --- Spatial hash grid ---

void EntityRegistry::init_spatial_grid(u32 map_width, u32 map_height) {
    // Reset all entity grid cells so stale coordinates are not reused on re-init
    for_each([](Entity& e) { e.set_grid_cell(-1, -1); });
    grid_cells_.clear();
    unit_cells_.clear();

    grid_width_ = (map_width + CELL_SIZE - 1) / CELL_SIZE;
    grid_height_ = (map_height + CELL_SIZE - 1) / CELL_SIZE;
    if (grid_width_ == 0) grid_width_ = 1;
    if (grid_height_ == 0) grid_height_ = 1;
    grid_cells_.resize(static_cast<size_t>(grid_width_) * grid_height_);
    unit_cells_.resize(grid_cells_.size());
    prop_cells_.clear();
    prop_grid_width_ = std::max(1u, (map_width + PROP_CELL_SIZE - 1) / PROP_CELL_SIZE);
    prop_grid_height_ = std::max(1u, (map_height + PROP_CELL_SIZE - 1) / PROP_CELL_SIZE);
    prop_cells_.resize(static_cast<size_t>(prop_grid_width_) * prop_grid_height_);
    grid_initialized_ = true;
    // The collision cells start empty: what exists links below, in id order
    // (the order it was made, as Moho links each as it is made).
    for_each([](Entity& e) { e.cell_link() = CellLink{}; });
    cells_.init(map_width, map_height);

    // Retroactively insert all existing entities (e.g. props created before grid init)
    for_each([this](Entity& e) {
        i32 cx, cz;
        world_to_cell(e.position().x, e.position().z, cx, cz);
        grid_insert(e, cx, cz);
        e.set_grid_cell(cx, cz);
        if (e.is_prop()) {
            e.set_prop_cell(-1);
            prop_cell_update(e);
        }
        cells_.link(e);
    });

    spdlog::info("Spatial hash grid: {}x{} cells (cell_size={}u, {} entities indexed)", grid_width_,
                 grid_height_, CELL_SIZE, live_count_);
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

void EntityRegistry::grid_insert(Entity& entity, i32 cx, i32 cz) {
    const size_t i = cell_index(cx, cz);
    grid_cells_[i].push_back(entity.entity_id());
    if (entity.is_unit()) unit_cells_[i].push_back({entity.entity_id(), &entity});
}

void EntityRegistry::grid_remove(const Entity& entity, i32 cx, i32 cz) {
    const size_t i = cell_index(cx, cz);
    auto& cell = grid_cells_[i];
    auto it = std::find(cell.begin(), cell.end(), entity.entity_id());
    if (it != cell.end()) {
        *it = cell.back(); // swap-and-pop for O(1) removal
        cell.pop_back();
    }
    if (!entity.is_unit()) return;
    auto& units = unit_cells_[i];
    auto u = std::find_if(units.begin(), units.end(),
                          [&](const UnitRef& r) { return r.id == entity.entity_id(); });
    if (u != units.end()) {
        *u = units.back();
        units.pop_back();
    }
}

void EntityRegistry::advance_coords() {
    for_each([](Entity& e) { e.advance_coords(); });
}

void EntityRegistry::notify_moved(Entity& entity, bool moved) {
    if (!grid_initialized_) {
        return;
    }
    cells_.link(entity);
    if (!moved) {
        return;
    }
    if (entity.is_prop()) {
        prop_cell_update(entity);
    }

    i32 new_cx, new_cz;
    world_to_cell(entity.position().x, entity.position().z, new_cx, new_cz);

    i32 old_cx = entity.grid_cell_x();
    i32 old_cz = entity.grid_cell_z();

    if (old_cx == new_cx && old_cz == new_cz) return; // same cell, no update

    if (old_cx >= 0) grid_remove(entity, old_cx, old_cz);
    grid_insert(entity, new_cx, new_cz);
    entity.set_grid_cell(new_cx, new_cz);
}

std::vector<EntityId> EntityRegistry::collect_in_radius(f32 x, f32 z, f32 radius) const {
    std::vector<EntityId> result;
    f32 r2 = radius * radius;

    if (!grid_initialized_) {
        // Fallback to O(N) scan (in the order made, which Moho's ids aren't)
        for_each([&](const Entity& e) {
            if (e.destroyed()) return;
            f32 dx = e.position().x - x;
            f32 dz = e.position().z - z;
            if (dx * dx + dz * dz <= r2) result.push_back(e.entity_id());
        });
        std::sort(result.begin(), result.end());
        return result;
    }

    // Compute cell range covering the bounding box of the circle
    i32 cx_min, cz_min, cx_max, cz_max;
    world_to_cell(x - radius, z - radius, cx_min, cz_min);
    world_to_cell(x + radius, z + radius, cx_max, cz_max);

    for (i32 cz = cz_min; cz <= cz_max; ++cz) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            const auto& cell = grid_cells_[cell_index(cx, cz)];
            for (EntityId id : cell) {
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

std::vector<Entity*> EntityRegistry::units_in_radius(f32 x, f32 z, f32 radius) const {
    std::vector<Entity*> result;
    const f32 r2 = radius * radius;
    const auto within = [&](const Entity& e) {
        const f32 dx = e.position().x - x;
        const f32 dz = e.position().z - z;
        return dx * dx + dz * dz <= r2;
    };
    if (!grid_initialized_) {
        for_each_unit([&](Entity& e) {
            if (!e.destroyed() && within(e)) result.push_back(&e);
        });
        std::sort(result.begin(), result.end(),
                  [](const Entity* a, const Entity* b) { return a->entity_id() < b->entity_id(); });
        return result;
    }
    i32 cx_min, cz_min, cx_max, cz_max;
    world_to_cell(x - radius, z - radius, cx_min, cz_min);
    world_to_cell(x + radius, z + radius, cx_max, cz_max);
    std::vector<UnitRef> found;
    for (i32 cz = cz_min; cz <= cz_max; ++cz) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            for (const UnitRef& unit : unit_cells_[cell_index(cx, cz)])
                if (!unit.entity->destroyed() && within(*unit.entity)) found.push_back(unit);
        }
    }
    std::sort(found.begin(), found.end(),
              [](const UnitRef& a, const UnitRef& b) { return a.id < b.id; });
    result.reserve(found.size());
    for (const UnitRef& unit : found) result.push_back(unit.entity);
    return result;
}

std::vector<EntityId> EntityRegistry::collect_in_rect(f32 x0, f32 z0, f32 x1, f32 z1) const {
    std::vector<EntityId> result;
    if (x0 > x1) std::swap(x0, x1);
    if (z0 > z1) std::swap(z0, z1);

    if (!grid_initialized_) {
        // Fallback to O(N) scan (in the order made, which Moho's ids aren't)
        for_each([&](const Entity& e) {
            if (e.destroyed()) return;
            f32 ex = e.position().x;
            f32 ez = e.position().z;
            if (ex >= x0 && ex <= x1 && ez >= z0 && ez <= z1) result.push_back(e.entity_id());
        });
        std::sort(result.begin(), result.end());
        return result;
    }

    i32 cx_min, cz_min, cx_max, cz_max;
    world_to_cell(x0, z0, cx_min, cz_min);
    world_to_cell(x1, z1, cx_max, cz_max);

    for (i32 cz = cz_min; cz <= cz_max; ++cz) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            const auto& cell = grid_cells_[cell_index(cx, cz)];
            for (EntityId id : cell) {
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

bool EntityRegistry::live_within(const Entity& e, f32 x0, f32 z0, f32 x1, f32 z1) {
    const f32 ex = e.position().x;
    const f32 ez = e.position().z;
    return !e.destroyed() && ex >= x0 && ex <= x1 && ez >= z0 && ez <= z1;
}

std::vector<Entity*> EntityRegistry::units_in_rect(f32 x0, f32 z0, f32 x1, f32 z1) const {
    std::vector<Entity*> result;
    if (x0 > x1) std::swap(x0, x1);
    if (z0 > z1) std::swap(z0, z1);
    const auto within = [&](const Entity& e) {
        const f32 ex = e.position().x;
        const f32 ez = e.position().z;
        return ex >= x0 && ex <= x1 && ez >= z0 && ez <= z1;
    };
    if (!grid_initialized_) {
        for_each_unit([&](Entity& e) {
            if (!e.destroyed() && within(e)) result.push_back(&e);
        });
        std::sort(result.begin(), result.end(),
                  [](const Entity* a, const Entity* b) { return a->entity_id() < b->entity_id(); });
        return result;
    }
    i32 cx_min, cz_min, cx_max, cz_max;
    world_to_cell(x0, z0, cx_min, cz_min);
    world_to_cell(x1, z1, cx_max, cz_max);
    std::vector<UnitRef> found;
    for (i32 cz = cz_min; cz <= cz_max; ++cz) {
        for (i32 cx = cx_min; cx <= cx_max; ++cx) {
            for (const UnitRef& unit : unit_cells_[cell_index(cx, cz)])
                if (!unit.entity->destroyed() && within(*unit.entity)) found.push_back(unit);
        }
    }
    std::sort(found.begin(), found.end(),
              [](const UnitRef& a, const UnitRef& b) { return a.id < b.id; });
    result.reserve(found.size());
    for (const UnitRef& unit : found) result.push_back(unit.entity);
    return result;
}

i32 EntityRegistry::prop_cell_index(f32 wx, f32 wz) const {
    const auto size = static_cast<f32>(PROP_CELL_SIZE);
    const i32 cx = std::clamp(static_cast<i32>(std::floor(wx / size)), 0,
                              static_cast<i32>(prop_grid_width_) - 1);
    const i32 cz = std::clamp(static_cast<i32>(std::floor(wz / size)), 0,
                              static_cast<i32>(prop_grid_height_) - 1);
    return cz * static_cast<i32>(prop_grid_width_) + cx;
}

void EntityRegistry::prop_cell_update(Entity& entity) {
    const i32 cell = prop_cell_index(entity.position().x, entity.position().z);
    if (cell == entity.prop_cell()) {
        return;
    }
    prop_cell_remove(entity);
    prop_cells_[static_cast<size_t>(cell)].push_back({entity.entity_id(), &entity});
    entity.set_prop_cell(cell);
}

void EntityRegistry::prop_cell_remove(Entity& entity) {
    if (entity.prop_cell() < 0) {
        return;
    }
    auto& props = prop_cells_[static_cast<size_t>(entity.prop_cell())];
    auto p = std::find_if(props.begin(), props.end(),
                          [&](const UnitRef& r) { return r.id == entity.entity_id(); });
    if (p != props.end()) {
        *p = props.back();
        props.pop_back();
    }
    entity.set_prop_cell(-1);
}

void EntityRegistry::props_touching(f32 x0, f32 z0, f32 x1, f32 z1,
                                    std::vector<Entity*>& out) const {
    out.clear();
    const auto consider = [&](Entity& e, f32 reach) {
        if (e.destroyed() || e.collision_shape().type == CollisionShapeType::NONE) {
            return;
        }
        const Vector3& p = e.position();
        if (p.x >= x0 - reach && p.x <= x1 + reach && p.z >= z0 - reach && p.z <= z1 + reach) {
            out.push_back(&e);
        }
    };
    if (!grid_initialized_) {
        for_each([&](Entity& e) {
            if (e.is_prop()) {
                consider(e, e.shape_reach());
            }
        });
        return;
    }
    const i32 lo = prop_cell_index(x0 - COLLIDER_REACH, z0 - COLLIDER_REACH);
    const i32 hi = prop_cell_index(x1 + COLLIDER_REACH, z1 + COLLIDER_REACH);
    const i32 w = static_cast<i32>(prop_grid_width_);
    for (i32 cz = lo / w; cz <= hi / w; ++cz) {
        for (i32 cx = lo % w; cx <= hi % w; ++cx) {
            for (const UnitRef& prop : prop_cells_[static_cast<size_t>(cz) * prop_grid_width_ +
                                                   static_cast<size_t>(cx)]) {
                consider(*prop.entity, prop.entity->shape_reach());
            }
        }
    }
    for (const EntityId id : large_colliders_) {
        if (Entity* e = find(id); e && e->is_prop()) {
            consider(*e, e->shape_reach());
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Entity* a, const Entity* b) { return a->entity_id() < b->entity_id(); });
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void EntityRegistry::notify_collision_shape_changed(Entity& entity) {
    if (entity.shape_reach() > COLLIDER_REACH) large_colliders_.insert(entity.entity_id());
    else large_colliders_.erase(entity.entity_id());
    // As Moho installs a new collision primitive: relinked at once.
    if (grid_initialized_) cells_.link(entity);
}

bool EntityRegistry::any_unit_collider_impl(f32 x0, f32 z0, f32 x1, f32 z1,
                                            bool (*visit)(const Entity&, void*), void* ctx) const {
    if (x0 > x1) std::swap(x0, x1);
    if (z0 > z1) std::swap(z0, z1);
    const auto near = [&](const Entity& e, f32 reach) {
        if (e.destroyed() || e.collision_shape().type == CollisionShapeType::NONE) return false;
        const Vector3& p = e.position();
        return p.x >= x0 - reach && p.x <= x1 + reach && p.z >= z0 - reach && p.z <= z1 + reach;
    };
    // Within COLLIDER_REACH, then within the unit's own reach: what a
    // visit turns into the world (a box, four rotations) is mostly too far
    // to touch, as the path searches' blocking test asks of every cell.
    // collision_reach bounds the box turned by a unit quaternion; one off
    // by |q|^2 = 1 + e stretches it by at most 1 + 2e, which the slack
    // covers (with the rounding at map coordinates). Farther off unit
    // length, or a NaN in a script's quaternion or shape (which fails
    // every comparison, so the tests are written to keep it), only
    // COLLIDER_REACH filters, as before.
    const auto may_reach = [&](const Entity& e) {
        if (!near(e, COLLIDER_REACH)) return false;
        const Quaternion& q = e.orientation();
        const f32 norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        const f32 reach = e.shape_reach() * 1.01f + 0.01f;
        if (!(std::abs(norm - 1.0f) <= 1e-3f) || !(reach < COLLIDER_REACH)) return true;
        return near(e, reach);
    };
    if (!grid_initialized_) {
        bool hit = false;
        for_each_unit([&](const Entity& e) {
            if (!hit && may_reach(e) && visit(e, ctx)) hit = true;
        });
        if (hit) return true;
    } else {
        i32 cx_min, cz_min, cx_max, cz_max;
        world_to_cell(x0 - COLLIDER_REACH, z0 - COLLIDER_REACH, cx_min, cz_min);
        world_to_cell(x1 + COLLIDER_REACH, z1 + COLLIDER_REACH, cx_max, cz_max);
        for (i32 cz = cz_min; cz <= cz_max; ++cz)
            for (i32 cx = cx_min; cx <= cx_max; ++cx)
                for (const UnitRef& unit : unit_cells_[cell_index(cx, cz)])
                    if (may_reach(*unit.entity) && visit(*unit.entity, ctx)) return true;
    }
    for (const EntityId id : large_colliders_) {
        const Entity* e = find(id);
        if (e && e->is_unit() && near(*e, e->shape_reach()) && visit(*e, ctx)) return true;
    }
    return false;
}

void EntityRegistry::collect_colliders(f32 x0, f32 z0, f32 x1, f32 z1,
                                       std::vector<EntityId>& out) const {
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
                for (EntityId id : grid_cells_[cell_index(cx, cz)])
                    if (const Entity* e = find(id)) consider(*e, COLLIDER_REACH);
    }
    for (EntityId id : large_colliders_)
        if (const Entity* e = find(id)) consider(*e, e->shape_reach());
    // A cell lists ids in the order they moved in, and a large shape may be
    // in a cell too: one canonical list.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

} // namespace osc::sim
