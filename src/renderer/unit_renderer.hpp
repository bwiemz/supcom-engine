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

/// A per-frame buffer's new capacity to hold `need` elements: `have`
/// doubled until it does, but not past `limit` (so it may fall short).
u32 grown_capacity(u32 have, u32 need, u32 limit);

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
    /// `meshes_drawn` false (strategic zoom, which draws icons in their
    /// place): no instances, only each entity's mesh birth kept.
    void update(const sim::FrameView& view, MeshCache& mesh_cache, lua_State* L,
                TextureCache* tex_cache = nullptr, const Camera* camera = nullptr,
                const std::unordered_set<u32>* selected_ids = nullptr,
                const Frustum* frustum = nullptr, bool meshes_drawn = true);

    void destroy(VkDevice device, VmaAllocator allocator);

    // --- Cube fallback accessors ---
    VkBuffer cube_vertex_buffer() const { return cube_verts_.buffer; }
    VkBuffer cube_index_buffer() const { return cube_indices_.buffer; }
    VkBuffer cube_instance_buffer() const { return cubes_[fi_].buf.buffer; }
    u32 cube_index_count() const { return 36; }
    u32 cube_instance_count() const { return cube_instance_count_; }

    // --- Mesh draw groups ---
    const std::vector<MeshDrawGroup>& mesh_groups() const {
        return mesh_groups_;
    }
    VkBuffer mesh_instance_buffer() const { return meshes_[fi_].buf.buffer; }
    /// This frame's mesh instances, which the groups' offsets index (the
    /// tests read a shield's transform and parameter; M211k).
    const MeshInstance* mesh_instances() const {
        return static_cast<const MeshInstance*>(meshes_[fi_].mapped);
    }
    VkBuffer bone_ssbo_buffer(u32 fi) const { return bones_[fi].buf.buffer; }
    VkBuffer bone_ssbo_buffer() const { return bones_[fi_].buf.buffer; }
    /// Counts the times slot `fi`'s bone SSBO was made: update() replaces it
    /// when it grows, and its descriptor set must then be written again.
    u32 bone_ssbo_generation(u32 fi) const { return bone_generation_[fi]; }
    /// How many mesh instances, cubes and bone matrices slot `fi` holds now.
    u32 mesh_capacity(u32 fi) const { return meshes_[fi].capacity; }
    u32 cube_capacity(u32 fi) const { return cubes_[fi].capacity; }
    u32 bone_capacity(u32 fi) const { return bones_[fi].capacity; }

    /// FA's `time` for this frame: the newest tick plus the interpolant
    /// toward it, wrapped as instance times are (MeshRenderer::ConfigureShader).
    f32 shader_time() const { return shader_time_; }

    /// Inject a single ghost mesh instance (for build preview).
    /// Call after update(). Returns true if the ghost was added.
    /// Room the next update() leaves for build ghosts
    void set_ghost_slots(u32 n) { ghost_slots_ = n; }

    bool inject_ghost(const GPUMesh* mesh, f32 x, f32 y, f32 z,
                      f32 r, f32 g, f32 b, f32 a,
                      TextureCache* tex_cache);

    void set_frame_index(u32 fi) { fi_ = fi; }

    /// This frame's instances, one sorted line each: mesh, model matrix,
    /// colour and a digest of the bone pose (the render-state dump).
    void dump(std::ostream& out) const;

    static constexpr u32 MAX_BONES_PER_UNIT = 64;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

    /// What each frame's buffers hold to start with; update() doubles them
    /// as a frame needs more. (A fixed 8192 instances left a whole map's
    /// worst meshes undrawn.)
    static constexpr u32 kInitialMeshInstances = 2048;
    static constexpr u32 kInitialCubes = 64;
    static constexpr u32 kInitialBones = 4096; ///< matrices: ~300 units' worth
    /// The most mesh instances or cubes a frame holds.
    static constexpr u32 kMaxInstances = 1u << 20;
    /// The most bone matrices: 128 MB, the storage buffer range every
    /// Vulkan device binds (maxStorageBufferRange's minimum).
    static constexpr u32 kMaxBones = (1u << 27) / (16 * sizeof(f32));

private:
    /// A per-frame buffer, persistently mapped, and how many elements it holds.
    struct MappedBuffer {
        AllocatedBuffer buf{};
        void* mapped = nullptr;
        u32 capacity = 0;
    };

    /// Make `buffer` hold `need` elements of `size` bytes, growing it to
    /// grown_capacity(), which drops what it held. False if it can't hold
    /// them all: it keeps what it has, and the frame draws what fits.
    bool reserve(MappedBuffer& buffer, u32 need, VkDeviceSize size, VkBufferUsageFlags usage,
                 u32 limit);
    void release(MappedBuffer& buffer);

    u32 fi_ = 0; // current frame index for double-buffering
    VmaAllocator allocator_ = VK_NULL_HANDLE;

    // Cube fallback geometry
    AllocatedBuffer cube_verts_{};
    AllocatedBuffer cube_indices_{};
    MappedBuffer cubes_[FRAMES_IN_FLIGHT];
    u32 cube_instance_count_ = 0;
    std::vector<CubeInstance> cube_scratch_; // this frame's cubes, before they're copied

    // Mesh instance buffer (shared across all mesh groups)
    MappedBuffer meshes_[FRAMES_IN_FLIGHT];

    // Bone SSBO (all bone matrices for all mesh instances)
    MappedBuffer bones_[FRAMES_IN_FLIGHT];
    u32 bone_generation_[FRAMES_IN_FLIGHT] = {};
    bool warned_full_ = false; // a frame fell short of a limit (logged once)

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
    u32 ghost_slots_ = 1;
};

} // namespace osc::renderer
