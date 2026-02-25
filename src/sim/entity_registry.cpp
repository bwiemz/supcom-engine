#include "sim/entity_registry.hpp"
#include "sim/entity.hpp"

namespace osc::sim {

EntityRegistry::EntityRegistry() = default;
EntityRegistry::~EntityRegistry() = default;

u32 EntityRegistry::register_entity(std::unique_ptr<Entity> entity) {
    u32 id = next_id_++;
    entity->set_entity_id(id);
    entities_[id] = std::move(entity);
    return id;
}

void EntityRegistry::unregister_entity(u32 id) {
    entities_.erase(id);
}

Entity* EntityRegistry::find(u32 id) const {
    auto it = entities_.find(id);
    return it != entities_.end() ? it->second.get() : nullptr;
}

std::vector<u32> EntityRegistry::collect_in_radius(f32 x, f32 z,
                                                    f32 radius) const {
    std::vector<u32> result;
    f32 r2 = radius * radius;
    for (const auto& [id, e] : entities_) {
        if (e->destroyed()) continue;
        f32 dx = e->position().x - x;
        f32 dz = e->position().z - z;
        if (dx * dx + dz * dz <= r2)
            result.push_back(id);
    }
    return result;
}

std::vector<u32> EntityRegistry::collect_in_rect(f32 x0, f32 z0,
                                                  f32 x1, f32 z1) const {
    std::vector<u32> result;
    // Normalise so x0<=x1, z0<=z1
    if (x0 > x1) std::swap(x0, x1);
    if (z0 > z1) std::swap(z0, z1);
    for (const auto& [id, e] : entities_) {
        if (e->destroyed()) continue;
        f32 ex = e->position().x;
        f32 ez = e->position().z;
        if (ex >= x0 && ex <= x1 && ez >= z0 && ez <= z1)
            result.push_back(id);
    }
    return result;
}

} // namespace osc::sim
