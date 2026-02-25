#pragma once

#include "core/types.hpp"

#include <string>

namespace osc::sim {

struct Vector3 {
    f32 x = 0, y = 0, z = 0;
};

struct Quaternion {
    f32 x = 0, y = 0, z = 0, w = 1;
};

class Entity {
public:
    Entity() = default;
    virtual ~Entity() = default;

    u32 entity_id() const { return entity_id_; }
    void set_entity_id(u32 id) { entity_id_ = id; }

    i32 army() const { return army_; }
    void set_army(i32 a) { army_ = a; }

    const Vector3& position() const { return position_; }
    void set_position(const Vector3& p) { position_ = p; }

    const Quaternion& orientation() const { return orientation_; }
    void set_orientation(const Quaternion& o) { orientation_ = o; }

    f32 health() const { return health_; }
    void set_health(f32 h) { health_ = std::max(0.0f, h); }

    f32 max_health() const { return max_health_; }
    void set_max_health(f32 h) { max_health_ = h; }

    f32 fraction_complete() const { return fraction_complete_; }
    void set_fraction_complete(f32 f) { fraction_complete_ = f; }

    bool destroyed() const { return destroyed_; }
    void mark_destroyed() { destroyed_ = true; }

    const std::string& blueprint_id() const { return blueprint_id_; }
    void set_blueprint_id(const std::string& id) { blueprint_id_ = id; }

    int lua_table_ref() const { return lua_table_ref_; }
    void set_lua_table_ref(int ref) { lua_table_ref_ = ref; }

    virtual bool is_unit() const { return false; }
    virtual bool is_projectile() const { return false; }
    virtual bool is_prop() const { return false; }

private:
    u32 entity_id_ = 0;
    i32 army_ = -1;
    Vector3 position_;
    Quaternion orientation_;
    f32 health_ = 0;
    f32 max_health_ = 0;
    f32 fraction_complete_ = 1.0f;
    bool destroyed_ = false;
    std::string blueprint_id_;
    int lua_table_ref_ = -2; // LUA_NOREF
};

} // namespace osc::sim
