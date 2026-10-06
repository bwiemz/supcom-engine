#include "renderer/command_graph_renderer.hpp"
#include "renderer/vk_cmd.hpp"

#include "core/color.hpp"
#include "renderer/camera.hpp"
#include "renderer/shader_utils.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/collision.hpp"
#include "sim/world_snapshot.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace osc::renderer {

namespace {

using sim::Vector3;

Vector3 add(const Vector3& a, const Vector3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vector3 sub(const Vector3& a, const Vector3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vector3 scale(const Vector3& a, f32 s) {
    return {a.x * s, a.y * s, a.z * s};
}
f32 length(const Vector3& a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

/// Pushes key `key` of CommandGraphParams' entry `name`, as the table's
/// header has it found: the entry's own, else its inherit_from's, else
/// default's; nil without.
void push_param(lua_State* L, int params, const std::string& name, const char* key, int depth) {
    lua_pushstring(L, name.c_str());
    lua_gettable(L, params);
    const int entry = lua_gettop(L);
    if (lua_istable(L, entry)) {
        lua_pushstring(L, key);
        lua_gettable(L, entry);
        if (!lua_isnil(L, -1)) {
            lua_replace(L, entry);
            return;
        }
        lua_pop(L, 1);
        lua_pushstring(L, "inherit_from");
        lua_gettable(L, entry);
        if (lua_type(L, -1) == LUA_TSTRING && depth < 8) {
            const std::string parent = lua_tostring(L, -1);
            lua_pop(L, 2);
            push_param(L, params, parent, key, depth + 1);
            return;
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (name != "default") {
        push_param(L, params, "default", key, depth + 1);
    } else {
        lua_pushnil(L);
    }
}

std::string param_string(lua_State* L, int params, const std::string& name, const char* key) {
    push_param(L, params, name, key, 0);
    std::string out = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return out;
}

f32 param_number(lua_State* L, int params, const std::string& name, const char* key, f32 fallback) {
    push_param(L, params, name, key, 0);
    const f32 out = lua_isnumber(L, -1) ? static_cast<f32>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return out;
}

std::array<f32, 4> rgba(u32 argb) {
    return {static_cast<f32>((argb >> 16) & 0xff) / 255.0f,
            static_cast<f32>((argb >> 8) & 0xff) / 255.0f, static_cast<f32>(argb & 0xff) / 255.0f,
            static_cast<f32>((argb >> 24) & 0xff) / 255.0f};
}

std::array<f32, 4> param_color(lua_State* L, int params, const std::string& name, const char* key) {
    const auto argb = decode_color(param_string(L, params, name, key));
    return argb ? rgba(*argb) : std::array<f32, 4>{1, 1, 1, 1};
}

/// The default entry as retail's CommandGraphParams has it, for a game
/// without the table
CommandGraphStyle fallback_style() {
    CommandGraphStyle s;
    s.line_texture = "/textures/ui/common/game/orderline/orderline_generic.dds";
    s.line_color = rgba(0x3300ffffu);
    s.line_selected_color = rgba(0xdd00ffffu);
    s.waypoint_color = rgba(0x44ffffffu);
    s.waypoint_selected_color = rgba(0x88ffffffu);
    s.waypoint_highlight_color = rgba(0xffffffffu);
    return s;
}

constexpr std::array<sim::CommandType, 19> kDrawnTypes = {
    sim::CommandType::Move,           sim::CommandType::Attack,
    sim::CommandType::Guard,          sim::CommandType::Patrol,
    sim::CommandType::BuildMobile,    sim::CommandType::Reclaim,
    sim::CommandType::Repair,         sim::CommandType::Capture,
    sim::CommandType::TransportLoad,  sim::CommandType::TransportUnload,
    sim::CommandType::Nuke,           sim::CommandType::Tactical,
    sim::CommandType::Overcharge,     sim::CommandType::Sacrifice,
    sim::CommandType::Teleport,       sim::CommandType::Ferry,
    sim::CommandType::Dock,           sim::CommandType::Script,
    sim::CommandType::AggressiveMove,
};

} // namespace

CommandGraphStyle command_graph_style(lua_State* L, int params, const std::string& name) {
    if (params < 0) params = lua_gettop(L) + params + 1;
    CommandGraphStyle s;
    s.line_texture = param_string(L, params, name, "orderline_texture");
    s.uv_aspect = param_number(L, params, name, "orderline_uv_aspect_ratio", 1.0f);
    s.anim_rate = param_number(L, params, name, "orderline_anim_rate", 0.0f);
    s.line_color = param_color(L, params, name, "orderline_color");
    s.line_selected_color = param_color(L, params, name, "orderline_selected_color");
    s.waypoint_texture = param_string(L, params, name, "waypoint_texture");
    s.waypoint_color = param_color(L, params, name, "waypoint_color");
    s.waypoint_selected_color = param_color(L, params, name, "waypoint_selected_color");
    s.waypoint_scale = param_number(L, params, name, "waypoint_scale", 1.0f);
    s.waypoint_selected_scale = param_number(L, params, name, "waypoint_selected_scale", 1.0f);
    s.waypoint_highlight_color = param_color(L, params, name, "waypoint_highlight_color");
    s.waypoint_highlight_scale = param_number(L, params, name, "waypoint_highlight_scale", 1.0f);
    return s;
}

std::array<f32, 4> build_pad(f32 x, f32 z, f32 size_x, f32 size_z, f32 skirt_x, f32 skirt_z,
                             f32 off_x, f32 off_z) {
    const f32 x0 = x - size_x * 0.5f + std::min(off_x, 0.0f);
    const f32 z0 = z - size_z * 0.5f + std::min(off_z, 0.0f);
    return {x0, z0, x0 + std::max(skirt_x, size_x), z0 + std::max(skirt_z, size_z)};
}

std::vector<std::pair<const sim::EntityRecord*, bool>>
command_graph_units(const sim::WorldSnapshot& world, const std::unordered_set<u32>* selected,
                    i32 player_army) {
    std::vector<std::pair<const sim::EntityRecord*, bool>> out;
    for (const sim::EntityRecord& e : world.entities) {
        const bool chosen = selected && selected->count(e.id) > 0;
        if (e.is_unit && (chosen || e.army == player_army)) {
            out.emplace_back(&e, chosen);
        }
    }
    return out;
}

bool build_started(const sim::WorldSnapshot& world, const sim::EntityRecord& builder, size_t index,
                   const sim::CommandRecord& order) {
    if (index == 0 && !order.pending && builder.build_target_id != 0) {
        return true;
    }
    for (const auto& e : world.entities) {
        if (e.is_unit && e.army == builder.army && e.blueprint_id == order.blueprint_id &&
            std::abs(e.position.x - order.target_pos.x) < 0.5f &&
            std::abs(e.position.z - order.target_pos.z) < 0.5f) {
            return true;
        }
    }
    return false;
}

std::vector<PlannedSite> planned_build_sites(const sim::WorldSnapshot& world,
                                             const std::unordered_set<u32>* selected,
                                             i32 player_army) {
    std::vector<PlannedSite> sites;
    for (const auto& [e, chosen] : command_graph_units(world, selected, player_army)) {
        const auto orders = world.orders_of(*e);
        for (size_t i = 0; i < orders.size(); ++i) {
            if (orders[i].type == sim::CommandType::BuildMobile &&
                !orders[i].blueprint_id.empty() && !build_started(world, *e, i, orders[i])) {
                sites.push_back({orders[i].blueprint_id, orders[i].target_pos});
            }
        }
    }
    return sites;
}

std::vector<CommandGraphPath>
command_graph_paths(const sim::FrameView& view, const std::unordered_set<u32>* selected,
                    i32 player_army,
                    const std::function<const CommandGraphStyle*(sim::CommandType)>& style_of) {
    std::vector<CommandGraphPath> paths;
    const sim::WorldSnapshot* cur = view.cur();
    if (!cur) {
        return paths;
    }
    for (const auto& [e, chosen] : command_graph_units(*cur, selected, player_army)) {
        CommandGraphPath path;
        path.unit = e;
        path.chosen = chosen;
        path.chain.push_back(view.position(*e));
        std::vector<sim::CommandRecord> cmds(cur->orders_of(*e).begin(), cur->orders_of(*e).end());
        const auto rally = cur->rally_of(*e);
        cmds.insert(cmds.end(), rally.begin(), rally.end());
        for (size_t n = 0; n < cmds.size(); ++n) {
            const sim::CommandRecord& c = cmds[n];
            const CommandGraphStyle* s = style_of(c.type);
            if (!s) {
                continue;
            }
            Vector3 to = c.target_pos;
            if (c.target_id > 0) {
                if (const sim::EntityRecord* target = view.find(c.target_id)) {
                    to = view.position(*target);
                } else if (to.x == 0.0f && to.y == 0.0f && to.z == 0.0f) {
                    continue;
                }
            }
            path.chain.push_back(to);
            CommandGraphPath::Leg leg;
            leg.order = c;
            leg.index = n;
            leg.style = s;
            leg.line_color = chosen ? s->line_selected_color : s->line_color;
            path.legs.push_back(std::move(leg));
        }
        const auto anchor =
            std::find_if(path.legs.begin(), path.legs.end(), [](const CommandGraphPath::Leg& l) {
                return l.order.type == sim::CommandType::Patrol ||
                       l.order.type == sim::CommandType::Guard ||
                       (l.order.type == sim::CommandType::Attack && l.order.target_id == 0);
            });
        if (anchor != path.legs.end() && anchor + 1 != path.legs.end()) {
            const auto at = static_cast<size_t>(anchor - path.legs.begin());
            CommandGraphPath::Leg back = *anchor;
            back.closes = true;
            path.chain.push_back(path.chain[at + 1]);
            path.legs.push_back(std::move(back));
        }
        paths.push_back(std::move(path));
    }
    return paths;
}

std::vector<CommandGraphNode> command_graph_nodes(const std::vector<CommandGraphPath>& paths,
                                                  u32 highlight, u32 hovered_unit) {
    std::vector<CommandGraphNode> nodes;
    std::vector<Vector3> sums;
    std::vector<u32> counts;
    std::vector<std::set<u64>> ins;
    std::vector<std::set<u64>> outs;
    std::unordered_map<u64, size_t> index;
    u64 unnumbered = 0;
    for (const CommandGraphPath& path : paths) {
        std::vector<u64> keys = {(u64{2} << 32) | path.unit->id};
        std::unordered_map<size_t, u64> key_of_leg;
        for (size_t i = 0; i < path.legs.size(); ++i) {
            const CommandGraphPath::Leg& leg = path.legs[i];
            if (leg.closes) {
                keys.push_back(key_of_leg[leg.index]);
                continue;
            }
            const u64 key =
                leg.order.command_id != 0 ? leg.order.command_id : (u64{1} << 32) | ++unnumbered;
            keys.push_back(key);
            key_of_leg[leg.index] = key;
            const auto [it, fresh] = index.emplace(key, nodes.size());
            if (fresh) {
                CommandGraphNode node;
                node.order = leg.order;
                node.style = leg.style;
                nodes.push_back(std::move(node));
                sums.push_back({0, 0, 0});
                counts.push_back(0);
                ins.emplace_back();
                outs.emplace_back();
            }
            const size_t n = it->second;
            sums[n] = add(sums[n], path.chain[i + 1]);
            ++counts[n];
            if (nodes[n].units.empty() || nodes[n].units.back() != path.unit->id) {
                nodes[n].units.push_back(path.unit->id);
            }
            nodes[n].chosen = nodes[n].chosen || path.chosen;
        }
        for (size_t k = 0; k + 1 < keys.size(); ++k) {
            if (const auto to = index.find(keys[k + 1]); to != index.end()) {
                ins[to->second].insert(keys[k]);
            }
            if (const auto from = index.find(keys[k]); from != index.end()) {
                outs[from->second].insert(keys[k + 1]);
            }
        }
    }
    for (size_t n = 0; n < nodes.size(); ++n) {
        nodes[n].position = scale(sums[n], 1.0f / static_cast<f32>(counts[n]));
        const size_t lanes = std::max<size_t>({ins[n].size(), outs[n].size(), 1});
        nodes[n].unit_scale = std::sqrt(static_cast<f32>(lanes));
        CommandGraphNode& node = nodes[n];
        node.highlighted = (highlight != 0 && node.order.command_id == highlight) ||
                           (hovered_unit != 0 && std::find(node.units.begin(), node.units.end(),
                                                           hovered_unit) != node.units.end());
        const CommandGraphStyle& s = *node.style;
        if (node.highlighted) {
            node.color = s.waypoint_highlight_color;
            node.scale = s.waypoint_highlight_scale;
        } else {
            node.color = node.chosen ? s.waypoint_selected_color : s.waypoint_color;
            node.scale = node.chosen ? s.waypoint_selected_scale : s.waypoint_scale;
        }
    }
    return nodes;
}

std::vector<WaypointOnScreen> waypoints_on_screen(const std::vector<CommandGraphNode>& nodes,
                                                  const Camera& camera, f32 width, f32 height) {
    std::vector<WaypointOnScreen> out;
    if (width <= 0.0f || height <= 0.0f) {
        return out;
    }
    const auto vp = camera.view_proj(width / height);
    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    camera.eye_position(ex, ey, ez);
    const f32 per_px = 2.0f * std::tan(camera.fov() * 0.5f) / height;
    for (const CommandGraphNode& node : nodes) {
        if (node.order.command_id == 0) {
            continue;
        }
        const Vector3& p = node.position;
        const f32 cx = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12];
        const f32 cy = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13];
        const f32 cw = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15];
        if (cw <= 0.001f || std::abs(cx) > cw || std::abs(cy) > cw) {
            continue;
        }
        const f32 px_world = per_px * length(sub(p, Vector3{ex, ey, ez}));
        out.push_back({node.order.command_id, (cx / cw + 1.0f) * 0.5f * width,
                       (cy / cw + 1.0f) * 0.5f * height,
                       px_world > 0.0f
                           ? CommandGraphRenderer::kWaypointSize * node.unit_scale / px_world
                           : 0.0f,
                       node.chosen});
    }
    return out;
}

u32 waypoint_under_cursor(const std::vector<WaypointOnScreen>& waypoints, f32 mx, f32 my) {
    u32 best = 0;
    f32 best_distance = 0;
    bool best_chosen = false;
    for (const WaypointOnScreen& w : waypoints) {
        const f32 reach = std::clamp(w.size_px, CommandGraphRenderer::kMinWaypointPx,
                                     CommandGraphRenderer::kMaxWaypointPx);
        const f32 distance = std::hypot(w.x - mx, w.y - my);
        if (distance > reach) {
            continue;
        }
        if (best == 0 || (w.chosen && !best_chosen) ||
            (w.chosen == best_chosen && distance < best_distance)) {
            best = w.command_id;
            best_distance = distance;
            best_chosen = w.chosen;
        }
    }
    return best;
}

std::string command_graph_key(sim::CommandType type) {
    switch (type) {
    case sim::CommandType::Move: return "UNITCOMMAND_Move";
    case sim::CommandType::Attack: return "UNITCOMMAND_Attack";
    case sim::CommandType::Guard: return "UNITCOMMAND_Guard";
    case sim::CommandType::Patrol: return "UNITCOMMAND_Patrol";
    case sim::CommandType::AggressiveMove: return "UNITCOMMAND_AggressiveMove";
    case sim::CommandType::BuildMobile: return "UNITCOMMAND_BuildMobile";
    case sim::CommandType::Reclaim: return "UNITCOMMAND_Reclaim";
    case sim::CommandType::Repair: return "UNITCOMMAND_Repair";
    case sim::CommandType::Capture: return "UNITCOMMAND_Capture";
    case sim::CommandType::TransportLoad: return "UNITCOMMAND_TransportLoadUnits";
    case sim::CommandType::TransportUnload: return "UNITCOMMAND_TransportUnloadUnits";
    case sim::CommandType::Nuke: return "UNITCOMMAND_Nuke";
    case sim::CommandType::Tactical: return "UNITCOMMAND_Tactical";
    case sim::CommandType::Overcharge: return "UNITCOMMAND_OverCharge";
    case sim::CommandType::Sacrifice: return "UNITCOMMAND_Sacrifice";
    case sim::CommandType::Teleport: return "UNITCOMMAND_Teleport";
    case sim::CommandType::Ferry: return "UNITCOMMAND_Ferry";
    case sim::CommandType::Dock: return "UNITCOMMAND_Dock";
    case sim::CommandType::Script: return "UNITCOMMAND_Script";
    // "not displayed right now" (commandgraphparams.lua), or none of a
    // place: a stop, a factory's build, an upgrade, a dive, an enhancement
    default: return "";
    }
}

std::vector<Vector3> command_curve(const std::vector<Vector3>& chain, size_t leg, u32 segments,
                                   f32 width, f32 smoothness) {
    std::vector<Vector3> out;
    if (leg + 1 >= chain.size() || segments == 0) return out;
    const auto unit = [](const Vector3& v) {
        const f32 len = length(v);
        return len > 1e-6f ? scale(v, 1.0f / len) : Vector3{0, 0, 0};
    };
    const auto tangent = [&](size_t i) {
        Vector3 t{0, 0, 0};
        if (i > 0) t = add(t, unit(sub(chain[i], chain[i - 1])));
        if (i + 1 < chain.size()) t = add(t, unit(sub(chain[i + 1], chain[i])));
        return length(t) < 1e-4f ? Vector3{0, 0, 0} : unit(t);
    };
    const Vector3& a = chain[leg];
    const Vector3& b = chain[leg + 1];
    const Vector3 d = sub(b, a);
    const Vector3 dir = unit(d);
    Vector3 ta = tangent(leg);
    Vector3 tb = tangent(leg + 1);
    if (length(ta) < 1e-4f) ta = dir;
    if (length(tb) < 1e-4f) tb = dir;
    const f32 m = std::min(0.25f * length(d), smoothness * width);
    ta = scale(ta, m);
    tb = scale(tb, m);
    out.reserve(segments + 1);
    for (u32 k = 0; k <= segments; ++k) {
        const f32 t = static_cast<f32>(k) / static_cast<f32>(segments);
        const f32 t2 = t * t;
        const f32 t3 = t2 * t;
        const f32 h00 = 2 * t3 - 3 * t2 + 1;
        const f32 h10 = t3 - 2 * t2 + t;
        const f32 h01 = -2 * t3 + 3 * t2;
        const f32 h11 = t3 - t2;
        out.push_back(add(add(scale(a, h00), scale(ta, h10)), add(scale(b, h01), scale(tb, h11))));
    }
    return out;
}

bool command_strip(const Vector3& a, const Vector3& b, f32 half_width,
                   std::array<Vector3, 4>& corners) {
    // Across the leg, level with the ground
    Vector3 side = {b.z - a.z, 0.0f, a.x - b.x};
    const f32 side_len = length(side);
    if (side_len < 1e-4f) return false;
    side = scale(side, half_width / side_len);
    corners = {add(a, side), add(b, side), sub(b, side), sub(a, side)};
    return true;
}

void CommandGraphRenderer::init(VkDevice device, VmaAllocator allocator, VkRenderPass render_pass,
                                VkDescriptorSetLayout texture_ds_layout) {
    batch_.init(device, allocator, render_pass, texture_ds_layout, MAX_QUADS,
                "CommandGraphRenderer");
}

const std::array<f32, 6>& CommandGraphRenderer::pad_of(const std::string& bp, lua_State* L) {
    if (auto it = pads_.find(bp); it != pads_.end()) {
        return it->second;
    }
    std::array<f32, 6>& out = pads_[bp];
    out = {1, 1, 0, 0, 0, 0};
    if (!L) {
        return out;
    }
    const int top = lua_gettop(L);
    lua_pushstring(L, "__blueprints");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, bp.c_str());
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            const int table = lua_gettop(L);
            const auto [fx, fz] = sim::blueprint_footprint(L, table);
            out[0] = fx > 0 ? fx : 1.0f;
            out[1] = fz > 0 ? fz : 1.0f;
            lua_pushstring(L, "Physics");
            lua_rawget(L, table);
            if (lua_istable(L, -1)) {
                const int physics = lua_gettop(L);
                const char* keys[] = {"SkirtSizeX", "SkirtSizeZ", "SkirtOffsetX", "SkirtOffsetZ"};
                for (size_t i = 0; i < 4; ++i) {
                    lua_pushstring(L, keys[i]);
                    lua_rawget(L, physics);
                    if (lua_isnumber(L, -1)) {
                        out[2 + i] = static_cast<f32>(lua_tonumber(L, -1));
                    }
                    lua_pop(L, 1);
                }
            }
        }
    }
    lua_settop(L, top);
    return out;
}

