#include "renderer/unit_renderer.hpp"
#include "renderer/army_colors.hpp"
#include "renderer/camera.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/vk_types.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstring>
#include <ostream>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace osc::renderer {

// Cube vertex: position + normal
struct CubeVertex {
    f32 x, y, z;
    f32 nx, ny, nz;
};

/// Resolve army color for an entity.
static void get_army_color(const sim::EntityRecord& entity, const sim::FrameView& view,
                            f32& r, f32& g, f32& b, f32& a) {
    i32 army = entity.army;
    const sim::ArmyRecord* brain = view.cur() ? view.cur()->army(army) : nullptr;
    if (brain) {
        if (brain->has_color) {
            r = brain->r / 255.0f;
            g = brain->g / 255.0f;
            b = brain->b / 255.0f;
        } else if (army < 8) {
            r = ARMY_COLORS[army][0];
            g = ARMY_COLORS[army][1];
            b = ARMY_COLORS[army][2];
        } else {
            r = g = b = 0.7f;
        }
    } else {
        r = g = b = 1.0f; // neutral/props: white (albedo shows through for mesh)
    }
    a = (entity.fraction_complete < 1.0f) ? 0.4f : 1.0f;
}

/// Build column-major 4x4 model matrix from position + quaternion + non-uniform scale.
static void build_model_matrix(f32* out, const sim::Vector3& pos,
                                const sim::Quaternion& q,
                                f32 sx, f32 sy, f32 sz) {
    // Quaternion to 3x3 rotation matrix with per-axis scale
    f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    // Column 0 (X-axis, scaled by sx)
    out[0]  = (1.0f - 2.0f * (yy + zz)) * sx;
    out[1]  = (2.0f * (xy + wz)) * sx;
    out[2]  = (2.0f * (xz - wy)) * sx;
    out[3]  = 0.0f;
    // Column 1 (Y-axis, scaled by sy)
    out[4]  = (2.0f * (xy - wz)) * sy;
    out[5]  = (1.0f - 2.0f * (xx + zz)) * sy;
    out[6]  = (2.0f * (yz + wx)) * sy;
    out[7]  = 0.0f;
    // Column 2 (Z-axis, scaled by sz)
    out[8]  = (2.0f * (xz + wy)) * sz;
    out[9]  = (2.0f * (yz - wx)) * sz;
    out[10] = (1.0f - 2.0f * (xx + yy)) * sz;
    out[11] = 0.0f;
    // Column 3 (translation)
    out[12] = pos.x;
    out[13] = pos.y;
    out[14] = pos.z;
    out[15] = 1.0f;
}

