// A mesh's technique in a cartographic view (Moho's RenderCartographic):
// retail mesh.fx's cartographicTechnique annotations, by the lane each
// graphics fidelity resolves to.

#include <catch2/catch_test_macros.hpp>

#include "renderer/cartographic_technique.hpp"
#include "renderer/mesh_cache.hpp"

#include <string>

using namespace osc::renderer;
using CT = CartographicTechnique;

namespace {

/// The technique at Low, Medium and High.
bool lanes(const std::string& shader, CT low, CT medium, CT high) {
    return cartographic_technique(shader, 0) == low &&
           cartographic_technique(shader, 1) == medium && cartographic_technique(shader, 2) == high;
}

} // namespace

TEST_CASE("Units draw CartographicUnit at every fidelity", "[cartographic]") {
    for (const char* s : {"Unit", "Metal", "Insect", "Aeon", "AeonCZAR", "Seraphim"})
        CHECK(lanes(s, CT::Unit, CT::Unit, CT::Unit));
    // Medium has no lane of its own: it draws Low's
    CHECK(lanes("SeraphimPersonalShield", CT::Unit, CT::Unit, CT::Unit));
    // The other personal shields draw the unit as the shield's highlight
    CHECK(lanes("PhaseShield", CT::Shield, CT::Shield, CT::Shield));
}

TEST_CASE("Props and wrecks draw CartographicFeature, but two at Low", "[cartographic]") {
    for (const char* s : {"VertexNormal", "NormalMappedAlpha", "UndulatingNormalMappedAlpha",
                          "BlackenedNormalMappedAlpha", "NormalMappedGlow", "NormalMappedTerrain"})
        CHECK(lanes(s, CT::Feature, CT::Feature, CT::Feature));
    // Wreckage has no High lane: High draws Medium's
    CHECK(lanes("Wreckage", CT::Feature, CT::Feature, CT::Feature));
    // These two Low lanes name CartographicUnit
    CHECK(lanes("NormalMappedAlphaNoShadow", CT::Unit, CT::Feature, CT::Feature));
    CHECK(lanes("BloatingNormalMappedAlpha", CT::Unit, CT::Feature, CT::Feature));
}

TEST_CASE("Order marks, ghosts, builds and shields", "[cartographic]") {
    for (const char* s : {"CommandFeedback", "CommandFeedback2", "CommandFeedback3",
                          "CommandFeedback4", "RallyPoint"})
        CHECK(lanes(s, CT::Feedback, CT::Feedback, CT::Feedback));
    // A Low lane alone, which every fidelity resolves to
    CHECK(lanes("UnitPlace", CT::Place, CT::Place, CT::Place));
    CHECK(lanes("UnitFormationPreview", CT::Place, CT::Place, CT::Place));
    for (const char* s : {"UEFBuild", "AeonBuild", "CybranBuild", "SeraphimBuild"})
        CHECK(lanes(s, CT::Build, CT::Build, CT::Build));
    for (const char* s : {"ShieldUEF", "ShieldCybran", "ShieldAeon", "ShieldSeraphim"})
        CHECK(lanes(s, CT::Shield, CT::Shield, CT::Shield));
}

TEST_CASE("What has no annotation, or no technique, isn't drawn", "[cartographic]") {
    for (const char* s : {"AlphaFade", "AeonBuildPuddle", "UEFBuildCube", "GlassAlpha",
                          "ShieldFill", "ShieldImpact", "CybranShieldImpact", "Effect", "Explosion",
                          "Cloud", "NukeEMP", "Flat", "NoSuchShader"})
        CHECK(lanes(s, CT::None, CT::None, CT::None));
    // Out of range, the nearest fidelity
    CHECK(cartographic_technique("NormalMappedAlphaNoShadow", -1) == CT::Unit);
    CHECK(cartographic_technique("NormalMappedAlphaNoShadow", 5) == CT::Feature);
}

TEST_CASE("A blueprint's legacy shader names resolve first", "[cartographic]") {
    CHECK(cartographic_technique(resolve_shader_name(""), 2) == CT::Unit);
    CHECK(cartographic_technique(resolve_shader_name("Team"), 1) == CT::Unit);
    CHECK(cartographic_technique(resolve_shader_name("TMeshAlpha"), 0) == CT::Feature);
    CHECK(cartographic_technique(resolve_shader_name("TMeshNoNormals"), 2) == CT::Feature);
    CHECK(cartographic_technique(resolve_shader_name("TMeshGlow"), 2) == CT::Feature);
    CHECK(cartographic_technique(resolve_shader_name("TMeshAlphaGlowFade"), 2) == CT::None);
}
