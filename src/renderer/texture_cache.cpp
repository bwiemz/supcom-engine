#include "renderer/texture_cache.hpp"
#include "renderer/vk_cmd.hpp"
#include "renderer/dds_parser.hpp"
#include "renderer/gpu_uploads.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstring>

namespace osc::renderer {

namespace {

constexpr const char* kFailsToLoad = "/textures/engine/b_fails_to_load.dds";

} // namespace

void TextureCache::init(VkDevice device, VmaAllocator allocator, GpuUploads& uploads,
                        VkDescriptorSetLayout ds_layout, VkSampler sampler,
                        vfs::VirtualFileSystem* vfs) {
    device_ = device;
    allocator_ = allocator;
    uploads_ = &uploads;
    ds_layout_ = ds_layout;
    sampler_ = sampler;
    vfs_ = vfs;

    if (!create_descriptor_pool()) return;

    fallback_ = pixel_texture({255, 255, 255, 255});
    // 1x1 transparent black pixel (alpha=0 means "no team color here")
    specteam_fallback_ = pixel_texture({0, 0, 0, 0});
    // 1x1 flat-normal pixel, (0,0,1) in tangent space in both of FA's
    // encodings: RGB (terrain strata: x=R, y=G, z=B) and DXT5nm (meshes:
    // x=G, y=A, z derived).
    normal_fallback_ = pixel_texture({128, 128, 255, 128});
    create_cube_fallback();
    (void)get_blocking(kFailsToLoad);
}

GPUTexture TextureCache::pixel_texture(const std::array<u8, 4>& rgba) {
    DDSTexture dds;
    dds.format = VK_FORMAT_R8G8B8A8_UNORM;
    dds.width = 1;
    dds.height = 1;
    dds.mip_count = 1;
    dds.mips.push_back({reinterpret_cast<const char*>(rgba.data()), 1, 1, 4});
    GPUTexture texture;
    texture.image = upload_dds(dds);
    if (texture.image.view) {
        texture.descriptor_set = allocate_and_write_descriptor(texture.image.view);
        texture.width = 1;
        texture.height = 1;
    }
    return texture;
}

AllocatedImage TextureCache::upload_dds(const DDSTexture& dds) {
    AllocatedImage result{};

    // Total staging size for all mip levels
    VkDeviceSize total_size = 0;
    for (auto& mip : dds.mips)
        total_size += mip.size;

    // Create VkImage
    VkImageCreateInfo img_ci{};
    img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img_ci.imageType = VK_IMAGE_TYPE_2D;
    img_ci.format = dds.format;
    img_ci.extent = {dds.width, dds.height, 1};
    img_ci.mipLevels = dds.mip_count;
    img_ci.arrayLayers = dds.faces;
    if (dds.faces == 6) img_ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo img_alloc{};
    img_alloc.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(allocator_, &img_ci, &img_alloc,
                       &result.image, &result.allocation,
                       nullptr) != VK_SUCCESS) {
        spdlog::warn("TextureCache: image creation failed");
        return {};
    }

    const GpuUploads::Staging staging = uploads_->stage(total_size);
    VkCommandBuffer cmd = staging.buffer ? uploads_->commands() : VK_NULL_HANDLE;
    if (!cmd) {
        spdlog::warn("TextureCache: staging buffer creation failed");
        vmaDestroyImage(allocator_, result.image, result.allocation);
        return {};
    }
    size_t offset = 0;
    for (const auto& mip : dds.mips) {
        std::memcpy(static_cast<char*>(staging.mapped) + offset, mip.data, mip.size);
        offset += mip.size;
    }

    // Transition entire image UNDEFINED -> TRANSFER_DST
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = result.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = dds.mip_count;
    barrier.subresourceRange.layerCount = dds.faces;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy each face's mip levels from the staging buffer (face by face, as
    // they were packed)
    VkDeviceSize buf_offset = 0;
    for (u32 face = 0; face < dds.faces; face++) {
        for (u32 i = 0; i < dds.mip_count; i++) {
            const DDSMipLevel& mip = dds.mips[static_cast<size_t>(face) * dds.mip_count + i];
            VkBufferImageCopy region{};
            region.bufferOffset = buf_offset;
            region.bufferRowLength = 0;   // tightly packed
            region.bufferImageHeight = 0; // tightly packed
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = i;
            region.imageSubresource.baseArrayLayer = face;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {mip.width, mip.height, 1};

            vkCmdCopyBufferToImage(cmd, staging.buffer, result.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            buf_offset += mip.size;
        }
    }

    // Transition TRANSFER_DST -> SHADER_READ_ONLY
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);

