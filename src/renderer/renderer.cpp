#define VMA_IMPLEMENTATION
#include "renderer/renderer.hpp"
#include "core/profiler.hpp"
#include "renderer/pipeline_builder.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/terrain_mesh.hpp"
#include "sim/scm_parser.hpp"
#include "sim/sim_state.hpp"
#include "sim/entity.hpp"
#include "map/terrain.hpp"
#include "renderer/normal_overlay.hpp"
#include "renderer/frustum.hpp"
#include "map/pathfinding_grid.hpp"
#include "map/visibility_grid.hpp"

#include <VkBootstrap.h>
#include <GLFW/glfw3.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

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
    ui_dispatch_.install_callbacks(window_);

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

    // Physical device — require BC texture compression + anisotropic filtering
    VkPhysicalDeviceFeatures required_features{};
    required_features.textureCompressionBC = VK_TRUE;
    required_features.samplerAnisotropy = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(vkb_inst);
    auto phys_ret = selector
        .set_surface(surface_)
        .set_minimum_version(1, 0)
        .set_required_features(required_features)
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
    cmd_ai.commandBufferCount = FRAMES_IN_FLIGHT;
    vkAllocateCommandBuffers(device_, &cmd_ai, cmd_buf_);

    // Sync objects (one set per frame in flight)
    VkFenceCreateInfo fence_ci{};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkSemaphoreCreateInfo sem_ci{};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        vkCreateFence(device_, &fence_ci, nullptr, &render_fence_[i]);
        vkCreateSemaphore(device_, &sem_ci, nullptr, &present_semaphore_[i]);
        vkCreateSemaphore(device_, &sem_ci, nullptr, &render_semaphore_[i]);
    }

    // Texture descriptor set layout (set=0, binding=0: combined image sampler)
    {
        VkDescriptorSetLayoutBinding sampler_binding{};
        sampler_binding.binding = 0;
        sampler_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_binding.descriptorCount = 1;
        sampler_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ds_ci{};
        ds_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_ci.bindingCount = 1;
        ds_ci.pBindings = &sampler_binding;
        vkCreateDescriptorSetLayout(device_, &ds_ci, nullptr, &texture_ds_layout_);
    }

    // Bone SSBO descriptor set layout (set=1, binding=0: storage buffer)
    {
        VkDescriptorSetLayoutBinding ssbo_binding{};
        ssbo_binding.binding = 0;
        ssbo_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ssbo_binding.descriptorCount = 1;
        ssbo_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo ds_ci{};
        ds_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_ci.bindingCount = 1;
        ds_ci.pBindings = &ssbo_binding;
        vkCreateDescriptorSetLayout(device_, &ds_ci, nullptr, &bone_ds_layout_);
    }

    // Terrain texture descriptor set layout (set=0: 22 combined image samplers)
    // bindings 0-1: blend maps, 2-10: stratum albedo, 11-19: stratum normal, 20: fog of war, 21: normal overlay
    {
        std::array<VkDescriptorSetLayoutBinding, 22> terrain_bindings{};
        for (u32 i = 0; i < 22; i++) {
            terrain_bindings[i].binding = i;
            terrain_bindings[i].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            terrain_bindings[i].descriptorCount = 1;
            terrain_bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }

        VkDescriptorSetLayoutCreateInfo ds_ci{};
        ds_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_ci.bindingCount = static_cast<u32>(terrain_bindings.size());
        ds_ci.pBindings = terrain_bindings.data();
        vkCreateDescriptorSetLayout(device_, &ds_ci, nullptr,
                                    &terrain_tex_ds_layout_);
    }

    // Texture sampler (trilinear, anisotropic, repeat wrap)
    {
        VkSamplerCreateInfo sampler_ci{};
        sampler_ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sampler_ci.magFilter = VK_FILTER_LINEAR;
        sampler_ci.minFilter = VK_FILTER_LINEAR;
        sampler_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sampler_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sampler_ci.anisotropyEnable = VK_TRUE;
        sampler_ci.maxAnisotropy = 8.0f;
        sampler_ci.maxLod = 16.0f;
        vkCreateSampler(device_, &sampler_ci, nullptr, &texture_sampler_);
    }

    // Shadow resources (must be created before pipelines — shadow_ds_layout_ is referenced)
    create_shadow_resources();

    // Pipelines
    create_pipelines();

    // Shadow depth-only pipelines (need shadow_render_pass_ + bone_ds_layout_)
    create_shadow_pipelines();

    // Bloom post-processing resources
    create_bloom_resources();

    // UI renderer instance buffer
    ui_renderer_.init(device_, allocator_);

    // Overlay renderer (health bars, selection, command lines)
    overlay_renderer_.init(device_, allocator_);

    // Particle renderer (emitter-driven billboard particles)
    particle_renderer_.init(device_, allocator_, render_pass_,
                            texture_ds_layout_, texture_sampler_);

    // Minimap renderer
    minimap_renderer_.init(device_, allocator_);

    // Strategic icon renderer
    strategic_icon_renderer_.init(device_, allocator_);

    // HUD renderer (economy bars)
    hud_renderer_.init(device_, allocator_);

    // Selection info panel
    selection_info_renderer_.init(device_, allocator_);

    // Profile overlay
    profile_overlay_.init(device_, allocator_);

    initialized_ = true;
    spdlog::info("Renderer initialized ({}x{})", width, height);
    return true;
}

