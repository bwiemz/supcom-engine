#include "sim/platoon.hpp"
#include "sim/entity_registry.hpp"

#include <algorithm>

namespace osc::sim {

void Platoon::add_unit(u32 entity_id) {
    if (!has_unit(entity_id))
        unit_ids_.push_back(entity_id);
}

void Platoon::remove_unit(u32 entity_id) {
    unit_ids_.erase(
        std::remove(unit_ids_.begin(), unit_ids_.end(), entity_id),
        unit_ids_.end());
    squad_map_.erase(entity_id);
}

bool Platoon::has_unit(u32 entity_id) const {
    return std::find(unit_ids_.begin(), unit_ids_.end(), entity_id)
           != unit_ids_.end();
}

Vector3 Platoon::get_position(const EntityRegistry& registry) const {
    f32 sx = 0, sy = 0, sz = 0;
    i32 count = 0;
    for (u32 id : unit_ids_) {
        auto* e = registry.find(id);
        if (e && !e->destroyed()) {
            sx += e->position().x;
            sy += e->position().y;
            sz += e->position().z;
            ++count;
        }
    }
    if (count == 0) return {};
    return {sx / count, sy / count, sz / count};
}

void Platoon::set_unit_squad(u32 entity_id, const std::string& squad) {
    squad_map_[entity_id] = squad;
}

static const std::string empty_squad;

const std::string& Platoon::get_unit_squad(u32 entity_id) const {
    auto it = squad_map_.find(entity_id);
    return (it != squad_map_.end()) ? it->second : empty_squad;
}

} // namespace osc::sim
