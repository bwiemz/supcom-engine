#include "renderer/shadow_casters.hpp"

#include "renderer/mesh_cache.hpp"
#include "renderer/pipeline_builder.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/terrain_mesh.hpp"
#include "renderer/unit_renderer.hpp"
#include "renderer/vk_cmd.hpp"
#include "sim/scm_parser.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace osc::renderer {

void ShadowCasters::create(VkDevice device, VkRenderPass map_pass,
                           VkDescriptorSetLayout bone_layout,
                           VkDescriptorSetLayout texture_layout) {
    device_ = device;
    // Compile shadow shaders
    auto sv = compile_glsl(device_, shaders::shadow_vert, "shadow.vert", true);
    auto smv = compile_glsl(device_, shaders::shadow_mesh_vert, "shadow_mesh.vert", true);
    auto suv = compile_glsl(device_, shaders::shadow_unit_vert, "shadow_unit.vert", true);
    auto stf = compile_glsl(device_, shaders::shadow_terrain_frag, "shadow_terrain.frag", false);
    auto scf = compile_glsl(device_, shaders::shadow_caster_frag, "shadow_caster.frag", false);
    auto smf = compile_glsl(device_, shaders::shadow_mesh_frag, "shadow_mesh.frag", false);
    const std::array<VkShaderModule, 6> modules = {sv, smv, suv, stf, scf, smf};
    const auto destroy_modules = [&] {
        for (VkShaderModule m : modules)
            if (m) vkDestroyShaderModule(device_, m, nullptr);
    };
    if (std::find(modules.begin(), modules.end(), VK_NULL_HANDLE) != modules.end()) {
        spdlog::error("Shadow shader compilation failed");
        destroy_modules();
        return;
    }
    // Moho's map (M210c) takes R and G; nothing writes B or A.
    constexpr VkColorComponentFlags kRG = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;

    // --- Shadow terrain pipeline (depth-only, same vertex layout as terrain) ---
    {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(TerrainVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 2> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};               // position
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3}; // normal

        // TTerrainDepth's state: no culling, and no depth bias (M210c)
        terrain_pipeline_ =
            PipelineBuilder()
                .set_shaders(sv, stf)
                .set_vertex_input(&binding, 1, attrs.data(), static_cast<u32>(attrs.size()))
                .set_depth_test(true, true)
                .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_push_constant(sizeof(f32) * 16, VK_SHADER_STAGE_VERTEX_BIT)
                .set_color_write_mask(kRG)
                .build(device_, map_pass, &terrain_layout_);
    }

    // --- Shadow mesh pipeline (depth-only, blend-weight skinning) ---
    {
        std::array<VkVertexInputBindingDescription, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].stride = static_cast<u32>(sizeof(sim::SCMMesh::Vertex));
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindings[1].binding = 1;
        bindings[1].stride = sizeof(MeshInstance);
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::array<VkVertexInputAttributeDescription, 12> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3};
        attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(f32) * 6};
        attrs[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + 0};
        attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                    offsetof(MeshInstance, model) + sizeof(f32) * 4};
        attrs[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                    offsetof(MeshInstance, model) + sizeof(f32) * 8};
        attrs[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                    offsetof(MeshInstance, model) + sizeof(f32) * 12};
        attrs[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, r)};
        attrs[8] = {8, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(sim::SCMMesh::Vertex, bone_indices)};
        attrs[9] = {9, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                    offsetof(sim::SCMMesh::Vertex, bone_weights)};
        attrs[10] = {10, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(sim::SCMMesh::Vertex, tx)};
        attrs[11] = {14, 1, VK_FORMAT_R32_SFLOAT, offsetof(MeshInstance, parameter)};

        // Push constant 84B: mat4 lightVP (64) + uint boneBase (4) + uint bonesPerInst (4) +
        // uint technique (4, M211f) + float time (4, M211j) + uint lane (4, M211n)
        mesh_pipeline_ =
            PipelineBuilder()
                .set_shaders(smv, smf)
                .set_vertex_input(bindings.data(), static_cast<u32>(bindings.size()), attrs.data(),
                                  static_cast<u32>(attrs.size()))
                .set_depth_test(true, true)
                .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                .set_push_constant(sizeof(f32) * 16 + sizeof(u32) * 4 + sizeof(f32),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
                .set_descriptor_set_layout(bone_layout)    // set=0: bone SSBO
                .add_descriptor_set_layout(texture_layout) // set=1: albedo (M211j)
                .set_color_write_mask(kRG)
                .build(device_, map_pass, &mesh_layout_);
    }

    // --- Shadow unit cube pipeline (depth-only, instanced) ---
    {
        std::array<VkVertexInputBindingDescription, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].stride = sizeof(f32) * 6;
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindings[1].binding = 1;
        bindings[1].stride = sizeof(CubeInstance);
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::array<VkVertexInputAttributeDescription, 5> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3};
        attrs[2] = {2, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeInstance, x)};
        attrs[3] = {3, 1, VK_FORMAT_R32_SFLOAT, offsetof(CubeInstance, scale)};
        attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, r)};

        cube_pipeline_ = PipelineBuilder()
                             .set_shaders(suv, scf)
                             .set_vertex_input(bindings.data(), static_cast<u32>(bindings.size()),
                                               attrs.data(), static_cast<u32>(attrs.size()))
                             .set_depth_test(true, true)
                             .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                             .set_push_constant(sizeof(f32) * 16, VK_SHADER_STAGE_VERTEX_BIT)
                             .set_color_write_mask(kRG)
                             .build(device_, map_pass, &cube_layout_);
    }

    destroy_modules();

    spdlog::info("Shadow pipelines created (terrain + mesh + unit)");
}

