#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"

#include <vulkan/vulkan.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::renderer {

struct DDSTexture; // forward

/// A GPU-resident texture with its descriptor set.
struct GPUTexture {
    AllocatedImage image{};
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
};

/// Lazy-loading texture cache keyed by VFS path.
/// Each texture gets its own VkDescriptorSet (combined image sampler).
class TextureCache {
public:
    void init(VkDevice device, VmaAllocator allocator,
              VkCommandPool cmd_pool, VkQueue queue,
              VkDescriptorSetLayout ds_layout, VkSampler sampler,
              vfs::VirtualFileSystem* vfs);

    /// Get or lazily load a GPU texture for a VFS path.
    /// Returns nullptr if the texture cannot be loaded.
    const GPUTexture* get(const std::string& vfs_path);

    /// Get or lazily load a GPU texture from raw DDS bytes (not VFS).
    /// Key is used for caching. Returns nullptr on failure.
    const GPUTexture* get_raw(const std::string& key, const std::vector<char>& raw_dds);

    /// Descriptor set for the 1x1 white fallback texture.
    VkDescriptorSet fallback_descriptor() const { return fallback_.descriptor_set; }

    /// Descriptor set for the 1x1 transparent-black fallback (specteam: alpha=0).
    VkDescriptorSet specteam_fallback_descriptor() const { return specteam_fallback_.descriptor_set; }

    /// Descriptor set for the 1x1 flat-normal fallback (GA=(128,128) = tangent-space (0,0,1)).
    VkDescriptorSet normal_fallback_descriptor() const { return normal_fallback_.descriptor_set; }

    /// Image view accessors for building multi-binding descriptor sets.
    VkImageView fallback_view() const { return fallback_.image.view; }
    VkImageView zero_fallback_view() const { return specteam_fallback_.image.view; }
    VkImageView normal_fallback_view() const { return normal_fallback_.image.view; }

    void destroy(VkDevice device, VmaAllocator allocator);

private:
    AllocatedImage upload_dds(const DDSTexture& dds);
    void create_fallback();
    void create_specteam_fallback();
    void create_normal_fallback();
    VkDescriptorSet allocate_and_write_descriptor(VkImageView view);

    std::unordered_map<std::string, std::unique_ptr<GPUTexture>> cache_;
    std::unordered_set<std::string> failed_;

    GPUTexture fallback_{};
    GPUTexture specteam_fallback_{};
    GPUTexture normal_fallback_{};

    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout ds_layout_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    vfs::VirtualFileSystem* vfs_ = nullptr;
};

} // namespace osc::renderer
