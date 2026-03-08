#include "renderer/unit_renderer.hpp"
#include "renderer/camera.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/vk_types.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/entity.hpp"
#include "sim/prop.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace osc::renderer {

// Cube vertex: position + normal
struct CubeVertex {
    f32 x, y, z;
    f32 nx, ny, nz;
};

// Default army colors (RGBA, 0-1)
static constexpr std::array<std::array<f32, 3>, 8> ARMY_COLORS = {{
    {0.2f, 0.4f, 1.0f},  // ARMY_1: Blue
    {1.0f, 0.2f, 0.2f},  // ARMY_2: Red
    {0.2f, 0.8f, 0.2f},  // ARMY_3: Green
    {1.0f, 1.0f, 0.2f},  // ARMY_4: Yellow
    {1.0f, 0.5f, 0.1f},  // ARMY_5: Orange
    {0.7f, 0.2f, 0.9f},  // ARMY_6: Purple
    {0.2f, 0.9f, 0.9f},  // ARMY_7: Cyan
    {0.9f, 0.9f, 0.9f},  // ARMY_8: White
}};

/// Resolve army color for an entity.
static void get_army_color(const sim::Entity& entity,
                            const sim::SimState& sim,
                            f32& r, f32& g, f32& b, f32& a) {
    i32 army = entity.army();
    if (army >= 0 && army < static_cast<i32>(sim.army_count())) {
        auto* brain = sim.army_at(static_cast<size_t>(army));
        if (brain && (brain->color_r() || brain->color_g() ||
                      brain->color_b())) {
            r = brain->color_r() / 255.0f;
            g = brain->color_g() / 255.0f;
            b = brain->color_b() / 255.0f;
        } else if (army < 8) {
            r = ARMY_COLORS[army][0];
            g = ARMY_COLORS[army][1];
            b = ARMY_COLORS[army][2];
        } else {
            r = g = b = 0.7f;
        }
    } else {
        r = g = b = 1.0f; // neutral/props: white (albedo shows through)
    }
    a = (entity.fraction_complete() < 1.0f) ? 0.4f : 1.0f;
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

        VmaAllocationInfo info{};
        vmaCreateBuffer(allocator, &ci, &alloc_ci,
                        &buf.buffer, &buf.allocation, &info);
        mapped = info.pMappedData;
    };

    create_instance_buf(cube_instance_buf_, cube_instance_mapped_,
                        sizeof(CubeInstance));
    create_instance_buf(mesh_instance_buf_, mesh_instance_mapped_,
                        sizeof(MeshInstance));

    // Bone SSBO (persistently mapped, for GPU skinning)
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

        VmaAllocationInfo info{};
        vmaCreateBuffer(allocator, &ci, &alloc_ci,
                        &bone_ssbo_.buffer, &bone_ssbo_.allocation, &info);
        bone_ssbo_mapped_ = info.pMappedData;
    }
}

void UnitRenderer::preload_meshes(const sim::SimState& sim,
                                   MeshCache& mesh_cache, lua_State* L) {
    // Collect unique blueprint IDs
    std::unordered_set<std::string> bp_ids;
    sim.entity_registry().for_each([&](const sim::Entity& entity) {
        if ((entity.is_unit() || entity.is_prop() || entity.is_projectile()) && !entity.destroyed() &&
            !entity.blueprint_id().empty()) {
            bp_ids.insert(entity.blueprint_id());
        }
    });

    for (auto& id : bp_ids) {
        mesh_cache.get(id, L);
    }

    spdlog::info("MeshCache: preloaded {} unique blueprints", bp_ids.size());
}

