#include "renderer/bloom_renderer.hpp"

#include "renderer/pipeline_builder.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/vk_cmd.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>

/// Log a Vulkan/VMA error with file and line context.
#define VK_CHECK(call)                                                                             \
    do {                                                                                           \
        VkResult vk_check_res_ = (call);                                                           \
        if (vk_check_res_ != VK_SUCCESS)                                                           \
            spdlog::error("Vulkan error {} at {}:{}: {}", static_cast<int>(vk_check_res_),         \
                          __FILE__, __LINE__, #call);                                              \
    } while (0)

namespace osc::renderer {

void BloomRenderer::create(VkDevice device, VmaAllocator allocator, u32 width, u32 height,
                           VkImageView scene_view, VkDescriptorSetLayout texture_layout,
                           VkSampler sampler) {
    device_ = device;
    allocator_ = allocator;
    half_w_ = std::max(width / 2, 1u);
    half_h_ = std::max(height / 2, 1u);
    const VkFormat hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;

    // The targets, HDR as the frame is, half its size.
    const auto create_hdr_image = [&](AllocatedImage& img) {
        VkImageCreateInfo img_ci{};
        img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_ci.imageType = VK_IMAGE_TYPE_2D;
        img_ci.format = hdr_format;
        img_ci.extent = {half_w_, half_h_, 1};
        img_ci.mipLevels = 1;
        img_ci.arrayLayers = 1;
        img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        VK_CHECK(
            vmaCreateImage(allocator_, &img_ci, &alloc_ci, &img.image, &img.allocation, nullptr));

        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = img.image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = hdr_format;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &view_ci, nullptr, &img.view));
    };
    create_hdr_image(bright_image_);
    create_hdr_image(blur_h_image_);
    create_hdr_image(blur_v_image_);

    // Its pass: one colour target, no depth.
    {
        VkAttachmentDescription att{};
        att.format = hdr_format;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;

        std::array<VkSubpassDependency, 2> deps{};
        // Incoming: previous pass output visible before we start writing, and
        // earlier samplings of this image done first: the blur ping-pongs
        // twice over the same two images (M211e), writing what the pass
        // before last read.
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask =
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // Outgoing: finalLayout transition visible to subsequent fragment reads
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rp_ci{};
        rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp_ci.attachmentCount = 1;
        rp_ci.pAttachments = &att;
        rp_ci.subpassCount = 1;
        rp_ci.pSubpasses = &subpass;
        rp_ci.dependencyCount = static_cast<u32>(deps.size());
        rp_ci.pDependencies = deps.data();
        VK_CHECK(vkCreateRenderPass(device_, &rp_ci, nullptr, &pass_));
    }

    // Its framebuffers (half size).
    const auto create_fb = [&](VkFramebuffer& fb, VkImageView view) {
        VkFramebufferCreateInfo fb_ci{};
        fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = pass_;
        fb_ci.attachmentCount = 1;
        fb_ci.pAttachments = &view;
        fb_ci.width = half_w_;
        fb_ci.height = half_h_;
        fb_ci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &fb_ci, nullptr, &fb));
    };
    create_fb(bright_fb_, bright_image_.view);
    create_fb(blur_h_fb_, blur_h_image_.view);
    create_fb(blur_v_fb_, blur_v_image_.view);

    // The sets the passes read: the scene, and each target.
    {
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.maxSets = 4;
        pool_ci.poolSizeCount = 1;
        pool_ci.pPoolSizes = &pool_size;
        VK_CHECK(vkCreateDescriptorPool(device_, &pool_ci, nullptr, &ds_pool_));

        const std::array<VkDescriptorSetLayout, 4> layouts = {texture_layout, texture_layout,
                                                              texture_layout, texture_layout};
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = ds_pool_;
        alloc.descriptorSetCount = static_cast<u32>(layouts.size());
        alloc.pSetLayouts = layouts.data();
        std::array<VkDescriptorSet, 4> sets{};
        VK_CHECK(vkAllocateDescriptorSets(device_, &alloc, sets.data()));
        scene_ds_ = sets[0];
        bright_ds_ = sets[1];
        blur_h_ds_ = sets[2];
        blur_v_ds_ = sets[3];

        const auto write_ds = [&](VkDescriptorSet ds, VkImageView view) {
            VkDescriptorImageInfo img_info{};
            img_info.sampler = sampler;
            img_info.imageView = view;
            img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = ds;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &img_info;
            vkc::update_descriptor_sets(device_, 1, &write, 0, nullptr);
        };
        write_ds(scene_ds_, scene_view);
        write_ds(bright_ds_, bright_image_.view);
        write_ds(blur_h_ds_, blur_h_image_.view);
        write_ds(blur_v_ds_, blur_v_image_.view);
    }
}