void UnitRenderer::build(VkDevice device, VmaAllocator allocator,
                         VkCommandPool cmd_pool, VkQueue queue) {
    // Unit cube: centered at origin, 1x1x1
    static const CubeVertex cube_verts[] = {
        // Front (+Z)
        {-0.5f, 0, -0.5f,  0, 0, -1}, { 0.5f, 0, -0.5f,  0, 0, -1},
        { 0.5f, 1, -0.5f,  0, 0, -1}, {-0.5f, 1, -0.5f,  0, 0, -1},
        // Back (-Z)
        { 0.5f, 0,  0.5f,  0, 0,  1}, {-0.5f, 0,  0.5f,  0, 0,  1},
        {-0.5f, 1,  0.5f,  0, 0,  1}, { 0.5f, 1,  0.5f,  0, 0,  1},
        // Left (-X)
        {-0.5f, 0,  0.5f, -1, 0,  0}, {-0.5f, 0, -0.5f, -1, 0,  0},
        {-0.5f, 1, -0.5f, -1, 0,  0}, {-0.5f, 1,  0.5f, -1, 0,  0},
        // Right (+X)
        { 0.5f, 0, -0.5f,  1, 0,  0}, { 0.5f, 0,  0.5f,  1, 0,  0},
        { 0.5f, 1,  0.5f,  1, 0,  0}, { 0.5f, 1, -0.5f,  1, 0,  0},
        // Top (+Y)
        {-0.5f, 1, -0.5f,  0, 1,  0}, { 0.5f, 1, -0.5f,  0, 1,  0},
        { 0.5f, 1,  0.5f,  0, 1,  0}, {-0.5f, 1,  0.5f,  0, 1,  0},
        // Bottom (-Y)
        {-0.5f, 0,  0.5f,  0, -1, 0}, { 0.5f, 0,  0.5f,  0, -1, 0},
        { 0.5f, 0, -0.5f,  0, -1, 0}, {-0.5f, 0, -0.5f,  0, -1, 0},
    };

    static const u32 cube_indices[] = {
        0,1,2, 2,3,0,       // front
        4,5,6, 6,7,4,       // back
        8,9,10, 10,11,8,    // left
        12,13,14, 14,15,12, // right
        16,17,18, 18,19,16, // top
        20,21,22, 22,23,20, // bottom
    };

    cube_verts_ = upload_buffer(device, allocator, cmd_pool, queue,
                                cube_verts, sizeof(cube_verts),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    cube_indices_ = upload_buffer(device, allocator, cmd_pool, queue,
                                  cube_indices, sizeof(cube_indices),
                                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    // Host-visible cube instance buffer (persistently mapped)
    auto create_instance_buf = [&](AllocatedBuffer& buf, void*& mapped,
                                    VkDeviceSize elem_size) {
        VkBufferCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size = MAX_INSTANCES * elem_size;
        ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        VmaAllocationInfo info{};
        vmaCreateBuffer(allocator, &ci, &alloc_ci,
                        &buf.buffer, &buf.allocation, &info);
        mapped = info.pMappedData;
    };

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        create_instance_buf(cube_instance_buf_[i], cube_instance_mapped_[i],
                            sizeof(CubeInstance));
        create_instance_buf(mesh_instance_buf_[i], mesh_instance_mapped_[i],
                            sizeof(MeshInstance));
    }

    // Bone SSBO (persistently mapped, for GPU skinning, per-frame)
    {
        VkBufferCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        // MAX_INSTANCES * MAX_BONES_PER_UNIT * sizeof(mat4) = 8192*64*64 = 32MB
        ci.size = static_cast<VkDeviceSize>(MAX_INSTANCES) *
                  MAX_BONES_PER_UNIT * sizeof(f32) * 16;
        ci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                         VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

        for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
            VmaAllocationInfo info{};
            vmaCreateBuffer(allocator, &ci, &alloc_ci,
                            &bone_ssbo_[i].buffer, &bone_ssbo_[i].allocation,
                            &info);
            bone_ssbo_mapped_[i] = info.pMappedData;
        }
    }
}

void UnitRenderer::preload_meshes(const std::vector<std::string>& bp_ids,
                                   MeshCache& mesh_cache, lua_State* L) {
    u32 loaded = 0, failed = 0;
    for (auto& id : bp_ids) {
        if (mesh_cache.get(id, L))
            loaded++;
        else
            failed++;
    }

    spdlog::info("MeshCache: preloaded {} unique blueprints ({} loaded, {} failed)",
                 bp_ids.size(), loaded, failed);
}