void UnitRenderer::update(const sim::SimState& sim, MeshCache& mesh_cache,
                           lua_State* L, TextureCache* tex_cache,
                           const Camera* camera,
                           const std::unordered_set<u32>* selected_ids) {
    mesh_groups_.clear();

    if (!cube_instance_mapped_ || !mesh_instance_mapped_) return;

    auto* cube_instances = static_cast<CubeInstance*>(cube_instance_mapped_);
    auto* mesh_instances = static_cast<MeshInstance*>(mesh_instance_mapped_);
    auto* bone_data = bone_ssbo_mapped_
                          ? static_cast<f32*>(bone_ssbo_mapped_) : nullptr;
    u32 cube_count = 0;
    u32 mesh_count = 0;

    // Camera eye XZ for ground-plane distance culling (props only)
    f32 cam_x = 0, cam_z = 0;
    constexpr f32 PROP_CULL_DIST_SQ = 600.0f * 600.0f;
    if (camera) {
        f32 cam_y = 0;
        camera->eye_position(cam_x, cam_y, cam_z);
        (void)cam_y; // XZ-only culling is intentional for RTS ground-plane
    }

    // Per-instance bone info (parallel to mesh_groups entries)
    struct InstanceBones {
        const sim::Unit* unit = nullptr;
        u32 bone_count = 0;  // 0 = no skinning
    };

    // Group mesh instances by GPUMesh pointer, with bone info
    struct GroupData {
        std::vector<MeshInstance> instances;
        std::vector<InstanceBones> bones;
    };
    std::unordered_map<const GPUMesh*, GroupData> mesh_groups;

    sim.entity_registry().for_each([&](const sim::Entity& entity) {
        if ((!entity.is_unit() && !entity.is_prop() && !entity.is_projectile()) || entity.destroyed())
            return;
        if (cube_count + mesh_count >= MAX_INSTANCES)
            return;

        // Distance-cull props (units always render)
        if (entity.is_prop() && camera) {
            f32 dx = entity.position().x - cam_x;
            f32 dz = entity.position().z - cam_z;
            if (dx * dx + dz * dz > PROP_CULL_DIST_SQ) return;
        }

        f32 r, g, b, a;
        get_army_color(entity, sim, r, g, b, a);

        // Try mesh lookup (mesh_override from SetMesh takes priority)
        const GPUMesh* gpu = nullptr;
        if (!entity.mesh_override().empty())
            gpu = mesh_cache.get(entity.mesh_override(), L);
        if (!gpu && !entity.blueprint_id().empty())
            gpu = mesh_cache.get(entity.blueprint_id(), L);

        if (gpu) {
            if (mesh_count >= MAX_INSTANCES) return;
            MeshInstance inst{};
            f32 sx = entity.scale_x() * gpu->uniform_scale;
            f32 sy = entity.scale_y() * gpu->uniform_scale;
            f32 sz = entity.scale_z() * gpu->uniform_scale;
            build_model_matrix(inst.model, entity.position(),
                               entity.orientation(), sx, sy, sz);
            // Wreckage: desaturate + darken to distinguish from live units
            if (entity.is_wreckage()) {
                f32 lum = 0.299f * r + 0.587f * g + 0.114f * b;
                r = lum * 0.5f + r * 0.15f;
                g = lum * 0.5f + g * 0.15f;
                b = lum * 0.5f + b * 0.15f;
            }
            // Selection highlight: brighten team color
            if (selected_ids && entity.is_unit() &&
                selected_ids->count(entity.entity_id())) {
                r = r * 0.5f + 0.5f;
                g = g * 0.5f + 0.5f;
                b = b * 0.5f + 0.5f;
            }
            inst.r = r; inst.g = g; inst.b = b; inst.a = a;

            auto& gd = mesh_groups[gpu];
            gd.instances.push_back(inst);

            // Track bone data for this instance (props have no bones)
            if (entity.is_unit()) {
                auto* unit = static_cast<const sim::Unit*>(&entity);
                u32 bc = unit->animated_bone_count();
                if (bc > MAX_BONES_PER_UNIT) bc = MAX_BONES_PER_UNIT;
                gd.bones.push_back({unit, bc});
            } else {
                gd.bones.push_back({nullptr, 0});
            }

            mesh_count++;
        } else {
            if (cube_count >= MAX_INSTANCES) return;
            auto& inst = cube_instances[cube_count];
            inst.x = entity.position().x;
            inst.y = entity.position().y;
            inst.z = entity.position().z;
            inst.scale = 2.0f;
            inst.r = r; inst.g = g; inst.b = b; inst.a = a;
            cube_count++;
        }
    });

    cube_instance_count_ = cube_count;

    // Flatten mesh groups into contiguous instance buffer + bone SSBO
    u32 offset = 0;
    u32 bone_offset = 0; // in mat4 units (each mat4 = 16 floats)
    static constexpr f32 IDENTITY[16] = {
        1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    u32 max_bone_entries = MAX_INSTANCES * MAX_BONES_PER_UNIT;

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

                auto* unit = gd.bones[i].unit;
                u32 bc = 0;
                if (unit) {
                    auto& mats = unit->animated_bone_matrices();
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
    if (!mesh || !mesh_instance_mapped_) return false;

    // Count total instances already used
    u32 total = 0;
    for (auto& g : mesh_groups_) total += g.instance_count;
    if (total >= MAX_INSTANCES) return false;

    auto* instances = static_cast<MeshInstance*>(mesh_instance_mapped_);
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
    safe_destroy(cube_instance_buf_);
    safe_destroy(mesh_instance_buf_);
    safe_destroy(bone_ssbo_);

    cube_instance_mapped_ = nullptr;
    mesh_instance_mapped_ = nullptr;
    bone_ssbo_mapped_ = nullptr;
    cube_instance_count_ = 0;
    mesh_groups_.clear();
}

} // namespace osc::renderer
