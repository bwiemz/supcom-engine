// The UI's order marks (AddCommandFeedbackBlip), as Moho keeps and draws
// them (faf-re UiRuntimeTypes.cpp, mesh.fx's CommandFeedbackVS): gone after
// their duration in real seconds, scaled up with the view's width at their
// depth.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "renderer/command_feedback.hpp"
#include "renderer/mesh_cache.hpp"

#include <cmath>

using Catch::Matchers::WithinAbs;
using osc::renderer::CommandFeedbackBlips;
using osc::renderer::FeedbackBlipSpec;
using osc::sim::Vector3;

namespace {

FeedbackBlipSpec blip(float duration) {
    FeedbackBlipSpec spec;
    spec.position = {10.0f, 0.0f, 20.0f};
    spec.mesh_name = "/meshes/game/flag02d_lod0.scm";
    spec.texture_name = "/meshes/game/flag02d_albedo.dds";
    spec.shader_name = "CommandFeedback";
    spec.uniform_scale = 0.5f;
    spec.duration = duration;
    return spec;
}

} // namespace

TEST_CASE("An order's mark goes once its duration has passed, in frame time", "[feedback]") {
    CommandFeedbackBlips blips;
    blips.add(blip(0.7f), 120.0f);
    blips.add(blip(0.75f), 120.0f);
    REQUIRE(blips.blips().size() == 2);
    CHECK(blips.blips()[0].created_tick == 120.0f);
    blips.update(0.5f);
    CHECK(blips.blips().size() == 2);
    blips.update(0.2f); // 0.7: the flag goes, the crosshair stays
    REQUIRE(blips.blips().size() == 1);
    CHECK(blips.blips()[0].spec.duration == 0.75f);
    blips.update(0.05f);
    CHECK(blips.blips().empty());
}

TEST_CASE("The marks' techniques: blended, after the water, no shadow", "[feedback]") {
    using osc::renderer::MeshTechnique;
    CHECK(osc::renderer::mesh_technique("CommandFeedback") == MeshTechnique::CommandFeedback);
    CHECK(osc::renderer::mesh_technique("CommandFeedback2") == MeshTechnique::CommandFeedback2);
    CHECK(osc::renderer::mesh_technique("RallyPoint") == MeshTechnique::RallyPoint);
    for (MeshTechnique t : {MeshTechnique::CommandFeedback, MeshTechnique::CommandFeedback2,
                            MeshTechnique::RallyPoint}) {
        CHECK(osc::renderer::is_feedback_technique(t));
        CHECK(osc::renderer::is_blended_technique(t));
        CHECK(osc::renderer::is_post_water_technique(t));
        CHECK_FALSE(osc::renderer::has_depth_stage(t));
    }
}

TEST_CASE("The LOD metric is the view's width at the depth; the mark grows with it", "[feedback]") {
    const Vector3 eye{0.0f, 100.0f, 0.0f};
    const Vector3 down{0.0f, -1.0f, 0.0f};
    const float fov = 60.0f * 3.14159265f / 180.0f;
    // 100 below the eye, a 60 degree view is 2 tan 30 x 100 = 115.5 across.
    CHECK_THAT(osc::renderer::lod_metric({5.0f, 0.0f, 7.0f}, eye, down, fov),
               WithinAbs(115.47, 0.01));
    // Depth is along the view, not the distance: off to the side, the same.
    CHECK_THAT(osc::renderer::lod_metric({50.0f, 0.0f, 0.0f}, eye, down, fov),
               WithinAbs(115.47, 0.01));
    // lerp(1, 15, (lod - 10.5) * 0.001): 1 at 10.5, 15 at 1010.5.
    CHECK_THAT(osc::renderer::feedback_distance_scale(10.5f), WithinAbs(1.0, 1e-6));
    CHECK_THAT(osc::renderer::feedback_distance_scale(1010.5f), WithinAbs(15.0, 1e-4));
    CHECK_THAT(osc::renderer::feedback_distance_scale(115.47f), WithinAbs(2.4696, 1e-3));
}

TEST_CASE("A mark's model matrix: at its point, unrotated, scaled", "[feedback]") {
    const auto m = osc::renderer::feedback_model({1.0f, 2.0f, 3.0f}, 0.25f);
    CHECK(m[0] == 0.25f);
    CHECK(m[5] == 0.25f);
    CHECK(m[10] == 0.25f);
    CHECK(m[1] == 0.0f);
    CHECK(m[12] == 1.0f);
    CHECK(m[13] == 2.0f);
    CHECK(m[14] == 3.0f);
    CHECK(m[15] == 1.0f);
}