bool Renderer::create_swapchain(u32 width, u32 height) {
    vkb::SwapchainBuilder builder(physical_device_, device_, surface_);
    auto sc_ret = builder
        .set_desired_format({VK_FORMAT_B8G8R8A8_UNORM,
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

void Renderer::create_shadow_resources() {
    // --- Shadow depth image (2048x2048, D32_SFLOAT, samplable) ---
    {
        VkImageCreateInfo img_ci{};
        img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_ci.imageType = VK_IMAGE_TYPE_2D;
        img_ci.format = VK_FORMAT_D32_SFLOAT;
        img_ci.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 1};
        img_ci.mipLevels = 1;
        img_ci.arrayLayers = 1;
        img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                     | VK_IMAGE_USAGE_SAMPLED_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        alloc_ci.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        vmaCreateImage(allocator_, &img_ci, &alloc_ci,
                       &shadow_image_.image, &shadow_image_.allocation, nullptr);

        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = shadow_image_.image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = VK_FORMAT_D32_SFLOAT;
        view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        view_ci.subresourceRange.levelCount = 1;
        view_ci.subresourceRange.layerCount = 1;
        vkCreateImageView(device_, &view_ci, nullptr, &shadow_image_.view);
    }

    // --- Shadow render pass (depth-only) ---
    {
        VkAttachmentDescription depth_att{};
        depth_att.format = VK_FORMAT_D32_SFLOAT;
        depth_att.samples = VK_SAMPLE_COUNT_1_BIT;
        depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth_att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth_att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference depth_ref{};
        depth_ref.attachment = 0;
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 0;
        subpass.pDepthStencilAttachment = &depth_ref;

        VkSubpassDependency dep{};
        dep.srcSubpass = 0;
        dep.dstSubpass = VK_SUBPASS_EXTERNAL;
        dep.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                         | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dep.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rp_ci{};
        rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp_ci.attachmentCount = 1;
        rp_ci.pAttachments = &depth_att;
        rp_ci.subpassCount = 1;
        rp_ci.pSubpasses = &subpass;
        rp_ci.dependencyCount = 1;
        rp_ci.pDependencies = &dep;

        vkCreateRenderPass(device_, &rp_ci, nullptr, &shadow_render_pass_);
    }

    // --- Shadow framebuffer ---
    {
        VkFramebufferCreateInfo fb_ci{};
        fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = shadow_render_pass_;
        fb_ci.attachmentCount = 1;
        fb_ci.pAttachments = &shadow_image_.view;
        fb_ci.width = SHADOW_MAP_SIZE;
        fb_ci.height = SHADOW_MAP_SIZE;
        fb_ci.layers = 1;
        vkCreateFramebuffer(device_, &fb_ci, nullptr, &shadow_framebuffer_);
    }

    // --- Shadow comparison sampler (for sampler2DShadow) ---
    {
        VkSamplerCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter = VK_FILTER_LINEAR;
        ci.minFilter = VK_FILTER_LINEAR;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        ci.compareEnable = VK_TRUE;
        ci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        ci.maxLod = 1.0f;
        vkCreateSampler(device_, &ci, nullptr, &shadow_sampler_);
    }

    // --- Light UBO (64B, persistently mapped) ---
    {
        VkBufferCreateInfo ubo_ci{};
        ubo_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ubo_ci.size = sizeof(f32) * 16;
        ubo_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo info{};
        vmaCreateBuffer(allocator_, &ubo_ci, &alloc_ci,
                        &light_ubo_.buffer, &light_ubo_.allocation, &info);
        light_ubo_mapped_ = info.pMappedData;
    }

    // --- Shadow descriptor set layout (binding 0: shadow sampler, binding 1: light UBO) ---
    {
        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo ds_ci{};
        ds_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ds_ci.bindingCount = static_cast<u32>(bindings.size());
        ds_ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(device_, &ds_ci, nullptr, &shadow_ds_layout_);
    }

    // --- Shadow descriptor pool + set ---
    {
        std::array<VkDescriptorPoolSize, 2> pool_sizes{};
        pool_sizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        pool_sizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1};

        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.maxSets = 1;
        pool_ci.poolSizeCount = static_cast<u32>(pool_sizes.size());
        pool_ci.pPoolSizes = pool_sizes.data();
        vkCreateDescriptorPool(device_, &pool_ci, nullptr, &shadow_ds_pool_);

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = shadow_ds_pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &shadow_ds_layout_;
        vkAllocateDescriptorSets(device_, &alloc_info, &shadow_ds_);

        VkDescriptorImageInfo img_info{};
        img_info.sampler = shadow_sampler_;
        img_info.imageView = shadow_image_.view;
        img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkDescriptorBufferInfo buf_info{};
        buf_info.buffer = light_ubo_.buffer;
        buf_info.offset = 0;
        buf_info.range = sizeof(f32) * 16;

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = shadow_ds_;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &img_info;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = shadow_ds_;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &buf_info;

        vkUpdateDescriptorSets(device_, static_cast<u32>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    spdlog::info("Shadow resources created ({}x{} depth map)", SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
}

std::array<f32, 16> Renderer::compute_light_vp() const {
    // Light direction (matches all lit shaders)
    constexpr f32 lx = 0.5f, ly = 1.0f, lz = 0.3f;
    constexpr f32 len = 1.1576f; // sqrt(0.25 + 1.0 + 0.09)
    constexpr f32 dx = lx / len, dy = ly / len, dz = lz / len;

    // Ortho frustum centered on camera target, proportional to zoom
    f32 half = std::clamp(camera_.distance() * 0.8f, 50.0f, 800.0f);
    f32 far_off = half * 2.0f;

    f32 ex = camera_.target_x() + dx * far_off;
    f32 ey = dy * far_off;
    f32 ez = camera_.target_z() + dz * far_off;

    auto view = math::look_at(ex, ey, ez,
                              camera_.target_x(), 0.0f, camera_.target_z(),
                              0.0f, 1.0f, 0.0f);
    auto proj = math::ortho(-half, half, -half, half, 0.1f, far_off * 2.0f);
    return math::mat4_mul(proj, view);
}

void Renderer::create_pipelines() {
    // Compile shaders from embedded GLSL
    auto tv = compile_glsl(device_, shaders::terrain_vert, "terrain.vert", true);
    auto tf = compile_glsl(device_, shaders::terrain_frag, "terrain.frag", false);
    auto uv = compile_glsl(device_, shaders::unit_vert, "unit.vert", true);
    auto uf = compile_glsl(device_, shaders::unit_frag, "unit.frag", false);
    auto wv = compile_glsl(device_, shaders::water_vert, "water.vert", true);
    auto wf = compile_glsl(device_, shaders::water_frag, "water.frag", false);
    auto mv = compile_glsl(device_, shaders::mesh_vert, "mesh.vert", true);
    auto mf = compile_glsl(device_, shaders::mesh_frag, "mesh.frag", false);
    auto dv = compile_glsl(device_, shaders::decal_vert, "decal.vert", true);
    auto df = compile_glsl(device_, shaders::decal_frag, "decal.frag", false);

    // Abort if any shader failed to compile
    if (!tv || !tf || !uv || !uf || !wv || !wf || !mv || !mf || !dv || !df) {
        spdlog::error("One or more shaders failed to compile");
        auto safe_destroy = [&](VkShaderModule m) {
            if (m) vkDestroyShaderModule(device_, m, nullptr);
        };
        safe_destroy(tv); safe_destroy(tf); safe_destroy(uv);
        safe_destroy(uf); safe_destroy(wv); safe_destroy(wf);
        safe_destroy(mv); safe_destroy(mf);
        safe_destroy(dv); safe_destroy(df);
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

        // Push constant: mat4 viewProj(64) + mapW(4) + mapH(4) + pad(8) + 3*vec4 scales(48) + eye(12) = 140B
        terrain_pipeline_ = PipelineBuilder()
            .set_shaders(tv, tf)
            .set_vertex_input(&binding, 1, attrs.data(),
                              static_cast<u32>(attrs.size()))
            .set_depth_test(true, true)
            .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_push_constant(140,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
            .set_descriptor_set_layout(terrain_tex_ds_layout_)   // set=0: terrain textures
            .add_descriptor_set_layout(shadow_ds_layout_)           // set=1: shadow
            .build(device_, render_pass_, &terrain_layout_);
    }

    // --- Unit pipeline (instanced cubes — fallback) ---
    {
        std::array<VkVertexInputBindingDescription, 2> bindings{};
        // Binding 0: per-vertex cube data
        bindings[0].binding = 0;
        bindings[0].stride = sizeof(f32) * 6; // pos + normal
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        // Binding 1: per-instance data
        bindings[1].binding = 1;
        bindings[1].stride = sizeof(CubeInstance);
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::array<VkVertexInputAttributeDescription, 5> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};                  // position
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3};    // normal
        attrs[2] = {2, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(CubeInstance, x)};   // instancePos
        attrs[3] = {3, 1, VK_FORMAT_R32_SFLOAT,       offsetof(CubeInstance, scale)};// scale
        attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, r)}; // color

        unit_pipeline_ = PipelineBuilder()
            .set_shaders(uv, uf)
            .set_vertex_input(bindings.data(),
                              static_cast<u32>(bindings.size()),
                              attrs.data(),
                              static_cast<u32>(attrs.size()))
            .set_depth_test(true, true)
            .set_blend(true)
            .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_push_constant(sizeof(f32) * 19,  // viewProj(64) + eye(12) = 76B
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
            .set_descriptor_set_layout(shadow_ds_layout_)   // set=0: shadow
            .build(device_, render_pass_, &unit_layout_);
    }

    // --- Mesh pipeline (real SCM meshes, GPU skinning, per-instance model matrix + texture) ---
    {
        std::array<VkVertexInputBindingDescription, 2> bindings{};
        // Binding 0: per-vertex mesh data (pos + normal + UV + bone_indices + bone_weights + tangent = 64 bytes)
        bindings[0].binding = 0;
        bindings[0].stride = static_cast<u32>(sizeof(sim::SCMMesh::Vertex));
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        // Binding 1: per-instance data (mat4 model + vec4 color = 80 bytes)
        bindings[1].binding = 1;
        bindings[1].stride = sizeof(MeshInstance);
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        // 11 attributes: pos(0), normal(1), uv(2), model col0-3(3-6), color(7), bone_indices(8), bone_weights(9), tangent(10)
        std::array<VkVertexInputAttributeDescription, 11> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};                              // position
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3};                // normal
        attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(f32) * 6};                   // UV
        attrs[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + 0};
        attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + sizeof(f32) * 4};
        attrs[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + sizeof(f32) * 8};
        attrs[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + sizeof(f32) * 12};
        attrs[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, r)};   // color
        attrs[8] = {8, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(sim::SCMMesh::Vertex, bone_indices)};    // bone_indices
        attrs[9] = {9, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(sim::SCMMesh::Vertex, bone_weights)}; // bone_weights
        attrs[10] = {10, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(sim::SCMMesh::Vertex, tx)};  // tangent

        // Push constant: mat4 viewProj (64B) + uint boneBase (4B) + uint bonesPerInst (4B) + vec3 eye (12B) = 84B
        mesh_pipeline_ = PipelineBuilder()
            .set_shaders(mv, mf)
            .set_vertex_input(bindings.data(),
                              static_cast<u32>(bindings.size()),
                              attrs.data(),
                              static_cast<u32>(attrs.size()))
            .set_depth_test(true, true)
            .set_blend(true)
            .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_push_constant(sizeof(f32) * 16 + sizeof(u32) * 2 + sizeof(f32) * 3,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
            .set_descriptor_set_layout(texture_ds_layout_)   // set=0: albedo
            .add_descriptor_set_layout(bone_ds_layout_)       // set=1: bone SSBO
            .add_descriptor_set_layout(texture_ds_layout_)    // set=2: specteam
            .add_descriptor_set_layout(texture_ds_layout_)    // set=3: normal map
            .add_descriptor_set_layout(shadow_ds_layout_)     // set=4: shadow
            .build(device_, render_pass_, &mesh_layout_);
    }

    // --- Water pipeline (tessellated grid with wave animation) ---
    {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(f32) * 4; // position(3) + depth(1)
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 2> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};                    // position
        attrs[1] = {1, 0, VK_FORMAT_R32_SFLOAT, sizeof(f32) * 3};            // depth

        water_pipeline_ = PipelineBuilder()
            .set_shaders(wv, wf)
            .set_vertex_input(&binding, 1, attrs.data(),
                              static_cast<u32>(attrs.size()))
            .set_depth_test(true, false) // test ON, write OFF
            .set_blend(true)
            .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_push_constant(WaterRenderer::PUSH_CONSTANT_SIZE,
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT)
            .build(device_, render_pass_, &water_layout_);
    }

    // --- Decal pipeline (textured quads on terrain, alpha-blended, depth-biased) ---
    {
        std::array<VkVertexInputBindingDescription, 2> bindings{};
        // Binding 0: per-vertex quad data (pos3 + uv2 = 20 bytes)
        bindings[0].binding = 0;
        bindings[0].stride = sizeof(f32) * 5;
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        // Binding 1: per-instance model matrix (mat4 = 64 bytes)
        bindings[1].binding = 1;
        bindings[1].stride = sizeof(f32) * 16;
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::array<VkVertexInputAttributeDescription, 6> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};                              // position
        attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(f32) * 3};                   // UV
        attrs[2] = {2, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0};                            // model col0
        attrs[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(f32) * 4};              // model col1
        attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(f32) * 8};              // model col2
        attrs[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(f32) * 12};             // model col3

        decal_pipeline_ = PipelineBuilder()
            .set_shaders(dv, df)
            .set_vertex_input(bindings.data(),
                              static_cast<u32>(bindings.size()),
                              attrs.data(),
                              static_cast<u32>(attrs.size()))
            .set_depth_test(true, false) // test ON, write OFF
            .set_blend(true)
            .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_depth_bias(-1.0f, -1.0f)
            .set_push_constant(sizeof(f32) * 16,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)
            .set_descriptor_set_layout(texture_ds_layout_)
            .build(device_, render_pass_, &decal_layout_);
    }

    // --- UI 2D pipeline (screen-space textured quads, no depth, alpha blend) ---
    auto uiv = compile_glsl(device_, shaders::ui_vert, "ui.vert", true);
    auto uif = compile_glsl(device_, shaders::ui_frag, "ui.frag", false);
    if (uiv && uif) {
        // Only per-instance input (no per-vertex — quad generated from gl_VertexIndex)
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(UIInstance);
        binding.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::array<VkVertexInputAttributeDescription, 3> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};                   // rect (x,y,w,h)
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(f32) * 4};     // uvRect
        attrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(f32) * 8};     // color

        ui_pipeline_ = PipelineBuilder()
            .set_shaders(uiv, uif)
            .set_vertex_input(&binding, 1, attrs.data(),
                              static_cast<u32>(attrs.size()))
            .set_depth_test(false, false)
            .set_blend(true)
            .set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_push_constant(sizeof(f32) * 2,
                               VK_SHADER_STAGE_VERTEX_BIT)
            .set_descriptor_set_layout(texture_ds_layout_)
            .build(device_, render_pass_, &ui_layout_);
    }

    // Destroy shader modules (already compiled into pipelines)
    vkDestroyShaderModule(device_, tv, nullptr);
    vkDestroyShaderModule(device_, tf, nullptr);
    vkDestroyShaderModule(device_, uv, nullptr);
    vkDestroyShaderModule(device_, uf, nullptr);
    vkDestroyShaderModule(device_, wv, nullptr);
    vkDestroyShaderModule(device_, wf, nullptr);
    vkDestroyShaderModule(device_, mv, nullptr);
    vkDestroyShaderModule(device_, mf, nullptr);
    vkDestroyShaderModule(device_, dv, nullptr);
    vkDestroyShaderModule(device_, df, nullptr);
    if (uiv) vkDestroyShaderModule(device_, uiv, nullptr);
    if (uif) vkDestroyShaderModule(device_, uif, nullptr);
}