void UnitRenderer::update(const sim::FrameView& view, MeshCache& mesh_cache,
                           lua_State* L, TextureCache* tex_cache,
                           const Camera* camera,
                           const std::unordered_set<u32>* selected_ids,
                           const Frustum* frustum) {
    mesh_groups_.clear();

    if (!cube_instance_mapped_[fi_] || !mesh_instance_mapped_[fi_]) return;

    auto* cube_instances = static_cast<CubeInstance*>(cube_instance_mapped_[fi_]);
    auto* mesh_instances = static_cast<MeshInstance*>(mesh_instance_mapped_[fi_]);
    auto* bone_data = bone_ssbo_mapped_[fi_]
                          ? static_cast<f32*>(bone_ssbo_mapped_[fi_]) : nullptr;
    u32 cube_count = 0;
    u32 mesh_count = 0;

    // Frustum culling replaces old distance-only prop culling

    // Per-instance bone info (parallel to mesh_groups entries)
    struct InstanceBones {
        u32 id = 0;          // 0 = no bones (props, projectiles)
        u32 bone_count = 0;  // 0 = no skinning
    };

    // Group mesh instances by GPUMesh pointer, with bone info
    struct GroupData {
        std::vector<MeshInstance> instances;
        std::vector<InstanceBones> bones;
    };
    std::unordered_map<const GPUMesh*, GroupData> mesh_groups;

    for (const sim::EntityRecord& entity : view.entities()) {
        if (!entity.is_unit && !entity.is_prop && !entity.is_projectile) continue;
        if (cube_count + mesh_count >= MAX_INSTANCES)
            continue;

        const sim::Vector3 pos = view.position(entity);

        // Frustum cull all entities (units, props, projectiles)
        if (frustum) {
            f32 bound_radius = 5.0f; // default for projectiles
            if (entity.is_unit) {
                bound_radius = std::max(entity.footprint_size_x * 1.5f, 5.0f);
            } else if (entity.is_prop) {
                bound_radius = std::max(entity.scale_x * 2.0f, 2.0f);
            }
            if (!frustum->is_sphere_visible(pos.x, pos.y, pos.z, bound_radius)) {
                continue;
            }
        }

        f32 r, g, b, a;
        get_army_color(entity, view, r, g, b, a);

        // Compute camera distance for LOD selection
        f32 cam_dist = 0.0f;
        if (camera) {
            f32 ex, ey, ez;
            camera->eye_position(ex, ey, ez);
            f32 dx = pos.x - ex;
            f32 dy = pos.y - ey;
            f32 dz = pos.z - ez;
            cam_dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        // Try mesh lookup (mesh_override from SetMesh takes priority). An
        // override is a mesh blueprint (a wreck, build or enhancement mesh),
        // which has no scale of its own: the entity's blueprint gives it.
        const GPUMesh* gpu = nullptr;
        f32 mesh_scale = 0.0f;
        if (!entity.mesh_override.empty()) {
            gpu = mesh_cache.get_lod(entity.mesh_override, cam_dist, L);
            if (gpu) mesh_scale = mesh_cache.blueprint_scale(entity.blueprint_id, L);
        }
        if (!gpu && !entity.blueprint_id.empty()) {
            gpu = mesh_cache.get_lod(entity.blueprint_id, cam_dist, L);
            if (gpu) mesh_scale = gpu->uniform_scale;
        }

        if (gpu) {
            if (mesh_count >= MAX_INSTANCES) continue;
            MeshInstance inst{};
            f32 sx = entity.scale_x * mesh_scale;
            f32 sy = entity.scale_y * mesh_scale;
            f32 sz = entity.scale_z * mesh_scale;
            build_model_matrix(inst.model, pos, view.orientation(entity), sx, sy, sz);
            // A wreck has no team colour: a negative red tells mesh.frag to
            // draw it burnt, as the Wreckage shader does.
            if (entity.is_wreckage || gpu->wreckage) {
                r = -1.0f;
                g = 0.0f;
                b = 0.0f;
            }
            // Selection highlight: brighten team color
            if (selected_ids && entity.is_unit &&
                selected_ids->count(entity.id)) {
                r = r * 0.5f + 0.5f;
                g = g * 0.5f + 0.5f;
                b = b * 0.5f + 0.5f;
            }
            inst.r = r; inst.g = g; inst.b = b; inst.a = a;

            auto& gd = mesh_groups[gpu];
            gd.instances.push_back(inst);

            // Track bone data for this instance (a prop has a pose only when
            // TryCopyPose gave it its unit's)
            if (entity.is_unit || entity.bone_count > 0) {
                u32 bc = entity.bone_count;
                if (bc > MAX_BONES_PER_UNIT) bc = MAX_BONES_PER_UNIT;
                gd.bones.push_back({entity.id, bc});
            } else {
                gd.bones.push_back({0, 0});
            }

            mesh_count++;
        } else {
            if (cube_count >= MAX_INSTANCES) continue;
            auto& inst = cube_instances[cube_count];
            inst.x = pos.x;
            inst.y = pos.y;
            inst.z = pos.z;
            inst.scale = 2.0f;
            // Use muted green for props (trees/rocks) to avoid white cube sea
            if (entity.is_prop) {
                inst.r = 0.28f; inst.g = 0.42f; inst.b = 0.18f; inst.a = a;
            } else {
                inst.r = r; inst.g = g; inst.b = b; inst.a = a;
            }
            cube_count++;
        }
    }

    cube_instance_count_ = cube_count;

    // Flatten mesh groups into contiguous instance buffer + bone SSBO
    u32 offset = 0;
    u32 bone_offset = 0; // in mat4 units (each mat4 = 16 floats)
    static constexpr f32 IDENTITY[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    u32 max_bone_entries = MAX_INSTANCES * MAX_BONES_PER_UNIT;
    std::vector<sim::BoneMatrix> blended; // this frame's pose, between ticks

    mesh_groups_.clear();
    for (auto& [gpu, gd] : mesh_groups) {
        u32 count = static_cast<u32>(gd.instances.size());
        if (offset + count > MAX_INSTANCES) {
            count = MAX_INSTANCES - offset;
        }
        std::memcpy(mesh_instances + offset, gd.instances.data(),
                     count * sizeof(MeshInstance));

        MeshDrawGroup group;
        group.mesh = gpu;
        group.instance_offset = offset;
        group.instance_count = count;

        // Determine bones_per_instance: use max across the group
        // (all instances in a group share the same blueprint/mesh)
        u32 group_bones = 0;
        for (u32 i = 0; i < count; i++) {
            if (gd.bones[i].bone_count > group_bones)
                group_bones = gd.bones[i].bone_count;
        }

        // Write bone matrices to SSBO
        group.bone_base_offset = bone_offset;
        group.bones_per_instance = group_bones;

        if (bone_data && group_bones > 0 && bone_offset < max_bone_entries) {
            u32 max_for_bones = (max_bone_entries - bone_offset) / group_bones;
            u32 safe_count = std::min(count, max_for_bones);
            for (u32 i = 0; i < safe_count; i++) {
                u32 base = bone_offset + i * group_bones;

                u32 bc = 0;
                if (gd.bones[i].id && view.bones(gd.bones[i].id, blended)) {
                    const auto& mats = blended;
                    bc = static_cast<u32>(mats.size());
                    if (bc > group_bones) bc = group_bones;

                    // Copy actual bone matrices
                    for (u32 b = 0; b < bc; b++) {
                        std::memcpy(bone_data + (base + b) * 16,
                                    mats[b].data(), sizeof(f32) * 16);
                    }
                }
                // Fill remaining with identity
                for (u32 b = bc; b < group_bones; b++) {
                    std::memcpy(bone_data + (base + b) * 16,
                                IDENTITY, sizeof(f32) * 16);
                }
            }
            bone_offset += safe_count * group_bones;
        }

        // Resolve texture descriptors for this group
        if (tex_cache && gpu && !gpu->texture_path.empty()) {
            auto* tex = tex_cache->get(gpu->texture_path);
            if (tex) {
                group.texture_ds = tex->descriptor_set;
            } else {
                group.texture_ds = tex_cache->fallback_descriptor();
            }
        } else if (tex_cache) {
            group.texture_ds = tex_cache->fallback_descriptor();
        }

        // Resolve SpecTeam texture (team color mask)
        if (tex_cache && gpu && !gpu->specteam_path.empty()) {
            auto* spec = tex_cache->get(gpu->specteam_path);
            if (spec) {
                group.specteam_ds = spec->descriptor_set;
            } else {
                group.specteam_ds = tex_cache->specteam_fallback_descriptor();
            }
        } else if (tex_cache) {
            group.specteam_ds = tex_cache->specteam_fallback_descriptor();
        }

        // Resolve normal map texture
        if (tex_cache && gpu && !gpu->normal_path.empty()) {
            auto* norm = tex_cache->get(gpu->normal_path);
            if (norm) {
                group.normal_ds = norm->descriptor_set;
            } else {
                group.normal_ds = tex_cache->normal_fallback_descriptor();
            }
        } else if (tex_cache) {
            group.normal_ds = tex_cache->normal_fallback_descriptor();
        }

        mesh_groups_.push_back(group);

        offset += count;
    }
}

bool UnitRenderer::inject_ghost(const GPUMesh* mesh, f32 x, f32 y, f32 z,
                                 f32 r, f32 g, f32 b, f32 a,
                                 TextureCache* tex_cache) {
    if (!mesh || !mesh_instance_mapped_[fi_]) return false;

    // Count total instances already used
    u32 total = 0;
    for (auto& g : mesh_groups_) total += g.instance_count;
    if (total >= MAX_INSTANCES) return false;

    auto* instances = static_cast<MeshInstance*>(mesh_instance_mapped_[fi_]);
    auto& inst = instances[total];

    // Identity rotation, uniform_scale from mesh
    f32 s = mesh->uniform_scale;
    std::memset(inst.model, 0, sizeof(inst.model));
    inst.model[0] = s;  inst.model[5] = s;  inst.model[10] = s;  inst.model[15] = 1.0f;
    inst.model[12] = x; inst.model[13] = y;  inst.model[14] = z;
    inst.r = r; inst.g = g; inst.b = b; inst.a = a;

    // Find existing group for this mesh or create new one
    MeshDrawGroup* target = nullptr;
    for (auto& grp : mesh_groups_) {
        if (grp.mesh == mesh &&
            grp.instance_offset + grp.instance_count == total) {
            target = &grp;
            break;
        }
    }

    if (target) {
        target->instance_count++;
    } else {
        MeshDrawGroup grp;
        grp.mesh = mesh;
        grp.instance_offset = total;
        grp.instance_count = 1;
        grp.bone_base_offset = 0;
        grp.bones_per_instance = 0;

        if (tex_cache && !mesh->texture_path.empty()) {
            auto* tex = tex_cache->get(mesh->texture_path);
            grp.texture_ds = tex ? tex->descriptor_set
                                 : tex_cache->fallback_descriptor();
        } else if (tex_cache) {
            grp.texture_ds = tex_cache->fallback_descriptor();
        }
        if (tex_cache && !mesh->specteam_path.empty()) {
            auto* spec = tex_cache->get(mesh->specteam_path);
            grp.specteam_ds = spec ? spec->descriptor_set
                                   : tex_cache->specteam_fallback_descriptor();
        } else if (tex_cache) {
            grp.specteam_ds = tex_cache->specteam_fallback_descriptor();
        }
        if (tex_cache && !mesh->normal_path.empty()) {
            auto* norm = tex_cache->get(mesh->normal_path);
            grp.normal_ds = norm ? norm->descriptor_set
                                 : tex_cache->normal_fallback_descriptor();
        } else if (tex_cache) {
            grp.normal_ds = tex_cache->normal_fallback_descriptor();
        }

        mesh_groups_.push_back(grp);
    }

    return true;
}

void UnitRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    auto safe_destroy = [&](AllocatedBuffer& buf) {
        if (buf.buffer)
            vmaDestroyBuffer(allocator, buf.buffer, buf.allocation);
        buf = {};
    };

    safe_destroy(cube_verts_);
    safe_destroy(cube_indices_);
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        safe_destroy(cube_instance_buf_[i]);
        safe_destroy(mesh_instance_buf_[i]);
        safe_destroy(bone_ssbo_[i]);
        cube_instance_mapped_[i] = nullptr;
        mesh_instance_mapped_[i] = nullptr;
        bone_ssbo_mapped_[i] = nullptr;
    }
    cube_instance_count_ = 0;
    mesh_groups_.clear();
}

