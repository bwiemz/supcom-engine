#include "sim/entity.hpp"
#include "sim/entity_registry.hpp"

namespace osc::sim {

void Entity::set_position(const Vector3& p) {
    position_ = p;
    if (registry_) registry_->notify_position_changed(*this);
}

void Entity::set_collision_shape(const CollisionShape& s) {
    collision_shape_ = s;
    if (registry_) registry_->notify_collision_shape_changed(*this);
}

} // namespace osc::sim