void Renderer::create_shadow_pipelines() {
    // Compile shadow shaders
    auto sv = compile_glsl(device_, shaders::shadow_vert, "shadow.vert", true);
    auto smv = compile_glsl(device_, shaders::shadow_mesh_vert, "shadow_mesh.vert", true);
    auto suv = compile_glsl(device_, shaders::shadow_unit_vert, "shadow_unit.vert", true);
    auto sf = compile_glsl(device_, shaders::shadow_frag, "shadow.frag", false);

    if (!sv || !smv || !suv || !sf) {
        spdlog::error("Shadow shader compilation failed");
        auto safe_destroy = [&](VkShaderModule m) {
            if (m) vkDestroyShaderModule(device_, m, nullptr);
        };
        safe_destroy(sv); safe_destroy(smv); safe_destroy(suv); safe_destroy(sf);
        return;
    }

    // --- Shadow terrain pipeline (depth-only, same vertex layout as terrain) ---
    {
        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(TerrainVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 2> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};                  // position
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3};    // normal

        shadow_terrain_pipeline_ = PipelineBuilder()
            .set_shaders(sv, sf)
            .set_vertex_input(&binding, 1, attrs.data(),
                              static_cast<u32>(attrs.size()))
            .set_depth_test(true, true)
            .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_push_constant(sizeof(f32) * 16, VK_SHADER_STAGE_VERTEX_BIT)
            .set_no_color_attachment()
            .set_depth_bias(4.0f, 1.5f)
            .build(device_, shadow_render_pass_, &shadow_terrain_layout_);
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

        std::array<VkVertexInputAttributeDescription, 11> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(f32) * 3};
        attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(f32) * 6};
        attrs[3] = {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + 0};
        attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + sizeof(f32) * 4};
        attrs[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + sizeof(f32) * 8};
        attrs[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, model) + sizeof(f32) * 12};
        attrs[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(MeshInstance, r)};
        attrs[8] = {8, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(sim::SCMMesh::Vertex, bone_indices)};
        attrs[9] = {9, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(sim::SCMMesh::Vertex, bone_weights)};
        attrs[10] = {10, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(sim::SCMMesh::Vertex, tx)};

        // Push constant 72B: mat4 lightVP (64) + uint boneBase (4) + uint bonesPerInst (4)
        shadow_mesh_pipeline_ = PipelineBuilder()
            .set_shaders(smv, sf)
            .set_vertex_input(bindings.data(),
                              static_cast<u32>(bindings.size()),
                              attrs.data(),
                              static_cast<u32>(attrs.size()))
            .set_depth_test(true, true)
            .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_push_constant(sizeof(f32) * 16 + sizeof(u32) * 2, VK_SHADER_STAGE_VERTEX_BIT)
            .set_descriptor_set_layout(bone_ds_layout_)   // set=0: bone SSBO
            .set_no_color_attachment()
            .set_depth_bias(4.0f, 1.5f)
            .build(device_, shadow_render_pass_, &shadow_mesh_layout_);
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
        attrs[3] = {3, 1, VK_FORMAT_R32_SFLOAT,       offsetof(CubeInstance, scale)};
        attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(CubeInstance, r)};

        shadow_unit_pipeline_ = PipelineBuilder()
            .set_shaders(suv, sf)
            .set_vertex_input(bindings.data(),
                              static_cast<u32>(bindings.size()),
                              attrs.data(),
                              static_cast<u32>(attrs.size()))
            .set_depth_test(true, true)
            .set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .set_push_constant(sizeof(f32) * 16, VK_SHADER_STAGE_VERTEX_BIT)
            .set_no_color_attachment()
            .set_depth_bias(4.0f, 1.5f)
            .build(device_, shadow_render_pass_, &shadow_unit_layout_);
    }

    vkDestroyShaderModule(device_, sv, nullptr);
    vkDestroyShaderModule(device_, smv, nullptr);
    vkDestroyShaderModule(device_, suv, nullptr);
    vkDestroyShaderModule(device_, sf, nullptr);

    spdlog::info("Shadow pipelines created (terrain + mesh + unit)");
}

void Renderer::create_bloom_resources() {
    u32 w = window_width_;
    u32 h = window_height_;
    u32 half_w = std::max(w / 2, 1u);
    u32 half_h = std::max(h / 2, 1u);
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
        img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        vmaCreateImage(allocator_, &img_ci, &alloc_ci,
                       &img.image, &img.allocation, nullptr);

        VkImageViewCreateInfo view_ci{};
        view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_ci.image = img.image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = hdr_format;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(device_, &view_ci, nullptr, &img.view);
    };

    create_hdr_image(scene_color_image_, w, h);
    create_hdr_image(bloom_bright_image_, half_w, half_h);
    create_hdr_image(bloom_blur_h_image_, half_w, half_h);
    create_hdr_image(bloom_blur_v_image_, half_w, half_h);

    // Scene render pass (color HDR + depth)
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

        VkAttachmentDescription depth_att{};
        depth_att.format = depth_format_;
        depth_att.samples = VK_SAMPLE_COUNT_1_BIT;
        depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth_att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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

        vkCreateRenderPass(device_, &rp_ci, nullptr, &scene_render_pass_);
    }

    // Scene framebuffer (full resolution, scene_render_pass_)
    {
        std::array<VkImageView, 2> views = {scene_color_image_.view, depth_image_.view};
        VkFramebufferCreateInfo fb_ci{};
        fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = scene_render_pass_;
        fb_ci.attachmentCount = static_cast<u32>(views.size());
        fb_ci.pAttachments = views.data();
        fb_ci.width = w;
        fb_ci.height = h;
        fb_ci.layers = 1;
        vkCreateFramebuffer(device_, &fb_ci, nullptr, &scene_framebuffer_);
    }

    // Bloom render pass (single color, no depth)
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

        VkRenderPassCreateInfo rp_ci{};
        rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp_ci.attachmentCount = 1;
        rp_ci.pAttachments = &att;
        rp_ci.subpassCount = 1;
        rp_ci.pSubpasses = &subpass;
        vkCreateRenderPass(device_, &rp_ci, nullptr, &bloom_render_pass_);
    }

    // Bloom framebuffers (half resolution)
    auto create_fb = [&](VkFramebuffer& fb, VkImageView view, u32 fw, u32 fh, VkRenderPass rp) {
        VkFramebufferCreateInfo fb_ci{};
        fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_ci.renderPass = rp;
        fb_ci.attachmentCount = 1;
        fb_ci.pAttachments = &view;
        fb_ci.width = fw;
        fb_ci.height = fh;
        fb_ci.layers = 1;
        vkCreateFramebuffer(device_, &fb_ci, nullptr, &fb);
    };

    create_fb(bloom_bright_fb_, bloom_bright_image_.view, half_w, half_h, bloom_render_pass_);
    create_fb(bloom_blur_h_fb_, bloom_blur_h_image_.view, half_w, half_h, bloom_render_pass_);
    create_fb(bloom_blur_v_fb_, bloom_blur_v_image_.view, half_w, half_h, bloom_render_pass_);

    // Descriptor pool + sets
    {
        VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.maxSets = 4;
        pool_ci.poolSizeCount = 1;
        pool_ci.pPoolSizes = &pool_size;
        vkCreateDescriptorPool(device_, &pool_ci, nullptr, &bloom_ds_pool_);

        VkDescriptorSetLayout layouts[4] = {texture_ds_layout_, texture_ds_layout_,
                                             texture_ds_layout_, texture_ds_layout_};
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = bloom_ds_pool_;
        alloc.descriptorSetCount = 4;
        alloc.pSetLayouts = layouts;
        VkDescriptorSet sets[4];
        vkAllocateDescriptorSets(device_, &alloc, sets);
        scene_ds_ = sets[0];
        bloom_bright_ds_ = sets[1];
        bloom_blur_h_ds_ = sets[2];
        bloom_blur_v_ds_ = sets[3];

        auto write_ds = [&](VkDescriptorSet ds, VkImageView view) {
            VkDescriptorImageInfo img_info{};
            img_info.sampler = texture_sampler_;
            img_info.imageView = view;
            img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = ds;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &img_info;
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        };

        write_ds(scene_ds_, scene_color_image_.view);
        write_ds(bloom_bright_ds_, bloom_bright_image_.view);
        write_ds(bloom_blur_h_ds_, bloom_blur_h_image_.view);
        write_ds(bloom_blur_v_ds_, bloom_blur_v_image_.view);
    }

    spdlog::info("Bloom resources created ({}x{}, half {}x{})", w, h, half_w, half_h);
}

