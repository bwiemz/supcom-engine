#include "renderer/strategic_icon_renderer.hpp"
#include "renderer/vk_cmd.hpp"
#include "renderer/army_colors.hpp"
#include "renderer/camera.hpp"
#include "renderer/minimap_renderer.hpp"
#include "renderer/recon_view.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/world_snapshot.hpp"

#include <lua.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace osc::renderer {

static void get_army_color(const sim::EntityRecord& entity, const sim::FrameView& view,
                            f32& r, f32& g, f32& b) {
    i32 army = entity.army;
    if (const sim::ArmyRecord* brain = view.cur() ? view.cur()->army(army) : nullptr) {
        if (brain->has_color) {
            r = brain->r / 255.0f;
            g = brain->g / 255.0f;
            b = brain->b / 255.0f;
            return;
        }
        if (army < 8) {
            r = ARMY_COLORS[army][0];
            g = ARMY_COLORS[army][1];
            b = ARMY_COLORS[army][2];
            return;
        }
    }
    r = g = b = 0.7f;
}

void StrategicIconRenderer::init(VkDevice /*device*/, VmaAllocator allocator) {
    VkBufferCreateInfo buf_info{};
    buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buf_info.size = MAX_ICON_QUADS * sizeof(UIInstance);
    buf_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo alloc_info{};
    alloc_info.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    alloc_info.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    alloc_info.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VmaAllocationInfo info{};
        vmaCreateBuffer(allocator, &buf_info, &alloc_info,
                        &instance_buf_[i].buffer, &instance_buf_[i].allocation, &info);
        instance_mapped_[i] = info.pMappedData;
    }
}

void StrategicIconRenderer::destroy(VkDevice /*device*/, VmaAllocator allocator) {
    for (u32 i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (instance_buf_[i].buffer)
            vmaDestroyBuffer(allocator, instance_buf_[i].buffer,
                             instance_buf_[i].allocation);
        instance_buf_[i] = {};
        instance_mapped_[i] = nullptr;
    }
}

// --- Procedural icon atlas generation ---

// Draw a filled circle centered in cell
static void draw_circle(u8* pixels, u32 atlas_w, u32 atlas_h,
                         u32 cx, u32 cy, u32 radius) {
    i32 r2 = static_cast<i32>(radius * radius);
    i32 icx = static_cast<i32>(cx);
    i32 icy = static_cast<i32>(cy);
    for (i32 dy = -static_cast<i32>(radius); dy <= static_cast<i32>(radius); dy++) {
        for (i32 dx = -static_cast<i32>(radius); dx <= static_cast<i32>(radius); dx++) {
            if (dx * dx + dy * dy <= r2) {
                i32 ipx = icx + dx;
                i32 ipy = icy + dy;
                if (ipx < 0 || ipy < 0 ||
                    static_cast<u32>(ipx) >= atlas_w ||
                    static_cast<u32>(ipy) >= atlas_h)
                    continue;
                u32 idx = (static_cast<u32>(ipy) * atlas_w + static_cast<u32>(ipx)) * 4;
                pixels[idx + 0] = 255;
                pixels[idx + 1] = 255;
                pixels[idx + 2] = 255;
                pixels[idx + 3] = 255;
            }
        }
    }
}

// Draw a filled convex polygon (scanline fill) - up to 8 vertices
static void draw_polygon(u8* pixels, u32 atlas_w, u32 atlas_h,
                          const f32 verts[][2], u32 vert_count) {
    // Find Y bounds
    f32 min_y = verts[0][1], max_y = verts[0][1];
    for (u32 i = 1; i < vert_count; i++) {
        if (verts[i][1] < min_y) min_y = verts[i][1];
        if (verts[i][1] > max_y) max_y = verts[i][1];
    }

    i32 iy_min = std::max(0, static_cast<i32>(min_y));
    i32 iy_max = std::min(static_cast<i32>(atlas_h) - 1,
                           static_cast<i32>(max_y));

    for (i32 y = iy_min; y <= iy_max; y++) {
        f32 fy = static_cast<f32>(y) + 0.5f;
        f32 x_min = 1e6f, x_max = -1e6f;

        for (u32 i = 0; i < vert_count; i++) {
            u32 j = (i + 1) % vert_count;
            f32 y0 = verts[i][1], y1 = verts[j][1];
            if ((y0 <= fy && y1 > fy) || (y1 <= fy && y0 > fy)) {
                f32 t = (fy - y0) / (y1 - y0);
                f32 x = verts[i][0] + t * (verts[j][0] - verts[i][0]);
                if (x < x_min) x_min = x;
                if (x > x_max) x_max = x;
            }
        }

        i32 ix_min = std::max(0, static_cast<i32>(x_min));
        i32 ix_max = std::min(static_cast<i32>(atlas_w) - 1,
                               static_cast<i32>(x_max));
        for (i32 x = ix_min; x <= ix_max; x++) {
            u32 idx = (static_cast<u32>(y) * atlas_w + static_cast<u32>(x)) * 4;
            pixels[idx + 0] = 255;
            pixels[idx + 1] = 255;
            pixels[idx + 2] = 255;
            pixels[idx + 3] = 255;
        }
    }
}

