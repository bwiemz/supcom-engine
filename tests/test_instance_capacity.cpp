#include <catch2/catch_test_macros.hpp>

#include "renderer/entity_cache.hpp"
#include "renderer/unit_renderer.hpp"

#include <vector>

using namespace osc;
using namespace osc::renderer;

namespace {

sim::EntityId ent(u32 moho) {
    return sim::EntityId::of(moho, 1);
}

} // namespace

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
    CHECK(births.tick(ent(3), 0, 10) == 10);
    CHECK(births.tick(ent(7), 0, 10) == 10);
    CHECK(births.tick(ent(9), 0, 10) == 10);
    births.finish();

    // Update 2 at tick 20: 3 is gone, 5 appears, 9 changed mesh
    CHECK(births.tick(ent(5), 0, 20) == 20);
    CHECK(births.tick(ent(7), 0, 20) == 10);
    CHECK(births.tick(ent(9), 1, 20) == 20);
    births.finish();
    CHECK(births.size() == 3);

    // Update 3 at tick 30: 3 comes back as new; the rest keep theirs
    CHECK(births.tick(ent(3), 0, 30) == 30);
    CHECK(births.tick(ent(5), 0, 30) == 20);
    CHECK(births.tick(ent(7), 0, 30) == 10);
    CHECK(births.tick(ent(9), 1, 30) == 20);
    births.finish();

    // An update that draws none forgets them all
    births.finish();
    CHECK(births.size() == 0);
    CHECK(births.tick(ent(7), 0, 40) == 40);
}

TEST_CASE("a mesh instance's tick holds whatever order its entities come in",
          "[renderer][unit_renderer]") {
    MeshBirths births;
    for (u32 n : {2u, 4u, 6u, 8u}) {
        births.tick(ent(n), 0, n * 10);
    }
    births.finish();

    // The world's entities ascending, then ghosts below them (remembered
    // structures, after the walk passed the end of the last update's)
    CHECK(births.tick(ent(6), 0, 99) == 60);
    CHECK(births.tick(ent(8), 0, 99) == 80);
    CHECK(births.tick(ent(2), 0, 99) == 20);
    CHECK(births.tick(ent(4), 0, 99) == 40);
    // One asked twice (a ghost of a live entity) gets the same answer
    CHECK(births.tick(ent(6), 0, 99) == 60);
    births.finish();
    CHECK(births.size() == 4);
    CHECK(births.tick(ent(2), 0, 100) == 20);
    CHECK(births.tick(ent(4), 0, 100) == 40);
    CHECK(births.tick(ent(6), 0, 100) == 60);
    CHECK(births.tick(ent(8), 0, 100) == 80);
}

TEST_CASE("an entity's cached entry lasts while updates ask for it", "[renderer][unit_renderer]") {
    EntityCache<int> cache;
    // Update 1: three entities appear, each new
    for (u32 n : {3u, 7u, 9u}) {
        CHECK(cache.at(ent(n)) == 0);
        cache.at(ent(n)) = static_cast<int>(n);
    }
    cache.finish();
    CHECK(cache.size() == 3);

    // Update 2: 3 is gone, 5 appears; 7 and 9 keep theirs
    CHECK(cache.at(ent(5)) == 0);
    cache.at(ent(5)) = 5;
    CHECK(cache.at(ent(7)) == 7);
    CHECK(cache.at(ent(9)) == 9);
    cache.finish();
    CHECK(cache.size() == 3);

    // Update 3: 3 comes back as new; 1 and 11 appear either side
    CHECK(cache.at(ent(1)) == 0);
    CHECK(cache.at(ent(3)) == 0);
    CHECK(cache.at(ent(5)) == 5);
    CHECK(cache.at(ent(7)) == 7);
    CHECK(cache.at(ent(9)) == 9);
    CHECK(cache.at(ent(11)) == 0);
    cache.finish();
    CHECK(cache.size() == 6);

    // An update that asks for none forgets them all
    cache.finish();
    CHECK(cache.size() == 0);
    CHECK(cache.at(ent(7)) == 0);
}

TEST_CASE("an entity's cached entry is one, whatever order its entities come in",
          "[renderer][unit_renderer]") {
    EntityCache<int> cache;
    for (u32 n : {2u, 4u, 6u, 8u}) cache.at(ent(n)) = static_cast<int>(n);
    cache.finish();

    // The world's entities ascending, then ghosts below them (remembered
    // structures, after the walk passed the end of the last update's)
    CHECK(cache.at(ent(6)) == 6);
    CHECK(cache.at(ent(8)) == 8);
    CHECK(cache.at(ent(2)) == 2);
    CHECK(cache.at(ent(4)) == 4);
    // One asked twice (a ghost of a live entity) is the same entry
    cache.at(ent(6)) = 60;
    CHECK(cache.at(ent(6)) == 60);
    // New ones, out of order and asked twice
    cache.at(ent(9)) = 9;
    cache.at(ent(5)) = 5;
    cache.at(ent(1)) = 1;
    CHECK(cache.at(ent(9)) == 9);
    CHECK(cache.at(ent(5)) == 5);
    CHECK(cache.at(ent(1)) == 1);
    cache.finish();
    CHECK(cache.size() == 7);
    for (u32 n : {1u, 2u, 4u, 5u, 8u, 9u}) CHECK(cache.at(ent(n)) == static_cast<int>(n));
    CHECK(cache.at(ent(6)) == 60);
    cache.finish();
    CHECK(cache.size() == 7);
}

TEST_CASE("the frames between two ticks move no cached entry", "[renderer][unit_renderer]") {
    EntityCache<int> cache;
    for (u32 n = 1; n <= 100; ++n) cache.at(ent(n)) = static_cast<int>(n);
    cache.finish();
    std::vector<const int*> where;
    for (u32 n = 1; n <= 100; ++n) where.push_back(&cache.at(ent(n)));
    cache.finish();
    // The same entities asked for again: each where it was
    for (u32 n = 1; n <= 100; ++n) CHECK(&cache.at(ent(n)) == where[n - 1]);
    cache.finish();
}

TEST_CASE("a later entity of an id is another entity", "[renderer][unit_renderer]") {
    EntityCache<int> cache;
    cache.at(sim::EntityId::of(7, 1)) = 1;
    cache.finish();
    // Moho's id 7 handed out again: not the entry of the one before
    CHECK(cache.at(sim::EntityId::of(7, 2)) == 0);
    cache.finish();
    CHECK(cache.size() == 1);
}
