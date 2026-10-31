#include <catch2/catch_test_macros.hpp>

#include "renderer/mesh_cache.hpp"

#include <string>
#include <vector>

using namespace osc;
using namespace osc::renderer;

namespace {

/// A LODEntry with only index_count set (GPU buffers left null).
LODEntry make_entry(u32 index_count, f32 cutoff) {
    LODEntry e;
    e.mesh.index_count = index_count;
    e.cutoff = cutoff;
    return e;
}

u32 drawn(const LODSet& set, f32 metric) {
    const GPUMesh* mesh = lod_at(set, metric);
    return mesh ? mesh->index_count : 0;
}

} // namespace

TEST_CASE("A mesh draws its first LOD whose cutoff takes the LOD metric, none past them all",
          "[lod]") {
    LODSet set;
    set.lods.push_back(make_entry(1000, 100.0f));
    set.lods.push_back(make_entry(500, 215.0f));
    CHECK(drawn(set, 0.0f) == 1000);
    CHECK(drawn(set, 100.0f) == 1000);
    CHECK(drawn(set, 100.5f) == 500);
    CHECK(drawn(set, 215.0f) == 500);
    CHECK(drawn(set, 215.5f) == 0);
}

TEST_CASE("A mesh's LODs are taken in its blueprint's order, one of cutoff 0 or less at any "
          "metric",
          "[lod]") {
    LODSet unordered;
    unordered.lods.push_back(make_entry(100, 600.0f));
    unordered.lods.push_back(make_entry(1000, 100.0f));
    CHECK(drawn(unordered, 50.0f) == 100);

    LODSet effect;
    effect.lods.push_back(make_entry(300, -1.0f));
    CHECK(drawn(effect, 1e6f) == 300);
    LODSet after;
    after.lods.push_back(make_entry(1000, 100.0f));
    after.lods.push_back(make_entry(300, 0.0f));
    CHECK(drawn(after, 50.0f) == 1000);
    CHECK(drawn(after, 1e6f) == 300);
}

TEST_CASE("An LOD's bones map onto the first LOD's of the same name, any other onto 0", "[lod]") {
    // UAB0101 (Aeon land factory): its LOD1 keeps 7 of LOD0's 22 bones, in
    // another order, under another root's name.
    const std::vector<std::string> first = {
        "UAB0101",    "B01",        "B02",        "B03",        "Ring01_B01", "Ring02_B02",
        "Ring02_B03", "Ring03_B02", "Ring03_B03", "Ring03_B04", "Ring03_B05", "Ring04_B03",
        "Ring03_B06", "Ring02_B04", "Ring02_B01", "Ring03_B01", "Tier1_Arm"};
    const std::vector<std::string> lod1 = {"lod1_UAB0101", "B01",       "B02",      "Ring02_B01",
                                           "Ring02_B04",   "Tier1_Arm", "tier1_arm"};
    CHECK(lod_bone_remap(lod1, first) == std::vector<u8>{0, 1, 2, 14, 13, 16, 0});
    CHECK(lod_bone_remap(first, first).size() == first.size());
    CHECK(lod_bone_remap(lod1, {}) == std::vector<u8>(lod1.size(), 0));
}