void StrategicIconRenderer::draw_icon_shape(u8* pixels, u32 atlas_w,
                                             u32 cell_x, u32 cell_y,
                                             u32 cell_size,
                                             StrategicIconType type) {
    f32 cx = static_cast<f32>(cell_x) + static_cast<f32>(cell_size) * 0.5f;
    f32 cy = static_cast<f32>(cell_y) + static_cast<f32>(cell_size) * 0.5f;
    f32 hs = static_cast<f32>(cell_size) * 0.4f; // half-size

    switch (type) {
    case StrategicIconType::Land: {
        // Diamond
        f32 verts[4][2] = {
            {cx, cy - hs}, {cx + hs, cy},
            {cx, cy + hs}, {cx - hs, cy}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts, 4);
        break;
    }
    case StrategicIconType::Air: {
        // Upward-pointing triangle (arrow)
        f32 verts[3][2] = {
            {cx, cy - hs},
            {cx + hs * 0.8f, cy + hs * 0.7f},
            {cx - hs * 0.8f, cy + hs * 0.7f}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts, 3);
        break;
    }
    case StrategicIconType::Naval: {
        // Downward chevron (boat-like)
        f32 verts[5][2] = {
            {cx - hs, cy - hs * 0.5f},
            {cx + hs, cy - hs * 0.5f},
            {cx + hs * 0.7f, cy + hs * 0.5f},
            {cx, cy + hs},
            {cx - hs * 0.7f, cy + hs * 0.5f}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts, 5);
        break;
    }
    case StrategicIconType::Engineer: {
        // Small diamond with circle (wrench-like composite)
        draw_circle(pixels, atlas_w, ATLAS_H,
                     static_cast<u32>(cx), static_cast<u32>(cy),
                     static_cast<u32>(hs * 0.45f));
        // Outer ring via larger circle minus inner
        // Just use a slightly larger filled circle for simplicity
        draw_circle(pixels, atlas_w, ATLAS_H,
                     static_cast<u32>(cx), static_cast<u32>(cy),
                     static_cast<u32>(hs * 0.8f));
        // Cut inner to make ring effect — draw inner as black
        // Actually, simpler: just draw a gear-like shape with small diamond + dots
        f32 verts[4][2] = {
            {cx, cy - hs * 0.5f}, {cx + hs * 0.5f, cy},
            {cx, cy + hs * 0.5f}, {cx - hs * 0.5f, cy}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts, 4);
        break;
    }
    case StrategicIconType::Commander: {
        // Star (8-pointed via two overlapping squares/diamonds)
        // Outer diamond
        f32 verts1[4][2] = {
            {cx, cy - hs}, {cx + hs, cy},
            {cx, cy + hs}, {cx - hs, cy}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts1, 4);
        // Rotated square overlay
        f32 s = hs * 0.65f;
        f32 verts2[4][2] = {
            {cx - s, cy - s}, {cx + s, cy - s},
            {cx + s, cy + s}, {cx - s, cy + s}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts2, 4);
        break;
    }
    case StrategicIconType::Structure: {
        // Square
        f32 s = hs * 0.8f;
        f32 verts[4][2] = {
            {cx - s, cy - s}, {cx + s, cy - s},
            {cx + s, cy + s}, {cx - s, cy + s}
        };
        draw_polygon(pixels, atlas_w, ATLAS_H, verts, 4);
        break;
    }
    case StrategicIconType::Generic:
    default: {
        // Circle
        draw_circle(pixels, atlas_w, ATLAS_H,
                     static_cast<u32>(cx), static_cast<u32>(cy),
                     static_cast<u32>(hs * 0.7f));
        break;
    }
    }
}

