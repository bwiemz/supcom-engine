#include "renderer/frame_targets.hpp"

#include <spdlog/spdlog.h>

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

void FrameTargets::create(VkDevice device, VmaAllocator allocator, u32 width, u32 height,
                          VkFormat depth_format, VkImageView depth_view) {
    device_ = device;
    allocator_ = allocator;
    const u32 w = width;
    const u32 h = height;
    VkFormat hdr_format = VK_FORMAT_R16G16B16A16_SFLOAT;

    // Helper to create an HDR image + view
    auto create_hdr_image = [&](AllocatedImage& img, u32 iw, u32 ih) {
        VkImageCreateInfo img_ci{};
        img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_ci.imageType = VK_IMAGE_TYPE_2D;
        img_ci.format = hdr_format;
        img_ci.extent = {iw, ih, 1};
        img_ci.mipLevels = 1;
        img_ci.arrayLayers = 1;
        img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Copyable both ways: the water refracts a copy of the frame (M213a).
        img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        VK_CHECK(vmaCreateImage(allocator_, &img_ci, &alloc_ci,
                       &img.image, &img.allocation, nullptr));

        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = img.image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = hdr_format;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(device_, &view_ci, nullptr, &img.view));
    };

    // Scene color uses HDR format — all scene pipelines are built against
    // scene_render_pass_ so format compatibility is guaranteed. HDR allows
    // overbright values for proper bloom extraction.
    create_hdr_image(scene_color_image_, w, h);
    // The frame as it is before the water, which the water refracts (M213a).
    create_hdr_image(refraction_image_, w, h);
    // The units reflected in the water (M213b).
    create_hdr_image(reflection_image_, w, h);
    // The normal target (M212e): the normal pass's, which the scene reads.
    create_hdr_image(terrain_normal_image_, w, h);

    // Scene render pass (color + depth, HDR format for overbright bloom extraction)
    {
        VkAttachmentDescription color_att{};
        color_att.format = hdr_format;
        color_att.samples = VK_SAMPLE_COUNT_1_BIT;
        color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color_att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // The stencil is cleared with the depth: the range overlays count
        // in it, and leave it clear (before the water, in the first pass)
        VkAttachmentDescription depth_att{};
        depth_att.format = depth_format;
        depth_att.samples = VK_SAMPLE_COUNT_1_BIT;
        depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth_att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;
        subpass.pDepthStencilAttachment = &depth_ref;

        std::array<VkAttachmentDescription, 2> attachments = {color_att, depth_att};

        std::array<VkSubpassDependency, 2> deps{};
        // Incoming: external writes complete before we start
        // The depth image is shared by every frame in flight, so the previous
        // frame's depth writes (early AND late tests) must finish before this
        // frame clears and writes it.
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                               VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        deps[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        // Outgoing: finalLayout transition visible to subsequent fragment reads
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

        VK_CHECK(vkCreateRenderPass(device_, &rp_ci, nullptr, &scene_render_pass_));

        // On a map with water the scene is drawn in two passes around it
        // (M213a): the first keeps its colour and depth as attachments; the
        // second goes on from where it was. Only their loads, stores and
        // layouts differ (their dependencies must not, for them to stay
        // compatible), so the scene's pipelines draw in both; the copy
        // between them has barriers of its own (copy_refraction).
        {
            std::array<VkAttachmentDescription, 2> first = attachments;
            first[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            first[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            rp_ci.pAttachments = first.data();
            VK_CHECK(vkCreateRenderPass(device_, &rp_ci, nullptr, &scene_first_pass_));

            std::array<VkAttachmentDescription, 2> second = attachments;
            second[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            second[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            second[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            second[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            second[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            rp_ci.pAttachments = second.data();
            VK_CHECK(vkCreateRenderPass(device_, &rp_ci, nullptr, &scene_second_pass_));

            // The middle one (M214d): goes on, and ends as the first.
            std::array<VkAttachmentDescription, 2> middle = second;
            middle[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            middle[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            rp_ci.pAttachments = middle.data();
            VK_CHECK(vkCreateRenderPass(device_, &rp_ci, nullptr, &scene_middle_pass_));
        }
    }

    // Scene framebuffer (full resolution, scene_render_pass_)
    {
        std::array<VkImageView, 2> views = {scene_color_image_.view, depth_view};
        VkFramebufferCreateInfo fb_ci{};
        fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = scene_render_pass_;
        fb_ci.attachmentCount = static_cast<u32>(views.size());
        fb_ci.pAttachments = views.data();
        fb_ci.width = w;
        fb_ci.height = h;
        fb_ci.layers = 1;
        VK_CHECK(vkCreateFramebuffer(device_, &fb_ci, nullptr, &scene_framebuffer_));

        // The reflection's (M213b): drawn with the scene's pass, whose
        // pipelines draw it, on the scene's depth, which the scene clears
        // again after.
        views[0] = reflection_image_.view;
        VK_CHECK(vkCreateFramebuffer(device_, &fb_ci, nullptr, &reflection_framebuffer_));
        // The normal pass's (M212e), likewise on the scene's depth.
        views[0] = terrain_normal_image_.view;
        VK_CHECK(vkCreateFramebuffer(device_, &fb_ci, nullptr, &terrain_normal_framebuffer_));
    }
}

void FrameTargets::destroy() {
    if (!device_) return;
    // Framebuffers
    if (scene_framebuffer_) vkDestroyFramebuffer(device_, scene_framebuffer_, nullptr);
    if (reflection_framebuffer_) vkDestroyFramebuffer(device_, reflection_framebuffer_, nullptr);
    reflection_framebuffer_ = VK_NULL_HANDLE;
    if (terrain_normal_framebuffer_)
        vkDestroyFramebuffer(device_, terrain_normal_framebuffer_, nullptr);
    terrain_normal_framebuffer_ = VK_NULL_HANDLE;

    // Render passes
    if (scene_render_pass_) vkDestroyRenderPass(device_, scene_render_pass_, nullptr);
    if (scene_first_pass_) vkDestroyRenderPass(device_, scene_first_pass_, nullptr);
    if (scene_second_pass_) vkDestroyRenderPass(device_, scene_second_pass_, nullptr);
    if (scene_middle_pass_) vkDestroyRenderPass(device_, scene_middle_pass_, nullptr);
    scene_first_pass_ = VK_NULL_HANDLE;
    scene_second_pass_ = VK_NULL_HANDLE;
    scene_middle_pass_ = VK_NULL_HANDLE;

    // Images
    auto destroy_img = [&](AllocatedImage& img) {
        if (img.view) vkDestroyImageView(device_, img.view, nullptr);
        if (img.image) vmaDestroyImage(allocator_, img.image, img.allocation);
        img = {};
    };
    destroy_img(scene_color_image_);
    destroy_img(refraction_image_);
    destroy_img(reflection_image_);
    destroy_img(terrain_normal_image_);

    // Reset handles
    scene_framebuffer_ = VK_NULL_HANDLE;
    scene_render_pass_ = VK_NULL_HANDLE;
    *this = FrameTargets{};
}

} // namespace osc::renderer
