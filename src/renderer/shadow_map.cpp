#include "renderer/shadow_map.hpp"

#include "renderer/pipeline_builder.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/vk_cmd.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <initializer_list>
#include <vector>

/// Log a Vulkan/VMA error with file and line context.
#define VK_CHECK(call)                                                                             \
    do {                                                                                           \
        VkResult vk_check_res_ = (call);                                                           \
        if (vk_check_res_ != VK_SUCCESS)                                                           \
            spdlog::error("Vulkan error {} at {}:{}: {}", static_cast<int>(vk_check_res_),         \
                          __FILE__, __LINE__, #call);                                              \
    } while (0)

namespace osc::renderer {

void ShadowMap::create(VkDevice device, VmaAllocator allocator, u32 size,
                       VkDescriptorSetLayout texture_layout) {
    device_ = device;
    allocator_ = allocator;
    size_ = size;
    const auto make_image = [&](AllocatedImage& img, VkFormat format, VkImageUsageFlags usage,
                                VkImageAspectFlags aspect) {
        VkImageCreateInfo img_ci{};
        img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_ci.imageType = VK_IMAGE_TYPE_2D;
        img_ci.format = format;
        img_ci.extent = {size_, size_, 1};
        img_ci.mipLevels = 1;
        img_ci.arrayLayers = 1;
        img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage = usage;
        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        VK_CHECK(
            vmaCreateImage(allocator_, &img_ci, &alloc_ci, &img.image, &img.allocation, nullptr));
        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = img.image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = format;
        view_ci.subresourceRange.aspectMask = aspect;
        view_ci.subresourceRange.levelCount = 1;
        view_ci.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(device_, &view_ci, nullptr, &img.view));
    };
    const VkImageUsageFlags colour_usage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    make_image(map_, kFormat, colour_usage, VK_IMAGE_ASPECT_COLOR_BIT);
    make_image(blur_a_, kFormat, colour_usage, VK_IMAGE_ASPECT_COLOR_BIT);
    make_image(blur_b_, kFormat, colour_usage, VK_IMAGE_ASPECT_COLOR_BIT);
    make_image(depth_, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT);

    // The map's pass: colour and depth, both cleared (to 1: lit, and far).
    // The map is shared by every frame in flight:
    //   [0] incoming: the last frame's reads (and depth writes) finish
    //       before this frame overwrites it;
    //   [1] outgoing: its writes are visible to the blur and the scene.
    {
        VkAttachmentDescription colour_att{};
        colour_att.format = kFormat;
        colour_att.samples = VK_SAMPLE_COUNT_1_BIT;
        colour_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colour_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colour_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colour_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colour_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colour_att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentDescription depth_att = colour_att;
        depth_att.format = VK_FORMAT_D32_SFLOAT;
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        const std::array<VkAttachmentDescription, 2> attachments = {colour_att, depth_att};

        VkAttachmentReference colour_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colour_ref;
        subpass.pDepthStencilAttachment = &depth_ref;

        std::array<VkSubpassDependency, 2> deps{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask =
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rp_ci{};
        rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp_ci.attachmentCount = static_cast<u32>(attachments.size());
        rp_ci.pAttachments = attachments.data();
        rp_ci.subpassCount = 1;
        rp_ci.pSubpasses = &subpass;
        rp_ci.dependencyCount = static_cast<u32>(deps.size());
        rp_ci.pDependencies = deps.data();
        VK_CHECK(vkCreateRenderPass(device_, &rp_ci, nullptr, &map_pass_));
    }

    // The blur's passes: colour alone, cleared to 1. Each reads the target
    // the pass before wrote (that pass's outgoing dependency), and writes
    // one the last frame's scene may still be reading.
    {
        VkAttachmentDescription colour_att{};
        colour_att.format = kFormat;
        colour_att.samples = VK_SAMPLE_COUNT_1_BIT;
        colour_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colour_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colour_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colour_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colour_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colour_att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkAttachmentReference colour_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colour_ref;

        std::array<VkSubpassDependency, 2> deps{};
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = 0; // WAR: an execution dependency suffices
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rp_ci{};
        rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp_ci.attachmentCount = 1;
        rp_ci.pAttachments = &colour_att;
        rp_ci.subpassCount = 1;
        rp_ci.pSubpasses = &subpass;
        rp_ci.dependencyCount = static_cast<u32>(deps.size());
        rp_ci.pDependencies = deps.data();
        VK_CHECK(vkCreateRenderPass(device_, &rp_ci, nullptr, &blur_pass_));
    }

    // Framebuffers: the map with its depth; the blur's targets.
    {
        const auto make_fb = [&](VkFramebuffer& fb, VkRenderPass pass,
                                 std::initializer_list<VkImageView> views) {
            const std::vector<VkImageView> list(views);
            VkFramebufferCreateInfo fb_ci{};
            fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fb_ci.renderPass = pass;
            fb_ci.attachmentCount = static_cast<u32>(list.size());
            fb_ci.pAttachments = list.data();
            fb_ci.width = size_;
            fb_ci.height = size_;
            fb_ci.layers = 1;
            VK_CHECK(vkCreateFramebuffer(device_, &fb_ci, nullptr, &fb));
        };
        make_fb(map_fb_, map_pass_, {map_.view, depth_.view});
        make_fb(blur_a_fb_, blur_pass_, {blur_a_.view});
        make_fb(blur_b_fb_, blur_pass_, {blur_b_.view});
    }

    // Samplers: point and clamped (the meshes' one tap, FA's shadowSampler,
    // and the blur's point taps); bilinear with a white border (the PCF's
    // taps, FA's shadowPCFSampler, and the terrain's mask, ShadowSampler).
    {
        VkSamplerCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter = VK_FILTER_NEAREST;
        ci.minFilter = VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.maxLod = 0.0f;
        VK_CHECK(vkCreateSampler(device_, &ci, nullptr, &point_sampler_));
        ci.magFilter = VK_FILTER_LINEAR;
        ci.minFilter = VK_FILTER_LINEAR;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        VK_CHECK(vkCreateSampler(device_, &ci, nullptr, &linear_sampler_));
    }

    // The blur's sources: the map (point taps), then target A (bilinear).
    {
        const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.maxSets = 2;
        pool_ci.poolSizeCount = 1;
        pool_ci.pPoolSizes = &pool_size;
        VK_CHECK(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &blur_ds_pool_));
        const std::array<VkDescriptorSetLayout, 2> layouts = {texture_layout, texture_layout};
        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = blur_ds_pool_;
        alloc_info.descriptorSetCount = static_cast<u32>(layouts.size());
        alloc_info.pSetLayouts = layouts.data();
        std::array<VkDescriptorSet, 2> sets{};
        VK_CHECK(vkAllocateDescriptorSets(device_, &alloc_info, sets.data()));
        blur_h_ds_ = sets[0];
        blur_v_ds_ = sets[1];
        const VkDescriptorImageInfo map_info{point_sampler_, map_.view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo a_info{linear_sampler_, blur_a_.view,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        std::array<VkWriteDescriptorSet, 2> writes{};
        for (size_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = sets[i];
            writes[i].dstBinding = 0;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        writes[0].pImageInfo = &map_info;
        writes[1].pImageInfo = &a_info;
        vkc::update_descriptor_sets(device_, static_cast<u32>(writes.size()), writes.data(), 0,
                                    nullptr);
    }

    // The blur's passes and the copy: a screen triangle over the target,
    // reading the pass before's from set 0. Moho's map takes R and G;
    // nothing writes B or A.
    {
        VkShaderModule qv =
            compile_glsl(device_, shaders::bloom_bright_vert, "shadow_quad.vert", true);
        VkShaderModule bhf =
            compile_glsl(device_, shaders::shadow_blur_h_frag, "shadow_blur_h.frag", false);
        VkShaderModule bvf =
            compile_glsl(device_, shaders::shadow_blur_v_frag, "shadow_blur_v.frag", false);
        VkShaderModule bcf =
            compile_glsl(device_, shaders::shadow_copy_frag, "shadow_copy.frag", false);
        if (qv && bhf && bvf && bcf) {
            constexpr VkColorComponentFlags kRG =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
            const auto blur = [&](VkShaderModule frag, VkPipelineLayout* layout) {
                return PipelineBuilder()
                    .set_shaders(qv, frag)
                    .set_depth_test(false, false)
                    .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                    .set_color_write_mask(kRG)
                    .set_descriptor_set_layout(texture_layout)
                    .build(device_, blur_pass_, layout);
            };
            blur_h_pipeline_ = blur(bhf, &blur_layout_);
            VkPipelineLayout same = VK_NULL_HANDLE;
            blur_v_pipeline_ = blur(bvf, &same);
            if (same) vkDestroyPipelineLayout(device_, same, nullptr);
            same = VK_NULL_HANDLE;
            copy_pipeline_ = blur(bcf, &same);
            if (same) vkDestroyPipelineLayout(device_, same, nullptr);
        } else {
            spdlog::error("Shadow blur shader compilation failed");
        }
        for (VkShaderModule m : {qv, bhf, bvf, bcf})
            if (m) vkDestroyShaderModule(device_, m, nullptr);
    }

    spdlog::info("Shadow map created ({}x{} map, its depth and the blur's targets)", size_, size_);
}

void ShadowMap::destroy() {
    if (!device_) return;
    vkDestroyPipeline(device_, blur_h_pipeline_, nullptr);
    vkDestroyPipeline(device_, blur_v_pipeline_, nullptr);
    vkDestroyPipeline(device_, copy_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, blur_layout_, nullptr);
    if (blur_ds_pool_) vkDestroyDescriptorPool(device_, blur_ds_pool_, nullptr);
    vkDestroyFramebuffer(device_, map_fb_, nullptr);
    vkDestroyFramebuffer(device_, blur_a_fb_, nullptr);
    vkDestroyFramebuffer(device_, blur_b_fb_, nullptr);
    vkDestroyRenderPass(device_, map_pass_, nullptr);
    vkDestroyRenderPass(device_, blur_pass_, nullptr);
    if (point_sampler_) vkDestroySampler(device_, point_sampler_, nullptr);
    if (linear_sampler_) vkDestroySampler(device_, linear_sampler_, nullptr);
    for (AllocatedImage* img : {&map_, &depth_, &blur_a_, &blur_b_}) {
        if (img->view) vkDestroyImageView(device_, img->view, nullptr);
        if (img->image) vmaDestroyImage(allocator_, img->image, img->allocation);
        *img = {};
    }
    *this = ShadowMap{};
}

void ShadowMap::begin(VkCommandBuffer cmd, VkRenderPass pass, VkFramebuffer fb,
                      u32 clear_count) const {
    std::array<VkClearValue, 2> clears{};
    clears[0].color = {{1.0f, 1.0f, 1.0f, 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    const u32 n = size_;
    const VkViewport inset{1.0f, 1.0f, static_cast<f32>(n - 2), static_cast<f32>(n - 2),
                           0.0f, 1.0f};
    const VkRect2D inset_rect{{1, 1}, {n - 2, n - 2}};
    VkRenderPassBeginInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp.renderPass = pass;
    rp.framebuffer = fb;
    rp.renderArea.extent = {n, n};
    rp.clearValueCount = clear_count;
    rp.pClearValues = clears.data();
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(cmd, 0, 1, &inset);
    vkCmdSetScissor(cmd, 0, 1, &inset_rect);
}

void ShadowMap::begin_map(VkCommandBuffer cmd) const {
    begin(cmd, map_pass_, map_fb_, 2);
}

void ShadowMap::record_mask(VkCommandBuffer cmd, bool blur) const {
    const auto pass = [&](VkFramebuffer fb, VkPipeline pipeline, VkDescriptorSet source) {
        begin(cmd, blur_pass_, fb, 1);
        if (pipeline && source && blur_layout_) {
            vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blur_layout_, 0, 1,
                                      &source, 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);
    };
    if (blur) {
        pass(blur_a_fb_, blur_h_pipeline_, blur_h_ds_);
        pass(blur_b_fb_, blur_v_pipeline_, blur_v_ds_);
    } else {
        pass(blur_b_fb_, copy_pipeline_, blur_h_ds_);
    }
}

} // namespace osc::renderer
