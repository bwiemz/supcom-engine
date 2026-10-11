#include "renderer/economy_overlay_renderer.hpp"

#include "core/color.hpp"
#include "renderer/camera.hpp"
#include "renderer/font_cache.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/vk_cmd.hpp"
#include "sim/world_snapshot.hpp"

#include <lua.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace osc::renderer {

namespace {

std::string string_field(lua_State* L, int t, const char* key) {
    lua_pushstring(L, key);
    lua_gettable(L, t);
    std::string out = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return out;
}

void number_field(lua_State* L, int t, const char* key, f32& out) {
    lua_pushstring(L, key);
    lua_gettable(L, t);
    if (lua_isnumber(L, -1)) {
        out = static_cast<f32>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
}

void color_field(lua_State* L, int t, const char* key, u32& out) {
    const std::string text = string_field(L, t, key);
    if (const auto c = decode_color(text)) {
        out = *c;
    }
}

std::array<f32, 4> channels(u32 argb) {
    return {static_cast<f32>(argb >> 16 & 0xFFu) / 255.0f,
            static_cast<f32>(argb >> 8 & 0xFFu) / 255.0f, static_cast<f32>(argb & 0xFFu) / 255.0f,
            static_cast<f32>(argb >> 24 & 0xFFu) / 255.0f};
}

} // namespace

std::string economy_rate_text(f32 per_second, bool trailing_space) {
    char buf[32];
    if (per_second < 10.0f && per_second > -10.0f) {
        std::snprintf(buf, sizeof(buf), trailing_space ? "%+4.1f " : "%+4.1f",
                      static_cast<double>(per_second));
    } else {
        std::snprintf(buf, sizeof(buf), trailing_space ? "%+4i " : "%+4i",
                      static_cast<int>(per_second));
    }
    return buf;
}

void EconomyOverlayRenderer::init(VkDevice /*device*/, VmaAllocator allocator) {
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = MAX_QUADS * sizeof(UIInstance);
    buf_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        VmaAllocationInfo info{};
        vmaCreateBuffer(allocator, &buf_info, &alloc_info, &instance_buf_[i].buffer,
                        &instance_buf_[i].allocation, &info);
        instance_mapped_[i] = info.pMappedData;
    }
}

void EconomyOverlayRenderer::destroy(VkDevice /*device*/, VmaAllocator allocator) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; ++i) {
        if (instance_buf_[i].buffer) {
            vmaDestroyBuffer(allocator, instance_buf_[i].buffer, instance_buf_[i].allocation);
        }
        instance_buf_[i] = {};
        instance_mapped_[i] = nullptr;
    }
}

void EconomyOverlayRenderer::load_params(lua_State* L, TextureCache& tex_cache) {
    params_loaded_ = true;
    params_ = Params{};
    if (!L) {
        return;
    }
    const int top = lua_gettop(L);
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, "/lua/ui/game/econoverlayparams.lua");
    if (lua_isfunction(L, -2) && lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
        lua_pushstring(L, "EconOverlayParams");
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            const int t = lua_gettop(L);
            color_field(L, t, "positiveColor", params_.positive);
            color_field(L, t, "negativeColor", params_.negative);
            params_.left = string_field(L, t, "leftTexture");
            params_.mid = string_field(L, t, "midTexture");
            params_.right = string_field(L, t, "rightTexture");
            params_.font = string_field(L, t, "fontName");
            f32 size = 0;
            number_field(L, t, "fontSize", size);
            params_.font_size = static_cast<i32>(size);
            number_field(L, t, "energyTopOffset", params_.energy_top);
            number_field(L, t, "massTopOffset", params_.mass_top);
        }
    }
    lua_settop(L, top);
    for (const std::string* path : {&params_.left, &params_.mid, &params_.right}) {
        if (!path->empty()) {
            (void)tex_cache.get_blocking(*path);
        }
    }
}

void EconomyOverlayRenderer::emit(VkDescriptorSet ds, f32 x0, f32 y0, f32 x1, f32 y1,
                                  std::array<f32, 4> uv, u32 argb) {
    if (quads_.size() >= MAX_QUADS || !ds) {
        return;
    }
    UIInstance inst{};
    inst.rect[0] = x0;
    inst.rect[1] = y0;
    inst.rect[2] = x1 - x0;
    inst.rect[3] = y1 - y0;
    std::copy(uv.begin(), uv.end(), inst.uv);
    const auto c = channels(argb);
    std::copy(c.begin(), c.end(), inst.color);
    if (groups_.empty() || groups_.back().ds != ds) {
        groups_.push_back({ds, static_cast<u32>(quads_.size()), 0});
    }
    ++groups_.back().count;
    quads_.push_back(inst);
}

void EconomyOverlayRenderer::emit_text(const std::string& text, f32 x, f32 baseline, u32 argb,
                                       FontCache& font_cache) {
    const FontAtlas* atlas = font_cache.get(params_.font, params_.font_size);
    if (!atlas) {
        return;
    }
    f32 cursor = x;
    for (const char ch : text) {
        const auto it = atlas->glyphs.find(static_cast<u32>(static_cast<u8>(ch)));
        if (it == atlas->glyphs.end()) {
            continue;
        }
        const GlyphInfo& g = it->second;
        if (g.width > 0 && g.height > 0) {
            const f32 gx = cursor + g.x_offset;
            const f32 gy = baseline + g.y_offset;
            emit(atlas->descriptor_set, gx, gy, gx + g.width, gy + g.height,
                 {g.u0, g.v0, g.u1, g.v1}, argb);
        }
        cursor += g.x_advance;
    }
}

