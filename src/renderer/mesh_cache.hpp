#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct lua_State;

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::blueprints {
class BlueprintStore;
}

namespace osc::renderer {

/// GPU-resident mesh data for a single blueprint.
/// Which of FA's mesh.fx techniques draws a mesh (its LOD's ShaderName).
enum class MeshTechnique : u32 {
    Unit = 0,     ///< NormalMappedPS: UEF, props, and anything unported
    Aeon = 1,     ///< AeonPS
    Insect = 2,   ///< NormalMappedInsectPS: Cybran
    Metal = 3,    ///< NormalMappedMetalPS
    Seraphim = 4, ///< UnitFalloffPS
    // A unit under construction's build mesh (M211f).
    UEFBuild = 5,      ///< UEFBuildHiFiPS, then UEFBuildOverlayHiFiPS
    AeonBuild = 6,     ///< AeonBuildPS, then AeonBuildOverlayPS; grows from 75%
    CybranBuild = 7,   ///< CybranBuildPS, then CybranBuildOverlayPS
    SeraphimBuild = 8, ///< SeraphimBuildPS; grows from 25%
    // Props' and the build effects' (M211g).
    NormalMappedAlpha = 9,           ///< NormalMappedPS unmasked, alpha-tested by f * albedo.a
    NormalMappedGlow = 10,           ///< NormalMappedPS unmasked, glowing
    AlphaFade = 11,                  ///< AlphaFadePS: fades out from two ticks old
    UEFBuildCube = 12,               ///< UEFBuildCubePS: unlit, writes no depth
    AeonBuildPuddle = 13,            ///< AeonBuildPuddlePS: scrolling, glowing
    BlackenedNormalMappedAlpha = 14, ///< burnt trees: NormalMappedAlpha, greyed
    // The props' (M211i).
    VertexNormal = 15,                ///< lit by the vertex's normal; blended, tested over 0x23
    NormalMappedTerrain = 16,         ///< unshadowed, no highlight
    UndulatingNormalMappedAlpha = 17, ///< NormalMappedAlpha, swaying in FA's wind
    // The shields' (M211k), drawn by the shield shaders.
    ShieldUEF = 18,          ///< ShieldPS: four scrolled layers, unculled
    ShieldCybran = 19,       ///< ShieldCybranPS, twice: then pushed out along the normal
    ShieldAeon = 20,         ///< ShieldAeonPS: normal-mapped, reflecting the environment
    ShieldSeraphim = 21,     ///< ShieldSeraphimPS: added, fading to the dome's top
    ShieldFill = 22,         ///< ShieldFillPS: depth alone, hiding the far side
    ShieldImpact = 23,       ///< ShieldImpactPS: a hit's patch, added
    CybranShieldImpact = 24, ///< CybranShieldImpactPS: a hit's patch, blended
};

/// Moho's ShaderDictionary (ResolveShaderAnnotationName): a legacy
/// ShaderName's current one (TMeshGlow is NormalMappedGlow); any other
/// unchanged, and an empty one "Unit".
std::string resolve_shader_name(const std::string& shader_name);

/// The technique a ShaderName names, once resolved; Unit for any other.
MeshTechnique mesh_technique(const std::string& shader_name);

/// One of the build techniques: translucent, drawn after the opaque meshes,
/// fed the instance's fraction complete.
inline bool is_build_technique(MeshTechnique t) {
    return t == MeshTechnique::UEFBuild || t == MeshTechnique::AeonBuild ||
           t == MeshTechnique::CybranBuild || t == MeshTechnique::SeraphimBuild;
}

/// A technique that blends: drawn after the opaque meshes (M211f-i).
inline bool is_blended_technique(MeshTechnique t) {
    return is_build_technique(t) || t == MeshTechnique::AlphaFade ||
           t == MeshTechnique::UEFBuildCube || t == MeshTechnique::VertexNormal;
}

/// A shield's technique (M211k), which the shield shaders draw.
inline bool is_shield_technique(MeshTechnique t) {
    return static_cast<u32>(t) >= static_cast<u32>(MeshTechnique::ShieldUEF) &&
           static_cast<u32>(t) <= static_cast<u32>(MeshTechnique::CybranShieldImpact);
}

/// A technique mesh.fx gives the POSTWATER render stage: Moho draws it after
/// the water (M213b), which writes no depth, so it shows over the surface.
/// The others the engine ports are PREWATER. (The shields are POSTWATER
/// too, but drawn later still: is_post_effect_technique.)
inline bool is_post_water_technique(MeshTechnique t) {
    return t == MeshTechnique::AlphaFade || t == MeshTechnique::BlackenedNormalMappedAlpha ||
           t == MeshTechnique::VertexNormal || t == MeshTechnique::UndulatingNormalMappedAlpha;
}

/// A technique of the POSTWATER + POSTEFFECT stage, the last meshes Moho
/// draws: after the water and the beams, particles and trails above it
/// (WRenViewport::Render's RenderMeshes(0x28)). The shields' (M211k).
inline bool is_post_effect_technique(MeshTechnique t) {
    return is_shield_technique(t);
}

/// The mesh.fx states the shields' passes draw with (M211k): blended
/// (SrcAlpha, InvSrcAlpha, RGBA), the same unculled, added colour (SrcAlpha,
/// One, RGB), added colour and glow (RGBA), and the fill's depth alone.
enum class ShieldState : u8 { Blend, BlendUnculled, AddRGB, AddRGBA, Fill };

/// A shield technique's passes: the state they draw with, and how many
/// (ShieldCybran's second is pushed out along the normal).
struct ShieldPasses {
    ShieldState state = ShieldState::Blend;
    u32 count = 1;
};

/// mesh.fx's table: UEF's and Cybran's impact cull nothing; Seraphim's
/// shield adds colour, the impact colour and glow; Cybran's draws twice.
inline ShieldPasses shield_passes(MeshTechnique t) {
    switch (t) {
    case MeshTechnique::ShieldUEF:
    case MeshTechnique::CybranShieldImpact: return {ShieldState::BlendUnculled, 1};
    case MeshTechnique::ShieldCybran: return {ShieldState::Blend, 2};
    case MeshTechnique::ShieldSeraphim: return {ShieldState::AddRGB, 1};
    case MeshTechnique::ShieldImpact: return {ShieldState::AddRGBA, 1};
    case MeshTechnique::ShieldFill: return {ShieldState::Fill, 1};
    default: return {ShieldState::Blend, 1}; // ShieldAeon's
    }
}

/// A technique with a depth stage (STAGE_DEPTH), which casts a shadow. Not
/// AeonBuild or AlphaFade: a unit Aeon are building, and UEF's build
/// slices, cast none (M211f/g); nor do shields (M211k).
inline bool has_depth_stage(MeshTechnique t) {
    return t != MeshTechnique::AeonBuild && t != MeshTechnique::AlphaFade &&
           !is_shield_technique(t);
}

/// What a technique reads as its instance's material.y (its `parameter`
/// annotation; HardwareMeshBatch::FillBatch).
enum class MeshParameter : u8 {
    FractionComplete, ///< how far built (the unit techniques, the build shaders)
    FractionHealth,   ///< health over max health (the shields, M211k)
};

inline MeshParameter mesh_parameter(MeshTechnique t) {
    return is_shield_technique(t) && t != MeshTechnique::ShieldImpact &&
                   t != MeshTechnique::CybranShieldImpact
               ? MeshParameter::FractionHealth
               : MeshParameter::FractionComplete;
}

struct GPUMesh {
    AllocatedBuffer vertex_buf{};
    AllocatedBuffer index_buf{};
    u32 index_count = 0;
    f32 uniform_scale = 1.0f;
    std::string texture_path;   // VFS path to albedo DDS (empty = no texture)
    std::string specteam_path;  // VFS path to SpecTeam DDS (empty = no team color mask)
    std::string normal_path;    // VFS path to normal map DDS (empty = no normal map)
    std::string lookup_path;    // VFS path to the LOD's LookupName (Seraphim's falloff)
    std::string secondary_path; // VFS path to the LOD's SecondaryName (the build shaders')
    bool wreckage = false;      // drawn with the Wreckage shader: a unit's wreck mesh
    MeshTechnique technique = MeshTechnique::Unit;
    /// The mesh blueprint's SortOrder: Moho draws meshes by it, smallest
    /// first (MeshBatchKeyLess; M211k).
    f32 sort_order = 0.0f;
};

/// A single LOD level: mesh data + camera distance cutoff.
struct LODEntry {
    GPUMesh mesh;
    f32 cutoff = 0.0f;  // max camera distance for this LOD (0 = no limit)
};

/// All LOD levels for a single blueprint, sorted by cutoff ascending
/// (highest detail / smallest cutoff first).
struct LODSet {
    std::vector<LODEntry> lods;  // sorted by cutoff ascending (highest detail first)
};

/// Caches GPU mesh buffers per blueprint ID.
/// Lazily loads .scm files via VFS, parses vertices+indices, uploads to GPU.
/// Supports multiple LOD levels per blueprint with distance-based selection.
class MeshCache {
public:
    void init(VkDevice device, VmaAllocator allocator,
              VkCommandPool cmd_pool, VkQueue queue,
              vfs::VirtualFileSystem* vfs,
              blueprints::BlueprintStore* store);

