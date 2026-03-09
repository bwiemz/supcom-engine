#include <catch2/catch_test_macros.hpp>

#include "renderer/mesh_cache.hpp"

#include <algorithm>

using namespace osc;
using namespace osc::renderer;

// Helper: build a LODEntry with only index_count set (GPU buffers left null).
static LODEntry make_entry(u32 index_count, f32 cutoff) {
    LODEntry e;
    e.mesh.index_count = index_count;
    e.cutoff = cutoff;
    return e;
}

TEST_CASE("LOD: single LOD always returned", "[lod]") {
    LODSet set;
    set.lods.push_back(make_entry(1000, 0.0f));

    auto select_lod = [&](f32 distance) -> const GPUMesh* {
        for (const auto& entry : set.lods) {
            if (entry.cutoff == 0.0f || entry.cutoff >= distance)
                return &entry.mesh;
        }
        return &set.lods.back().mesh;
    };

    CHECK(set.lods.size() == 1);
    CHECK(select_lod(0.0f)->index_count == 1000);
    CHECK(select_lod(50.0f)->index_count == 1000);
    CHECK(select_lod(9999.0f)->index_count == 1000);
}

TEST_CASE("LOD: multiple LODs sorted by cutoff", "[lod]") {
    LODSet set;
    // Insert out of order: low, high, medium
    set.lods.push_back(make_entry(100, 600.0f));  // low detail
    set.lods.push_back(make_entry(1000, 100.0f)); // high detail
    set.lods.push_back(make_entry(500, 300.0f));   // medium detail

    // Sort by cutoff ascending (highest detail = smallest cutoff first).
    // cutoff 0 means "no limit" and should sort last.
    std::sort(set.lods.begin(), set.lods.end(), [](const LODEntry& a, const LODEntry& b) {
        if (a.cutoff == 0.0f) return false;
        if (b.cutoff == 0.0f) return true;
        return a.cutoff < b.cutoff;
    });

    REQUIRE(set.lods.size() == 3);
    CHECK(set.lods[0].cutoff == 100.0f);
    CHECK(set.lods[0].mesh.index_count == 1000);
    CHECK(set.lods[1].cutoff == 300.0f);
    CHECK(set.lods[1].mesh.index_count == 500);
    CHECK(set.lods[2].cutoff == 600.0f);
    CHECK(set.lods[2].mesh.index_count == 100);
}

TEST_CASE("LOD: distance-based selection", "[lod]") {
    LODSet set;
    set.lods.push_back(make_entry(1000, 100.0f)); // high detail, cutoff 100
    set.lods.push_back(make_entry(500, 300.0f));   // medium detail, cutoff 300
    set.lods.push_back(make_entry(100, 600.0f));   // low detail, cutoff 600

    auto select_lod = [&](f32 distance) -> const GPUMesh* {
        for (const auto& entry : set.lods) {
            if (entry.cutoff == 0.0f || entry.cutoff >= distance)
                return &entry.mesh;
        }
        return &set.lods.back().mesh;
    };

    SECTION("close range selects high detail") {
        CHECK(select_lod(0.0f)->index_count == 1000);
        CHECK(select_lod(50.0f)->index_count == 1000);
    }

    SECTION("exactly at cutoff boundary selects that LOD") {
        CHECK(select_lod(100.0f)->index_count == 1000);
        CHECK(select_lod(300.0f)->index_count == 500);
        CHECK(select_lod(600.0f)->index_count == 100);
    }

    SECTION("just beyond cutoff selects next LOD") {
        CHECK(select_lod(100.1f)->index_count == 500);
        CHECK(select_lod(300.1f)->index_count == 100);
    }

    SECTION("beyond all cutoffs returns lowest detail") {
        CHECK(select_lod(601.0f)->index_count == 100);
        CHECK(select_lod(9999.0f)->index_count == 100);
    }

    SECTION("mid-range distances") {
        CHECK(select_lod(200.0f)->index_count == 500);
        CHECK(select_lod(450.0f)->index_count == 100);
    }
}
