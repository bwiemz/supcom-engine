#pragma once

#include "core/types.hpp"
#include "sim/entity.hpp"

#include <string>

namespace osc::sim {

/// A decal or splat a script made (Moho's SDecalInfo; M212c): CreateDecal,
/// CreateSplat and CreateSplatOnBone. Placed by its corner, as the map's
/// decals are: its footprint runs from `position` along its turned x and z
/// axes, `size_x` by `size_z`.
struct DecalSpec {
    bool splat = false;
    std::string type;     ///< CreateDecal's type name ("Albedo", ...); empty for a splat
    std::string texture1; ///< as the script named them (the renderer resolves them)
    std::string texture2;
    Vector3 position; ///< the footprint's corner
    f32 rotation_y = 0;
    f32 size_x = 1, size_z = 1;
    f32 lod = 0;         ///< its LOD cutoff; 0 or less: from its size
    u32 remove_tick = 0; ///< the tick it goes at; 0: never
    i32 army = -1;       ///< 0-based; none out of range
    u32 fidelity = 1;
};

/// A heading as Moho turns one into a transform (BuildHeadingTransform):
/// about +y, w = cos h/2, y = sin h/2.
Quaternion heading_quaternion(f32 heading);

/// CreateDecalFromTransform's placement: `orientation`'s x and z axes,
/// flattened onto the ground, give the footprint's sides, and its middle
/// is `centre`. The corner is the centre less half of each side, and the
/// turn is -atan2(2(xz + wy), 1 - 2(z^2 + y^2)): -h for a heading h.
/// `duration` (seconds) ends it at `tick` + floor(duration * 10), or
/// never if it isn't over 0.
DecalSpec make_decal_spec(const Vector3& centre, const Quaternion& orientation, f32 size_x,
                          f32 size_z, f32 duration, u32 tick);

/// A decal's bounds on the ground (ProjectDecalToBoundsXZ): the extent of
/// its footprint.
struct DecalBounds {
    f32 min_x = 0, min_z = 0, max_x = 0, max_z = 0;
};
DecalBounds decal_bounds(const DecalSpec& spec);

/// The armies as the decals' sight rules see them (CDecalBuffer).
class DecalArmies {
public:
    virtual ~DecalArmies() = default;
    virtual size_t count() const = 0;
    /// Army `index` has a brain.
    virtual bool exists(size_t index) const = 0;
    virtual bool civilian(size_t index) const = 0;
    /// `army` counts `other` among its allies (itself included).
    virtual bool allied(size_t army, size_t other) const = 0;
    /// `observer`'s recon can detect the rectangle (ReconCanDetect with
    /// LOSNow): some cell of it is in its line of sight now.
    virtual bool detects(size_t observer, const DecalBounds& rect, f32 y) const = 0;
};

/// Which armies see a decal as it is made (CDecalBuffer::CreateHandle), a
/// bit per army (the first 32).
/// - A splat from a non-civilian army: every army; from a civilian one,
///   its allies.
/// - A decal, or a splat with no army: each non-civilian army allied to its
///   source or able to detect it, and that army's allies (those from its
///   own index up, as Moho's PropagateVisibilityToObserverAllies loops).
u32 decal_sight_at_creation(const DecalSpec& spec, const DecalArmies& armies);

/// One tick's look (CDecalBuffer::CleanupTick): army `tick % count`, if it
/// isn't a civilian and doesn't see it yet, looks at a decal, or a splat
/// with a lifetime made within 10 ticks. If it is allied to the source or
/// can detect it, it and its allies see it. Flags are never cleared.
u32 decal_sight_on_tick(const DecalSpec& spec, u32 seen_by, u32 created_tick, u32 tick,
                        const DecalArmies& armies);

} // namespace osc::sim
