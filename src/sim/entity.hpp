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

// --- Quaternion math utilities ---

/// Rotate a vector by a quaternion (Hamilton product: q * v * q_conjugate).
inline Vector3 quat_rotate(const Quaternion& q, const Vector3& v) {
    f32 t2  =  q.w * q.x;
    f32 t3  =  q.w * q.y;
    f32 t4  =  q.w * q.z;
    f32 t5  = -q.x * q.x;
    f32 t6  =  q.x * q.y;
    f32 t7  =  q.x * q.z;
    f32 t8  = -q.y * q.y;
    f32 t9  =  q.y * q.z;
    f32 t10 = -q.z * q.z;
    return {
        2.0f * ((t8 + t10) * v.x + (t6 -  t4) * v.y + (t3 + t7) * v.z) + v.x,
        2.0f * ((t4 +  t6) * v.x + (t5 + t10) * v.y + (t9 - t2) * v.z) + v.y,
        2.0f * ((t7 -  t3) * v.x + (t2 +  t9) * v.y + (t5 + t8) * v.z) + v.z
    };
}

/// Multiply two quaternions (Hamilton product).
inline Quaternion quat_multiply(const Quaternion& a, const Quaternion& b) {
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
    };
}

struct BoneData; // forward decl

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

    f32 regen_rate() const { return regen_rate_; }
    void set_regen_rate(f32 r) { regen_rate_ = r; }

    f32 fraction_complete() const { return fraction_complete_; }
    void set_fraction_complete(f32 f) { fraction_complete_ = f; }

    bool destroyed() const { return destroyed_; }
    void mark_destroyed() { destroyed_ = true; }

    const std::string& blueprint_id() const { return blueprint_id_; }
    void set_blueprint_id(const std::string& id) { blueprint_id_ = id; }

    int lua_table_ref() const { return lua_table_ref_; }
    void set_lua_table_ref(int ref) { lua_table_ref_ = ref; }

    u32 ambient_sound_handle() const { return ambient_sound_handle_; }
    void set_ambient_sound_handle(u32 h) { ambient_sound_handle_ = h; }

    const BoneData* bone_data() const { return bone_data_; }
    void set_bone_data(const BoneData* bd) { bone_data_ = bd; }

    // Targeting/reclaimable flags
    bool do_not_target() const { return do_not_target_; }
    void set_do_not_target(bool b) { do_not_target_ = b; }
    bool reclaimable() const { return reclaimable_; }
    void set_reclaimable(bool b) { reclaimable_ = b; }

    virtual bool is_unit() const { return false; }
    virtual bool is_projectile() const { return false; }
    virtual bool is_prop() const { return false; }
    virtual bool is_shield() const { return false; }

private:
    u32 entity_id_ = 0;
    i32 army_ = -1;
    Vector3 position_;
    Quaternion orientation_;
    f32 health_ = 0;
    f32 max_health_ = 0;
    f32 regen_rate_ = 0;
    f32 fraction_complete_ = 1.0f;
    bool destroyed_ = false;
    std::string blueprint_id_;
    int lua_table_ref_ = -2; // LUA_NOREF
    u32 ambient_sound_handle_ = 0; ///< Active ambient loop (SoundHandle)
    const BoneData* bone_data_ = nullptr; // shared per-blueprint, not owned
    bool do_not_target_ = false;
    bool reclaimable_ = true;
};

} // namespace osc::sim
