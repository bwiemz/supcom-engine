#define VMA_IMPLEMENTATION
#include "renderer/renderer.hpp"
#include "renderer/pipeline_builder.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/terrain_mesh.hpp"
#include "sim/sim_state.hpp"
#include "map/terrain.hpp"

#include <VkBootstrap.h>
#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <array>

namespace osc::renderer {

// GLFW scroll callback — forward to Renderer via user pointer
static void glfw_scroll_callback(GLFWwindow* window, double /*xoffset*/,
                                 double yoffset) {
    auto* renderer =
        static_cast<Renderer*>(glfwGetWindowUserPointer(window));
    if (renderer) renderer->on_scroll(yoffset);
}

void Renderer::on_scroll(f64 y_offset) {
    f32 zoom_factor = 1.0f - static_cast<f32>(y_offset) * 0.1f;
    camera_.set_distance(
        std::clamp(camera_.distance() * zoom_factor, 30.0f, 2000.0f));
}

bool Renderer::init(u32 width, u32 height, const std::string& title) {
    // GLFW
    if (!glfwInit()) {
        spdlog::error("Failed to initialize GLFW");
        return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    window_ = glfwCreateWindow(static_cast<int>(width),
                               static_cast<int>(height),
                               title.c_str(), nullptr, nullptr);
    if (!window_) {
        spdlog::error("Failed to create GLFW window");
        glfwTerminate();
        return false;
    }
    window_width_ = width;
    window_height_ = height;

    glfwSetWindowUserPointer(window_, this);
    glfwSetScrollCallback(window_, glfw_scroll_callback);

    // Vulkan instance (vk-bootstrap)
    vkb::InstanceBuilder inst_builder;
    auto inst_ret = inst_builder
        .set_app_name("OpenSupCom")
        .request_validation_layers(true)
        .use_default_debug_messenger()
        .require_api_version(1, 0, 0)
        .build();

    if (!inst_ret) {
        spdlog::error("Failed to create Vulkan instance: {}",
                      inst_ret.error().message());
        glfwDestroyWindow(window_);
        glfwTerminate();
        return false;
    }

    auto vkb_inst = inst_ret.value();
    instance_ = vkb_inst.instance;
    debug_messenger_ = vkb_inst.debug_messenger;

    // Surface
    if (glfwCreateWindowSurface(instance_, window_, nullptr, &surface_) !=
        VK_SUCCESS) {
        spdlog::error("Failed to create window surface");
        return false;
    }

    // Physical device
    vkb::PhysicalDeviceSelector selector(vkb_inst);
    auto phys_ret = selector
        .set_surface(surface_)
        .set_minimum_version(1, 0)
        .select();

    if (!phys_ret) {
        spdlog::error("No suitable Vulkan GPU found: {}",
                      phys_ret.error().message());
        return false;
    }

    auto vkb_phys = phys_ret.value();
    physical_device_ = vkb_phys.physical_device;
    spdlog::info("Vulkan GPU: {}", vkb_phys.name);

    // Logical device
    vkb::DeviceBuilder dev_builder(vkb_phys);
    auto dev_ret = dev_builder.build();
    if (!dev_ret) {
        spdlog::error("Failed to create Vulkan device: {}",
                      dev_ret.error().message());
        return false;
    }

    auto vkb_dev = dev_ret.value();
    device_ = vkb_dev.device;

    auto gq = vkb_dev.get_queue(vkb::QueueType::graphics);
    auto gqi = vkb_dev.get_queue_index(vkb::QueueType::graphics);
    if (!gq || !gqi) {
        spdlog::error("Failed to get graphics queue");
        return false;
    }
    graphics_queue_ = gq.value();
    graphics_queue_family_ = gqi.value();

    // VMA allocator
    VmaAllocatorCreateInfo alloc_ci{};
    alloc_ci.physicalDevice = physical_device_;
    alloc_ci.device = device_;
    alloc_ci.instance = instance_;
    if (vmaCreateAllocator(&alloc_ci, &allocator_) != VK_SUCCESS) {
        spdlog::error("Failed to create VMA allocator");
        return false;
    }

    // Swapchain
    if (!create_swapchain(width, height)) return false;

    // Depth image
    create_depth_image();

    // Render pass & framebuffers
    create_render_pass();
    create_framebuffers();

    // Command pool + buffer
    VkCommandPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_ci.queueFamilyIndex = graphics_queue_family_;
    pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(device_, &pool_ci, nullptr, &cmd_pool_);

    VkCommandBufferAllocateInfo cmd_ai{};
    cmd_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_ai.commandPool = cmd_pool_;
    cmd_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(device_, &cmd_ai, &cmd_buf_);

    // Sync objects
    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(device_, &fence_ci, nullptr, &render_fence_);

    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(device_, &sem_ci, nullptr, &present_semaphore_);
    vkCreateSemaphore(device_, &sem_ci, nullptr, &render_semaphore_);

    // Pipelines
    create_pipelines();

    initialized_ = true;
    spdlog::info("Renderer initialized ({}x{})", width, height);
    return true;
}

bool Renderer::create_swapchain(u32 width, u32 height) {
    vkb::SwapchainBuilder builder(physical_device_, device_, surface_);
    auto sc_ret = builder
        .set_desired_format({VK_FORMAT_B8G8R8A8_SRGB,
                             VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_desired_extent(width, height)
        .set_old_swapchain(swapchain_)
        .build();

    if (!sc_ret) {
        spdlog::error("Failed to create swapchain: {}",
                      sc_ret.error().message());
        return false;
    }

    auto vkb_sc = sc_ret.value();
    swapchain_ = vkb_sc.swapchain;
    swapchain_format_ = vkb_sc.image_format;
    swapchain_images_ = vkb_sc.get_images().value();
    swapchain_image_views_ = vkb_sc.get_image_views().value();

    window_width_ = vkb_sc.extent.width;
    window_height_ = vkb_sc.extent.height;

    return true;
}

void Renderer::create_depth_image() {
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = depth_format_;
    img_ci.extent = {window_width_, window_height_, 1};
    img_ci.mipLevels = 1;
    img_ci.arrayLayers = 1;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    VmaAllocationCreateInfo alloc_ci{};
    alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    vmaCreateImage(allocator_, &img_ci, &alloc_ci,
                   &depth_image_.image, &depth_image_.allocation, nullptr);

    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = depth_image_.image;
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = depth_format_;
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view_ci.subresourceRange.levelCount = 1;
    view_ci.subresourceRange.layerCount = 1;
    vkCreateImageView(device_, &view_ci, nullptr, &depth_image_.view);
}

void Renderer::create_render_pass() {
    VkAttachmentDescription color_att{};
    color_att.format = swapchain_format_;
    color_att.samples = VK_SAMPLE_COUNT_1_BIT;
    color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth_att{};
    depth_att.format = depth_format_;
    depth_att.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_att.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_ref{};
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    std::array<VkAttachmentDescription, 2> attachments = {color_att, depth_att};

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_ci{};
    rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = static_cast<u32>(attachments.size());
    rp_ci.pAttachments = attachments.data();
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &subpass;
    rp_ci.dependencyCount = 1;
    rp_ci.pDependencies = &dep;

    vkCreateRenderPass(device_, &rp_ci, nullptr, &render_pass_);
}

void Renderer::create_framebuffers() {
    framebuffers_.resize(swapchain_image_views_.size());
    for (size_t i = 0; i < swapchain_image_views_.size(); i++) {
        std::array<VkImageView, 2> views = {swapchain_image_views_[i],
                                             depth_image_.view};

        VkFramebufferCreateInfo fb_ci{};
        fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = render_pass_;
        fb_ci.attachmentCount = static_cast<u32>(views.size());
        fb_ci.pAttachments = views.data();
        fb_ci.width = window_width_;
        fb_ci.height = window_height_;
        fb_ci.layers = 1;

        vkCreateFramebuffer(device_, &fb_ci, nullptr, &framebuffers_[i]);
    }
}

void Renderer::create_pipelines() {
    // Compile shaders from embedded GLSL
    auto tv = compile_glsl(device_, shaders::terrain_vert, "terrain.vert", true);
    auto tf = compile_glsl(device_, shaders::terrain_frag, "terrain.frag", false);
    auto uv = compile_glsl(device_, shaders::unit_vert, "unit.vert", true);
    auto uf = compile_glsl(device_, shaders::unit_frag, "unit.frag", false);
    auto wv = compile_glsl(device_, shaders::water_vert, "water.vert", true);
    auto wf = compile_glsl(device_, shaders::water_frag, "water.frag", false);

    // Abort if any shader failed to compile
    if (!tv || !tf || !uv || !uf || !wv || !wf) {
        spdlog::error("One or more shaders failed to compile");
        auto safe_destroy = [&](VkShaderModule m) {
            if (m) vkDestroyShaderModule(device_, m, nullptr);
        };
        safe_destroy(tv); safe_destroy(tf); safe_destroy(uv);
        safe_destroy(uf); safe_destroy(wv); safe_destroy(wf);
        return;
    }

    // --- Terrain pipeline ---
    {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(TerrainVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 2> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};                  // position
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3};    // normal

        terrain_pipeline_ = PipelineBuilder()
            .set_shaders(tv, tf)
            .set_vertex_input(&binding, 1, attrs.data(),
                              static_cast<u32>(attrs.size()))
            .set_depth_test(true, true)
            .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_push_constant(sizeof(f32) * 16)
            .build(device_, render_pass_, &terrain_layout_);
    }

    // --- Unit pipeline (instanced) ---
    {
        std::array<VkVertexInputBindingDescription, 2> bindings{};
        // Binding 0: per-vertex cube data
        bindings[0].binding = 0;
        bindings[0].stride = sizeof(f32) * 6; // pos + normal
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        // Binding 1: per-instance data
        bindings[1].binding = 1;
        bindings[1].stride = sizeof(UnitInstance);
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::array<VkVertexInputAttributeDescription, 5> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};                  // position
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3};    // normal
        attrs[2] = {2, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(UnitInstance, x)};   // instancePos
        attrs[3] = {3, 1, VK_FORMAT_R32_SFLOAT,       offsetof(UnitInstance, scale)};// scale
        attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(UnitInstance, r)}; // color

        unit_pipeline_ = PipelineBuilder()
            .set_shaders(uv, uf)
            .set_vertex_input(bindings.data(),
                              static_cast<u32>(bindings.size()),
                              attrs.data(),
                              static_cast<u32>(attrs.size()))
            .set_depth_test(true, true)
            .set_blend(true)
            .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_push_constant(sizeof(f32) * 16)
            .build(device_, render_pass_, &unit_layout_);
    }

