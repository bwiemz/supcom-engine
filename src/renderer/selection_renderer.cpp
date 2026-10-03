#include "renderer/selection_renderer.hpp"

#include "renderer/camera.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/world_snapshot.hpp"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <cctype>
#include <cmath>

namespace osc::renderer {

namespace {

using sim::Vector3;

constexpr const char* kBracketsPlayer =
    "/textures/ui/common/game/selection/selection_brackets_player.dds";
constexpr const char* kBracketsHighlighted =
    "/textures/ui/common/game/selection/selection_brackets_player_highlighted.dds";
constexpr const char* kBracketsEnemy =
    "/textures/ui/common/game/selection/selection_brackets_enemy.dds";
constexpr const char* kBracketsNeutral =
    "/textures/ui/common/game/selection/selection_brackets_neutral.dds";
constexpr const char* kDragBox = "/textures/ui/common/game/selection/selection.dds";

/// ren_SelectBracketSize (a blueprint without SelectionThickness),
/// ren_SelectBracketMinPixelSize
constexpr f32 kSelectBracketSize = 0.2f;
constexpr f32 kSelectBracketMinPixels = 3.0f;
/// The bracket texture's stroke, of a corner's quarter (12 of 64 pixels)
constexpr f32 kStrokeOfCorner = 12.0f / 64.0f;

Vector3 add(const Vector3& a, const Vector3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vector3 scale(const Vector3& v, f32 s) {
    return {v.x * s, v.y * s, v.z * s};
}

/// `v` flattened onto the ground, unit length (`fallback` when vertical)
Vector3 on_ground(const Vector3& v, const Vector3& fallback) {
    const f32 len = std::sqrt(v.x * v.x + v.z * v.z);
    return len > 1e-4f ? Vector3{v.x / len, 0.0f, v.z / len} : fallback;
}

f32 number_field(lua_State* L, int table, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, table);
    const f32 v = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
    lua_pop(L, 1);
    return v;
}

} // namespace

f32 bracket_corner(const SelectionBox& box, f32 thickness, f32 min_px, f32 px_world) {
    const f32 half = std::min(box.half_x, box.half_z);
    return std::min(std::max(thickness * half, min_px * px_world / kStrokeOfCorner), half);
}

std::array<std::array<Vector3, 4>, 4> bracket_quads(const SelectionBox& box, f32 corner) {
    const std::array<std::array<f32, 2>, 4> signs = {{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    std::array<std::array<Vector3, 4>, 4> out{};
    for (size_t i = 0; i < 4; ++i) {
        const f32 sx = signs[i][0];
        const f32 sz = signs[i][1];
        const Vector3 outer = add(box.center, add(scale(box.right, sx * box.half_x),
                                                  scale(box.forward, sz * box.half_z)));
        const Vector3 along_x = scale(box.right, -sx * corner);
        const Vector3 along_z = scale(box.forward, -sz * corner);
        out[i] = {outer, add(outer, along_x), add(add(outer, along_x), along_z),
                  add(outer, along_z)};
    }
    return out;
}

void SelectionRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
                             VkDescriptorSetLayout texture_ds_layout) {
    batch_.init(device, allocator, render_pass, texture_ds_layout, MAX_QUADS, "SelectionRenderer");
}

const SelectionRenderer::BoxBlueprint& SelectionRenderer::box_blueprint(const std::string& id,
                                                                        lua_State* L) {
    std::string key = id;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (auto it = box_blueprints_.find(key); it != box_blueprints_.end()) {
        return it->second;
    }
    BoxBlueprint& out = box_blueprints_[key];
    if (!L) {
        return out;
    }
    const int top = lua_gettop(L);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, key.c_str());
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            const int bp = lua_gettop(L);
            out.size_x = number_field(L, bp, "SelectionSizeX");
            out.size_z = number_field(L, bp, "SelectionSizeZ");
            out.offset_x = number_field(L, bp, "SelectionCenterOffsetX");
            out.offset_z = number_field(L, bp, "SelectionCenterOffsetZ");
            out.thickness = number_field(L, bp, "SelectionThickness");
        }
    }
    lua_settop(L, top);
    return out;
}