void ShadowCasters::destroy() {
    if (!device_) return;
    vkDestroyPipeline(device_, terrain_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, terrain_layout_, nullptr);
    vkDestroyPipeline(device_, mesh_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, mesh_layout_, nullptr);
    vkDestroyPipeline(device_, cube_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, cube_layout_, nullptr);
    *this = ShadowCasters{};
}

void ShadowCasters::record(VkCommandBuffer cmd, const std::array<f32, 16>& light_vp,
                           const Frame& frame) const {
    // Shadow terrain
    if (frame.terrain && frame.terrain->index_count() > 0 && terrain_pipeline_) {
        vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, terrain_pipeline_);
        vkc::push_constants(cmd, terrain_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32) * 16,
                            light_vp.data());

        VkBuffer vbufs[] = {frame.terrain->vertex_buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vbufs, offsets);
        vkCmdBindIndexBuffer(cmd, frame.terrain->index_buffer(), 0, VK_INDEX_TYPE_UINT32);
        vkc::draw_indexed(cmd, frame.terrain->index_count(), 1, 0, 0, 0);
    }
    if (!frame.units || frame.strategic) return;
    const UnitRenderer& units = *frame.units;

    // Shadow meshes (skip when strategic zoom replaces 3D units with icons)
    if (!units.mesh_groups().empty() && mesh_pipeline_ && frame.bones) {
        vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_);

        // Bind bone SSBO at set=0
        vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 0, 1,
                                  &frame.bones, 0, nullptr);

        struct ShadowMeshPC {
            f32 lightVP[16];
            u32 boneBase;
            u32 bonesPerInst;
            u32 technique; // MeshTechnique (M211f)
            f32 time;      // FA's time, for the swaying trees (M211j)
            u32 lane;      // the lane by graphics fidelity: at Low wrecks cast none (M211n)
        } spc{};
        static_assert(sizeof(ShadowMeshPC) == 84, "matches shadow_mesh_vert/frag's push block");
        std::memcpy(spc.lightVP, light_vp.data(), sizeof(f32) * 16);
        spc.time = units.shader_time();
        spc.lane = frame.lane;

        for (const auto& group : units.mesh_groups()) {
            if (!group.mesh || group.instance_count == 0) continue;
            // Only a technique with a depth stage casts a shadow.
            if (!has_depth_stage(drawn_technique(group))) continue;

            spc.boneBase = group.bone_base_offset;
            spc.bonesPerInst = group.bones_per_instance;
            spc.technique = static_cast<u32>(base_technique(drawn_technique(group)));
            vkc::push_constants(cmd, mesh_layout_,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                sizeof(spc), &spc);
            // The albedo, whose alpha cuts an alpha-tested mesh's shadow
            // (DepthClip, M211j).
            VkDescriptorSet albedo = group.texture_ds ? group.texture_ds : frame.albedo_fallback;
            if (albedo) {
                vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 1, 1,
                                          &albedo, 0, nullptr);
            }

            VkBuffer vbufs[] = {group.mesh->vertex_buf.buffer, units.mesh_instance_buffer()};
            VkDeviceSize buf_offsets[] = {0, static_cast<VkDeviceSize>(group.instance_offset) *
                                                 sizeof(MeshInstance)};
            vkCmdBindVertexBuffers(cmd, 0, 2, vbufs, buf_offsets);
            vkCmdBindIndexBuffer(cmd, group.mesh->index_buf.buffer, 0, VK_INDEX_TYPE_UINT32);
            vkc::draw_indexed(cmd, group.mesh->index_count, group.instance_count, 0, 0, 0);
        }
    }

    // Shadow cubes (skip when strategic zoom active)
    if (units.cube_instance_count() > 0 && cube_pipeline_) {
        vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cube_pipeline_);
        vkc::push_constants(cmd, cube_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32) * 16,
                            light_vp.data());

        VkBuffer vbufs[] = {units.cube_vertex_buffer(), units.cube_instance_buffer()};
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, vbufs, offsets);
        vkCmdBindIndexBuffer(cmd, units.cube_index_buffer(), 0, VK_INDEX_TYPE_UINT32);
        vkc::draw_indexed(cmd, units.cube_index_count(), units.cube_instance_count(), 0, 0, 0);
    }
}

} // namespace osc::renderer