void BloomRenderer::create_pipelines(VkRenderPass present_pass,
                                     VkDescriptorSetLayout texture_layout) {
    // Compile bloom shaders from embedded GLSL
    auto bright_v = compile_glsl(device_, shaders::bloom_bright_vert, "bloom_bright.vert", true);
    auto bright_f = compile_glsl(device_, shaders::bloom_bright_frag, "bloom_bright.frag", false);
    auto blur_f = compile_glsl(device_, shaders::bloom_blur_frag, "bloom_blur.frag", false);
    auto comp_f =
        compile_glsl(device_, shaders::bloom_composite_frag, "bloom_composite.frag", false);

    if (!bright_v || !bright_f || !blur_f || !comp_f) {
        spdlog::error("One or more bloom shaders failed to compile");
        for (VkShaderModule m : {bright_v, bright_f, blur_f, comp_f})
            if (m) vkDestroyShaderModule(device_, m, nullptr);
        return;
    }

    // Fullscreen triangle is CW in Vulkan's Y-down coords — disable culling
    // for all post-process passes (standard practice for screen-space effects)

    // Bright pass pipeline (extract bright pixels from scene)
    bright_pipeline_ = PipelineBuilder()
                           .set_shaders(bright_v, bright_f)
                           .set_depth_test(false, false)
                           .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                           .set_push_constant(8, VK_SHADER_STAGE_FRAGMENT_BIT)
                           .set_descriptor_set_layout(texture_layout)
                           .build(device_, pass_, &bright_layout_);

    // Blur pipeline (separable Gaussian, used for both H and V passes)
    blur_pipeline_ = PipelineBuilder()
                         .set_shaders(bright_v, blur_f)
                         .set_depth_test(false, false)
                         .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                         .set_push_constant(12, VK_SHADER_STAGE_FRAGMENT_BIT)
                         .set_descriptor_set_layout(texture_layout)
                         .build(device_, pass_, &blur_layout_);

    // Composite pipeline (blend scene + bloom onto swapchain)
    composite_pipeline_ = PipelineBuilder()
                              .set_shaders(bright_v, comp_f)
                              .set_depth_test(false, false)
                              .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                              .set_push_constant(4, VK_SHADER_STAGE_FRAGMENT_BIT)
                              .set_descriptor_set_layout(texture_layout) // set 0: scene
                              .add_descriptor_set_layout(texture_layout) // set 1: bloom
                              .build(device_, present_pass, &composite_layout_);

    for (VkShaderModule m : {bright_v, bright_f, blur_f, comp_f})
        vkDestroyShaderModule(device_, m, nullptr);

    spdlog::info("Bloom pipelines created — bright={} blur={} composite={}",
                 static_cast<void*>(bright_pipeline_), static_cast<void*>(blur_pipeline_),
                 static_cast<void*>(composite_pipeline_));
}