void Renderer::destroy_bloom_resources() {
    // Descriptor pool (frees all allocated sets)
    if (bloom_ds_pool_) {
        vkDestroyDescriptorPool(device_, bloom_ds_pool_, nullptr);
        bloom_ds_pool_ = VK_NULL_HANDLE;
    }

    // Pipelines (may be VK_NULL_HANDLE if not yet created -- safe to destroy)
    if (bloom_bright_pipeline_) vkDestroyPipeline(device_, bloom_bright_pipeline_, nullptr);
    if (bloom_bright_layout_) vkDestroyPipelineLayout(device_, bloom_bright_layout_, nullptr);
    if (bloom_blur_pipeline_) vkDestroyPipeline(device_, bloom_blur_pipeline_, nullptr);
    if (bloom_blur_layout_) vkDestroyPipelineLayout(device_, bloom_blur_layout_, nullptr);
    if (bloom_composite_pipeline_) vkDestroyPipeline(device_, bloom_composite_pipeline_, nullptr);
    if (bloom_composite_layout_) vkDestroyPipelineLayout(device_, bloom_composite_layout_, nullptr);

    // Framebuffers
    if (bloom_bright_fb_) vkDestroyFramebuffer(device_, bloom_bright_fb_, nullptr);
    if (bloom_blur_h_fb_) vkDestroyFramebuffer(device_, bloom_blur_h_fb_, nullptr);
    if (bloom_blur_v_fb_) vkDestroyFramebuffer(device_, bloom_blur_v_fb_, nullptr);
    if (scene_framebuffer_) vkDestroyFramebuffer(device_, scene_framebuffer_, nullptr);

    // Render passes
    if (bloom_render_pass_) vkDestroyRenderPass(device_, bloom_render_pass_, nullptr);
    if (scene_render_pass_) vkDestroyRenderPass(device_, scene_render_pass_, nullptr);

    // Images
    auto destroy_img = [&](AllocatedImage& img) {
        if (img.view) vkDestroyImageView(device_, img.view, nullptr);
        if (img.image) vmaDestroyImage(allocator_, img.image, img.allocation);
        img = {};
    };
    destroy_img(scene_color_image_);
    destroy_img(bloom_bright_image_);
    destroy_img(bloom_blur_h_image_);
    destroy_img(bloom_blur_v_image_);

    // Reset handles
    bloom_bright_pipeline_ = VK_NULL_HANDLE;
    bloom_bright_layout_ = VK_NULL_HANDLE;
    bloom_blur_pipeline_ = VK_NULL_HANDLE;
    bloom_blur_layout_ = VK_NULL_HANDLE;
    bloom_composite_pipeline_ = VK_NULL_HANDLE;
    bloom_composite_layout_ = VK_NULL_HANDLE;
    bloom_bright_fb_ = VK_NULL_HANDLE;
    bloom_blur_h_fb_ = VK_NULL_HANDLE;
    bloom_blur_v_fb_ = VK_NULL_HANDLE;
    scene_framebuffer_ = VK_NULL_HANDLE;
    bloom_render_pass_ = VK_NULL_HANDLE;
    scene_render_pass_ = VK_NULL_HANDLE;
}

