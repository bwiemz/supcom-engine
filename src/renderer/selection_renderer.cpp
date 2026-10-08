#include "renderer/selection_renderer.hpp"

#include "renderer/camera.hpp"
#include "renderer/mesh_cache.hpp"
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

/// /lua/RenderSelectParams.lua
constexpr f32 kSelectionSizeFudge = 1.85f;
constexpr f32 kSelectionHeightFudge = 0.12f;
constexpr f32 kUnitSelectionScale = 0.75f;
constexpr f32 kSelectBracketMinPixels = 3.0f;
constexpr f32 kSelectBracketSize = 0.2f;

Vector3 add(const Vector3& a, const Vector3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vector3 scale(const Vector3& v, f32 s) {
    return {v.x * s, v.y * s, v.z * s};
}

f32 number_field(lua_State* L, int table, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, table);
    const f32 v = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : 0.0f;
    lua_pop(L, 1);
    return v;
}

} // namespace

SelectionBox selection_box(const SelectionBlueprint& bp, const Vector3& mesh_min,
                           const Vector3& mesh_max, const Vector3& position,
                           const sim::Quaternion& orientation) {
    const Vector3 centre = sim::quat_rotate(orientation, scale(add(mesh_min, mesh_max), 0.5f));
    const Vector3 offset = sim::quat_rotate(orientation, bp.offset);
    SelectionBox box;
    box.right = sim::quat_rotate(orientation, {1, 0, 0});
    box.forward = sim::quat_rotate(orientation, {0, 0, 1});
    box.half_x = bp.size_x > 0 ? bp.size_x * kUnitSelectionScale
                               : kSelectionSizeFudge * 0.5f * (mesh_max.x - mesh_min.x);
    box.half_z = bp.size_z > 0 ? bp.size_z * kUnitSelectionScale
                               : kSelectionSizeFudge * 0.5f * (mesh_max.z - mesh_min.z);
    box.center = {position.x + centre.x + offset.x, position.y + kSelectionHeightFudge + offset.y,
                  position.z + centre.z + offset.z};
    return box;
}

f32 bracket_corner(const SelectionBox& box, f32 thickness, f32 min_px, f32 px_world) {
    return std::max(std::max(box.half_x, box.half_z) * thickness, min_px * px_world);
}

std::array<std::array<Vector3, 4>, 4> bracket_quads(const SelectionBox& box, f32 corner) {
    const std::array<std::array<f32, 2>, 4> signs = {{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}};
    std::array<std::array<Vector3, 4>, 4> out{};
    for (size_t i = 0; i < 4; ++i) {
        const Vector3 at = add(box.center, add(scale(box.right, signs[i][0] * box.half_x),
                                               scale(box.forward, signs[i][1] * box.half_z)));
        for (size_t k = 0; k < 4; ++k) {
            out[i][k] = add(at, add(scale(box.right, signs[k][0] * corner),
                                    scale(box.forward, signs[k][1] * corner)));
        }
    }
    return out;
}

void SelectionRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
                             VkDescriptorSetLayout texture_ds_layout) {
    batch_.init(device, allocator, render_pass, texture_ds_layout, MAX_QUADS, "SelectionRenderer");
}

const SelectionBlueprint& SelectionRenderer::box_blueprint(const std::string& id, lua_State* L) {
    std::string key = id;
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (auto it = box_blueprints_.find(key); it != box_blueprints_.end()) {
        return it->second;
    }
    SelectionBlueprint& out = box_blueprints_[key];
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
            out.offset = {number_field(L, bp, "SelectionCenterOffsetX"),
                          number_field(L, bp, "SelectionCenterOffsetY"),
                          number_field(L, bp, "SelectionCenterOffsetZ")};
            out.thickness = number_field(L, bp, "SelectionThickness");
        }
    }
    lua_settop(L, top);
    return out;
}

void SelectionRenderer::update(const sim::FrameView& view, const Camera& camera, u32 viewport_h,
                               const std::unordered_set<u32>* selected, u32 hovered,
                               i32 player_army, const std::optional<std::array<Vector3, 4>>& drag,
                               TextureCache& tex_cache, MeshCache& mesh_cache, lua_State* L,
                               u32 fi) {
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
        const SelectionBlueprint& bp = box_blueprint(e->blueprint_id, L);
        Vector3 mesh_min;
        Vector3 mesh_max;
        if (const GPUMesh* mesh = mesh_cache.get(e->blueprint_id, L)) {
            const Vector3 s = {e->scale_x * mesh->uniform_scale, e->scale_y * mesh->uniform_scale,
                               e->scale_z * mesh->uniform_scale};
            mesh_min = {s.x * mesh->bounds_min.x, s.y * mesh->bounds_min.y,
                        s.z * mesh->bounds_min.z};
            mesh_max = {s.x * mesh->bounds_max.x, s.y * mesh->bounds_max.y,
                        s.z * mesh->bounds_max.z};
        }
        const Vector3 position = view.position(*e);
        const SelectionBox box =
            selection_box(bp, mesh_min, mesh_max, position, view.orientation(*e));
        const char* texture = texture_for(*e, is_selected);
        const GPUTexture* tex = tex_cache.get(texture);
        if (!tex) {
            return;
        }
        const f32 dx = position.x - ex;
        const f32 dy = position.y - ey;
        const f32 dz = position.z - ez;
        const f32 px_world = per_px * std::sqrt(dx * dx + dy * dy + dz * dz);
        const f32 thickness = std::abs(bp.thickness) >= 1e-5f ? bp.thickness : kSelectBracketSize;
        const f32 corner = bracket_corner(box, thickness, kSelectBracketMinPixels, px_world);
        brackets_.push_back({id, texture, box, corner});
        const std::array<std::array<f32, 2>, 4> quarter = {
            {{0, 0}, {0.5f, 0}, {0.5f, 0.5f}, {0, 0.5f}}};
        const auto tiles = bracket_quads(box, corner);
        for (size_t i = 0; i < 4; ++i) {
            const f32 u = quarter[i][0];
            const f32 v = quarter[i][1];
            const auto& c = tiles[i];
            const auto a = vertex(c[0], u, v);
            const auto b = vertex(c[1], u + 0.5f, v);
            const auto m = vertex(c[2], u + 0.5f, v + 0.5f);
            const auto d = vertex(c[3], u, v + 0.5f);
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
