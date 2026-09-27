#include "intel_probe.hpp"

#include "integration_tests.hpp"

#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "renderer/camera.hpp"
#include "renderer/minimap_renderer.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <sstream>

namespace osc::test {

/// The last frame's meshes, strategic icons, overlays and minimap, from its
/// render-state dump.
Frame drawn(renderer::Renderer& r) {
    std::ostringstream out;
    r.dump_frame(out);
    std::istringstream in(out.str());
    Frame frame;
    std::string section;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line[0] == '[') {
            section = line.substr(0, line.find(']') + 1);
            continue;
        }
        std::vector<std::string> parts;
        for (size_t start = 0;;) {
            const size_t bar = line.find(" | ", start);
            parts.push_back(line.substr(start, bar == std::string::npos ? bar : bar - start));
            if (bar == std::string::npos) break;
            start = bar + 3;
        }
        if (section == "[units]" && line.rfind("mesh ", 0) == 0 && parts.size() >= 3) {
            Drawn d;
            d.texture = parts[0].substr(5);
            std::transform(d.texture.begin(), d.texture.end(), d.texture.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::istringstream model(parts[1]);
            std::array<f32, 16> m{};
            for (f32& v : m) model >> v;
            d.x = m[12];
            d.z = m[14];
            if (parts.size() >= 4) d.pose = parts[3];
            frame.meshes.push_back(std::move(d));
        } else if ((section == "[icons]" && parts.size() == 4) ||
                   (parts.size() == 3 && (section == "[overlay]" || section == "[minimap-hud]"))) {
            Quad q;
            // An icon's line starts with its texture (M215c).
            if (parts.size() == 4) {
                q.texture = parts[0];
                parts.erase(parts.begin());
            }
            std::istringstream rect(parts[0]);
            rect >> q.x >> q.y >> q.w >> q.h;
            q.x += q.w * 0.5f;
            q.y += q.h * 0.5f;
            std::istringstream colour(parts[2]);
            colour >> q.r >> q.g >> q.b;
            (section == "[icons]"     ? frame.icons
             : section == "[overlay]" ? frame.overlay
                                      : frame.minimap)
                .push_back(q);
        }
    }
    return frame;
}

/// The mesh drawn at (x, z).
const Drawn* mesh_at(const Frame& frame, f32 x, f32 z) {
    for (const Drawn& d : frame.meshes)
        if (std::abs(d.x - x) < 1.0f && std::abs(d.z - z) < 1.0f) return &d;
    return nullptr;
}

/// How many meshes of texture `texture` were drawn within `radius` of x.
int meshes_near(const Frame& frame, const std::string& texture, f32 x, f32 radius) {
    return static_cast<int>(
        std::count_if(frame.meshes.begin(), frame.meshes.end(), [&](const Drawn& d) {
            return d.texture.find(texture) != std::string::npos && std::abs(d.x - x) < radius;
        }));
}

/// The spot, on a 16-unit lattice, farthest from every unit whose box
/// [x - 30, x + 50] x [z - 40, z + 70] is dry land; none within 60 of a
/// unit will do.
std::optional<Spot> quiet_spot(sim::SimState& sim) {
    const map::Terrain& t = *sim.terrain();
    const i32 w = static_cast<i32>(t.map_width());
    const i32 h = static_cast<i32>(t.map_height());
    std::vector<Spot> units;
    sim.entity_registry().for_each([&](const sim::Entity& e) {
        if (e.is_unit() && !e.destroyed()) units.push_back({e.position().x, e.position().z});
    });
    std::optional<Spot> best;
    f32 best_d2 = 60.0f * 60.0f;
    for (i32 gz = 80; gz < h - 80; gz += 16) {
        for (i32 gx = 80; gx < w - 80; gx += 16) {
            const f32 x = static_cast<f32>(gx);
            const f32 z = static_cast<f32>(gz);
            f32 near2 = std::numeric_limits<f32>::max();
            for (const Spot& u : units)
                near2 = std::min(near2, (u.x - x) * (u.x - x) + (u.z - z) * (u.z - z));
            if (near2 < best_d2) continue;
            bool dry = true;
            for (i32 dz = -40; dz <= 70 && dry; dz += 8)
                for (i32 dx = -30; dx <= 50 && dry; dx += 8)
                    dry = !t.has_water() ||
                          t.get_terrain_height(x + static_cast<f32>(dx), z + static_cast<f32>(dz)) >
                              t.water_elevation() + 0.5f;
            if (!dry) continue;
            best = Spot{x, z};
            best_d2 = near2;
        }
    }
    return best;
}