void Renderer::build_scene(const sim::SimState& sim,
                           vfs::VirtualFileSystem* vfs, lua_State* L) {
    auto* terrain = sim.terrain();
    if (!terrain) {
        spdlog::warn("No terrain loaded — skipping scene build");
        return;
    }

    terrain_mesh_.build(*terrain, device_, allocator_, cmd_pool_,
                        graphics_queue_);

    unit_renderer_.build(device_, allocator_, cmd_pool_, graphics_queue_);

    // Initialize mesh cache, texture cache, and preload meshes
    if (vfs && sim.blueprint_store()) {
        mesh_cache_.init(device_, allocator_, cmd_pool_, graphics_queue_,
                         vfs, sim.blueprint_store());
        texture_cache_.init(device_, allocator_, cmd_pool_, graphics_queue_,
                            texture_ds_layout_, texture_sampler_, vfs);
        font_cache_.init(device_, allocator_, cmd_pool_, graphics_queue_,
                         texture_ds_layout_, texture_sampler_, vfs);
        unit_renderer_.preload_meshes(sim, mesh_cache_, L);
    }

    // Create bone SSBO descriptor pool and set
    if (unit_renderer_.bone_ssbo_buffer() && bone_ds_layout_) {
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        pool_size.descriptorCount = 1;

        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.maxSets = 1;
        pool_ci.poolSizeCount = 1;
        pool_ci.pPoolSizes = &pool_size;
        vkCreateDescriptorPool(device_, &pool_ci, nullptr, &bone_ds_pool_);

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = bone_ds_pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &bone_ds_layout_;
        vkAllocateDescriptorSets(device_, &alloc_info, &bone_ds_);

        VkDescriptorBufferInfo buf_info{};
        buf_info.buffer = unit_renderer_.bone_ssbo_buffer();
        buf_info.offset = 0;
        buf_info.range = VK_WHOLE_SIZE;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = bone_ds_;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.descriptorCount = 1;
        write.pBufferInfo = &buf_info;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    }

    // Load terrain stratum textures and create terrain descriptor set
    // Requires texture_cache_ to be initialized (needs VFS for albedo textures)
    if (terrain_tex_ds_layout_ && !terrain->strata().empty() &&
        texture_cache_.fallback_view()) {
        // Store map dimensions and strata scales
        terrain_map_width_ = static_cast<f32>(terrain->map_width());
        terrain_map_height_ = static_cast<f32>(terrain->map_height());
        auto& strata = terrain->strata();
        // Only strata 0-8 are blended; stratum 9 (UpperStratum) has no
        // blend map channel and is handled separately in FA.
        for (size_t i = 0; i < 9 && i < strata.size(); i++) {
            terrain_strata_scales_[i] = strata[i].albedo_scale;
            spdlog::info("Terrain stratum {}: albedo='{}' normal='{}' scale={:.1f}",
                         i, strata[i].albedo_path, strata[i].normal_path,
                         strata[i].albedo_scale);
        }

        // Collect 20 image views:
        // [blend0, blend1, stratum0..8 albedo, stratum0..8 normal]
        std::array<VkImageView, 20> views{};

        // Blend maps from embedded DDS
        VkImageView white_view = texture_cache_.fallback_view();
        VkImageView zero_view = texture_cache_.zero_fallback_view();
        VkImageView normal_fb_view = texture_cache_.normal_fallback_view();

        auto* blend0 = terrain->blend_dds_0().empty() ? nullptr
            : texture_cache_.get_raw("__terrain_blend0", terrain->blend_dds_0());
        auto* blend1 = terrain->blend_dds_1().empty() ? nullptr
            : texture_cache_.get_raw("__terrain_blend1", terrain->blend_dds_1());

        views[0] = blend0 ? blend0->image.view : zero_view;  // black = no blending
        views[1] = blend1 ? blend1->image.view : zero_view;

        spdlog::info("Terrain blend maps: blend0={} ({}B), blend1={} ({}B), map={}x{}",
                     blend0 ? "OK" : "NONE", terrain->blend_dds_0().size(),
                     blend1 ? "OK" : "NONE", terrain->blend_dds_1().size(),
                     terrain->map_width(), terrain->map_height());

        // Stratum albedo textures (0-8) at bindings 2-10
        // Empty strata use black (zero) so blend weights don't add white
        for (size_t i = 0; i < 9; i++) {
            if (i < strata.size() && !strata[i].albedo_path.empty()) {
                auto* tex = texture_cache_.get(strata[i].albedo_path);
                views[2 + i] = tex ? tex->image.view : white_view;
            } else {
                views[2 + i] = zero_view; // black = no color contribution
            }
        }

        // Stratum normal map textures (0-8) at bindings 11-19
        for (size_t i = 0; i < 9; i++) {
            if (i < strata.size() && !strata[i].normal_path.empty()) {
                auto* tex = texture_cache_.get(strata[i].normal_path);
                views[11 + i] = tex ? tex->image.view : normal_fb_view;
            } else {
                views[11 + i] = normal_fb_view;
            }
        }

        // Create descriptor pool and set
        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = 22;

        VkDescriptorPoolCreateInfo pool_ci{};
        pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_ci.maxSets = 1;
        pool_ci.poolSizeCount = 1;
        pool_ci.pPoolSizes = &pool_size;
        vkCreateDescriptorPool(device_, &pool_ci, nullptr,
                                &terrain_tex_ds_pool_);

        VkDescriptorSetAllocateInfo alloc_info{};
        alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc_info.descriptorPool = terrain_tex_ds_pool_;
        alloc_info.descriptorSetCount = 1;
        alloc_info.pSetLayouts = &terrain_tex_ds_layout_;
        vkAllocateDescriptorSets(device_, &alloc_info, &terrain_tex_ds_);

        // Write all 20 image descriptors
        std::array<VkDescriptorImageInfo, 20> img_infos{};
        std::array<VkWriteDescriptorSet, 20> writes{};
        for (u32 i = 0; i < 20; i++) {
            img_infos[i].sampler = texture_sampler_;
            img_infos[i].imageView = views[i];
            img_infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = terrain_tex_ds_;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &img_infos[i];
        }
        vkUpdateDescriptorSets(device_, 20, writes.data(), 0, nullptr);

        spdlog::info("Terrain textures: {} strata loaded, blend0={}, blend1={}",
                     strata.size(),
                     blend0 ? "OK" : "fallback",
                     blend1 ? "OK" : "fallback");
    }

    if (terrain->has_water()) {
        water_renderer_.build(*terrain, device_, allocator_, cmd_pool_,
                              graphics_queue_);
    }

    // Init fog of war texture (same grid dimensions as visibility grid)
    {
        u32 fog_w = static_cast<u32>(terrain->map_width()) /
                        map::VisibilityGrid::CELL_SIZE + 1;
        u32 fog_h = static_cast<u32>(terrain->map_height()) /
                        map::VisibilityGrid::CELL_SIZE + 1;
        fog_renderer_.init(fog_w, fog_h, device_, allocator_, cmd_pool_,
                           graphics_queue_);

        // Write fog texture to terrain descriptor set binding 20
        if (fog_renderer_.initialized() && terrain_tex_ds_) {
            VkDescriptorImageInfo fog_info{};
            fog_info.sampler = fog_renderer_.sampler();
            fog_info.imageView = fog_renderer_.image_view();
            fog_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet fog_write{};
            fog_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            fog_write.dstSet = terrain_tex_ds_;
            fog_write.dstBinding = 20;
            fog_write.descriptorCount = 1;
            fog_write.descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            fog_write.pImageInfo = &fog_info;
            vkUpdateDescriptorSets(device_, 1, &fog_write, 0, nullptr);
        }
    }

    // Bake and upload normal overlay from type-2 decals
    {
        auto& nd = terrain->normal_decals();
        u32 ow = terrain->map_width();
        u32 oh = terrain->map_height();

        if (nd.empty()) {
            // No normal decals — upload 1x1 neutral fallback directly
            if (terrain_tex_ds_) {
                u8 neutral[] = {128, 128, 0, 255};
                auto* fb = texture_cache_.upload_rgba("__normal_overlay__",
                                                      neutral, 1, 1);
                if (fb) {
                    VkDescriptorImageInfo info{};
                    info.sampler = texture_sampler_;
                    info.imageView = fb->image.view;
                    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                    VkWriteDescriptorSet write{};
                    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    write.dstSet = terrain_tex_ds_;
                    write.dstBinding = 21;
                    write.descriptorCount = 1;
                    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = &info;
                    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
                }
            }
            spdlog::info("Normal overlay: no decals, using neutral fallback");
        } else {

        auto overlay = bake_normal_overlay(nd, ow, oh, vfs);

        // Encode float perturbations to RGBA8: R = nx*0.5+0.5, G = ny*0.5+0.5
        // Neutral = (128, 128, 0, 255)
        std::vector<u8> rgba(ow * oh * 4);
        for (u32 i = 0; i < ow * oh; i++) {
            f32 nx = overlay.pixels[i * 2 + 0];
            f32 ny = overlay.pixels[i * 2 + 1];
            rgba[i * 4 + 0] = static_cast<u8>(
                std::clamp((nx * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            rgba[i * 4 + 1] = static_cast<u8>(
                std::clamp((ny * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            rgba[i * 4 + 2] = 0;
            rgba[i * 4 + 3] = 255;
        }

        // Use texture_cache to upload as RGBA texture
        auto* tex = texture_cache_.upload_rgba("__normal_overlay__",
                                               rgba.data(), ow, oh);

        // Write binding 21 of terrain descriptor set
        if (tex && terrain_tex_ds_) {
            VkDescriptorImageInfo info{};
            info.sampler = texture_sampler_;
            info.imageView = tex->image.view;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = terrain_tex_ds_;
            write.dstBinding = 21;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &info;
            vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        } else if (terrain_tex_ds_) {
            // No normal decals or upload failed — bind fallback (neutral)
            u8 neutral[] = {128, 128, 0, 255};
            auto* fb = texture_cache_.upload_rgba("__normal_overlay__",
                                                  neutral, 1, 1);
            if (fb) {
                VkDescriptorImageInfo info{};
                info.sampler = texture_sampler_;
                info.imageView = fb->image.view;
                info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkWriteDescriptorSet write{};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = terrain_tex_ds_;
                write.dstBinding = 21;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &info;
                vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
            }
        }

        spdlog::info("Normal overlay: {}x{} ({} decals baked)", ow, oh, nd.size());
        } // else (non-empty decals)
    }

    // Build decal quad mesh + instance buffer + populate stored decals
    if (!terrain->decals().empty() && decal_pipeline_) {
        // Unit quad: (-0.5, 0, -0.5) to (0.5, 0, 0.5) with UV
        // Each vertex: pos(3) + uv(2)
        const f32 quad_verts[] = {
            -0.5f, 0.0f, -0.5f, 0.0f, 0.0f,
             0.5f, 0.0f, -0.5f, 1.0f, 0.0f,
             0.5f, 0.0f,  0.5f, 1.0f, 1.0f,
            -0.5f, 0.0f,  0.5f, 0.0f, 1.0f,
        };
        const u32 quad_indices[] = {0, 1, 2, 0, 2, 3};

        decal_quad_verts_ = upload_buffer(
            device_, allocator_, cmd_pool_, graphics_queue_,
            quad_verts, sizeof(quad_verts),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        decal_quad_indices_ = upload_buffer(
            device_, allocator_, cmd_pool_, graphics_queue_,
            quad_indices, sizeof(quad_indices),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        // Host-visible, persistently-mapped instance buffer for model matrices
        VkDeviceSize inst_size = MAX_DECALS * sizeof(f32) * 16;
        VkBufferCreateInfo buf_ci{};
        buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buf_ci.size = inst_size;
        buf_ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        alloc_ci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo alloc_info{};
        VkResult vma_res = vmaCreateBuffer(allocator_, &buf_ci, &alloc_ci,
                        &decal_instance_buf_.buffer,
                        &decal_instance_buf_.allocation, &alloc_info);
        if (vma_res != VK_SUCCESS || !alloc_info.pMappedData) {
            spdlog::error("Decal: failed to allocate instance buffer");
            // decal_instance_mapped_ stays null, render() guard will skip decals
        } else {
        decal_instance_mapped_ = alloc_info.pMappedData;

        // Build model matrix helper (same as unit_renderer.cpp)
        auto build_mat = [](f32* out, f32 px, f32 py, f32 pz,
                            const sim::Quaternion& q,
                            f32 sx, f32 sy, f32 sz) {
            f32 xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
            f32 xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
            f32 wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
            out[0]  = (1.f - 2.f*(yy+zz))*sx; out[1]  = (2.f*(xy+wz))*sx;
            out[2]  = (2.f*(xz-wy))*sx;        out[3]  = 0.f;
            out[4]  = (2.f*(xy-wz))*sy;         out[5]  = (1.f - 2.f*(xx+zz))*sy;
            out[6]  = (2.f*(yz+wx))*sy;          out[7]  = 0.f;
            out[8]  = (2.f*(xz+wy))*sz;         out[9]  = (2.f*(yz-wx))*sz;
            out[10] = (1.f - 2.f*(xx+yy))*sz;   out[11] = 0.f;
            out[12] = px; out[13] = py; out[14] = pz; out[15] = 1.f;
        };

        // Populate stored decals and preload textures
        stored_decals_.reserve(terrain->decals().size());
        for (auto& d : terrain->decals()) {
            StoredDecal sd;
            sd.texture_path = d.texture_path;
            sd.position_x = d.position_x;
            sd.position_y = d.position_y;
            sd.position_z = d.position_z;
            sd.cut_off_lod = d.cut_off_lod;

            auto q = sim::euler_to_quat(d.rotation_y, d.rotation_x, d.rotation_z);
            build_mat(sd.model, d.position_x, d.position_y, d.position_z,
                      q, d.scale_x, d.scale_y, d.scale_z);
            stored_decals_.push_back(std::move(sd));
        }

        // Preload all unique decal textures into texture_cache_
        std::unordered_map<std::string, VkDescriptorSet> preloaded;
        for (auto& sd : stored_decals_) {
            if (preloaded.count(sd.texture_path)) continue;
            auto* tex = texture_cache_.get(sd.texture_path);
            preloaded[sd.texture_path] = tex ? tex->descriptor_set : VK_NULL_HANDLE;
        }

        // Sort stored_decals_ by texture_path for efficient per-frame grouping
        std::sort(stored_decals_.begin(), stored_decals_.end(),
                  [](const StoredDecal& a, const StoredDecal& b) {
                      return a.texture_path < b.texture_path;
                  });

        spdlog::info("Decals: {} stored for rendering ({} unique textures)",
                     stored_decals_.size(), preloaded.size());
        } // else (vma success)
    }

    // Build minimap terrain texture
    minimap_renderer_.build_terrain_texture(*terrain, texture_cache_);

    // Build strategic icon atlas
    strategic_icon_renderer_.build_atlas(texture_cache_);

    camera_.init(static_cast<f32>(terrain->map_width()),
                 static_cast<f32>(terrain->map_height()));

    spdlog::info("Scene built");
}

void Renderer::render(sim::SimState& sim, lua_State* L,
                      ui::UIControlRegistry* ui_registry,
                      const std::unordered_set<u32>* selected_ids) {
    PROFILE_ZONE("Render::frame");
    // Select current frame's sync objects
    u32 fi = frame_index_;

    // Wait for this frame's previous GPU work to complete
    {
        PROFILE_ZONE("Render::gpu_wait");
        vkWaitForFences(device_, 1, &render_fence_[fi], VK_TRUE, UINT64_MAX);
    }

    // Acquire swapchain image
    u32 image_index = 0;
    VkResult acq_result = vkAcquireNextImageKHR(
        device_, swapchain_, UINT64_MAX, present_semaphore_[fi],
        VK_NULL_HANDLE, &image_index);

    if (acq_result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        return;  // don't reset fence — no submission this frame
    }

    vkResetFences(device_, 1, &render_fence_[fi]);

    // Process camera shake events from sim
    {
        auto shakes = sim.camera_shake_events(); // copy before clear
        if (!shakes.empty()) {
            sim.clear_camera_shake_events();
            f32 total_intensity = 0;
            for (const auto& ev : shakes) {
                f32 dx = camera_.target_x() - ev.x;
                f32 dz = camera_.target_z() - ev.z;
                f32 dist = std::sqrt(dx * dx + dz * dz);
                if (dist < ev.radius) {
                    f32 t = 1.0f - dist / ev.radius;
                    total_intensity += ev.min_shake + t * (ev.max_shake - ev.min_shake);
                }
            }
            camera_.apply_shake(total_intensity);
        }
    }

    // Finalize any async texture loads that completed this frame
    texture_cache_.flush_uploads(4);

    // View-projection matrix (computed early for frustum culling)
    f32 aspect = static_cast<f32>(window_width_) /
                 static_cast<f32>(window_height_);
    auto vp = camera_.view_proj(aspect);
    Frustum frustum(vp);

    // Update unit instances (mesh + cube fallback + texture resolution + frustum culling)
    {
        PROFILE_ZONE("Render::unit_update");
        unit_renderer_.update(sim, mesh_cache_, L, &texture_cache_, &camera_,
                              selected_ids, &frustum);
    }

    // Build preview ghost — render a semi-transparent mesh at the cursor
    const auto& ghost_bp = sim.build_ghost_bp();
    if (!ghost_bp.empty() && sim.terrain()) {
        f64 mx, my;
        mouse_position(mx, my);
        f32 wx, wz;
        if (camera_.screen_to_world(static_cast<f32>(mx), static_cast<f32>(my),
                                     static_cast<f32>(window_width_),
                                     static_cast<f32>(window_height_),
                                     0.0f, wx, wz)) {
            f32 size_x = sim.build_ghost_foot_x();
            f32 size_z = sim.build_ghost_foot_z();

            // Snap to grid (structures align to 1-unit grid in FA)
            wx = std::floor(wx) + 0.5f;
            wz = std::floor(wz) + 0.5f;

            // Re-snap to footprint grid (center on even/odd footprint)
            if (static_cast<int>(size_x) % 2 == 0)
                wx = std::floor(wx);
            if (static_cast<int>(size_z) % 2 == 0)
                wz = std::floor(wz);

            f32 wy = sim.terrain()->get_terrain_height(wx, wz);

            // Check placement validity via pathfinding grid
            bool valid = true;
            auto* grid = sim.pathfinding_grid();
            if (grid) {
                f32 half_x = size_x * 0.5f;
                f32 half_z = size_z * 0.5f;
                u32 gx0, gz0, gx1, gz1;
                grid->world_to_grid(wx - half_x, wz - half_z, gx0, gz0);
                grid->world_to_grid(wx + half_x, wz + half_z, gx1, gz1);
                for (u32 gz = gz0; gz <= gz1 && valid; ++gz) {
                    for (u32 gx = gx0; gx <= gx1 && valid; ++gx) {
                        auto cell = grid->get(gx, gz);
                        if (cell == map::CellPassability::Impassable) {
                            valid = false;
                        }
                    }
                }
            }

            // Green = valid, Red = invalid, semi-transparent
            f32 gr = valid ? 0.2f : 1.0f;
            f32 gg = valid ? 0.9f : 0.2f;
            f32 gb = valid ? 0.3f : 0.2f;
            f32 ga = 0.35f;

            const GPUMesh* ghost_mesh = mesh_cache_.get(ghost_bp, L);
            if (ghost_mesh) {
                unit_renderer_.inject_ghost(ghost_mesh, wx, wy, wz,
                                            gr, gg, gb, ga, &texture_cache_);
            }
        }
    }

    // Update UI quads (walk control tree, read LazyVar positions)
    if (ui_registry) {
        PROFILE_ZONE("Render::ui_update");
        f64 now = glfwGetTime();
        f32 dt = (last_frame_time_ > 0.0) ? static_cast<f32>(now - last_frame_time_) : 0.0f;
        last_frame_time_ = now;
        total_time_ += dt;
        frame_dt_ = dt;
        if (dt > 0.0f && dt < 1.0f) {
            ui_renderer_.advance_animations(L, *ui_registry, dt);
            ui_dispatch_.update_controls(L, *ui_registry, static_cast<f64>(dt));
        }
        ui_dispatch_.dispatch_events(L, *ui_registry);
        ui_renderer_.update(L, *ui_registry, texture_cache_, font_cache_,
                            window_width_, window_height_,
                            static_cast<f32>(ui_dispatch_.mouse_x()),
                            static_cast<f32>(ui_dispatch_.mouse_y()));
    }

    // Stage fog of war data from visibility grid (CPU side)
    if (fog_enabled_ && fog_renderer_.initialized() && sim.visibility_grid()) {
        fog_renderer_.stage(*sim.visibility_grid(), player_army_);
    }

    // Update game overlays (health bars, selection circles, command lines, game over)
    {
        PROFILE_ZONE("Render::overlay_update");
        overlay_renderer_.update(sim, camera_, vp, selected_ids, texture_cache_,
                                 window_width_, window_height_,
                                 sim.player_result(), frame_dt_, &frustum);
    }

    // Update particle system (sync effects, step physics, build GPU data)
    {
        PROFILE_ZONE("Render::particle_update");
        particle_system_.sync_effects(sim, emitter_bp_cache_, L);
        particle_system_.update(frame_dt_);
    }
    f32 p_eye_x, p_eye_y, p_eye_z;
    camera_.eye_position(p_eye_x, p_eye_y, p_eye_z);
    const auto& particle_instances = particle_system_.build_instances(
        p_eye_x, p_eye_y, p_eye_z, &frustum);
    particle_renderer_.update(particle_instances, particle_system_, texture_cache_, fi);

    // Update minimap (terrain bg, unit dots, camera frustum box)
    minimap_renderer_.update(sim, camera_, texture_cache_, selected_ids,
                              window_width_, window_height_);

    // Update strategic icons (zoom-dependent 2D icons replacing 3D meshes)
    strategic_icon_renderer_.update(sim, camera_, vp, selected_ids,
                                     texture_cache_,
                                     window_width_, window_height_);

    // Update economy HUD
    hud_renderer_.update(sim, player_army_, font_cache_, texture_cache_,
                          window_width_, window_height_);

    // Update selection info panel
    selection_info_renderer_.update(sim, selected_ids, font_cache_, texture_cache_,
                                    strategic_icon_renderer_.atlas_descriptor(),
                                    window_width_, window_height_);

    // Update profile overlay
    profile_overlay_.set_frame_index(fi);
    profile_overlay_.update(font_cache_, texture_cache_,
                             window_width_, window_height_);

    // Record command buffer
    vkResetCommandBuffer(cmd_buf_[fi], 0);

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_buf_[fi], &begin_info);

    // Upload fog of war texture (barriers + copy, before any render pass)
    if (fog_renderer_.initialized()) {
        fog_renderer_.record_upload(cmd_buf_[fi]);
    }

    // ==================== SHADOW PASS ====================
    if (shadow_render_pass_ && shadow_framebuffer_ && light_ubo_mapped_) {
        PROFILE_ZONE("Render::shadow_pass");
        // Update light UBO
        auto light_vp = compute_light_vp();
        std::memcpy(light_ubo_mapped_, light_vp.data(), sizeof(f32) * 16);

        VkClearValue shadow_clear{};
        shadow_clear.depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo shadow_rp{};
        shadow_rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        shadow_rp.renderPass = shadow_render_pass_;
        shadow_rp.framebuffer = shadow_framebuffer_;
        shadow_rp.renderArea.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
        shadow_rp.clearValueCount = 1;
        shadow_rp.pClearValues = &shadow_clear;
        vkCmdBeginRenderPass(cmd_buf_[fi], &shadow_rp, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport shadow_vp{};
        shadow_vp.width = static_cast<f32>(SHADOW_MAP_SIZE);
        shadow_vp.height = static_cast<f32>(SHADOW_MAP_SIZE);
        shadow_vp.minDepth = 0.0f;
        shadow_vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd_buf_[fi], 0, 1, &shadow_vp);

        VkRect2D shadow_sc{};
        shadow_sc.extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE};
        vkCmdSetScissor(cmd_buf_[fi], 0, 1, &shadow_sc);

        // Shadow terrain
        if (terrain_mesh_.index_count() > 0 && shadow_terrain_pipeline_) {
            vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                              shadow_terrain_pipeline_);
            vkCmdPushConstants(cmd_buf_[fi], shadow_terrain_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(f32) * 16, light_vp.data());

            VkBuffer vbufs[] = {terrain_mesh_.vertex_buffer()};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(cmd_buf_[fi], 0, 1, vbufs, offsets);
            vkCmdBindIndexBuffer(cmd_buf_[fi], terrain_mesh_.index_buffer(), 0,
                                 VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd_buf_[fi], terrain_mesh_.index_count(), 1, 0, 0, 0);
        }

        // Shadow meshes (skip when strategic zoom replaces 3D units with icons)
        if (!strategic_icon_renderer_.is_strategic_zoom() &&
            !unit_renderer_.mesh_groups().empty() && shadow_mesh_pipeline_ && bone_ds_) {
            vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                              shadow_mesh_pipeline_);

            // Bind bone SSBO at set=0
            vkCmdBindDescriptorSets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    shadow_mesh_layout_, 0, 1, &bone_ds_,
                                    0, nullptr);

            struct ShadowMeshPC {
                f32 lightVP[16];
                u32 boneBase;
                u32 bonesPerInst;
            } spc{};
            std::memcpy(spc.lightVP, light_vp.data(), sizeof(f32) * 16);

            for (auto& group : unit_renderer_.mesh_groups()) {
                if (!group.mesh || group.instance_count == 0) continue;

                spc.boneBase = group.bone_base_offset;
                spc.bonesPerInst = group.bones_per_instance;
                vkCmdPushConstants(cmd_buf_[fi], shadow_mesh_layout_,
                                   VK_SHADER_STAGE_VERTEX_BIT,
                                   0, sizeof(spc), &spc);

                VkBuffer vbufs[] = {group.mesh->vertex_buf.buffer,
                                    unit_renderer_.mesh_instance_buffer()};
                VkDeviceSize buf_offsets[] = {
                    0,
                    static_cast<VkDeviceSize>(group.instance_offset) *
                        sizeof(MeshInstance)};
                vkCmdBindVertexBuffers(cmd_buf_[fi], 0, 2, vbufs, buf_offsets);
                vkCmdBindIndexBuffer(cmd_buf_[fi], group.mesh->index_buf.buffer, 0,
                                     VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd_buf_[fi], group.mesh->index_count,
                                 group.instance_count, 0, 0, 0);
            }
        }

        // Shadow cubes (skip when strategic zoom active)
        if (!strategic_icon_renderer_.is_strategic_zoom() &&
            unit_renderer_.cube_instance_count() > 0 && shadow_unit_pipeline_) {
            vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                              shadow_unit_pipeline_);
            vkCmdPushConstants(cmd_buf_[fi], shadow_unit_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(f32) * 16, light_vp.data());

            VkBuffer vbufs[] = {unit_renderer_.cube_vertex_buffer(),
                                unit_renderer_.cube_instance_buffer()};
            VkDeviceSize offsets[] = {0, 0};
            vkCmdBindVertexBuffers(cmd_buf_[fi], 0, 2, vbufs, offsets);
            vkCmdBindIndexBuffer(cmd_buf_[fi], unit_renderer_.cube_index_buffer(), 0,
                                 VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd_buf_[fi], unit_renderer_.cube_index_count(),
                             unit_renderer_.cube_instance_count(), 0, 0, 0);
        }

        vkCmdEndRenderPass(cmd_buf_[fi]);
    }

    // ==================== MAIN PASS ====================
    PROFILE_ZONE("Render::main_pass");
    // Begin render pass
    std::array<VkClearValue, 2> clear_values{};
    clear_values[0].color = {{0.55f, 0.62f, 0.72f, 1.0f}}; // sky haze (matches atmos fog)
    clear_values[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = render_pass_;
    rp_begin.framebuffer = framebuffers_[image_index];
    rp_begin.renderArea.extent = {window_width_, window_height_};
    rp_begin.clearValueCount = static_cast<u32>(clear_values.size());
    rp_begin.pClearValues = clear_values.data();
    vkCmdBeginRenderPass(cmd_buf_[fi], &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    // Dynamic viewport + scissor
    VkViewport viewport{};
    viewport.width = static_cast<f32>(window_width_);
    viewport.height = static_cast<f32>(window_height_);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd_buf_[fi], 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = {window_width_, window_height_};
    vkCmdSetScissor(cmd_buf_[fi], 0, 1, &scissor);

    // 1. Draw terrain
    if (terrain_mesh_.index_count() > 0 && terrain_pipeline_) {
        vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                          terrain_pipeline_);

        // Push constants: viewProj(64) + mapW(4) + mapH(4) + pad(8) + 3*vec4 scales(48) + eye(12) = 140B
        struct TerrainPC {
            f32 viewProj[16];
            f32 mapWidth;
            f32 mapHeight;
            f32 _pad0, _pad1;  // align scales to vec4 boundary (GLSL std430)
            f32 scales0_3[4];
            f32 scales4_7[4];
            f32 scales8_pad[4];
            f32 eyeX, eyeY, eyeZ;
        } tpc{};
        std::memcpy(tpc.viewProj, vp.data(), sizeof(f32) * 16);
        tpc.mapWidth = terrain_map_width_;
        tpc.mapHeight = terrain_map_height_;
        std::memcpy(tpc.scales0_3, &terrain_strata_scales_[0], sizeof(f32) * 4);
        std::memcpy(tpc.scales4_7, &terrain_strata_scales_[4], sizeof(f32) * 4);
        tpc.scales8_pad[0] = terrain_strata_scales_[8];
        camera_.eye_position(tpc.eyeX, tpc.eyeY, tpc.eyeZ);

        vkCmdPushConstants(cmd_buf_[fi], terrain_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(tpc), &tpc);

        // Bind terrain texture descriptor set (set=0)
        if (terrain_tex_ds_) {
            vkCmdBindDescriptorSets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    terrain_layout_, 0, 1, &terrain_tex_ds_,
                                    0, nullptr);
        }
        // Bind shadow descriptor set (set=1)
        if (shadow_ds_) {
            vkCmdBindDescriptorSets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    terrain_layout_, 1, 1, &shadow_ds_,
                                    0, nullptr);
        }

        VkBuffer vbufs[] = {terrain_mesh_.vertex_buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd_buf_[fi], 0, 1, vbufs, offsets);
        vkCmdBindIndexBuffer(cmd_buf_[fi], terrain_mesh_.index_buffer(), 0,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd_buf_[fi], terrain_mesh_.index_count(), 1, 0, 0, 0);
    }

    // 2. Draw decals (textured quads on terrain)
    // stored_decals_ is pre-sorted by texture_path (in build_scene) for
    // allocation-free per-frame grouping via linear scan.
    if (decals_enabled_ && !stored_decals_.empty() && decal_pipeline_ && decal_instance_mapped_) {
        f32 cam_x, cam_y, cam_z;
        camera_.eye_position(cam_x, cam_y, cam_z);

        // Linear scan over pre-sorted decals: distance-cull + group by texture
        decal_groups_.clear();
        u32 total_instances = 0;
        auto* dst = static_cast<f32*>(decal_instance_mapped_);
        const std::string* cur_path = nullptr;
        VkDescriptorSet cur_ds = VK_NULL_HANDLE;

        for (auto& sd : stored_decals_) {
            if (total_instances >= MAX_DECALS) break;

            // Distance cull
            f32 dx = sd.position_x - cam_x;
            f32 dz = sd.position_z - cam_z;
            if (dx * dx + dz * dz > sd.cut_off_lod * sd.cut_off_lod) continue;

            // New texture group?
            if (!cur_path || *cur_path != sd.texture_path) {
                cur_path = &sd.texture_path;
                auto* tex = texture_cache_.get(sd.texture_path);
                cur_ds = tex ? tex->descriptor_set : VK_NULL_HANDLE;
                if (cur_ds) {
                    decal_groups_.push_back({cur_ds, total_instances, 0});
                }
            }

            if (!cur_ds) continue;

            std::memcpy(dst + total_instances * 16, sd.model, sizeof(f32) * 16);
            total_instances++;
            decal_groups_.back().instance_count++;
        }

        if (total_instances > 0) {
            vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                              decal_pipeline_);
            vkCmdPushConstants(cmd_buf_[fi], decal_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT |
                                   VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(f32) * 16, vp.data());

            VkBuffer quad_buf = decal_quad_verts_.buffer;
            VkBuffer inst_buf = decal_instance_buf_.buffer;

            for (auto& group : decal_groups_) {
                vkCmdBindDescriptorSets(cmd_buf_[fi],
                                        VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        decal_layout_, 0, 1,
                                        &group.texture_ds, 0, nullptr);

                VkBuffer vbufs[] = {quad_buf, inst_buf};
                VkDeviceSize buf_offsets[] = {
                    0,
                    static_cast<VkDeviceSize>(group.instance_offset) *
                        sizeof(f32) * 16};
                vkCmdBindVertexBuffers(cmd_buf_[fi], 0, 2, vbufs, buf_offsets);
                vkCmdBindIndexBuffer(cmd_buf_[fi], decal_quad_indices_.buffer, 0,
                                     VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd_buf_[fi], 6, group.instance_count, 0, 0, 0);
            }
        }
    }

    // 3. Draw mesh units (real SCM models with GPU skinning)
    //    Skip when strategic zoom replaces 3D units with 2D icons.
    if (!strategic_icon_renderer_.is_strategic_zoom() &&
        !unit_renderer_.mesh_groups().empty() && mesh_pipeline_) {
        vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                          mesh_pipeline_);

        // Push viewProj as first 64 bytes (bone offsets per-group below)
        struct MeshPushConstants {
            f32 viewProj[16];
            u32 boneBase;
            u32 bonesPerInst;
            f32 eyeX, eyeY, eyeZ;
        } mesh_pc{};
        std::memcpy(mesh_pc.viewProj, vp.data(), sizeof(f32) * 16);
        camera_.eye_position(mesh_pc.eyeX, mesh_pc.eyeY, mesh_pc.eyeZ);

        // Bind fallback (1x1 white) as baseline — ensures set=0 is always valid
        VkDescriptorSet fallback_ds = texture_cache_.fallback_descriptor();
        if (fallback_ds) {
            vkCmdBindDescriptorSets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    mesh_layout_, 0, 1, &fallback_ds,
                                    0, nullptr);
        }

        // Bind bone SSBO at set=1 (once for all groups)
        if (bone_ds_) {
            vkCmdBindDescriptorSets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    mesh_layout_, 1, 1, &bone_ds_,
                                    0, nullptr);
        }

        // Bind specteam fallback at set=2 (alpha=0 = no team color)
        VkDescriptorSet specteam_fallback =
            texture_cache_.specteam_fallback_descriptor();
        if (specteam_fallback) {
            vkCmdBindDescriptorSets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    mesh_layout_, 2, 1, &specteam_fallback,
                                    0, nullptr);
        }

        // Bind normal map fallback at set=3 (flat normal = no perturbation)
        VkDescriptorSet normal_fallback =
            texture_cache_.normal_fallback_descriptor();
        if (normal_fallback) {
            vkCmdBindDescriptorSets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    mesh_layout_, 3, 1, &normal_fallback,
                                    0, nullptr);
        }

        // Bind shadow descriptor set at set=4
        if (shadow_ds_) {
            vkCmdBindDescriptorSets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    mesh_layout_, 4, 1, &shadow_ds_,
                                    0, nullptr);
        }

        for (auto& group : unit_renderer_.mesh_groups()) {
            if (!group.mesh || group.instance_count == 0) continue;

            // Bind per-group albedo texture descriptor (always bind to avoid
            // stale set=0 from prior group)
            VkDescriptorSet albedo_ds = group.texture_ds ? group.texture_ds : fallback_ds;
            if (albedo_ds) {
                vkCmdBindDescriptorSets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        mesh_layout_, 0, 1, &albedo_ds,
                                        0, nullptr);
            }

            // Bind per-group specteam texture descriptor (always bind to avoid
            // stale set=2 from prior group)
            VkDescriptorSet spec_ds = group.specteam_ds ? group.specteam_ds : specteam_fallback;
            if (spec_ds) {
                vkCmdBindDescriptorSets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        mesh_layout_, 2, 1, &spec_ds,
                                        0, nullptr);
            }

            // Bind per-group normal map descriptor (always bind to avoid
            // stale set=3 from prior group)
            VkDescriptorSet norm_ds = group.normal_ds ? group.normal_ds : normal_fallback;
            if (norm_ds) {
                vkCmdBindDescriptorSets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        mesh_layout_, 3, 1, &norm_ds,
                                        0, nullptr);
            }

            // Push bone offsets per group
            mesh_pc.boneBase = group.bone_base_offset;
            mesh_pc.bonesPerInst = group.bones_per_instance;
            vkCmdPushConstants(cmd_buf_[fi], mesh_layout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(mesh_pc), &mesh_pc);

            VkBuffer vbufs[] = {group.mesh->vertex_buf.buffer,
                                unit_renderer_.mesh_instance_buffer()};
            VkDeviceSize buf_offsets[] = {
                0,
                static_cast<VkDeviceSize>(group.instance_offset) *
                    sizeof(MeshInstance)};
            vkCmdBindVertexBuffers(cmd_buf_[fi], 0, 2, vbufs, buf_offsets);
            vkCmdBindIndexBuffer(cmd_buf_[fi], group.mesh->index_buf.buffer, 0,
                                 VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd_buf_[fi], group.mesh->index_count,
                             group.instance_count, 0, 0, 0);
        }
    }

    // 4. Draw cube fallback units (skip when strategic zoom active)
    if (!strategic_icon_renderer_.is_strategic_zoom() &&
        unit_renderer_.cube_instance_count() > 0 && unit_pipeline_) {
        vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                          unit_pipeline_);

        // Bind shadow descriptor set at set=0
        if (shadow_ds_) {
            vkCmdBindDescriptorSets(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    unit_layout_, 0, 1, &shadow_ds_,
                                    0, nullptr);
        }

        struct UnitPC {
            f32 viewProj[16];
            f32 eyeX, eyeY, eyeZ;
        } upc{};
        std::memcpy(upc.viewProj, vp.data(), sizeof(f32) * 16);
        camera_.eye_position(upc.eyeX, upc.eyeY, upc.eyeZ);
        vkCmdPushConstants(cmd_buf_[fi], unit_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(upc), &upc);

        VkBuffer vbufs[] = {unit_renderer_.cube_vertex_buffer(),
                            unit_renderer_.cube_instance_buffer()};
        VkDeviceSize offsets[] = {0, 0};
        vkCmdBindVertexBuffers(cmd_buf_[fi], 0, 2, vbufs, offsets);
        vkCmdBindIndexBuffer(cmd_buf_[fi], unit_renderer_.cube_index_buffer(), 0,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd_buf_[fi], unit_renderer_.cube_index_count(),
                         unit_renderer_.cube_instance_count(), 0, 0, 0);
    }

    // 5. Draw water (tessellated grid with wave animation, depth coloring)
    if (water_renderer_.has_water() && water_pipeline_) {
        vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                          water_pipeline_);

        WaterRenderer::WaterPushConstants wpc{};
        std::memcpy(wpc.view_proj, vp.data(), sizeof(f32) * 16);
        wpc.time = total_time_;
        camera_.eye_position(wpc.eye_x, wpc.eye_y, wpc.eye_z);
        wpc.water_elevation = water_renderer_.water_elevation();

        vkCmdPushConstants(cmd_buf_[fi], water_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, WaterRenderer::PUSH_CONSTANT_SIZE, &wpc);

        VkBuffer vbufs[] = {water_renderer_.vertex_buffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(cmd_buf_[fi], 0, 1, vbufs, offsets);
        vkCmdBindIndexBuffer(cmd_buf_[fi], water_renderer_.index_buffer(), 0,
                             VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd_buf_[fi], water_renderer_.index_count(), 1, 0, 0, 0);
    }

    // 5b. Draw particles (billboard emitter effects)
    if (particle_renderer_.draw_count() > 0) {
        // Extract camera right and up vectors from the view matrix
        // View matrix row 0 = right, row 1 = up (transposed from columns)
        f32 eye_x, eye_y, eye_z;
        camera_.eye_position(eye_x, eye_y, eye_z);
        auto view = math::look_at(eye_x, eye_y, eye_z,
                            camera_.target_x(), 0.0f, camera_.target_z(),
                            0.0f, 1.0f, 0.0f);
        // Column-major: col 0 = right, col 1 = up
        f32 cam_right[3] = {view[0], view[1], view[2]};
        f32 cam_up[3] = {view[4], view[5], view[6]};

        particle_renderer_.render(cmd_buf_[fi],
                                  window_width_, window_height_,
                                  vp.data(), cam_right, cam_up, fi);
    }

    // 6. Draw strategic icons (when zoomed out, replaces 3D unit meshes)
    if (ui_pipeline_ && strategic_icon_renderer_.quad_count() > 0) {
        vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                          ui_pipeline_);
        strategic_icon_renderer_.render(cmd_buf_[fi], ui_layout_,
                                         window_width_, window_height_);
    }

    // 7. Draw game overlays (health bars, selection, command lines)
    if (ui_pipeline_ && overlay_renderer_.quad_count() > 0) {
        vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                          ui_pipeline_);
        overlay_renderer_.render(cmd_buf_[fi], ui_layout_,
                                 window_width_, window_height_);
    }

    // 8. Draw minimap (terrain bg + unit dots + camera box)
    if (ui_pipeline_ && minimap_renderer_.quad_count() > 0) {
        vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                          ui_pipeline_);
        minimap_renderer_.render(cmd_buf_[fi], ui_layout_,
                                  window_width_, window_height_);
    }

    // 9. Draw economy HUD (resource bars + text at top of screen)
    if (ui_pipeline_ && hud_renderer_.quad_count() > 0) {
        vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                          ui_pipeline_);
        hud_renderer_.render(cmd_buf_[fi], ui_layout_,
                              window_width_, window_height_);
    }

    // 10. Draw selection info panel (bottom-center unit details)
    if (ui_pipeline_ && selection_info_renderer_.quad_count() > 0) {
        vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                          ui_pipeline_);
        selection_info_renderer_.render(cmd_buf_[fi], ui_layout_,
                                         window_width_, window_height_);
    }

    // 11. Draw UI (screen-space 2D quads, last — always on top)
    if (ui_pipeline_ && ui_renderer_.quad_count() > 0) {
        vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                          ui_pipeline_);
        ui_renderer_.render(cmd_buf_[fi], ui_layout_,
                            window_width_, window_height_);
    }

    // 12. Draw profile overlay (topmost, after all other UI)
    if (ui_pipeline_ && profile_overlay_.quad_count() > 0) {
        vkCmdBindPipeline(cmd_buf_[fi], VK_PIPELINE_BIND_POINT_GRAPHICS,
                          ui_pipeline_);
        profile_overlay_.render(cmd_buf_[fi], ui_layout_,
                                window_width_, window_height_);
    }

    vkCmdEndRenderPass(cmd_buf_[fi]);
    vkEndCommandBuffer(cmd_buf_[fi]);

    // Submit
    PROFILE_ZONE("Render::submit");
    VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &present_semaphore_[fi];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_buf_[fi];
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_semaphore_[fi];
    vkQueueSubmit(graphics_queue_, 1, &submit, render_fence_[fi]);

    // Present
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_semaphore_[fi];
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image_index;
    VkResult pres_result = vkQueuePresentKHR(graphics_queue_, &present);

    if (pres_result == VK_ERROR_OUT_OF_DATE_KHR ||
        pres_result == VK_SUBOPTIMAL_KHR) {
        recreate_swapchain();
    }

    // Advance frame index for next frame
    frame_index_ = (frame_index_ + 1) % FRAMES_IN_FLIGHT;
}