void UnitRenderer::dump(std::ostream& out) const {
    auto fnv = [](const std::string& text) {
        u64 h = 1469598103934665603ULL;
        for (unsigned char c : text) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    };
    std::vector<std::string> lines;
    const auto* meshes = static_cast<const MeshInstance*>(mesh_instance_mapped_[fi_]);
    const auto* bones = static_cast<const f32*>(bone_ssbo_mapped_[fi_]);
    for (const auto& g : mesh_groups_) {
        for (u32 i = 0; meshes && i < g.instance_count; ++i) {
            const MeshInstance& m = meshes[g.instance_offset + i];
            std::string line = fmt::format("mesh {} |", g.mesh ? g.mesh->texture_path : "?");
            for (f32 v : m.model) line += fmt::format(" {:.4f}", v);
            line += fmt::format(" | {:.3f} {:.3f} {:.3f} {:.3f}", m.r, m.g, m.b, m.a);
            if (bones && g.bones_per_instance > 0) {
                std::string pose;
                const f32* b = bones + static_cast<size_t>(g.bone_base_offset +
                                                           i * g.bones_per_instance) * 16;
                for (u32 k = 0; k < g.bones_per_instance * 16; ++k) pose += fmt::format("{:.4f},", b[k]);
                line += fmt::format(" | bones {} {:016x}", g.bones_per_instance, fnv(pose));
            }
            lines.push_back(std::move(line));
        }
    }
    const auto* cubes = static_cast<const CubeInstance*>(cube_instance_mapped_[fi_]);
    for (u32 i = 0; cubes && i < cube_instance_count_; ++i) {
        const CubeInstance& c = cubes[i];
        lines.push_back(fmt::format("cube {:.4f} {:.4f} {:.4f} {:.3f} | {:.3f} {:.3f} {:.3f} {:.3f}",
                                    c.x, c.y, c.z, c.scale, c.r, c.g, c.b, c.a));
    }
    std::sort(lines.begin(), lines.end());
    for (const auto& l : lines) out << l << '\n';
}

} // namespace osc::renderer