const char* name(renderer::Sight s) {
    switch (s) {
    case renderer::Sight::Hidden: return "hidden";
    case renderer::Sight::Blip: return "blip";
    case renderer::Sight::SeenBlip: return "seen blip";
    case renderer::Sight::Seen: return "seen";
    case renderer::Sight::Remembered: return "remembered";
    }
    return "?";
}

/// Run `code` in the sim state, logging a failure.
void run_lua(TestContext& ctx, const std::string& code) {
    const auto ran = ctx.lua_state.do_string(code);
    if (!ran) spdlog::warn("intel test Lua: {}", ran.error().message);
}

/// Make a `bp` of `army`'s on the ground at `at`, as Lua global `global`;
/// its entity id.
u32 spawn_unit(TestContext& ctx, const char* global, const char* bp, const char* army, Spot at,
               f32 lift) {
    run_lua(ctx, fmt::format("{0} = CreateUnitHPR('{1}', '{2}', {3}, {4}, {5}, 0, 0, 0)\n"
                             "__osc_intel_id = tonumber({0}:GetEntityId())\n",
                             global, bp, army, at.x,
                             ctx.sim.terrain()->get_terrain_height(at.x, at.z) + lift, at.z));
    lua_pushstring(ctx.L, "__osc_intel_id");
    lua_rawget(ctx.L, LUA_GLOBALSINDEX);
    const u32 id = lua_isnumber(ctx.L, -1) ? static_cast<u32>(lua_tonumber(ctx.L, -1)) : 0;
    lua_pop(ctx.L, 1);
    return id;
}

/// Where `p` is on `r`'s screen, as its icons and overlays project it.
std::optional<std::array<f32, 2>> screen_of(renderer::Renderer& r, const sim::Vector3& p) {
    const f32 sw = static_cast<f32>(r.width());
    const f32 sh = static_cast<f32>(r.height());
    const auto vp = r.camera().view_proj(sw / sh);
    const f32 cx = vp[0] * p.x + vp[4] * p.y + vp[8] * p.z + vp[12];
    const f32 cy = vp[1] * p.x + vp[5] * p.y + vp[9] * p.z + vp[13];
    const f32 cw = vp[3] * p.x + vp[7] * p.y + vp[11] * p.z + vp[15];
    if (cw <= 0.001f) return std::nullopt;
    return std::array<f32, 2>{(cx / cw + 1.0f) * 0.5f * sw, (cy / cw + 1.0f) * 0.5f * sh};
}

/// The w x h quad centred at (x, y), within a pixel, or null.
const Quad* quad_at(const std::vector<Quad>& quads, f32 x, f32 y, f32 w, f32 h) {
    for (const Quad& q : quads)
        if (std::abs(q.x - x) < 1.0f && std::abs(q.y - y) < 1.0f && q.w == w && q.h == h) return &q;
    return nullptr;
}

bool same_colour(const Quad& q, f32 r, f32 g, f32 b) {
    return std::abs(q.r - r) < 0.01f && std::abs(q.g - g) < 0.01f && std::abs(q.b - b) < 0.01f;
}

const Quad* minimap_dot(const Frame& frame, renderer::Renderer& r, f32 map_w, f32 map_h,
                        const sim::Vector3& p) {
    const f32 size = static_cast<f32>(renderer::MinimapRenderer::MINIMAP_SIZE);
    const f32 margin = static_cast<f32>(renderer::MinimapRenderer::MINIMAP_MARGIN);
    const renderer::MapArea area = renderer::fit_map_area(
        margin, static_cast<f32>(r.height()) - size - margin, size, size, map_w, map_h);
    return quad_at(frame.minimap, area.x + p.x / map_w * area.w, area.y + p.z / map_h * area.h,
                   3.0f, 3.0f);
}

} // namespace osc::test