    // Image view (all mip levels)
    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = result.image;
    view_ci.viewType = dds.faces == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = dds.format;
    if (dds.alpha_only) {
        // D3D's A8 reads as (0, 0, 0, a)
        view_ci.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                              VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
    }
    view_ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_ci.subresourceRange.baseMipLevel = 0;
    view_ci.subresourceRange.levelCount = dds.mip_count;
    view_ci.subresourceRange.layerCount = dds.faces;

    if (vkCreateImageView(device_, &view_ci, nullptr, &result.view) !=
        VK_SUCCESS) {
        spdlog::warn("TextureCache: image view creation failed");
        drop(result);
    }

    return result;
}

bool TextureCache::create_descriptor_pool() {
    sets_in_pool_ = 0;
    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = kSetsPerPool;

    VkDescriptorPoolCreateInfo pool_ci{};
    pool_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_ci.maxSets = kSetsPerPool;
    pool_ci.poolSizeCount = 1;
    pool_ci.pPoolSizes = &pool_size;
    if (vkCreateDescriptorPool(device_, &pool_ci, nullptr, &descriptor_pool_) !=
        VK_SUCCESS) {
        spdlog::error("TextureCache: failed to create descriptor pool");
        descriptor_pool_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

VkDescriptorSet TextureCache::allocate_and_write_descriptor(VkImageView view) {
    // A long game can load more textures than one pool holds. Move to a fresh
    // pool before this one would overflow, keeping the full one alive for its
    // sets. (Before, allocation failed and the texture drew as nothing.)
    if (sets_in_pool_ >= kSetsPerPool) {
        full_pools_.push_back(descriptor_pool_);
        descriptor_pool_ = VK_NULL_HANDLE;
        if (!create_descriptor_pool()) return VK_NULL_HANDLE;
        spdlog::debug("TextureCache: descriptor pool {} started",
                      full_pools_.size() + 1);
    }

    VkDescriptorSetAllocateInfo alloc_ci{};
    alloc_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_ci.descriptorPool = descriptor_pool_;
    alloc_ci.descriptorSetCount = 1;
    alloc_ci.pSetLayouts = &ds_layout_;

    VkDescriptorSet ds = VK_NULL_HANDLE;
    const VkResult res = vkAllocateDescriptorSets(device_, &alloc_ci, &ds);
    if (res == VK_SUCCESS) ++sets_in_pool_;
    if (res != VK_SUCCESS) {
        spdlog::error("TextureCache: descriptor set allocation failed ({})",
                      static_cast<int>(res));
        return VK_NULL_HANDLE;
    }

    VkDescriptorImageInfo img_info{};
    img_info.sampler = sampler_;
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
    return ds;
}

const GPUTexture* TextureCache::get_raw(const std::string& key,
                                        const std::vector<char>& raw_dds) {
    if (device_ == VK_NULL_HANDLE) return nullptr;

    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.get();

    if (failed_.count(key)) return nullptr;

    auto dds = parse_dds(raw_dds);
    if (!dds) {
        spdlog::debug("TextureCache: DDS parse failed for raw key '{}'", key);
        failed_.insert(key);
        return nullptr;
    }

    auto image = upload_dds(*dds);
    if (!image.image) {
        spdlog::debug("TextureCache: upload failed for raw key '{}'", key);
        failed_.insert(key);
        return nullptr;
    }

    auto ds = allocate_and_write_descriptor(image.view);
    if (ds == VK_NULL_HANDLE) {
        drop(image);
        failed_.insert(key);
        return nullptr;
    }

    auto gpu = std::make_unique<GPUTexture>();
    gpu->image = image;
    gpu->descriptor_set = ds;
    gpu->width = dds->width;
    gpu->height = dds->height;

    spdlog::debug("TextureCache: loaded raw '{}' ({}x{}, {} mips)",
                   key, dds->width, dds->height, dds->mip_count);

    auto* raw = gpu.get();
    cache_[key] = std::move(gpu);
    return raw;
}

const GPUTexture* TextureCache::upload_rgba(const std::string& key,
                                              const u8* pixels,
                                              u32 width, u32 height) {
    return upload_pixels(key, VK_FORMAT_R8G8B8A8_UNORM, pixels, 4, width, height);
}

const GPUTexture* TextureCache::upload_pixels(const std::string& key, VkFormat format,
                                              const void* pixels, u32 texel_bytes, u32 width,
                                              u32 height) {
    if (device_ == VK_NULL_HANDLE) return nullptr;

    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.get();

    DDSTexture dds;
    dds.format = format;
    dds.width = width;
    dds.height = height;
    dds.mip_count = 1;
    dds.mips.push_back(
        {static_cast<const char*>(pixels), width, height, width * height * texel_bytes});
    AllocatedImage image = upload_dds(dds);
    if (!image.image) return nullptr;

    auto ds = allocate_and_write_descriptor(image.view);
    if (ds == VK_NULL_HANDLE) {
        drop(image);
        return nullptr;
    }

    auto gpu = std::make_unique<GPUTexture>();
    gpu->image = image;
    gpu->descriptor_set = ds;
    gpu->width = width;
    gpu->height = height;

    auto* result = gpu.get();
    cache_[key] = std::move(gpu);
    return result;
}

const GPUTexture* TextureCache::finalize_load(const std::string& path,
                                              const std::vector<char>& file_data) {
    auto dds = parse_dds(file_data);
    if (!dds) {
        spdlog::debug("TextureCache: DDS parse failed for '{}'", path);
        failed_.insert(path);
        return nullptr;
    }
    if (dds->faces != 1) {
        // A cube's view can't stand in a 2D sampler: get_cube_blocking().
        spdlog::warn("TextureCache: '{}' is a cubemap, not a 2D texture", path);
        failed_.insert(path);
        return nullptr;
    }

    auto image = upload_dds(*dds);
    if (!image.image) {
        spdlog::debug("TextureCache: upload failed for '{}'", path);
        failed_.insert(path);
        return nullptr;
    }

    auto ds = allocate_and_write_descriptor(image.view);
    if (ds == VK_NULL_HANDLE) {
        drop(image);
        failed_.insert(path);
        return nullptr;
    }

    auto gpu = std::make_unique<GPUTexture>();
    gpu->image = image;
    gpu->descriptor_set = ds;
    gpu->width = dds->width;
    gpu->height = dds->height;

    spdlog::debug("TextureCache: loaded '{}' ({}x{}, {} mips)",
                   path, dds->width, dds->height, dds->mip_count);

    auto* raw = gpu.get();
    cache_[path] = std::move(gpu);
    return raw;
}

const GPUTexture* TextureCache::drawable(const std::string& vfs_path) {
    if (vfs_path.empty()) {
        return nullptr;
    }
    if (const GPUTexture* texture = get(vfs_path)) {
        return texture;
    }
    return failed_.count(vfs_path) > 0 ? get(kFailsToLoad) : nullptr;
}

const GPUTexture* TextureCache::get(const std::string& vfs_path) {
    if (device_ == VK_NULL_HANDLE) return nullptr;

    auto it = cache_.find(vfs_path);
    if (it != cache_.end()) return it->second.get();

    if (failed_.count(vfs_path) || pending_.count(vfs_path)) return nullptr;

    if (!vfs_) {
        failed_.insert(vfs_path);
        return nullptr;
    }

    // Launch async VFS read on a background thread
    pending_.insert(vfs_path);
    auto* vfs = vfs_;
    async_loads_.push_back({vfs_path,
        std::async(std::launch::async, [vfs, vfs_path]()
            -> std::optional<std::vector<char>> {
            return vfs->read_file(vfs_path);
        })
    });
    return nullptr;
}

bool TextureCache::exists(const std::string& vfs_path) const {
    return vfs_ && vfs_->file_exists(vfs_path);
}

const GPUTexture* TextureCache::get_blocking(const std::string& vfs_path) {
    if (device_ == VK_NULL_HANDLE) return nullptr;

    auto it = cache_.find(vfs_path);
    if (it != cache_.end()) return it->second.get();

    if (failed_.count(vfs_path)) return nullptr;

    // If already pending async, block until that specific future completes
    if (pending_.count(vfs_path)) {
        for (auto it2 = async_loads_.begin(); it2 != async_loads_.end(); ++it2) {
            if (it2->path == vfs_path) {
                auto file_data = it2->future.get(); // truly blocks
                auto path = std::move(it2->path);
                async_loads_.erase(it2);
                pending_.erase(path);
                if (file_data)
                    return finalize_load(path, *file_data);
                failed_.insert(path);
                return nullptr;
            }
        }
        return nullptr;
    }

    if (!vfs_) {
        failed_.insert(vfs_path);
        return nullptr;
    }

    auto file_data = vfs_->read_file(vfs_path);
    if (!file_data) {
        spdlog::debug("TextureCache: VFS read failed for '{}'", vfs_path);
        failed_.insert(vfs_path);
        return nullptr;
    }

    return finalize_load(vfs_path, *file_data);
}

void TextureCache::wait_reads() const {
    for (const auto& load : async_loads_) {
        load.future.wait();
    }
}

void TextureCache::flush_uploads() {
    for (auto it = async_loads_.begin(); it != async_loads_.end();) {
        // Check if the future is ready (non-blocking)
        auto status = it->future.wait_for(std::chrono::seconds(0));
        if (status != std::future_status::ready) {
            ++it;
            continue;
        }

        auto file_data = it->future.get();
        auto path = std::move(it->path);
        it = async_loads_.erase(it);
        pending_.erase(path);

        if (!file_data) {
            spdlog::debug("TextureCache: async VFS read failed for '{}'", path);
            failed_.insert(path);
        } else {
            finalize_load(path, *file_data);
        }
    }
}

void TextureCache::evict(const std::string& key) {
    failed_.erase(key);
    const auto it = cache_.find(key);
    if (it == cache_.end()) return;
    drop(it->second->image);
    cache_.erase(it);
}

void TextureCache::drop(AllocatedImage& image) {
    uploads_->finish();
    if (image.view) {
        vkDestroyImageView(device_, image.view, nullptr);
    }
    if (image.image) {
        vmaDestroyImage(allocator_, image.image, image.allocation);
    }
    image = {};
}

VkImageView TextureCache::get_cube_blocking(const std::string& vfs_path) {
    if (device_ == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    if (auto it = cubes_.find(vfs_path); it != cubes_.end()) return it->second.view;
    if (!vfs_) return VK_NULL_HANDLE;
    const auto file_data = vfs_->read_file(vfs_path);
    if (!file_data) {
        spdlog::warn("TextureCache: no cubemap '{}'", vfs_path);
        return VK_NULL_HANDLE;
    }
    const auto dds = parse_dds(*file_data);
    if (!dds || dds->faces != 6) {
        spdlog::warn("TextureCache: '{}' is not a cubemap", vfs_path);
        return VK_NULL_HANDLE;
    }
    AllocatedImage image = upload_dds(*dds);
    if (!image.image) return VK_NULL_HANDLE;
    spdlog::debug("TextureCache: loaded cubemap '{}' ({}x{}, {} mips)", vfs_path, dds->width,
                  dds->height, dds->mip_count);
    return cubes_.emplace(vfs_path, image).first->second.view;
}

void TextureCache::create_cube_fallback() {
    // A black 1x1 cube: nothing to reflect.
    static const u8 kBlack[4] = {0, 0, 0, 255};
    DDSTexture dds;
    dds.format = VK_FORMAT_R8G8B8A8_UNORM;
    dds.width = 1;
    dds.height = 1;
    dds.mip_count = 1;
    dds.faces = 6;
    for (u32 f = 0; f < 6; ++f)
        dds.mips.push_back({reinterpret_cast<const char*>(kBlack), 1, 1, 4});
    cube_fallback_ = upload_dds(dds);
}

void TextureCache::destroy(VkDevice device, VmaAllocator allocator) {
    // Drain all in-flight async loads before tearing down Vulkan resources
    for (auto& al : async_loads_)
        al.future.wait();
    async_loads_.clear();
    pending_.clear();
    if (uploads_) {
        uploads_->finish();
    }

    for (auto& [path, gpu] : cache_) {
        if (gpu->image.view)
            vkDestroyImageView(device, gpu->image.view, nullptr);
        if (gpu->image.image)
            vmaDestroyImage(allocator, gpu->image.image, gpu->image.allocation);
    }
    cache_.clear();
    failed_.clear();
    for (auto& [path, cube] : cubes_) {
        if (cube.view) vkDestroyImageView(device, cube.view, nullptr);
        if (cube.image) vmaDestroyImage(allocator, cube.image, cube.allocation);
    }
    cubes_.clear();
    if (cube_fallback_.view) vkDestroyImageView(device, cube_fallback_.view, nullptr);
    if (cube_fallback_.image)
        vmaDestroyImage(allocator, cube_fallback_.image, cube_fallback_.allocation);
    cube_fallback_ = {};

    // Fallbacks
    if (fallback_.image.view)
        vkDestroyImageView(device, fallback_.image.view, nullptr);
    if (fallback_.image.image)
        vmaDestroyImage(allocator, fallback_.image.image, fallback_.image.allocation);
    fallback_ = {};

    if (specteam_fallback_.image.view)
        vkDestroyImageView(device, specteam_fallback_.image.view, nullptr);
    if (specteam_fallback_.image.image)
        vmaDestroyImage(allocator, specteam_fallback_.image.image,
                        specteam_fallback_.image.allocation);
    specteam_fallback_ = {};

    if (normal_fallback_.image.view)
        vkDestroyImageView(device, normal_fallback_.image.view, nullptr);
    if (normal_fallback_.image.image)
        vmaDestroyImage(allocator, normal_fallback_.image.image,
                        normal_fallback_.image.allocation);
    normal_fallback_ = {};

    // Pools (free all descriptor sets)
    for (auto pool : full_pools_) vkDestroyDescriptorPool(device, pool, nullptr);
    full_pools_.clear();
    if (descriptor_pool_)
        vkDestroyDescriptorPool(device, descriptor_pool_, nullptr);
    descriptor_pool_ = VK_NULL_HANDLE;
}

} // namespace osc::renderer