void EconomyOverlayRenderer::update(const sim::FrameView& view, const Camera& camera,
                                    const std::array<f32, 16>& vp, i32 focus_army,
                                    const std::function<f32(const std::string&)>& fade_in_zoom,
                                    TextureCache& tex_cache, FontCache& font_cache, u32 viewport_w,
                                    u32 viewport_h, lua_State* L) {
    readouts_.clear();
    quads_.clear();
    groups_.clear();
    if (!enabled_ || focus_army < 0) {
        return;
    }
    if (!params_loaded_) {
        load_params(L, tex_cache);
    }
    const GPUTexture* left = params_.left.empty() ? nullptr : tex_cache.get(params_.left);
    const GPUTexture* mid = params_.mid.empty() ? nullptr : tex_cache.get(params_.mid);
    const GPUTexture* right = params_.right.empty() ? nullptr : tex_cache.get(params_.right);
    const FontAtlas* font = font_cache.get(params_.font, params_.font_size);
    if (!left || !mid || !right || !font) {
        return;
    }
    const f32 font_height = font->metrics.ascent + font->metrics.descent;
    const f32 sw = static_cast<f32>(viewport_w);
    const f32 sh = static_cast<f32>(viewport_h);
    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    camera.eye_position(ex, ey, ez);
    const auto dir = camera.direction();

    for (const sim::EntityRecord& e : view.entities()) {
        if (!e.is_unit || e.army != focus_army || e.is_dying) {
            continue;
        }
        const sim::Vector3& p = e.position;
        const f32 depth = (p.x - ex) * dir[0] + (p.y - ey) * dir[1] + (p.z - ez) * dir[2];
        if (depth >= fade_in_zoom(e.blueprint_id)) {
            continue;
        }
        const f32 energy = e.energy_produced - e.energy_requested;
        const f32 mass = e.mass_produced - e.mass_requested;
        if (energy == 0.0f && mass == 0.0f) {
            continue;
        }
        const f32 cx = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12];
        const f32 cy = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13];
        const f32 cw = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15];
        if (cw <= 0.001f) {
            continue;
        }
        const f32 sx = (cx / cw + 1.0f) * 0.5f * sw;
        const f32 sy = (cy / cw + 1.0f) * 0.5f * sh;
        if (sx < -64.0f || sx > sw + 64.0f || sy < -64.0f || sy > sh + 64.0f) {
            continue;
        }

        EconomyReadout r;
        r.unit_id = e.id;
        r.energy = economy_rate_text(energy, true);
        r.mass = economy_rate_text(mass, false);
        r.energy_color = energy < 0.0f ? params_.negative : params_.positive;
        r.mass_color = mass < 0.0f ? params_.negative : params_.positive;
        r.bar_width = std::max(font_cache.string_advance(params_.font, params_.font_size, r.energy),
                               font_cache.string_advance(params_.font, params_.font_size, r.mass));
        r.bar_height = static_cast<f32>(mid->height);
        r.bar_left = std::floor(sx - r.bar_width * 0.5f);
        r.bar_top = std::floor(sy - r.bar_height * 0.5f);
        const f32 bar_right = r.bar_left + r.bar_width;
        const f32 bar_bottom = r.bar_top + r.bar_height;
        constexpr std::array<f32, 4> kWhole = {0.0f, 0.0f, 1.0f, 1.0f};
        emit(mid->descriptor_set, r.bar_left, r.bar_top, bar_right, bar_bottom, kWhole,
             0xFFFFFFFFu);
        emit(left->descriptor_set, r.bar_left - static_cast<f32>(left->width), r.bar_top,
             r.bar_left, bar_bottom, kWhole, 0xFFFFFFFFu);
        emit(right->descriptor_set, bar_right, r.bar_top,
             bar_right + static_cast<f32>(right->width), bar_bottom, kWhole, 0xFFFFFFFFu);
        const f32 energy_base = std::floor(params_.energy_top) + font_height + r.bar_top;
        const f32 mass_base = std::floor(params_.mass_top) + font_height + r.bar_top;
        emit_text(r.energy, r.bar_left + 1.0f, energy_base + 1.0f, 0xFF000000u, font_cache);
        emit_text(r.energy, r.bar_left, energy_base, r.energy_color, font_cache);
        emit_text(r.mass, r.bar_left + 1.0f, mass_base + 1.0f, 0xFF000000u, font_cache);
        emit_text(r.mass, r.bar_left, mass_base, r.mass_color, font_cache);
        readouts_.push_back(std::move(r));
    }

    if (!quads_.empty() && instance_mapped_[fi_]) {
        std::memcpy(instance_mapped_[fi_], quads_.data(), quads_.size() * sizeof(UIInstance));
    }
}

void EconomyOverlayRenderer::render(VkCommandBuffer cmd, VkPipelineLayout layout, u32 viewport_w,
                                    u32 viewport_h) {
    if (quads_.empty()) {
        return;
    }
    f32 vp[2] = {static_cast<f32>(viewport_w), static_cast<f32>(viewport_h)};
    vkc::push_constants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32) * 2, vp);
    VkRect2D scissor{};
    scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    VkBuffer buf = instance_buf_[fi_].buffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);
    for (const Group& g : groups_) {
        vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &g.ds, 0,
                                  nullptr);
        vkc::draw(cmd, 6, g.count, 0, g.first);
    }
}

} // namespace osc::renderer