    /// Get or lazily load GPU mesh for a blueprint ID (highest detail LOD).
    /// Returns nullptr if mesh unavailable (caller falls back to cube).
    const GPUMesh* get(const std::string& blueprint_id, lua_State* L);

    /// Get best LOD mesh for given camera distance.
    /// Returns nullptr if mesh unavailable.
    const GPUMesh* get_lod(const std::string& blueprint_id, f32 camera_distance,
                           lua_State* L);

    /// Get the full LODSet for introspection. Returns nullptr if not loaded.
    const LODSet* get_lod_set(const std::string& blueprint_id) const;

    /// A blueprint's Display.UniformScale (1 without one), cached. A mesh
    /// set with SetMesh is a mesh blueprint, which has no scale: the
    /// entity's own blueprint gives it.
    f32 blueprint_scale(const std::string& blueprint_id, lua_State* L);

    void destroy(VkDevice device, VmaAllocator allocator);

private:
    std::string resolve_mesh_path(const std::string& bp_id, lua_State* L);
    std::string resolve_albedo_path(const std::string& bp_id, lua_State* L);
    std::string resolve_specteam_path(const std::string& bp_id, lua_State* L);
    std::string resolve_normal_path(const std::string& bp_id, lua_State* L);
    f32 resolve_uniform_scale(const std::string& bp_id, lua_State* L);
    /// The mesh blueprint a blueprint draws with: a unit's or prop's
    /// Display.MeshBlueprint, or the id itself when it names a mesh
    /// blueprint (SetMesh's wreck, build and enhancement meshes).
    std::string resolve_mesh_bp_id(const std::string& bp_id, lua_State* L);
    /// Derive base path from mesh bp ID: "/units/uel0001/uel0001_mesh" -> "/units/uel0001/uel0001".
    /// The variants Blueprints.lua derives ("_mesh_wreck", "_mesh_build") share the files.
    static std::string derive_base_path(const std::string& mesh_bp_id);

