#pragma once

#include "core/types.hpp"

#include <vector>

namespace osc::map {
struct ScmapSky;
}

namespace osc::renderer {

/// A vertex of the sky's dome: its world position, and its azimuth (sky.fx's
/// theta, 0 to 2 pi around the dome).
struct SkyDomeVertex {
    f32 pos[3];
    f32 theta;
};

/// The dome's mesh (M210b): Moho's SkyDome::CreateDomeVertexBuffer and
/// CreateDomeIndexBuffer.
struct SkyDomeMesh {
    std::vector<SkyDomeVertex> vertices;
    std::vector<u16> indices;
};

/// The dome over a map's sky:
/// - `height` rings of `width + 1` vertices, from the start angle up toward
///   the pole, then an apex;
/// - each ring pair's quads, then a fan to the apex, indexed in 16 bits as
///   Moho's are.
///
/// It is empty for a shape that can't be drawn: no segments, a start angle
/// whose cosine is 0, or more vertices than 16-bit indices reach.
SkyDomeMesh build_sky_dome(const map::ScmapSky& sky);

} // namespace osc::renderer
