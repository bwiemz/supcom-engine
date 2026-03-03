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

    /// Descriptor set for the 1x1 white fallback texture.
    VkDescriptorSet fallback_descriptor() const { return fallback_.descriptor_set; }

    void destroy(VkDevice device, VmaAllocator allocator);

private:
    AllocatedImage upload_dds(const DDSTexture& dds);
    void create_fallback();
    VkDescriptorSet allocate_and_write_descriptor(VkImageView view);

    std::unordered_map<std::string, std::unique_ptr<GPUTexture>> cache_;
    std::unordered_set<std::string> failed_;

    GPUTexture fallback_{};

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
