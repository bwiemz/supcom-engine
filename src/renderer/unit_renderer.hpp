#pragma once

#include "renderer/vk_types.hpp"
#include "renderer/mesh_cache.hpp"
#include "renderer/frustum.hpp"
#include "core/types.hpp"
#include "sim/game_colors.hpp"

#include <iosfwd>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct lua_State;

namespace osc::sim {
class FrameView;
struct ArmyRecord;
}

namespace osc::renderer {

class Camera;       // forward
class ReconView;    // forward
class UserPlayableRect; // forward
class TextureCache; // forward

/// Per-instance data for cube fallback (old format).
struct CubeInstance {
    f32 x, y, z;     // world position
    f32 scale;        // footprint-based scale
    f32 r, g, b, a;  // army color + alpha
};

/// Per-instance data for real mesh rendering.
struct MeshInstance {
    f32 model[16];    // column-major 4x4 model matrix
    f32 r, g, b, a;  // army color + alpha
    f32 color_lookup; // the row of the mesh's lookup texture (team_color_lookup)
    f32 shader_time;  // FA's material.x: the tick its mesh instance was made (mod 36000)
    f32 parameter; // FA's material.y: the fraction complete (M211f), or a shield's health (M211k)
};

/// FA's colorLookup (UserUnit::CreateMeshInstance), the row a mesh's lookup
/// texture is read at for its army: (i + 0.5) / n. i is the army colour's
/// index in GameColors.ArmyColors (func_GetColorIndex: 3 if it isn't there,
/// 0 for no army), clamped below n, the count of PlayerColors (at least 1).
f32 team_color_lookup(const sim::ArmyRecord* army, const sim::GameColors& colors);

/// A group of instances sharing the same GPU mesh.
struct MeshDrawGroup {
    const GPUMesh* mesh = nullptr;
    u32 instance_offset = 0;  // offset into shared instance buffer
    u32 instance_count = 0;
    VkDescriptorSet texture_ds = VK_NULL_HANDLE;  // albedo texture descriptor (set=0)
    VkDescriptorSet specteam_ds = VK_NULL_HANDLE; // SpecTeam texture descriptor (set=2)
    VkDescriptorSet normal_ds = VK_NULL_HANDLE;   // Normal map descriptor (set=3)
    VkDescriptorSet lookup_ds = VK_NULL_HANDLE;   // The mesh's lookup texture (set=5)
    VkDescriptorSet secondary_ds = VK_NULL_HANDLE; // The mesh's secondary texture (set=6)
    u32 bone_base_offset = 0; // index into bone SSBO (in mat4 units)
    u32 bones_per_instance = 0; // 0 = no skinning, else bone count
    /// Its instances blend (the build ghost's fade, a build technique's own
    /// alpha), drawn after the opaque groups.
    bool fading = false;
    /// Its instances are units, which the water reflects (M213b): Moho makes
    /// a unit's mesh instance reflected, and clears any other entity's.
    bool reflected = false;
};

/// Renders units as real SCM meshes where available, with cube fallback.
class UnitRenderer {
public:
    /// Upload static cube mesh to GPU.
    void build(VkDevice device, VmaAllocator allocator,
               VkCommandPool cmd_pool, VkQueue queue);

    /// The game's colour tables, which pick each army's lookup row.
    void set_game_colors(sim::GameColors colors) { game_colors_ = std::move(colors); }

    /// The player's intel, which hides what it doesn't see and freezes the
    /// structures it remembers (null: everything seen; M215a).
    void set_recon(const ReconView* recon) { recon_ = recon; }

    /// The user side's playable rect, whose last sync hid the meshes then
    /// outside it (null: none hidden).
    void set_playable_rect(const UserPlayableRect* rect) { playable_rect_ = rect; }

    /// Pre-load GPU meshes for these blueprints (sim::world_blueprints).
    void preload_meshes(const std::vector<std::string>& bp_ids, MeshCache& mesh_cache,
                        lua_State* L);