    // --- Water pipeline ---
    {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(f32) * 3; // position only
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attr{};
        attr = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};

        water_pipeline_ = PipelineBuilder()
            .set_shaders(wv, wf)
            .set_vertex_input(&binding, 1, &attr, 1)
            .set_depth_test(true, false) // test ON, write OFF
            .set_blend(true)
            .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_push_constant(sizeof(f32) * 16)
            .build(device_, render_pass_, &water_layout_);
    }

    // Destroy shader modules (already compiled into pipelines)
    vkDestroyShaderModule(device_, tv, nullptr);
    vkDestroyShaderModule(device_, tf, nullptr);
    vkDestroyShaderModule(device_, uv, nullptr);
    vkDestroyShaderModule(device_, uf, nullptr);
    vkDestroyShaderModule(device_, wv, nullptr);
    vkDestroyShaderModule(device_, wf, nullptr);
}

void Renderer::build_scene(const sim::SimState& sim) {
    auto* terrain = sim.terrain();
    if (!terrain) {
        spdlog::warn("No terrain loaded — skipping scene build");
        return;
    }

    terrain_mesh_.build(*terrain, device_, allocator_, cmd_pool_,
                        graphics_queue_);

    unit_renderer_.build(device_, allocator_, cmd_pool_, graphics_queue_);

    if (terrain->has_water()) {
        water_renderer_.build(
            static_cast<f32>(terrain->map_width()),
            static_cast<f32>(terrain->map_height()),
            terrain->water_elevation(), device_, allocator_, cmd_pool_,
            graphics_queue_);
    }

    camera_.init(static_cast<f32>(terrain->map_width()),
                 static_cast<f32>(terrain->map_height()));

    spdlog::info("Scene built");
}

