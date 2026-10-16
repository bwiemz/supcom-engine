#pragma once

// The technique a mesh draws with in a cartographic view (Moho's
// MeshRenderer::RenderCartographic): the one its material's technique
// names in a `cartographicTechnique` annotation (retail's effects/mesh.fx),
// read from the lane its graphics fidelity resolves to. No annotation, no
// drawing.

#include "core/types.hpp"

#include <array>
#include <string>

namespace osc::renderer {

/// mesh.fx's six cartographic techniques.
enum class CartographicTechnique : u8 {
    None,     ///< not drawn (effects, build cubes and puddles, fills, glass...)
    Unit,     ///< CartographicUnit: the instance's colour, then an edge glow
    Feature,  ///< CartographicFeature: the height ramp, banded by the light
    Feedback, ///< CartographicFeedback: the albedo (order marks, rally points)
    Place,    ///< CartographicPlace: the instance's colour at alpha 1/8
    Build,    ///< CartographicBuild: the instance's colour lightened, at 0.4
    Shield,   ///< CartographicShield: a white highlight
};

/// The cartographic technique of a material whose shader is `shader` (a
/// blueprint's ShaderName, already through resolve_shader_name) at graphics
/// fidelity `fidelity` (0 Low, 1 Medium, 2 High). A shader without a lane
/// at that fidelity takes Moho's next (High uses Medium; Medium and Low use
/// each other), and the annotation is that lane's. An unknown shader is
/// None, as Moho finds no technique for it.
CartographicTechnique cartographic_technique(const std::string& shader, int fidelity);

/// The states mesh.fx's cartographic passes draw with:
/// - UnitColour: SrcAlpha/InvSrcAlpha into RGB, depth tested and written
///   (CartographicUnit's P0);
/// - UnitGlow: alpha alone, unblended, no depth (its P1, the edge's glow);
/// - Opaque: RGB, unblended, depth tested and written (Feature, Feedback);
/// - OpaqueAlpha: RGBA, unblended, depth tested and written (Place, Build);
/// - Shield: SrcAlpha/InvSrcAlpha into RGB, depth tested, unwritten.
/// All cull as the meshes do (Rasterizer_Cull_CW).
enum class CartographicMeshState : u8 { UnitColour, UnitGlow, Opaque, OpaqueAlpha, Shield };
inline constexpr u32 kCartographicMeshStates = 5;

/// A technique's passes, in order: none for None, two for Unit.
struct CartographicMeshPasses {
    u32 count = 0;
    std::array<CartographicMeshState, 2> state = {CartographicMeshState::Opaque,
                                                  CartographicMeshState::Opaque};
};
CartographicMeshPasses cartographic_mesh_passes(CartographicTechnique technique);

} // namespace osc::renderer
