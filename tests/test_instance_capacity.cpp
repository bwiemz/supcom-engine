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

TEST_CASE("a mesh instance keeps the tick it was made", "[renderer][unit_renderer]") {
    MeshBirths births;
    // Update 1 at tick 10: three entities appear
    CHECK(births.tick(3, 0, 10) == 10);
    CHECK(births.tick(7, 0, 10) == 10);
    CHECK(births.tick(9, 0, 10) == 10);
    births.finish();

    // Update 2 at tick 20: 3 is gone, 5 appears, 9 changed mesh
    CHECK(births.tick(5, 0, 20) == 20);
    CHECK(births.tick(7, 0, 20) == 10);
    CHECK(births.tick(9, 1, 20) == 20);
    births.finish();
    CHECK(births.size() == 3);

    // Update 3 at tick 30: 3 comes back as new; the rest keep theirs
    CHECK(births.tick(3, 0, 30) == 30);
    CHECK(births.tick(5, 0, 30) == 20);
    CHECK(births.tick(7, 0, 30) == 10);
    CHECK(births.tick(9, 1, 30) == 20);
    births.finish();

    // An update that draws none forgets them all
    births.finish();
    CHECK(births.size() == 0);
    CHECK(births.tick(7, 0, 40) == 40);
}

TEST_CASE("a mesh instance's tick holds whatever order its entities come in",
          "[renderer][unit_renderer]") {
    MeshBirths births;
    for (u32 id : {2u, 4u, 6u, 8u}) births.tick(id, 0, id * 10);
    births.finish();

    // The world's entities ascending, then ghosts below them (remembered
    // structures, after the walk passed the end of the last update's)
    CHECK(births.tick(6, 0, 99) == 60);
    CHECK(births.tick(8, 0, 99) == 80);
    CHECK(births.tick(2, 0, 99) == 20);
    CHECK(births.tick(4, 0, 99) == 40);
    // One asked twice (a ghost of a live entity) gets the same answer
    CHECK(births.tick(6, 0, 99) == 60);
    births.finish();
    CHECK(births.size() == 4);
    CHECK(births.tick(2, 0, 100) == 20);
    CHECK(births.tick(4, 0, 100) == 40);
    CHECK(births.tick(6, 0, 100) == 60);
    CHECK(births.tick(8, 0, 100) == 80);
}
