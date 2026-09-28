// The terrain shader's Time (M212f), as Moho's HighFidelityTerrain sets it:
// the tick and interpolant, only when the terrain re-tessellates.

#include <catch2/catch_test_macros.hpp>

#include "renderer/terrain_time.hpp"

#include <array>
#include <cmath>

using namespace osc;
using osc::renderer::TerrainTime;

TEST_CASE("The terrain's Time moves only with the camera or the decals (M212f)",
          "[renderer][terrain]") {
    std::array<f32, 16> view{};
    view[0] = view[5] = view[10] = view[15] = 1.0f;
    TerrainTime time;
    CHECK(time.update(view, 3, 1.5f) == 1.5f); // the first frame sets it
    CHECK(time.update(view, 3, 9.0f) == 1.5f); // a still camera: it stands
    CHECK(time.value() == 1.5f);

    // The camera moved, however little: it takes the tick
    std::array<f32, 16> nudged = view;
    nudged[12] = std::nextafter(0.0f, 1.0f);
    CHECK(time.update(nudged, 3, 12.25f) == 12.25f);
    CHECK(time.update(nudged, 3, 14.0f) == 12.25f);

    // A decal came or went
    CHECK(time.update(nudged, 4, 20.0f) == 20.0f);
    CHECK(time.update(nudged, 4, 21.0f) == 20.0f);

    // A new scene: the next frame sets it, whatever the camera
    time.reset();
    CHECK(time.update(nudged, 4, 30.0f) == 30.0f);
}
