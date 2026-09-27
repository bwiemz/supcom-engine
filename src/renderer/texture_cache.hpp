#pragma once

#include "renderer/vk_types.hpp"
#include "core/types.hpp"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <future>
#include <memory>
#include <optional>
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
    u32 width = 0, height = 0; ///< the top mip's, in texels
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
    /// Returns nullptr if the texture is still loading or cannot be loaded.
    /// On first miss, starts an async VFS read; call flush_uploads() each
    /// frame to finalize completed loads.
    const GPUTexture* get(const std::string& vfs_path);

    /// Synchronous load — blocks until the texture is ready.
    /// Use for one-time init (terrain textures in build_scene).
    const GPUTexture* get_blocking(const std::string& vfs_path);

    /// Get or lazily load a GPU texture from raw DDS bytes (not VFS).
    /// Key is used for caching. Returns nullptr on failure.
    /// Always synchronous (data already in memory).
    const GPUTexture* get_raw(const std::string& key, const std::vector<char>& raw_dds);

    /// Process completed async texture loads (up to max_per_frame).
    /// Call once per frame from the render loop.
    void flush_uploads(u32 max_per_frame = 4);

    /// Textures requested but not yet loaded.
    std::size_t loading() const { return pending_.size(); }

    /// Upload raw RGBA pixels as a cached texture.
    /// Key is used for caching. Pixels must be width*height*4 bytes.
    const GPUTexture* upload_rgba(const std::string& key,
                                   const u8* pixels, u32 width, u32 height);

    /// Descriptor set for the 1x1 white fallback texture.
    VkDescriptorSet fallback_descriptor() const { return fallback_.descriptor_set; }

    /// Descriptor set for the 1x1 transparent-black fallback (specteam: alpha=0).
    VkDescriptorSet specteam_fallback_descriptor() const { return specteam_fallback_.descriptor_set; }

    /// Descriptor set for the 1x1 flat-normal fallback (GA=(128,128) = tangent-space (0,0,1)).
    VkDescriptorSet normal_fallback_descriptor() const { return normal_fallback_.descriptor_set; }

    /// A cubemap (a DDS with six faces), loaded now: its cube view, or null
    /// if it can't be read or isn't a cube.
    VkImageView get_cube_blocking(const std::string& vfs_path);
    /// A black 1x1 cube, for a scene with no cubemap.
    VkImageView cube_fallback_view() const { return cube_fallback_.view; }

    /// Image view accessors for building multi-binding descriptor sets.
    VkImageView fallback_view() const { return fallback_.image.view; }
    VkImageView zero_fallback_view() const { return specteam_fallback_.image.view; }
    VkImageView normal_fallback_view() const { return normal_fallback_.image.view; }

    void destroy(VkDevice device, VmaAllocator allocator);

    /// Drop the texture cached as `key`, so the next upload under that
    /// name makes a new one: for textures made per map (terrain blends,
    /// the normal overlay, the minimap). The device must be idle and
    /// nothing may draw with it again; its descriptor set stays in its
    /// pool, unused, until the cache is destroyed.
    void evict(const std::string& key);

private:
    AllocatedImage upload_dds(const DDSTexture& dds);
    void create_fallback();
    void create_specteam_fallback();
    void create_normal_fallback();
    void create_cube_fallback();
    VkDescriptorSet allocate_and_write_descriptor(VkImageView view);

    const GPUTexture* finalize_load(const std::string& path,
                                    const std::vector<char>& file_data);

    std::unordered_map<std::string, std::unique_ptr<GPUTexture>> cache_;
    std::unordered_set<std::string> failed_;

    struct AsyncLoad {
        std::string path;
        std::future<std::optional<std::vector<char>>> future;
    };
    std::vector<AsyncLoad> async_loads_;
    std::unordered_set<std::string> pending_;

    std::unordered_map<std::string, AllocatedImage> cubes_;
    AllocatedImage cube_fallback_{};

    GPUTexture fallback_{};
    GPUTexture specteam_fallback_{};
    GPUTexture normal_fallback_{};

    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE; // current (allocating)
    /// Exhausted pools, kept alive because their sets are still in use.
    std::vector<VkDescriptorPool> full_pools_;
    /// Sets allocated from descriptor_pool_; it is replaced before it would
    /// overflow (on Vulkan 1.0 allocating from a full pool is invalid usage).
    u32 sets_in_pool_ = 0;
    static constexpr u32 kSetsPerPool = 512;
    /// Create a fresh descriptor_pool_ (one texture = one set).
    bool create_descriptor_pool();
    VkDescriptorSetLayout ds_layout_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    vfs::VirtualFileSystem* vfs_ = nullptr;
};

} // namespace osc::renderer
