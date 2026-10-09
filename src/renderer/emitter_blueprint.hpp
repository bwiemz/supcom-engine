#pragma once

#include "core/types.hpp"

#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::renderer {

/// One key of an emitter curve: at tick x, value y, spread z (the full
/// width of the random range about y).
struct CurveKey {
    f32 x = 0;
    f32 y = 0;
    f32 z = 0;
};

/// An EmitterBlueprint curve, as Moho's SEfxCurve reads it (M214c): keys in
/// ticks, in order.
struct EmitterCurve {
    std::vector<CurveKey> keys;
    /// Its length in ticks (XRange; SEfxCurve's x bounds from 0), which
    /// ResizeEmitterCurve stretches its keys from.
    f32 x_range = 0.0f;

    /// SEfxCurve::GetValue at tick `x`: the first key past x (before the
    /// first key, or past the last, that key; between two, y and z
    /// interpolated), its y plus (random - 0.5) times its z. `random` is a
    /// uniform [0, 1) draw; empty, the value is 0.
    template <typename Random> f32 value(f32 x, Random&& random) const {
        if (keys.empty()) return 0.0f;
        size_t i = 0;
        while (i < keys.size() && keys[i].x <= x) ++i;
        f32 y = 0;
        f32 z = 0;
        if (i == keys.size() || i == 0) {
            const CurveKey& k = i == 0 ? keys.front() : keys.back();
            y = k.y;
            z = k.z;
        } else {
            const CurveKey& a = keys[i - 1];
            const CurveKey& b = keys[i];
            const f32 f = (x - a.x) / (b.x - a.x);
            y = a.y + (b.y - a.y) * f;
            z = a.z + (b.z - a.z) * f;
        }
        return (random() - 0.5f) * z + y;
    }

    /// Its greatest reach, max(y + z / 2) over its keys (UpdateCurve's
    /// "peak lifetime"); -infinity when empty.
    f32 peak() const;
};

/// The curves of an emitter blueprint, in Moho's EEmitterCurve order.
enum EmitterCurveId : u8 {
    kXDirection,
    kYDirection,
    kZDirection,
    kEmitRate,
    kLifetime,
    kVelocity,
    kXAccel,
    kYAccel,
    kZAccel,
    kResistance,
    kSize,
    kXPosition,
    kYPosition,
    kZPosition,
    kStartSize,
    kEndSize,
    kInitialRotation,
    kRotationRate,
    kFrameRate,
    kTextureSelection,
    kRampSelection,
    kEmitterCurveCount
};

/// An `EmitterBlueprint { ... }` (M214c), with Moho's defaults
/// (REmitterBlueprint) for what it leaves out.
/// particle.fx's REFRACT blend (M214d): drawn apart, last, over a copy of
/// the frame.
constexpr i32 kBlendRefract = 5;
constexpr i32 kBlendAdd = 3;

struct EmitterBlueprintData {
    std::string blueprint_id; ///< its VFS path
    f32 lifetime = 0.0f;      ///< ticks it emits (negative: until its effect ends)
    f32 repeattime = 0.0f;    ///< ticks: the period its curves are read over
    f32 frame_count = 0.0f;   ///< TextureFramecount: frames across its texture
    f32 strip_count = 1.0f;   ///< TextureStripcount: strips down it
    i32 blendmode = 0;        ///< particle.fx's TRamp suffix (5: REFRACT)
    f32 lod_cutoff = 100.0f;  ///< emits within this of the camera
    u8 fidelity = 0b111;      ///< the graphics fidelities it is made at (blueprint_fidelity)
    f32 sort_order = 0.0f;    ///< below kParticleWaterSurface, drawn under the water
    bool local_velocity = true;
    bool local_acceleration = false;
    bool gravity = false;
    bool align_rotation = false;
    bool align_to_bone = false;
    bool flat = false;
    bool light = false;          ///< particle.fx's TLight
    bool emit_if_visible = true; ///< emits only while the player could see it
    bool catchup_emit = true;
    bool create_if_visible = false; ///< made only if the player could see it then
    bool particle_resistance = false;
    bool interpolate_emission = true;
    bool snap_to_waterline = true;
    bool only_emit_on_water = false;
    std::string texture;      ///< Texture
    std::string ramp_texture; ///< RampTexture: colour over a particle's life
    std::array<EmitterCurve, kEmitterCurveCount> curves;
    /// UpdateCurve's mMaxLifetime: the longest a particle lives, in whole
    /// ticks, which bounds how far back it catches up.
    i32 max_lifetime = 0;

    const EmitterCurve& curve(EmitterCurveId id) const { return curves[id]; }
    /// Its texture has frames or strips to pick from (TRampAnimate*).
    bool animated() const { return frame_count > 1.0f || strip_count > 1.0f; }

    /// The particle systems' draw ordering: the ordering it was last counted
    /// in (a stamp unique to each), and its bucket then.
    mutable u64 draw_update = 0;
    mutable u32 draw_bucket = 0;
};

/// Emitter blueprints by VFS path, read as the beam and trail caches read
/// theirs. Null for a path that isn't an emitter's; a failed path is
/// remembered.
class EmitterBlueprintCache {
public:
    void set_vfs(const vfs::VirtualFileSystem* vfs) { vfs_ = vfs; }
    const EmitterBlueprintData* get(const std::string& path, lua_State* L);
    void clear() {
        cache_.clear();
        failed_.clear();
    }

private:
    const vfs::VirtualFileSystem* vfs_ = nullptr;
    std::unordered_map<std::string, EmitterBlueprintData> cache_;
    std::unordered_set<std::string> failed_;
};

} // namespace osc::renderer