void BloomRenderer::destroy() {
    if (!device_) return;
    // Descriptor pool (frees all allocated sets)
    if (ds_pool_) vkDestroyDescriptorPool(device_, ds_pool_, nullptr);
    // Pipelines (may be VK_NULL_HANDLE if not yet created -- safe to destroy)
    if (bright_pipeline_) vkDestroyPipeline(device_, bright_pipeline_, nullptr);
    if (bright_layout_) vkDestroyPipelineLayout(device_, bright_layout_, nullptr);
    if (blur_pipeline_) vkDestroyPipeline(device_, blur_pipeline_, nullptr);
    if (blur_layout_) vkDestroyPipelineLayout(device_, blur_layout_, nullptr);
    if (composite_pipeline_) vkDestroyPipeline(device_, composite_pipeline_, nullptr);
    if (composite_layout_) vkDestroyPipelineLayout(device_, composite_layout_, nullptr);
    if (bright_fb_) vkDestroyFramebuffer(device_, bright_fb_, nullptr);
    if (blur_h_fb_) vkDestroyFramebuffer(device_, blur_h_fb_, nullptr);
    if (blur_v_fb_) vkDestroyFramebuffer(device_, blur_v_fb_, nullptr);
    if (pass_) vkDestroyRenderPass(device_, pass_, nullptr);
    for (AllocatedImage* img : {&bright_image_, &blur_h_image_, &blur_v_image_}) {
        if (img->view) vkDestroyImageView(device_, img->view, nullptr);
        if (img->image) vmaDestroyImage(allocator_, img->image, img->allocation);
    }
    *this = BloomRenderer{};
}

void BloomRenderer::record(VkCommandBuffer cmd, f32 copy_scale, f32 add, f32 kernel_scale,
                           int blur_count) const {
    VkViewport vp{};
    vp.width = static_cast<f32>(half_w_);
    vp.height = static_cast<f32>(half_h_);
    vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.extent = {half_w_, half_h_};

    // FA's CBloomRenderer::DoBloom: the glow copied out of the frame
    // (half size), blurred twice over, then added back (M211e).
    const auto pass = [&](VkFramebuffer fb, VkPipeline pipeline, VkPipelineLayout layout,
                          const void* pc, u32 pc_size, VkDescriptorSet input) {
        VkRenderPassBeginInfo rp{};
        rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp.renderPass = pass_;
        rp.framebuffer = fb;
        rp.renderArea.extent = {half_w_, half_h_};
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkc::push_constants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pc_size, pc);
        vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &input, 0,
                                  nullptr);
        vkc::draw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
    };
    const struct {
        f32 scale, add;
    } copy_pc = {copy_scale, add};
    pass(bright_fb_, bright_pipeline_, bright_layout_, &copy_pc, sizeof(copy_pc), scene_ds_);
    const struct {
        f32 dx, dy, scale;
    } blur_h_pc = {1.0f / static_cast<f32>(half_w_), 0.0f, kernel_scale},
      blur_v_pc = {0.0f, 1.0f / static_cast<f32>(half_h_), kernel_scale};
    for (int i = 0; i < blur_count; ++i) {
        pass(blur_h_fb_, blur_pipeline_, blur_layout_, &blur_h_pc, sizeof(blur_h_pc),
             i == 0 ? bright_ds_ : blur_v_ds_);
        pass(blur_v_fb_, blur_pipeline_, blur_layout_, &blur_v_pc, sizeof(blur_v_pc), blur_h_ds_);
    }
}

void BloomRenderer::composite(VkCommandBuffer cmd, bool with_bloom) const {
    if (!composite_pipeline_) return;
    const f32 strength = with_bloom ? 1.0f : 0.0f;
    vkc::bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_pipeline_);
    vkc::push_constants(cmd, composite_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(strength),
                        &strength);
    vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_layout_, 0, 1,
                              &scene_ds_, 0, nullptr);
    // Without bloom its input adds nothing (strength 0), but must still be
    // an image in a defined layout. The bloom images are written only by
    // bloom frames, and until the first one they are UNDEFINED (a NaN
    // there would survive the 0), so the scene stands in.
    VkDescriptorSet bloom_input = with_bloom ? blur_v_ds_ : scene_ds_;
    vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, composite_layout_, 1, 1,
                              &bloom_input, 0, nullptr);
    vkc::draw(cmd, 3, 1, 0, 0);
}

} // namespace osc::renderer