void StrategicIconRenderer::build_atlas(TextureCache& tex_cache) {
    std::vector<u8> pixels(ATLAS_W * ATLAS_H * 4, 0); // transparent black

    for (u32 i = 0; i < static_cast<u32>(StrategicIconType::COUNT); i++) {
        draw_icon_shape(pixels.data(), ATLAS_W,
                         i * ICON_CELL_SIZE, 0, ICON_CELL_SIZE,
                         static_cast<StrategicIconType>(i));
    }

    auto* tex = tex_cache.upload_rgba("__strategic_icon_atlas",
                                       pixels.data(), ATLAS_W, ATLAS_H);
    if (tex)
        atlas_ds_ = tex->descriptor_set;
}

// --- Unit classification ---

StrategicIconType StrategicIconRenderer::classify_unit(const sim::EntityRecord& unit) {
    switch (unit.icon) {
    case sim::IconClass::Commander: return StrategicIconType::Commander;
    case sim::IconClass::Engineer:  return StrategicIconType::Engineer;
    case sim::IconClass::Structure: return StrategicIconType::Structure;
    case sim::IconClass::Air:       return StrategicIconType::Air;
    case sim::IconClass::Naval:     return StrategicIconType::Naval;
    case sim::IconClass::Land:      return StrategicIconType::Land;
    case sim::IconClass::Generic:   break;
    }
    return StrategicIconType::Generic;
}

StrategicIconType StrategicIconRenderer::classify_blip(const sim::EntityRecord& unit) {
    if (!unit.is_mobile || unit.icon == sim::IconClass::Structure)
        return StrategicIconType::Structure;
    if (unit.icon == sim::IconClass::Air) return StrategicIconType::Air;
    if (unit.icon == sim::IconClass::Naval) return StrategicIconType::Naval;
    return StrategicIconType::Land;
}

// --- Rendering ---

bool StrategicIconRenderer::world_to_screen(f32 wx, f32 wy, f32 wz,
                                              const std::array<f32, 16>& vp,
                                              f32 sw, f32 sh,
                                              f32& out_x, f32& out_y) {
    f32 cx = vp[0]*wx + vp[4]*wy + vp[8]*wz  + vp[12];
    f32 cy = vp[1]*wx + vp[5]*wy + vp[9]*wz  + vp[13];
    f32 cw = vp[3]*wx + vp[7]*wy + vp[11]*wz + vp[15];

    if (cw <= 0.001f) return false;

    f32 ndc_x = cx / cw;
    f32 ndc_y = cy / cw;

    out_x = (ndc_x + 1.0f) * 0.5f * sw;
    out_y = (ndc_y + 1.0f) * 0.5f * sh;
    return true;
}

UIInstance StrategicIconRenderer::icon_quad(f32 x, f32 y, const GPUTexture& tex, f32 r, f32 g,
                                            f32 b) {
    // Centred at its own size (DrawStrategicIconQuad: the texture's width
    // and height, halved to radii).
    const f32 half_w = static_cast<f32>(tex.width >> 1);
    const f32 half_h = static_cast<f32>(tex.height >> 1);
    UIInstance inst{};
    inst.rect[0] = x - half_w;
    inst.rect[1] = y - half_h;
    inst.rect[2] = half_w * 2.0f;
    inst.rect[3] = half_h * 2.0f;
    inst.uv[0] = 0.0f;
    inst.uv[1] = 0.0f;
    inst.uv[2] = 1.0f;
    inst.uv[3] = 1.0f;
    inst.color[0] = r;
    inst.color[1] = g;
    inst.color[2] = b;
    inst.color[3] = -1.0f; // the UI shader's StrategicIconPS
    return inst;
}

void StrategicIconRenderer::emit_icon(f32 x, f32 y, const std::string& path, const GPUTexture& tex,
                                      f32 r, f32 g, f32 b) {
    if (quad_count_ >= MAX_ICON_QUADS) return;
    quads_.push_back(icon_quad(x, y, tex, r, g, b));
    quad_textures_.push_back(path);
    if (groups_.empty() || groups_.back().ds != tex.descriptor_set)
        groups_.push_back({tex.descriptor_set, quad_count_, 0});
    ++groups_.back().count;
    ++quad_count_;
}

namespace {

/// Lowercase ASCII (__blueprints keys and FA's paths are lowercase).
std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// t[key] of the table at `idx`, pushed (nil when `idx` isn't a table).
void push_field(lua_State* L, int idx, const char* key) {
    if (!lua_istable(L, idx)) {
        lua_pushnil(L);
        return;
    }
    lua_pushstring(L, key);
    lua_rawget(L, idx < 0 && idx > LUA_REGISTRYINDEX ? idx - 1 : idx);
}

std::string string_field(lua_State* L, int idx, const char* key) {
    push_field(L, idx, key);
    std::string out = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return out;
}

} // namespace