void Renderer::render(const sim::SimState& sim) {
    // Wait for ALL previous GPU work (including present) to complete.
    // Using vkDeviceWaitIdle instead of fence-only sync avoids the classic
    // single-buffered semaphore race: fence signals on submit completion,
    // but present may not have consumed render_semaphore_ yet.
    vkDeviceWaitIdle(device_);

    // Acquire swapchain image
    u32 image_index = 0;
    VkResult acq_result = vkAcquireNextImageKHR(
        device_, swapchain_, UINT64_MAX, present_semaphore_,
        VK_NULL_HANDLE, &image_index);

    if (acq_result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return;
    }

    vkResetFences(device_, 1, &render_fence_);

    // Update unit instances
    unit_renderer_.update(sim);

    // View-projection matrix
    f32 aspect = static_cast<f32>(window_width_) /
                 static_cast<f32>(window_height_);
    auto vp = camera_.view_proj(aspect);

    // Record command buffer
    vkResetCommandBuffer(cmd_buf_, 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_buf_, &begin_info);

    // Begin render pass
    std::array<VkClearValue, 2> clear_values{};
    clear_values[0].color = {{0.1f, 0.15f, 0.3f, 1.0f}}; // dark blue sky
    clear_values[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass_;
    rp_begin.framebuffer = framebuffers_[image_index];
    rp_begin.renderArea.extent = {window_width_, window_height_};
    rp_begin.clearValueCount = static_cast<u32>(clear_values.size());
    rp_begin.pClearValues = clear_values.data();
    vkCmdBeginRenderPass(cmd_buf_, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    // Dynamic viewport + scissor
    VkViewport viewport{};
    viewport.width = static_cast<f32>(window_width_);
    viewport.height = static_cast<f32>(window_height_);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd_buf_, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {window_width_, window_height_};
    vkCmdSetScissor(cmd_buf_, 0, 1, &scissor);

    // 1. Draw terrain
    if (terrain_mesh_.index_count() > 0 && terrain_pipeline_) {
        vkCmdBindPipeline(cmd_buf_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          terrain_pipeline_);
        vkCmdPushConstants(cmd_buf_, terrain_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(f32) * 16, vp.data());

        VkBuffer vbufs[] = {terrain_mesh_.vertex_buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd_buf_, 0, 1, vbufs, offsets);
        vkCmdBindIndexBuffer(cmd_buf_, terrain_mesh_.index_buffer(), 0,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd_buf_, terrain_mesh_.index_count(), 1, 0, 0, 0);
    }

    // 2. Draw units
    if (unit_renderer_.instance_count() > 0 && unit_pipeline_) {
        vkCmdBindPipeline(cmd_buf_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          unit_pipeline_);
        vkCmdPushConstants(cmd_buf_, unit_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(f32) * 16, vp.data());

        VkBuffer vbufs[] = {unit_renderer_.vertex_buffer(),
                            unit_renderer_.instance_buffer()};
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(cmd_buf_, 0, 2, vbufs, offsets);
        vkCmdBindIndexBuffer(cmd_buf_, unit_renderer_.index_buffer(), 0,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd_buf_, unit_renderer_.index_count(),
                         unit_renderer_.instance_count(), 0, 0, 0);
    }

    // 3. Draw water (last, blended)
    if (water_renderer_.has_water() && water_pipeline_) {
        vkCmdBindPipeline(cmd_buf_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          water_pipeline_);
        vkCmdPushConstants(cmd_buf_, water_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(f32) * 16, vp.data());

        VkBuffer vbufs[] = {water_renderer_.vertex_buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd_buf_, 0, 1, vbufs, offsets);
        vkCmdBindIndexBuffer(cmd_buf_, water_renderer_.index_buffer(), 0,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd_buf_, water_renderer_.index_count(), 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cmd_buf_);
    vkEndCommandBuffer(cmd_buf_);

    // Submit
    VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &present_semaphore_;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_buf_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_semaphore_;
    vkQueueSubmit(graphics_queue_, 1, &submit, render_fence_);

    // Present
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_semaphore_;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image_index;
    VkResult pres_result = vkQueuePresentKHR(graphics_queue_, &present);

    if (pres_result == VK_ERROR_OUT_OF_DATE_KHR ||
        pres_result == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
    }
}

bool Renderer::should_close() const {
    return window_ && glfwWindowShouldClose(window_);
}

void Renderer::poll_events(f64 dt) {
    if (!window_) return;
    glfwPollEvents();

    // Check for ESC to close
    if (glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window_, GLFW_TRUE);

    camera_.update(window_, dt);
}

void Renderer::recreate_swapchain() {
    // Handle minimize
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    while (w == 0 || h == 0) {
        glfwGetFramebufferSize(window_, &w, &h);
        glfwWaitEvents();
    }

    vkDeviceWaitIdle(device_);

    // Destroy old framebuffers and depth image
    for (auto fb : framebuffers_)
        vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();

    vkDestroyImageView(device_, depth_image_.view, nullptr);
    vmaDestroyImage(allocator_, depth_image_.image, depth_image_.allocation);

    for (auto iv : swapchain_image_views_)
        vkDestroyImageView(device_, iv, nullptr);

    // Save old swapchain handle (create_swapchain overwrites swapchain_)
    VkSwapchainKHR old_sc = swapchain_;

    // Recreate
    create_swapchain(static_cast<u32>(w), static_cast<u32>(h));

    // Destroy retired swapchain (spec requires explicit destroy after recreation)
    if (old_sc != VK_NULL_HANDLE)
        vkDestroySwapchainKHR(device_, old_sc, nullptr);
    create_depth_image();
    create_framebuffers();
}

void Renderer::shutdown() {
    if (!initialized_) return;

    vkDeviceWaitIdle(device_);

    // Sub-renderers
    terrain_mesh_.destroy(device_, allocator_);
    unit_renderer_.destroy(device_, allocator_);
    water_renderer_.destroy(device_, allocator_);

    // Pipelines
    vkDestroyPipeline(device_, terrain_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, terrain_layout_, nullptr);
    vkDestroyPipeline(device_, unit_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, unit_layout_, nullptr);
    vkDestroyPipeline(device_, water_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, water_layout_, nullptr);

    // Sync
    vkDestroyFence(device_, render_fence_, nullptr);
    vkDestroySemaphore(device_, present_semaphore_, nullptr);
    vkDestroySemaphore(device_, render_semaphore_, nullptr);

    // Command pool
    vkDestroyCommandPool(device_, cmd_pool_, nullptr);

    // Framebuffers
    for (auto fb : framebuffers_)
        vkDestroyFramebuffer(device_, fb, nullptr);

    // Render pass
    vkDestroyRenderPass(device_, render_pass_, nullptr);

    // Depth image
    vkDestroyImageView(device_, depth_image_.view, nullptr);
    vmaDestroyImage(allocator_, depth_image_.image, depth_image_.allocation);

    // Swapchain image views
    for (auto iv : swapchain_image_views_)
        vkDestroyImageView(device_, iv, nullptr);

    // Swapchain
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);

    // VMA
    vmaDestroyAllocator(allocator_);

    // Device & instance
    vkDestroyDevice(device_, nullptr);
    vkDestroySurfaceKHR(instance_, surface_, nullptr);

    vkb::destroy_debug_utils_messenger(instance_, debug_messenger_);
    vkDestroyInstance(instance_, nullptr);

    // GLFW
    glfwDestroyWindow(window_);
    glfwTerminate();

    initialized_ = false;
    spdlog::info("Renderer shut down");
}

} // namespace osc::renderer