void SelectionRenderer::update(const sim::FrameView& view, const Camera& camera, u32 viewport_h,
                               const std::unordered_set<u32>* selected, u32 hovered,
                               i32 player_army, const std::optional<std::array<Vector3, 4>>& drag,
                               TextureCache& tex_cache, lua_State* L, u32 fi) {
    brackets_.clear();
    drew_drag_box_ = false;
    std::vector<WorldQuadBatch::Quad> quads;
    const sim::WorldSnapshot* cur = view.cur();
    if (!enabled_ || !cur || !batch_.ready(fi)) {
        batch_.upload(quads, fi);
        return;
    }
    const sim::ArmyRecord* me = cur->army(player_army);
    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    camera.eye_position(ex, ey, ez);
    // The world's size of a pixel at distance d: 2 d tan(fov/2) / height
    const f32 per_px =
        viewport_h > 0 ? 2.0f * std::tan(camera.fov() * 0.5f) / static_cast<f32>(viewport_h) : 0.0f;
    const auto texture_for = [&](const sim::EntityRecord& e, bool is_selected) {
        if (is_selected) {
            return kBracketsPlayer;
        }
        if (e.army == player_army) {
            return kBracketsHighlighted;
        }
        const bool allied = me && e.army >= 0 && (me->allies & (1u << e.army)) != 0;
        return allied || e.army < 0 ? kBracketsNeutral : kBracketsEnemy;
    };
    const auto vertex = [](const Vector3& p, f32 u, f32 v) {
        return WorldQuadBatch::Vertex{{p.x, p.y, p.z}, {u, v}, {1.0f, 1.0f, 1.0f, 1.0f}};
    };
    const auto add_brackets = [&](u32 id, bool is_selected) {
        const sim::EntityRecord* e = view.find(id);
        if (!e || !e->is_unit) {
            return;
        }
        const BoxBlueprint& bp = box_blueprint(e->blueprint_id, L);
        const sim::Quaternion q = view.orientation(*e);
        SelectionBox box;
        box.right = on_ground(sim::quat_rotate(q, {1, 0, 0}), {1, 0, 0});
        box.forward = on_ground(sim::quat_rotate(q, {0, 0, 1}), {0, 0, 1});
        box.half_x = bp.size_x > 0 ? bp.size_x : 1.0f;
        box.half_z = bp.size_z > 0 ? bp.size_z : 1.0f;
        box.center = add(view.position(*e),
                         add(scale(box.right, bp.offset_x), scale(box.forward, bp.offset_z)));
        const char* texture = texture_for(*e, is_selected);
        const GPUTexture* tex = tex_cache.get(texture);
        if (!tex) {
            return;
        }
        const f32 dx = box.center.x - ex;
        const f32 dy = box.center.y - ey;
        const f32 dz = box.center.z - ez;
        const f32 px_world = per_px * std::sqrt(dx * dx + dy * dy + dz * dz);
        const f32 corner = bracket_corner(box, bp.thickness > 0 ? bp.thickness : kSelectBracketSize,
                                          kSelectBracketMinPixels, px_world);
        brackets_.push_back({id, texture, box, corner});
        // Each corner takes one quarter of the texture, its outer corner at
        // the texture's
        const std::array<std::array<f32, 2>, 4> uv_outer = {{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        const auto corners = bracket_quads(box, corner);
        for (size_t i = 0; i < 4; ++i) {
            const f32 u0 = uv_outer[i][0];
            const f32 v0 = uv_outer[i][1];
            const auto& c = corners[i];
            const auto a = vertex(c[0], u0, v0);
            const auto b = vertex(c[1], 0.5f, v0);
            const auto m = vertex(c[2], 0.5f, 0.5f);
            const auto d = vertex(c[3], u0, 0.5f);
            quads.push_back({tex->descriptor_set, {a, b, m, a, m, d}});
        }
    };
    if (selected) {
        for (u32 id : *selected) {
            add_brackets(id, true);
        }
    }
    if (hovered != 0 && !(selected && selected->count(hovered))) {
        add_brackets(hovered, false);
    }
    if (drag) {
        if (const GPUTexture* tex = tex_cache.get(kDragBox)) {
            const auto& c = *drag;
            const auto a = vertex(c[0], 0, 0);
            const auto b = vertex(c[1], 1, 0);
            const auto m = vertex(c[2], 1, 1);
            const auto d = vertex(c[3], 0, 1);
            quads.push_back({tex->descriptor_set, {a, b, m, a, m, d}});
            drew_drag_box_ = true;
        }
    }
    batch_.upload(quads, fi);
}

void SelectionRenderer::render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h,
                               const f32* view_proj, u32 fi) const {
    batch_.render(cmd, viewport_w, viewport_h, view_proj, fi);
}

void SelectionRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    batch_.destroy(device, allocator);
}

} // namespace osc::renderer
