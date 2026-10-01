#include <catch2/catch_test_macros.hpp>

#include "renderer/unit_renderer.hpp"

using namespace osc;
using namespace osc::renderer;

TEST_CASE("grown_capacity doubles until the need fits", "[renderer][unit_renderer]") {
    CHECK(grown_capacity(2048, 2048, 1u << 20) == 2048);
    CHECK(grown_capacity(2048, 2049, 1u << 20) == 4096);
    CHECK(grown_capacity(2048, 9000, 1u << 20) == 16384);
    // From nothing, the power of two that holds it
    CHECK(grown_capacity(0, 64, 1u << 20) == 64);
    CHECK(grown_capacity(0, 100, 1u << 20) == 128);
}

TEST_CASE("grown_capacity stops at its limit", "[renderer][unit_renderer]") {
    // Short of the need: the frame draws what fits
    CHECK(grown_capacity(4096, 100000, 65536) == 65536);
    // A limit that isn't a power of two is still the most it grows to
    CHECK(grown_capacity(4096, 100000, 50000) == 50000);
    CHECK(grown_capacity(65536, 70000, 65536) == 65536);
    // It never shrinks
    CHECK(grown_capacity(8192, 10, 1u << 20) == 8192);
}

TEST_CASE("the bone limit is what every device binds", "[renderer][unit_renderer]") {
    // 128 MB of matrices: Vulkan's least maxStorageBufferRange
    CHECK(static_cast<u64>(UnitRenderer::kMaxBones) * 16 * sizeof(f32) == (u64{1} << 27));
    CHECK(UnitRenderer::kInitialMeshInstances <= UnitRenderer::kMaxInstances);
    CHECK(UnitRenderer::kInitialBones <= UnitRenderer::kMaxBones);
}
