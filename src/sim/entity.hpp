#pragma once

#include "core/types.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

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

/// Convert Euler angles (heading=Y, pitch=X, roll=Z, intrinsic YXZ) to quaternion.
/// FA convention: heading rotates around Y axis, pitch around X, roll around Z.
inline Quaternion euler_to_quat(f32 heading, f32 pitch, f32 roll) {
    f32 ch = std::cos(heading * 0.5f), sh = std::sin(heading * 0.5f);
    f32 cp = std::cos(pitch * 0.5f),   sp = std::sin(pitch * 0.5f);
    f32 cr = std::cos(roll * 0.5f),    sr = std::sin(roll * 0.5f);
    // YXZ intrinsic = ZXY extrinsic
    return {
        ch * sp * cr + sh * cp * sr,  // x
        sh * cp * cr - ch * sp * sr,  // y
        ch * cp * sr - sh * sp * cr,  // z
        ch * cp * cr + sh * sp * sr   // w
    };
}

/// Convert 3x3 rotation matrix (row-major: [row0][row1][row2]) to quaternion.
/// Uses Shepperd's method to avoid numerical instability.
inline Quaternion rot_matrix_to_quat(const f32 m[9]) {
    // m[0..2] = row 0 (X basis), m[3..5] = row 1 (Y basis), m[6..8] = row 2 (Z basis)
    f32 m00 = m[0], m01 = m[1], m02 = m[2];
    f32 m10 = m[3], m11 = m[4], m12 = m[5];
    f32 m20 = m[6], m21 = m[7], m22 = m[8];

    f32 trace = m00 + m11 + m22;
    Quaternion q;
    if (trace > 0) {
        f32 s = std::sqrt(trace + 1.0f) * 2.0f; // s = 4*w
        q.w = 0.25f * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        f32 s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q.w = (m21 - m12) / s;
        q.x = 0.25f * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        f32 s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25f * s;
        q.z = (m12 + m21) / s;
    } else {
        f32 s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25f * s;
    }
    return q;
}

/// Visibility mode for per-army rendering control.
enum class VizMode : u8 { ALWAYS, INTEL, NEVER };

/// Collision shape variant.
enum class CollisionShapeType : u8 { NONE, SPHERE, BOX };
struct CollisionShape {
    CollisionShapeType type = CollisionShapeType::NONE;
    f32 cx = 0, cy = 0, cz = 0; // center offset
    f32 sx = 0, sy = 0, sz = 0; // radius (sphere) or half-extents (box)
};

/// Attachment child record.
struct ChildAttachment {
    u32 entity_id = 0;
    i32 bone = -1; // which bone of parent the child is attached to
};

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

    const std::string& custom_name() const { return custom_name_; }
    void set_custom_name(const std::string& name) { custom_name_ = name; }

    f32 scale_x() const { return scale_x_; }
    f32 scale_y() const { return scale_y_; }
    f32 scale_z() const { return scale_z_; }
    void set_scale(f32 sx, f32 sy, f32 sz) { scale_x_ = sx; scale_y_ = sy; scale_z_ = sz; }

    // Visibility
    VizMode viz_allies() const { return viz_allies_; }
    VizMode viz_enemies() const { return viz_enemies_; }
    VizMode viz_focus_player() const { return viz_focus_player_; }
    VizMode viz_neutrals() const { return viz_neutrals_; }
    void set_viz_allies(VizMode m) { viz_allies_ = m; }
    void set_viz_enemies(VizMode m) { viz_enemies_ = m; }
    void set_viz_focus_player(VizMode m) { viz_focus_player_ = m; }
    void set_viz_neutrals(VizMode m) { viz_neutrals_ = m; }

    // Collision shape
    const CollisionShape& collision_shape() const { return collision_shape_; }
    void set_collision_shape(const CollisionShape& s) { collision_shape_ = s; }

    // Mesh override (runtime mesh switching via SetMesh)
    const std::string& mesh_override() const { return mesh_override_; }
    void set_mesh_override(const std::string& path) { mesh_override_ = path; }

    // Selection
    bool unselectable() const { return unselectable_; }
    void set_unselectable(bool b) { unselectable_ = b; }

    // Attachment — parent tracking
    u32 parent_entity_id() const { return parent_entity_id_; }
    i32 parent_bone() const { return parent_bone_; }
    i32 attached_bone() const { return attached_bone_; }
    void set_parent(u32 pid, i32 pbone, i32 abone = -1) {
        parent_entity_id_ = pid; parent_bone_ = pbone; attached_bone_ = abone;
    }
    void clear_parent() { parent_entity_id_ = 0; parent_bone_ = -1; attached_bone_ = -1; }
    const Vector3& parent_offset() const { return parent_offset_; }
    void set_parent_offset(const Vector3& off) { parent_offset_ = off; }

    // Attachment — children tracking
    const std::vector<ChildAttachment>& children() const { return children_; }
    void add_child(u32 eid, i32 bone) { children_.push_back({eid, bone}); }
    void remove_child(u32 eid) {
        children_.erase(std::remove_if(children_.begin(), children_.end(),
            [eid](const ChildAttachment& c) { return c.entity_id == eid; }),
            children_.end());
    }
    void remove_children_at_bone(i32 bone) {
        children_.erase(std::remove_if(children_.begin(), children_.end(),
            [bone](const ChildAttachment& c) { return c.bone == bone; }),
            children_.end());
    }

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
    std::string custom_name_;
    f32 scale_x_ = 1.0f;
    f32 scale_y_ = 1.0f;
    f32 scale_z_ = 1.0f;
    VizMode viz_allies_ = VizMode::INTEL;
    VizMode viz_enemies_ = VizMode::INTEL;
    VizMode viz_focus_player_ = VizMode::ALWAYS;
    VizMode viz_neutrals_ = VizMode::INTEL;
    CollisionShape collision_shape_;
    std::string mesh_override_;
    bool unselectable_ = false;
    u32 parent_entity_id_ = 0;
    i32 parent_bone_ = -1;
    i32 attached_bone_ = -1;
    Vector3 parent_offset_;
    std::vector<ChildAttachment> children_;
};

} // namespace osc::sim
