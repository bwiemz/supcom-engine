#pragma once

#include "core/types.hpp"
#include "renderer/playable_rect.hpp"
#include "renderer/vk_types.hpp"
#include "sim/resource_deposit.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <functional>
#include <span>
#include <vector>

namespace osc::renderer {

class Camera;
class TextureCache;

/// UI_ResourceLODCutoff
inline constexpr f32 kResourceLodCutoff = 75.0f;

inline constexpr const char* kMassIconTexture = "/env/common/splats/mass_strategic.dds";
inline constexpr const char* kHydrocarbonIconTexture =
    "/env/common/splats/hydrocarbon_strategic.dds";

struct ResourceIcon {
    f32 x = 0, y = 0;
    sim::ResourceDeposit::Type type = sim::ResourceDeposit::Mass;
};

/// The centre of CSimResources::AddDepositPoint's whole-unit footprint
std::array<f32, 2> deposit_centre(const sim::ResourceDeposit& deposit);

bool deposit_in_rect(const sim::ResourceDeposit& deposit, const PlayableRect& rect);

/// CWldSession::RenderResources' deposits
std::vector<ResourceIcon> resource_icons(std::span<const sim::ResourceDeposit> deposits,
                                         const Camera& camera, f32 width, f32 height,
                                         const PlayableRect& playable,
                                         const std::function<f32(f32, f32)>& ground);

/// primbatcher.fx's ResourceIconPS(glow) at pixel (x, y)
f32 resource_icon_glow(f32 alpha, f32 time, f32 x, f32 y);

/// primbatcher.fx's TResourceIcon, before the bloom as WRenViewport::Render
class ResourceIconRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
              VkDescriptorSetLayout texture_ds_layout);

    void update(std::span<const ResourceIcon> icons, TextureCache& tex_cache, u32 fi);

    void render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h, f32 time, u32 fi) const;

    void destroy(VkDevice device, VmaAllocator allocator);

    const std::vector<std::array<f32, 4>>& quads() const { return quads_; }

    static constexpr u32 MAX_ICONS = 4096;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    struct Group {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        u32 first = 0, count = 0;
    };

    VkPipeline colour_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout colour_layout_ = VK_NULL_HANDLE;
    VkPipeline glow_pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout glow_layout_ = VK_NULL_HANDLE;
    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    std::vector<std::array<f32, 4>> quads_;
    std::vector<Group> groups_;
};

} // namespace osc::renderer