    /// Update per-frame instance data from the world as `view` draws it
    /// (between the last two ticks).
    /// If selected_ids is non-null, those units get a selection highlight.
    void update(const sim::FrameView& view, MeshCache& mesh_cache,
                lua_State* L, TextureCache* tex_cache = nullptr,
                const Camera* camera = nullptr,
                const std::unordered_set<u32>* selected_ids = nullptr,
                const Frustum* frustum = nullptr);

    void destroy(VkDevice device, VmaAllocator allocator);

    // --- Cube fallback accessors ---
    VkBuffer cube_vertex_buffer() const { return cube_verts_.buffer; }
    VkBuffer cube_index_buffer() const { return cube_indices_.buffer; }
    VkBuffer cube_instance_buffer() const { return cube_instance_buf_[fi_].buffer; }
    u32 cube_index_count() const { return 36; }
    u32 cube_instance_count() const { return cube_instance_count_; }

    // --- Mesh draw groups ---
    const std::vector<MeshDrawGroup>& mesh_groups() const {
        return mesh_groups_;
    }
    VkBuffer mesh_instance_buffer() const { return mesh_instance_buf_[fi_].buffer; }
    /// This frame's mesh instances, which the groups' offsets index (the
    /// tests read a shield's transform and parameter; M211k).
    const MeshInstance* mesh_instances() const {
        return static_cast<const MeshInstance*>(mesh_instance_mapped_[fi_]);
    }
    VkBuffer bone_ssbo_buffer(u32 fi) const { return bone_ssbo_[fi].buffer; }
    VkBuffer bone_ssbo_buffer() const { return bone_ssbo_[fi_].buffer; }

    /// FA's `time` for this frame: the newest tick plus the interpolant
    /// toward it, wrapped as instance times are (MeshRenderer::ConfigureShader).
    f32 shader_time() const { return shader_time_; }

    /// Inject a single ghost mesh instance (for build preview).
    /// Call after update(). Returns true if the ghost was added.
    bool inject_ghost(const GPUMesh* mesh, f32 x, f32 y, f32 z,
                      f32 r, f32 g, f32 b, f32 a,
                      TextureCache* tex_cache);

    void set_frame_index(u32 fi) { fi_ = fi; }

    /// This frame's instances, one sorted line each: mesh, model matrix,
    /// colour and a digest of the bone pose (the render-state dump).
    void dump(std::ostream& out) const;

    static constexpr u32 MAX_INSTANCES = 8192;
    static constexpr u32 MAX_BONES_PER_UNIT = 64;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    u32 fi_ = 0; // current frame index for double-buffering

    // Cube fallback geometry
    AllocatedBuffer cube_verts_{};
    AllocatedBuffer cube_indices_{};
    AllocatedBuffer cube_instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* cube_instance_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 cube_instance_count_ = 0;

    // Mesh instance buffer (shared across all mesh groups)
    AllocatedBuffer mesh_instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* mesh_instance_mapped_[FRAMES_IN_FLIGHT] = {};

    // Bone SSBO (all bone matrices for all mesh instances)
    AllocatedBuffer bone_ssbo_[FRAMES_IN_FLIGHT] = {};
    void* bone_ssbo_mapped_[FRAMES_IN_FLIGHT] = {};

    // Per-frame draw groups (rebuilt each frame)
    std::vector<MeshDrawGroup> mesh_groups_;

    sim::GameColors game_colors_;
    const ReconView* recon_ = nullptr;
    const UserPlayableRect* playable_rect_ = nullptr;

    /// When each entity's mesh instance was made: FA makes one when an
    /// entity appears or changes mesh, stamped with the tick (material.x).
    struct MeshBirth {
        std::string mesh; ///< the blueprint or override it was drawn with
        u32 tick = 0;
        u64 frame = 0; ///< the last update that saw it
    };
    std::unordered_map<u32, MeshBirth> births_;
    u64 frame_ = 0;
    f32 shader_time_ = 0.0f;
};

} // namespace osc::renderer
