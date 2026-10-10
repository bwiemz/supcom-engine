#pragma once

#include "renderer/ui_renderer.hpp" // UIInstance
#include "renderer/vk_types.hpp"
#include "core/types.hpp"

#include <array>
#include <functional>
#include <string>
#include <vector>

struct lua_State;

namespace osc::sim {
class FrameView;
}

namespace osc::renderer {

class Camera;
class FontCache;
class TextureCache;

std::string economy_rate_text(f32 per_second, bool trailing_space);

struct EconomyReadout {
    u32 unit_id = 0;
    f32 bar_left = 0, bar_top = 0, bar_width = 0, bar_height = 0;
    std::string energy, mass;
    u32 energy_color = 0, mass_color = 0;
};

/// RenderOverlayEconomy's readout, as Moho's CWldSession::DrawEconomyOverlay
/// draws it (/lua/ui/game/econoverlayparams.lua).
class EconomyOverlayRenderer {
public:
    void init(VkDevice device, VmaAllocator allocator);
    void destroy(VkDevice device, VmaAllocator allocator);

    void set_enabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_; }
    void forget_params() { params_loaded_ = false; }

    void update(const sim::FrameView& view, const Camera& camera,
                const std::array<f32, 16>& vp_matrix, i32 focus_army,
                const std::function<f32(const std::string&)>& fade_in_zoom, TextureCache& tex_cache,
                FontCache& font_cache, u32 viewport_w, u32 viewport_h, lua_State* L);
    void render(VkCommandBuffer cmd, VkPipelineLayout layout, u32 viewport_w, u32 viewport_h);

    void set_frame_index(u32 fi) { fi_ = fi; }
    const std::vector<EconomyReadout>& readouts() const { return readouts_; }
    u32 quad_count() const { return static_cast<u32>(quads_.size()); }

    static constexpr u32 MAX_QUADS = 4096;
    static constexpr u32 FRAMES_IN_FLIGHT = 2;

private:
    struct Params {
        u32 positive = 0, negative = 0;
        std::string left, mid, right;
        std::string font;
        i32 font_size = 0;
        f32 energy_top = 0, mass_top = 0;
    };
    void load_params(lua_State* L, TextureCache& tex_cache);
    void emit(VkDescriptorSet ds, f32 x0, f32 y0, f32 x1, f32 y1, std::array<f32, 4> uv, u32 argb);
    void emit_text(const std::string& text, f32 x, f32 baseline, u32 argb, FontCache& font_cache);

    bool enabled_ = false;
    bool params_loaded_ = false;
    Params params_;
    std::vector<EconomyReadout> readouts_;
    std::vector<UIInstance> quads_;
    struct Group {
        VkDescriptorSet ds = VK_NULL_HANDLE;
        u32 first = 0, count = 0;
    };
    std::vector<Group> groups_;
    AllocatedBuffer instance_buf_[FRAMES_IN_FLIGHT] = {};
    void* instance_mapped_[FRAMES_IN_FLIGHT] = {};
    u32 fi_ = 0;
};

} // namespace osc::renderer
