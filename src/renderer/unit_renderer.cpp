#include "renderer/unit_renderer.hpp"
#include "renderer/vk_types.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <spdlog/spdlog.h>

#include <array>

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

void UnitRenderer::build(VkDevice device, VmaAllocator allocator,
                         VkCommandPool cmd_pool, VkQueue queue) {
    // Unit cube: centered at origin, 1x1x1
    // 24 vertices (4 per face for correct normals)
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

    // Host-visible instance buffer (persistently mapped)
    VkBufferCreateInfo inst_ci{};
    inst_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    inst_ci.size = MAX_INSTANCES * sizeof(UnitInstance);
    inst_ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo inst_alloc_ci{};
    inst_alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    inst_alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                          VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    VmaAllocationInfo alloc_info{};
    vmaCreateBuffer(allocator, &inst_ci, &inst_alloc_ci,
                    &instance_buf_.buffer, &instance_buf_.allocation,
                    &alloc_info);
    instance_mapped_ = alloc_info.pMappedData;
}

void UnitRenderer::update(const sim::SimState& sim) {
    if (!instance_mapped_) return;
    auto* instances = static_cast<UnitInstance*>(instance_mapped_);
    u32 count = 0;

    sim.entity_registry().for_each([&](const sim::Entity& entity) {
        if (!entity.is_unit() || entity.destroyed())
            return;
        if (count >= MAX_INSTANCES)
            return;

        auto& inst = instances[count];
        inst.x = entity.position().x;
        inst.y = entity.position().y;
        inst.z = entity.position().z;
        inst.scale = 2.0f; // default cube scale

        // Army color
        i32 army = entity.army();
        if (army >= 0 && army < static_cast<i32>(sim.army_count())) {
            auto* brain = sim.army_at(static_cast<size_t>(army));
            if (brain && (brain->color_r() || brain->color_g() ||
                          brain->color_b())) {
                inst.r = brain->color_r() / 255.0f;
                inst.g = brain->color_g() / 255.0f;
                inst.b = brain->color_b() / 255.0f;
            } else if (army < 8) {
                inst.r = ARMY_COLORS[army][0];
                inst.g = ARMY_COLORS[army][1];
                inst.b = ARMY_COLORS[army][2];
            } else {
                inst.r = inst.g = inst.b = 0.7f;
            }
        } else {
            inst.r = inst.g = inst.b = 0.5f;
        }

        // Semi-transparent for units being built
        inst.a = (entity.fraction_complete() < 1.0f) ? 0.4f : 1.0f;

        count++;
    });

    instance_count_ = count;
}

void UnitRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    if (cube_verts_.buffer)
        vmaDestroyBuffer(allocator, cube_verts_.buffer, cube_verts_.allocation);
    if (cube_indices_.buffer)
        vmaDestroyBuffer(allocator, cube_indices_.buffer,
                         cube_indices_.allocation);
    if (instance_buf_.buffer)
        vmaDestroyBuffer(allocator, instance_buf_.buffer,
                         instance_buf_.allocation);
    cube_verts_ = {};
    cube_indices_ = {};
    instance_buf_ = {};
    instance_mapped_ = nullptr;
    instance_count_ = 0;
}

} // namespace osc::renderer
