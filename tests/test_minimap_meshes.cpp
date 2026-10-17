// The cartographic minimap's meshes (Moho's Cartographic::RenderMeshes): a
// mesh shows while some LOD takes the minimap camera's metric, its view's
// width times the LOD scale the view was made with (cam_DefaultMiniLOD,
// 1.8); FAF's patched game draws none while that convar is 0.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "renderer/minimap_meshes.hpp"
#include "ui/world_view.hpp"

#include <cmath>
#include <optional>
#include <vector>

using namespace osc;
using namespace osc::renderer;
using Catch::Matchers::WithinAbs;

namespace {

/// cam_DefaultMiniLOD is a global: each test leaves it as Moho starts it.
struct MiniLodGuard {
    ~MiniLodGuard() { ui::WorldView::set_default_mini_lod(ui::WorldView::kDefaultMiniLod); }
};

} // namespace

TEST_CASE("A mesh reaches its LODs' largest cutoff, the default where one gives none",
          "[minimap_meshes]") {
    CHECK(lod_reach({}) == 0.0f); // no LODs: nothing shows
    CHECK(lod_reach({100.0f, 215.0f}) == 215.0f);
    CHECK(lod_reach({215.0f, 100.0f}) == 215.0f); // in any order (ComputeLOD tries each)
    // RMeshBlueprintLOD's default, 1000
    CHECK(lod_reach({std::nullopt}) == kDefaultLodCutoff);
    CHECK(lod_reach({100.0f, std::nullopt}) == 1000.0f);
    CHECK(lod_reach({1500.0f, std::nullopt}) == 1500.0f);
    // A cutoff of 0 or less takes any metric
    CHECK(std::isinf(lod_reach({100.0f, 0.0f})));
    CHECK(std::isinf(lod_reach({-1.0f})));
}

TEST_CASE("The minimap's metric is its view's width times its LOD scale", "[minimap_meshes]") {
    CHECK_THAT(minimap_lod_metric(1024.0f, 1.8f), WithinAbs(1843.2, 1e-3));
    CHECK_THAT(minimap_lod_metric(512.0f, 1.8f), WithinAbs(921.6, 1e-3));
    CHECK_THAT(minimap_lod_metric(256.0f, 1.8f), WithinAbs(460.8, 1e-3));
}

TEST_CASE("FAF's patched game draws no minimap meshes while cam_DefaultMiniLOD is 0",
          "[minimap_meshes]") {
    CHECK_FALSE(minimap_meshes_on(true, 0.0f));
    CHECK(minimap_meshes_on(true, 1.8f));
    // Retail's game has no such switch: 0 is only a scale (any LOD shows)
    CHECK(minimap_meshes_on(false, 0.0f));
}

TEST_CASE("A minimap view takes cam_DefaultMiniLOD when made, and keeps it", "[minimap_meshes]") {
    MiniLodGuard guard;
    CHECK(ui::WorldView::default_mini_lod() == 1.8f);
    ui::WorldView world;
    CHECK(world.lod_scale() == 1.0f); // cam_DefaultLOD's
    ui::WorldView mini;
    mini.set_minimap(true);
    CHECK(mini.lod_scale() == 1.8f);

    // Set later (FAF's minimap.lua sets 0 once its view is made): the view
    // keeps the scale it was made with; a view made now takes the new one
    ui::WorldView::set_default_mini_lod(0.0f);
    CHECK(mini.lod_scale() == 1.8f);
    ui::WorldView later;
    later.set_minimap(true);
    CHECK(later.lod_scale() == 0.0f);
    // Made a minimap no more: cam_DefaultLOD's again
    later.set_minimap(false);
    CHECK(later.lod_scale() == 1.0f);
}