bool Renderer::should_close() const {
    return window_ && glfwWindowShouldClose(window_);
}

bool Renderer::is_key_pressed(int glfw_key) const {
    return window_ && glfwGetKey(window_, glfw_key) == GLFW_PRESS;
}

void Renderer::set_window_title(const char* title) {
    if (window_) glfwSetWindowTitle(window_, title);
}

void Renderer::mouse_position(f64& x, f64& y) const {
    if (window_) glfwGetCursorPos(window_, &x, &y);
    else { x = 0; y = 0; }
}

bool Renderer::is_mouse_pressed(int glfw_button) const {
    return window_ && glfwGetMouseButton(window_, glfw_button) == GLFW_PRESS;
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

    // Terrain texture pool first (references image views owned by texture_cache_)
    if (terrain_tex_ds_pool_)
        vkDestroyDescriptorPool(device_, terrain_tex_ds_pool_, nullptr);
    if (terrain_tex_ds_layout_)
        vkDestroyDescriptorSetLayout(device_, terrain_tex_ds_layout_, nullptr);

    // Sub-renderers
    terrain_mesh_.destroy(device_, allocator_);
    unit_renderer_.destroy(device_, allocator_);
    water_renderer_.destroy(device_, allocator_);
    fog_renderer_.destroy(device_, allocator_);
    ui_renderer_.destroy(device_, allocator_);
    overlay_renderer_.destroy(device_, allocator_);
    particle_renderer_.destroy(device_, allocator_);
    minimap_renderer_.destroy(device_, allocator_);
    strategic_icon_renderer_.destroy(device_, allocator_);
    hud_renderer_.destroy(device_, allocator_);
    selection_info_renderer_.destroy(device_, allocator_);
    profile_overlay_.destroy(device_, allocator_);
    font_cache_.destroy(device_, allocator_);
    mesh_cache_.destroy(device_, allocator_);
    texture_cache_.destroy(device_, allocator_);

    // Bone SSBO infrastructure
    if (bone_ds_pool_)
        vkDestroyDescriptorPool(device_, bone_ds_pool_, nullptr);
    if (bone_ds_layout_)
        vkDestroyDescriptorSetLayout(device_, bone_ds_layout_, nullptr);

    // Texture infrastructure
    if (texture_sampler_) vkDestroySampler(device_, texture_sampler_, nullptr);
    if (texture_ds_layout_)
        vkDestroyDescriptorSetLayout(device_, texture_ds_layout_, nullptr);

    // Decal buffers
    if (decal_quad_verts_.buffer)
        vmaDestroyBuffer(allocator_, decal_quad_verts_.buffer,
                         decal_quad_verts_.allocation);
    if (decal_quad_indices_.buffer)
        vmaDestroyBuffer(allocator_, decal_quad_indices_.buffer,
                         decal_quad_indices_.allocation);
    if (decal_instance_buf_.buffer)
        vmaDestroyBuffer(allocator_, decal_instance_buf_.buffer,
                         decal_instance_buf_.allocation);

    // Shadow infrastructure
    if (shadow_ds_pool_)
        vkDestroyDescriptorPool(device_, shadow_ds_pool_, nullptr);
    if (shadow_ds_layout_)
        vkDestroyDescriptorSetLayout(device_, shadow_ds_layout_, nullptr);
    vkDestroyPipeline(device_, shadow_terrain_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, shadow_terrain_layout_, nullptr);
    vkDestroyPipeline(device_, shadow_mesh_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, shadow_mesh_layout_, nullptr);
    vkDestroyPipeline(device_, shadow_unit_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, shadow_unit_layout_, nullptr);
    vkDestroyFramebuffer(device_, shadow_framebuffer_, nullptr);
    vkDestroyRenderPass(device_, shadow_render_pass_, nullptr);
    if (shadow_sampler_) vkDestroySampler(device_, shadow_sampler_, nullptr);
    if (shadow_image_.view) vkDestroyImageView(device_, shadow_image_.view, nullptr);
    if (shadow_image_.image)
        vmaDestroyImage(allocator_, shadow_image_.image, shadow_image_.allocation);
    if (light_ubo_.buffer)
        vmaDestroyBuffer(allocator_, light_ubo_.buffer, light_ubo_.allocation);

    // Bloom resources
    destroy_bloom_resources();

    // Pipelines
    vkDestroyPipeline(device_, terrain_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, terrain_layout_, nullptr);
    vkDestroyPipeline(device_, unit_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, unit_layout_, nullptr);
    if (water_pipeline_)
        vkDestroyPipeline(device_, water_pipeline_, nullptr);
    if (water_layout_)
        vkDestroyPipelineLayout(device_, water_layout_, nullptr);
    vkDestroyPipeline(device_, mesh_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, mesh_layout_, nullptr);
    vkDestroyPipeline(device_, decal_pipeline_, nullptr);
    vkDestroyPipelineLayout(device_, decal_layout_, nullptr);
    if (ui_pipeline_) vkDestroyPipeline(device_, ui_pipeline_, nullptr);
    if (ui_layout_) vkDestroyPipelineLayout(device_, ui_layout_, nullptr);

    // Sync (per-frame)
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        vkDestroyFence(device_, render_fence_[i], nullptr);
        vkDestroySemaphore(device_, present_semaphore_[i], nullptr);
        vkDestroySemaphore(device_, render_semaphore_[i], nullptr);
    }

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