    /// Load all LOD levels for a blueprint. Returns true if at least one LOD loaded.
    bool load_lod_set(const std::string& bp_id, lua_State* L);

    /// Read LODCutoff from __blueprints[mesh_bp_id].LODs[lod_index].
    f32 read_lod_cutoff(const std::string& mesh_bp_id, i32 lod_index, lua_State* L);
    /// A number at a mesh blueprint's top (its SortOrder), 0 without one.
    f32 read_mesh_number(const std::string& mesh_bp_id, const char* field, lua_State* L);

    /// Read MeshName from __blueprints[mesh_bp_id].LODs[lod_index].
    std::string resolve_mesh_path_for_lod(const std::string& mesh_bp_id, i32 lod_index,
                                          lua_State* L);

    /// Read texture paths from __blueprints[mesh_bp_id].LODs[lod_index].
    /// Falls back to LOD 1 texture if field missing (textures often shared).
    std::string resolve_albedo_path_for_lod(const std::string& mesh_bp_id, i32 lod_index,
                                            lua_State* L);
    std::string resolve_specteam_path_for_lod(const std::string& mesh_bp_id, i32 lod_index,
                                              lua_State* L);
    std::string resolve_normal_path_for_lod(const std::string& mesh_bp_id, i32 lod_index,
                                            lua_State* L);
    /// A file the LOD names in `field` (LookupName, SecondaryName), LOD 1's
    /// if it names none, relative to the mesh blueprint's folder unless
    /// absolute; empty if neither names one.
    std::string resolve_lod_file(const std::string& mesh_bp_id, i32 lod_index, const char* field,
                                 lua_State* L);

    /// Read a string field from __blueprints[mesh_bp_id].LODs[lod_index].
    /// Returns empty string if not found.
    std::string read_lod_string_field(const std::string& mesh_bp_id, i32 lod_index,
                                      const char* field_name, lua_State* L);

    /// Upload a single SCM mesh to GPU buffers. Returns empty GPUMesh on failure.
    GPUMesh upload_scm_mesh(const std::string& mesh_path);

    std::unordered_map<std::string, LODSet> lod_cache_;
    std::unordered_map<std::string, f32> scale_cache_;
    std::unordered_set<std::string> failed_;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    vfs::VirtualFileSystem* vfs_ = nullptr;
    blueprints::BlueprintStore* store_ = nullptr;
};

} // namespace osc::renderer