const StrategicIconRenderer::IconBlueprint&
StrategicIconRenderer::icon_blueprint(const std::string& id, lua_State* L) {
    static const IconBlueprint kNone;
    if (!L) return kNone; // nothing to read, and nothing to remember
    const std::string key = lowered(id);
    if (auto it = icon_blueprints_.find(key); it != icon_blueprints_.end()) return it->second;
    IconBlueprint& out = icon_blueprints_[key];

    const int top = lua_gettop(L);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const int bps = lua_gettop(L);
    push_field(L, bps, key.c_str());
    const int bp = lua_gettop(L);
    if (lua_istable(L, bp)) {
        // StrategicIconName names the four textures under the icon
        // directory (an absolute one is its own base).
        const std::string name = lowered(string_field(L, bp, "StrategicIconName"));
        if (!name.empty()) {
            const std::string base = name.front() == '/' ? name : kIconDirectory + name;
            out.rest = base + "_rest.dds";
            out.selected = base + "_selected.dds";
        }
        // Moho keeps the sort priority in a byte.
        push_field(L, bp, "StrategicIconSortPriority");
        if (lua_isnumber(L, -1))
            out.sort_priority = static_cast<u8>(static_cast<i64>(lua_tonumber(L, -1)) & 0xFF);
        lua_pop(L, 1);
        push_field(L, bp, "Air");
        push_field(L, -1, "CanFly");
        out.can_fly = lua_toboolean(L, -1) != 0;
        lua_pop(L, 2);
        // Its mesh blueprint's IconFadeInZoom (0 when it says none).
        push_field(L, bp, "Display");
        const int display = lua_gettop(L);
        const std::string mesh = lowered(string_field(L, display, "MeshBlueprint"));
        if (!mesh.empty()) push_field(L, bps, mesh.c_str());
        else push_field(L, display, "Mesh");
        push_field(L, -1, "IconFadeInZoom");
        if (lua_isnumber(L, -1)) out.fade_in_zoom = static_cast<f32>(lua_tonumber(L, -1));
    }
    lua_settop(L, top);
    return out;
}

const std::string& StrategicIconRenderer::underlay_texture(const std::string& name) {
    auto [it, made] = underlay_textures_.try_emplace(name);
    if (made) it->second = (name.front() == '/' ? name : kIconDirectory + name) + "_rest.dds";
    return it->second;
}

void StrategicIconRenderer::load_generic_icons(lua_State* L) {
    if (generic_loaded_) return;
    generic_loaded_ = true;
    // strategicIcons.lua's own paths, should the import fail.
    generic_structure_ = std::string(kIconDirectory) + "icon_structure_generic_rest.dds";
    generic_land_ = std::string(kIconDirectory) + "icon_land_generic_rest.dds";
    generic_naval_ = std::string(kIconDirectory) + "icon_ship_generic_rest.dds";
    generic_air_ = std::string(kIconDirectory) + "icon_fighter_generic_rest.dds";
    stunned_ = std::string(kIconDirectory) + "stunned_rest.dds";
    if (!L) return;
    const int top = lua_gettop(L);
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, "/lua/ui/game/strategicIcons.lua");
    if (lua_isfunction(L, -2) && lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
        const int module = lua_gettop(L);
        push_field(L, module, "GenericIcons");
        const int generic = lua_gettop(L);
        const auto take = [&](const char* key, std::string& into) {
            const std::string path = lowered(string_field(L, generic, key));
            if (!path.empty()) into = path;
        };
        take("Structure", generic_structure_);
        take("Land", generic_land_);
        take("Naval", generic_naval_);
        take("Air", generic_air_);
        push_field(L, module, "StunnedIcons");
        const std::string stunned = lowered(string_field(L, lua_gettop(L), "StunnedRest"));
        if (!stunned.empty()) stunned_ = stunned;
    }
    lua_settop(L, top);
}

void StrategicIconRenderer::preload(const std::vector<std::string>& blueprint_ids,
                                    TextureCache& tex_cache, lua_State* L) {
    load_generic_icons(L);
    for (const std::string* path :
         {&generic_structure_, &generic_land_, &generic_naval_, &generic_air_, &stunned_})
        (void)tex_cache.get_blocking(*path);
    for (const std::string& id : blueprint_ids) {
        const IconBlueprint& bp = icon_blueprint(id, L);
        if (bp.rest.empty()) continue;
        (void)tex_cache.get_blocking(bp.rest);
        (void)tex_cache.get_blocking(bp.selected);
    }
}

