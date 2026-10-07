#include "sim/decal.hpp"

#include <algorithm>
#include <cmath>

namespace osc::sim {

namespace {

/// A vector flattened onto the ground and normalised (NormalizeXZ).
Vector3 flat_unit(const Vector3& v) {
    const f32 length = std::sqrt(v.x * v.x + v.z * v.z);
    if (length <= 0.0f) return {0, 0, 0};
    return {v.x / length, 0.0f, v.z / length};
}

u32 army_bit(size_t index) {
    return index < 32 ? 1u << index : 0u;
}

/// CDecalBuffer::IsDecalVisibleForArmy: allied to the source, or able to
/// detect the decal's bounds.
bool visible_to(const DecalSpec& spec, size_t source, size_t observer, const DecalArmies& armies) {
    if (armies.allied(observer, source)) return true;
    return armies.detects(observer, decal_bounds(spec), spec.position.y);
}

/// The source army, if the spec names one that exists.
bool source_of(const DecalSpec& spec, const DecalArmies& armies, size_t& source) {
    if (spec.army < 0 || static_cast<size_t>(spec.army) >= armies.count()) return false;
    source = static_cast<size_t>(spec.army);
    return armies.exists(source);
}

} // namespace

Quaternion heading_quaternion(f32 heading) {
    return {0.0f, std::sin(heading * 0.5f), 0.0f, std::cos(heading * 0.5f)};
}

DecalSpec make_decal_spec(const Vector3& centre, const Quaternion& orientation, f32 size_x,
                          f32 size_z, f32 duration, u32 tick) {
    DecalSpec spec;
    const Vector3 right = flat_unit(quat_rotate(orientation, {1, 0, 0}));
    const Vector3 forward = flat_unit(quat_rotate(orientation, {0, 0, 1}));
    spec.position = {centre.x - right.x * size_x * 0.5f - forward.x * size_z * 0.5f, centre.y,
                     centre.z - right.z * size_x * 0.5f - forward.z * size_z * 0.5f};
    const Quaternion& q = orientation;
    spec.rotation_y =
        -std::atan2(2.0f * (q.x * q.z + q.w * q.y), 1.0f - 2.0f * (q.z * q.z + q.y * q.y));
    spec.size_x = size_x;
    spec.size_z = size_z;
    if (duration > 0.0f) spec.remove_tick = tick + static_cast<u32>(std::floor(duration * 10.0f));
    return spec;
}

DecalBounds decal_bounds(const DecalSpec& spec) {
    const f32 c = std::cos(spec.rotation_y);
    const f32 s = std::sin(spec.rotation_y);
    const f32 xx = spec.size_x * c;
    const f32 xz = spec.size_x * s;
    const f32 zx = -spec.size_z * s;
    const f32 zz = spec.size_z * c;
    return {spec.position.x + std::min({0.0f, xx, zx, xx + zx}),
            spec.position.z + std::min({0.0f, xz, zz, xz + zz}),
            spec.position.x + std::max({0.0f, xx, zx, xx + zx}),
            spec.position.z + std::max({0.0f, xz, zz, xz + zz})};
}

u32 decal_sight_at_creation(const DecalSpec& spec, const DecalArmies& armies) {
    const size_t count = armies.count();
    size_t source = 0;
    const bool has_source = source_of(spec, armies, source);
    u32 seen = 0;
    if (has_source && spec.splat) {
        const bool from_civilian = armies.civilian(source);
        for (size_t i = 0; i < count; ++i)
            if (armies.allied(source, i) || !from_civilian) seen |= army_bit(i);
        return seen;
    }
    for (size_t i = 0; i < count; ++i) {
        if ((seen & army_bit(i)) != 0) continue;
        if (!armies.exists(i) || armies.civilian(i)) continue;
        if (has_source && !visible_to(spec, source, i, armies)) continue;
        for (size_t j = i; j < count; ++j)
            if (armies.exists(j) && armies.allied(j, i)) seen |= army_bit(j);
    }
    return seen;
}

u32 decal_sight_lookers(const DecalArmies& armies) {
    u32 lookers = 0;
    for (size_t i = 0; i < armies.count(); ++i)
        if (armies.exists(i) && !armies.civilian(i)) lookers |= army_bit(i);
    return lookers;
}

bool decal_sight_settled(const DecalSpec& spec, u32 seen_by, u32 created_tick, u32 tick,
                         u32 lookers) {
    if (spec.splat) return !(spec.remove_tick != 0 && created_tick + 10 > tick);
    return (seen_by & lookers) == lookers;
}

u32 decal_sight_on_tick(const DecalSpec& spec, u32 seen_by, u32 created_tick, u32 tick,
                        const DecalArmies& armies) {
    const size_t count = armies.count();
    if (count == 0) return seen_by;
    const size_t turn = tick % count;
    if (!armies.exists(turn) || armies.civilian(turn)) return seen_by;
    const u32 bit = army_bit(turn);
    if (bit == 0 || (seen_by & bit) != 0) return seen_by;
    const bool new_splat = spec.remove_tick != 0 && created_tick + 10 > tick;
    if (spec.splat && !new_splat) return seen_by;
    size_t source = 0;
    if (source_of(spec, armies, source) && !visible_to(spec, source, turn, armies)) return seen_by;
    for (size_t j = 0; j < count; ++j)
        if (armies.exists(j) && armies.allied(j, turn)) seen_by |= army_bit(j);
    return seen_by;
}

} // namespace osc::sim
