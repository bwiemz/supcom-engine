#include "renderer/cartographic_technique.hpp"

#include <array>
#include <string_view>

namespace osc::renderer {

namespace {

using CT = CartographicTechnique;

/// A shader's cartographic technique at Low, Medium and High, each lane
/// already resolved (a missing High lane is Medium's, a missing Medium
/// lane Low's and a missing Low lane Medium's).
struct Lanes {
    std::string_view shader;
    std::array<CT, 3> by_fidelity;
};

// From retail's mesh.fx: each technique's abstractTechnique, fidelity and
// cartographicTechnique annotations. Those it names without lanes
// (CommandFeedback, RallyPoint, NormalMappedTerrain) serve every fidelity.
constexpr std::array<Lanes, 37> kLanes = {{
    {"Unit", {CT::Unit, CT::Unit, CT::Unit}},
    {"Metal", {CT::Unit, CT::Unit, CT::Unit}},
    {"Insect", {CT::Unit, CT::Unit, CT::Unit}},
    {"Aeon", {CT::Unit, CT::Unit, CT::Unit}},
    {"AeonCZAR", {CT::Unit, CT::Unit, CT::Unit}},
    {"Seraphim", {CT::Unit, CT::Unit, CT::Unit}},
    // No Medium lane: Medium draws the Low one
    {"SeraphimPersonalShield", {CT::Unit, CT::Unit, CT::Unit}},
    // The UEF, Cybran and Aeon personal shields: the unit itself is drawn
    // as the shield's highlight
    {"PhaseShield", {CT::Shield, CT::Shield, CT::Shield}},
    {"VertexNormal", {CT::Feature, CT::Feature, CT::Feature}},
    {"NormalMappedAlpha", {CT::Feature, CT::Feature, CT::Feature}},
    // Their Low lanes name CartographicUnit
    {"NormalMappedAlphaNoShadow", {CT::Unit, CT::Feature, CT::Feature}},
    {"BloatingNormalMappedAlpha", {CT::Unit, CT::Feature, CT::Feature}},
    {"UndulatingNormalMappedAlpha", {CT::Feature, CT::Feature, CT::Feature}},
    {"BlackenedNormalMappedAlpha", {CT::Feature, CT::Feature, CT::Feature}},
    {"NormalMappedGlow", {CT::Feature, CT::Feature, CT::Feature}},
    {"NormalMappedTerrain", {CT::Feature, CT::Feature, CT::Feature}},
    // No High lane
    {"Wreckage", {CT::Feature, CT::Feature, CT::Feature}},
    {"CommandFeedback", {CT::Feedback, CT::Feedback, CT::Feedback}},
    {"CommandFeedback2", {CT::Feedback, CT::Feedback, CT::Feedback}},
    {"CommandFeedback3", {CT::Feedback, CT::Feedback, CT::Feedback}},
    {"CommandFeedback4", {CT::Feedback, CT::Feedback, CT::Feedback}},
    {"RallyPoint", {CT::Feedback, CT::Feedback, CT::Feedback}},
    // A Low lane alone: every fidelity draws it
    {"UnitPlace", {CT::Place, CT::Place, CT::Place}},
    {"UnitFormationPreview", {CT::Place, CT::Place, CT::Place}},
    {"UEFBuild", {CT::Build, CT::Build, CT::Build}},
    {"AeonBuild", {CT::Build, CT::Build, CT::Build}},
    {"CybranBuild", {CT::Build, CT::Build, CT::Build}},
    // No High lane
    {"SeraphimBuild", {CT::Build, CT::Build, CT::Build}},
    // The faction shields: no High lane
    {"ShieldUEF", {CT::Shield, CT::Shield, CT::Shield}},
    {"ShieldCybran", {CT::Shield, CT::Shield, CT::Shield}},
    {"ShieldAeon", {CT::Shield, CT::Shield, CT::Shield}},
    {"ShieldSeraphim", {CT::Shield, CT::Shield, CT::Shield}},
    // Named, but not drawn
    {"AlphaFade", {CT::None, CT::None, CT::None}},
    {"AeonBuildPuddle", {CT::None, CT::None, CT::None}},
    {"UEFBuildCube", {CT::None, CT::None, CT::None}},
    {"GlassAlpha", {CT::None, CT::None, CT::None}},
    {"ShieldFill", {CT::None, CT::None, CT::None}},
}};

} // namespace

CartographicTechnique cartographic_technique(const std::string& shader, int fidelity) {
    const auto lane = static_cast<size_t>(fidelity < 0 ? 0 : (fidelity > 2 ? 2 : fidelity));
    for (const Lanes& l : kLanes)
        if (l.shader == shader) return l.by_fidelity[lane];
    return CT::None;
}

CartographicMeshPasses cartographic_mesh_passes(CartographicTechnique technique) {
    using S = CartographicMeshState;
    CartographicMeshPasses p;
    switch (technique) {
    case CT::None: break;
    case CT::Unit:
        p.count = 2;
        p.state[0] = S::UnitColour;
        p.state[1] = S::UnitGlow;
        break;
    case CT::Feature:
    case CT::Feedback:
        p.count = 1;
        p.state[0] = S::Opaque;
        break;
    case CT::Place:
    case CT::Build:
        p.count = 1;
        p.state[0] = S::OpaqueAlpha;
        break;
    case CT::Shield:
        p.count = 1;
        p.state[0] = S::Shield;
        break;
    }
    return p;
}

} // namespace osc::renderer