template <class Place>
StrategicIconRenderer::Runs
StrategicIconRenderer::collect(const sim::FrameView& view,
                               const std::unordered_set<u32>* selected_ids, TextureCache& tex_cache,
                               lua_State* L, bool fade, f32 cam_dist, f32 fade_cap, Place&& place) {
    Runs runs;

    // One unit's icon, into its run: the world's units, then the
    // player's remembered structures gone from it unseen (M215d).
    const auto one = [&](const sim::EntityRecord& entity) {
        if (!entity.is_unit || entity.is_being_built) return;
        const Sight sight = recon_ ? recon_->sight(entity) : Sight::Seen;
        if (!shows_icon(sight)) return;

        const IconBlueprint& bp = icon_blueprint(entity.blueprint_id, L);
        // A blip never seen has a generic icon; anything identified, its own.
        const bool identified = sight != Sight::Blip;
        if (identified && bp.rest.empty()) return;
        // A drawn mesh keeps its icon until the camera is out past the
        // mesh's IconFadeInZoom; a blip, having none, shows its icon at
        // any zoom.
        if (fade && shows_mesh(sight) && cam_dist < std::min(bp.fade_in_zoom, fade_cap)) return;

        f32 sx = 0;
        f32 sy = 0;
        if (!place(view.position(entity), sx, sy)) return;

        const bool selected = selected_ids && selected_ids->count(entity.id) > 0;
        const std::string* path = &bp.rest;
        if (!identified) {
            switch (classify_blip(entity)) {
            case StrategicIconType::Structure: path = &generic_structure_; break;
            case StrategicIconType::Air: path = &generic_air_; break;
            case StrategicIconType::Naval: path = &generic_naval_; break;
            default: path = &generic_land_; break;
            }
        } else if (selected) {
            path = &bp.selected;
        }
        const GPUTexture* tex = tex_cache.get(*path);
        if (!tex || tex->width == 0) return; // loading, or not there

        Icon icon;
        icon.x = std::floor(sx);
        icon.y = std::floor(sy);
        icon.path = path;
        icon.tex = tex;
        icon.stunned = entity.stunned;
        // An identified unit's underlay (the objectives' rings), beneath it
        if (identified && !entity.strategic_underlay.empty()) {
            icon.underlay_path = &underlay_texture(entity.strategic_underlay);
            icon.underlay = tex_cache.get(*icon.underlay_path);
        }
        if (identified) {
            get_army_color(entity, view, icon.r, icon.g, icon.b);
        } else {
            const auto [ur, ug, ub] = recon_->unidentified_rgb();
            icon.r = ur;
            icon.g = ug;
            icon.b = ub;
        }
        const size_t run = selected                                  ? 3
                           : bp.sort_priority < static_cast<u8>('A') ? 2
                           : bp.can_fly                              ? 1
                                                                     : 0;
        // A remembered structure maybe dead: its tint halved
        // (DarkenRgbPreserveAlpha, for intel's MaybeDead; M215d).
        if (recon_ && recon_->maybe_dead(entity.id)) {
            icon.r *= 0.5f;
            icon.g *= 0.5f;
            icon.b *= 0.5f;
        }
        runs[run].push_back(icon);
    };
    for (const sim::EntityRecord& entity : view.entities()) one(entity);
    if (recon_) {
        for (const sim::EntityRecord& ghost : recon_->ghosts()) one(ghost);
        for (const sim::EntityRecord& fake : recon_->fakes()) one(fake); // jammers' (M215e)
    }
    return runs;
}

template <class Emit>
void StrategicIconRenderer::emit_runs(const Runs& runs, TextureCache& tex_cache, Emit&& emit) {
    // The underlay at its own colour, the base icon tinted over it, the
    // stunned badge over that at its own colour (Moho's RenderUnitIcon).
    const GPUTexture* stunned = tex_cache.get(stunned_);
    for (const auto& run : runs) {
        for (const Icon& icon : run) {
            if (icon.underlay && icon.underlay->width > 0) {
                emit(icon.x, icon.y, *icon.underlay_path, *icon.underlay, 1.0f, 1.0f, 1.0f);
            }
            emit(icon.x, icon.y, *icon.path, *icon.tex, icon.r, icon.g, icon.b);
            if (icon.stunned && stunned && stunned->width > 0) {
                emit(icon.x, icon.y, stunned_, *stunned, 1.0f, 1.0f, 1.0f);
            }
        }
    }
}

