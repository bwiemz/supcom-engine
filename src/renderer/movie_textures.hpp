#pragma once

#include "core/types.hpp"
#include "renderer/vk_types.hpp"

#include <unordered_map>
#include <vector>

namespace osc::ui {
class UIControl;
class UIControlRegistry;
} // namespace osc::ui

namespace osc::renderer {

class TextureCache;

/// The textures Movie controls draw with: one per movie, holding its
/// current frame (Moho's CMovie texture sheet). A new frame is copied into
/// a staging buffer as the UI is built, and into the texture by the frame's
/// command buffer, before any render pass.
class MovieTextures {
public:
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

    void init(VkDevice device, VmaAllocator allocator, TextureCache* textures);
    void destroy();

    /// Follow the registry's movies: a texture for each loaded one, its new
    /// frame staged for frame slot `fi` (whose last use has finished).
    void prepare(const ui::UIControlRegistry& registry, u32 fi);
    /// Record the staged copies (outside a render pass).
    void record(VkCommandBuffer cmd);

    /// What `ctrl`'s movie draws with, or null (no movie, or no frame yet).
    VkDescriptorSet descriptor(const ui::UIControl* ctrl) const;

    /// Let go of every texture (the UI state is being replaced); they are
    /// freed once no frame in flight can use them.
    void forget();

    /// Textures held, for tests.
    size_t count() const { return entries_.size(); }

private:
    struct Entry {
        u64 movie_id = 0;
        u32 width = 0, height = 0;
        AllocatedImage image{};
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        AllocatedBuffer staging[FRAMES_IN_FLIGHT]{};
        void* mapped[FRAMES_IN_FLIGHT]{};
        u64 uploaded_serial = 0; ///< the frame in the image (0: none yet)
        u64 staged_serial = 0;   ///< the frame staged for this frame's copy
        u32 staged_slot = 0;
        bool shader_readable = false; ///< in SHADER_READ_ONLY layout
    };
    bool create(Entry& e, u32 width, u32 height);
    void retire(const Entry& e);
    void free_entry(Entry& e);

    VkDevice device_ = VK_NULL_HANDLE;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    TextureCache* textures_ = nullptr;
    std::unordered_map<const ui::UIControl*, Entry> entries_;
    /// Textures let go of, and the frame after which they may be freed.
    std::vector<std::pair<u64, Entry>> retired_;
    u64 frame_ = 0;
};

} // namespace osc::renderer