const CommandGraphStyle* CommandGraphRenderer::style(sim::CommandType type, lua_State* L) {
    if (!styles_read_ && L) {
        styles_read_ = true;
        const int top = lua_gettop(L);
        static const char* const kRead =
            "return import('/lua/ui/game/commandgraphparams.lua').CommandGraphParams";
        if (luaL_loadbuffer(L, kRead, std::strlen(kRead), "CommandGraphParams") == 0 &&
            lua_pcall(L, 0, 1, 0) == 0 && lua_istable(L, -1)) {
            for (sim::CommandType t : kDrawnTypes) {
                const std::string key = command_graph_key(t);
                styles_[key] = command_graph_style(L, lua_gettop(L), key);
            }
        } else {
            spdlog::warn("CommandGraphParams: not read ({}); the default's style for all",
                         lua_isstring(L, -1) ? lua_tostring(L, -1) : "no table");
        }
        lua_settop(L, top);
    }
    const std::string key = command_graph_key(type);
    if (key.empty()) return nullptr;
    auto it = styles_.find(key);
    if (it == styles_.end()) it = styles_.emplace(key, fallback_style()).first;
    return &it->second;
}

void CommandGraphRenderer::update(const sim::FrameView& view, const Camera& camera,
                                  const std::unordered_set<u32>* selected, i32 player_army,
                                  TextureCache& tex_cache, lua_State* L, f32 time, u32 viewport_h,
                                  bool shown, u32 fi) {
    legs_.clear();
    planned_.clear();
    const sim::WorldSnapshot* cur = view.cur();
    if (!shown || !cur || !batch_.ready(fi)) {
        batch_.upload({}, fi);
        return;
    }
    planned_ = planned_build_sites(*cur, selected, player_army);

    f32 ex = 0;
    f32 ey = 0;
    f32 ez = 0;
    camera.eye_position(ex, ey, ez);
    const Vector3 eye{ex, ey, ez};
    // The world's size of a pixel at distance d: 2 d tan(fov/2) / height
    const f32 per_px =
        viewport_h > 0 ? 2.0f * std::tan(camera.fov() * 0.5f) / static_cast<f32>(viewport_h) : 0.0f;

    using Quad = WorldQuadBatch::Quad;
    using Vertex = WorldQuadBatch::Vertex;
    std::vector<Quad> lines;
    std::vector<Quad> waypoints;
    const auto vertex = [](const Vector3& p, f32 u, f32 v, const std::array<f32, 4>& c) {
        return Vertex{{p.x, p.y, p.z}, {u, v}, {c[0], c[1], c[2], c[3]}};
    };
    auto paths = command_graph_paths(view, selected, player_army,
                                     [&](sim::CommandType type) { return style(type, L); });
    std::stable_partition(paths.begin(), paths.end(),
                          [](const CommandGraphPath& p) { return p.chosen; });
    for (const CommandGraphPath& path : paths) {
        const sim::EntityRecord* e = path.unit;
        const u32 uid = e->id;
        const auto& chain = path.chain;
        for (size_t i = 0; i < path.legs.size(); ++i) {
            const CommandGraphPath::Leg& leg = path.legs[i];
            const sim::CommandType type = leg.order.type;
            const CommandGraphStyle* s = leg.style;
            const std::string* blueprint = &leg.order.blueprint_id;
            const sim::CommandRecord* order = &leg.order;
            const size_t index = leg.index;
            const Vector3& from = chain[i];
            const Vector3& to = chain[i + 1];
            if (lines.size() + waypoints.size() + kCurveSegments + 1 > MAX_QUADS) break;
            const GPUTexture* line_tex =
                s->line_texture.empty() ? nullptr : tex_cache.get(s->line_texture);
            if (line_tex) {
                const auto& col = leg.line_color;
                const f32 shift = -s->anim_rate * time;
                const std::vector<Vector3> points =
                    command_curve(chain, i, kCurveSegments, kLineWidth);
                f32 u0 = 0.0f;
                for (size_t k = 0; k + 1 < points.size(); ++k) {
                    std::array<Vector3, 4> q;
                    const f32 u1 =
                        u0 + length(sub(points[k + 1], points[k])) / kLineWidth * s->uv_aspect;
                    if (command_strip(points[k], points[k + 1], kLineWidth * 0.5f, q)) {
                        const Vertex a = vertex(q[0], u0 + shift, 0.0f, col);
                        const Vertex b = vertex(q[1], u1 + shift, 0.0f, col);
                        const Vertex cc = vertex(q[2], u1 + shift, 1.0f, col);
                        const Vertex d = vertex(q[3], u0 + shift, 1.0f, col);
                        lines.push_back({line_tex->descriptor_set, {a, b, cc, a, cc, d}});
                    }
                    u0 = u1;
                }
                legs_.push_back({uid, type, from, to, col, s->line_texture});
            }
            const GPUTexture* wp_tex =
                s->waypoint_texture.empty() ? nullptr : tex_cache.get(s->waypoint_texture);
            const bool site = wp_tex && !leg.closes && !blueprint->empty() && per_px > 0.0f &&
                              !build_started(*cur, *e, index, *order);
            if (site) {
                const auto& p = pad_of(*blueprint, L);
                const auto pad = build_pad(to.x, to.z, p[0], p[1], p[2], p[3], p[4], p[5]);
                const std::array<Vector3, 4> corner = {{{pad[0], to.y, pad[1]},
                                                        {pad[2], to.y, pad[1]},
                                                        {pad[2], to.y, pad[3]},
                                                        {pad[0], to.y, pad[3]}}};
                const f32 half = kPadOutlinePx * 0.5f * per_px * length(sub(to, eye));
                for (size_t k = 0; k < 4; ++k) {
                    std::array<Vector3, 4> q;
                    if (command_strip(corner[k], corner[(k + 1) % 4], half, q)) {
                        const Vertex qa = vertex(q[0], 0.0f, 0.0f, kPadOutlineColor);
                        const Vertex qb = vertex(q[1], 1.0f, 0.0f, kPadOutlineColor);
                        const Vertex qc = vertex(q[2], 1.0f, 1.0f, kPadOutlineColor);
                        const Vertex qd = vertex(q[3], 0.0f, 1.0f, kPadOutlineColor);
                        waypoints.push_back(
                            {tex_cache.fallback_descriptor(), {qa, qb, qc, qa, qc, qd}});
                    }
                }
            }
        }
    }
    for (const CommandGraphNode& node : command_graph_nodes(paths, highlight_, hovered_unit_)) {
        const CommandGraphStyle* s = node.style;
        const GPUTexture* wp_tex =
            s->waypoint_texture.empty() ? nullptr : tex_cache.get(s->waypoint_texture);
        if (!wp_tex || per_px <= 0.0f || lines.size() + waypoints.size() + 1 > MAX_QUADS) {
            continue;
        }
        const Vector3& to = node.position;
        // Its world size, held between its least and most on screen
        const f32 px_world = per_px * length(sub(to, eye));
        f32 size = kWaypointSize * node.unit_scale * node.scale;
        if (px_world > 0.0f) {
            size = std::clamp(size / px_world, kMinWaypointPx, kMaxWaypointPx) * px_world;
        }
        const Vector3 r = {size * 0.5f, 0.0f, 0.0f};
        const Vector3 u = {0.0f, 0.0f, -size * 0.5f};
        const auto& col = node.color;
        const Vertex a = vertex(sub(add(to, u), r), 0.0f, 0.0f, col);
        const Vertex b = vertex(add(add(to, u), r), 1.0f, 0.0f, col);
        const Vertex cc = vertex(add(sub(to, u), r), 1.0f, 1.0f, col);
        const Vertex d = vertex(sub(sub(to, u), r), 0.0f, 1.0f, col);
        waypoints.push_back({wp_tex->descriptor_set, {a, b, cc, a, cc, d}});
    }

    const auto by_texture = [](const Quad& x, const Quad& y) { return x.ds < y.ds; };
    std::stable_sort(lines.begin(), lines.end(), by_texture);
    std::stable_sort(waypoints.begin(), waypoints.end(), by_texture);
    lines.insert(lines.end(), waypoints.begin(), waypoints.end());
    batch_.upload(lines, fi);
}

void CommandGraphRenderer::render(VkCommandBuffer cmd, u32 viewport_w, u32 viewport_h,
                                  const f32* view_proj, u32 fi) const {
    batch_.render(cmd, viewport_w, viewport_h, view_proj, fi);
}

void CommandGraphRenderer::destroy(VkDevice device, VmaAllocator allocator) {
    batch_.destroy(device, allocator);
}

} // namespace osc::renderer