bool StrategicIconRenderer::update(const sim::FrameView& view, const Camera& camera,
                                   const std::array<f32, 16>& vp_matrix,
                                   const std::unordered_set<u32>* selected_ids,
                                   TextureCache& tex_cache, u32 viewport_w, u32 viewport_h,
                                   lua_State* L) {
    quads_.clear();
    quad_textures_.clear();
    groups_.clear();
    quad_count_ = 0;
    load_generic_icons(L);

    const f32 cam_dist = camera.eye_distance();
    strategic_zoom_active_ = cam_dist >= ZOOM_THRESHOLD;
    if (!nis_icons_) return strategic_zoom_active_; // a NIS draws none
    const f32 sw = static_cast<f32>(viewport_w);
    const f32 sh = static_cast<f32>(viewport_h);
    // An icon fades in with the camera out past its mesh's IconFadeInZoom
    // (no further than 0.89 of the farthest zoom, as Moho caps it).
    const f32 fade_cap = camera.max_zoom() * 0.89f;

    const Runs runs =
        collect(view, selected_ids, tex_cache, L, !always_, cam_dist, fade_cap,
                [&](const sim::Vector3& pos, f32& sx, f32& sy) {
                    return world_to_screen(pos.x, pos.y, pos.z, vp_matrix, sw, sh, sx, sy) &&
                           sx >= -32.0f && sx <= sw + 32.0f && sy >= -32.0f && sy <= sh + 32.0f;
                });
    emit_runs(runs, tex_cache,
              [&](f32 x, f32 y, const std::string& path, const GPUTexture& tex, f32 r, f32 g,
                  f32 b) { emit_icon(x, y, path, tex, r, g, b); });

    // Upload to GPU
    if (!quads_.empty() && instance_mapped_[fi_]) {
        const u32 count = std::min(quad_count_, MAX_ICON_QUADS);
        std::memcpy(instance_mapped_[fi_], quads_.data(), count * sizeof(UIInstance));
    }
    return strategic_zoom_active_;
}

void StrategicIconRenderer::paint_map(const sim::FrameView& view, const MapArea& area, f32 map_w,
                                      f32 map_h, const std::unordered_set<u32>* selected_ids,
                                      TextureCache& tex_cache, lua_State* L,
                                      std::vector<UIQuad>& out) {
    load_generic_icons(L);
    if (!nis_icons_ || map_w <= 0.0f || map_h <= 0.0f) {
        return;
    }
    const Runs runs = collect(view, selected_ids, tex_cache, L, false, 0.0f, 0.0f,
                              [&](const sim::Vector3& pos, f32& sx, f32& sy) {
                                  const f32 nx = pos.x / map_w;
                                  const f32 nz = pos.z / map_h;
                                  sx = area.x + nx * area.w;
                                  sy = area.y + nz * area.h;
                                  return nx >= 0.0f && nx <= 1.0f && nz >= 0.0f && nz <= 1.0f;
                              });
    emit_runs(runs, tex_cache,
              [&](f32 x, f32 y, const std::string&, const GPUTexture& tex, f32 r, f32 g, f32 b) {
                  UIQuad q{};
                  q.inst = icon_quad(x, y, tex, r, g, b);
                  q.texture_ds = tex.descriptor_set;
                  out.push_back(q);
              });
}

void StrategicIconRenderer::render(VkCommandBuffer cmd, VkPipelineLayout layout, u32 viewport_w,
                                   u32 viewport_h) {
    if (quad_count_ == 0) return;

    f32 vp[2] = {static_cast<f32>(viewport_w), static_cast<f32>(viewport_h)};
    vkc::push_constants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(f32) * 2, vp);

    VkRect2D scissor{};
    scissor.extent = {viewport_w, viewport_h};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkBuffer buf = instance_buf_[fi_].buffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buf, &offset);

    // A run of icons per texture, in the order they were collected.
    for (const Group& g : groups_) {
        if (!g.ds || g.first >= MAX_ICON_QUADS) continue;
        const u32 count = std::min(g.count, MAX_ICON_QUADS - g.first);
        vkc::bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &g.ds, 0,
                                  nullptr);
        vkc::draw(cmd, 6, count, 0, g.first);
    }
}

} // namespace osc::renderer
