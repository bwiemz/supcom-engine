// --map-border-test: the map's border (MapBorderAdd; Moho's MapImager, faf-re
// render/MapImager.cpp), as retail's UpdateWorldBorderState asks for it:
// - each mesh added is drawn once, over the heightfield's centre at the
//   midpoint of its heights plus the map's imager elevation offset, scaled
//   by the map's width times the mesh blueprint's UniformScale;
// - it covers what lies past the map's edge and nothing on the map;
// - MapImagerPS0 lights it (never darker than its 0.02), MapImagerPS1
//   writes its glow (the SpecTeam's blue plus glowMinimum, 0.01);
// - it is drawn at strategic zoom, where the units' meshes are not, and
//   isn't counted among them;
// - MapBorderClear takes it away.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "core/image.hpp"
#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "renderer/renderer.hpp"
#include "renderer/strategic_icon_renderer.hpp"
#include "renderer/texture_cache.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr const char* kUefBorder = "/meshes/game/map-border_squ_uef_mesh";
constexpr const char* kAeonBorder = "/meshes/game/map-border_squ_aeon_mesh";
constexpr const char* kAeonDetails = "/meshes/game/map-border_squ_aeon_a_mesh";

/// The border meshes the last frame drew: their dump lines' model matrices,
/// by their albedo's path.
struct BorderMesh {
    std::string texture;
    std::array<f32, 16> model{};
};

std::vector<BorderMesh> border_meshes(renderer::Renderer& r) {
    std::ostringstream out;
    r.dump_frame(out);
    std::istringstream in(out.str());
    std::vector<BorderMesh> found;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("mesh ", 0) != 0 || line.find("map-border") == std::string::npos) continue;
        const size_t bar = line.find(" | ");
        if (bar == std::string::npos) continue;
        BorderMesh mesh;
        mesh.texture = line.substr(5, bar - 5);
        std::transform(mesh.texture.begin(), mesh.texture.end(), mesh.texture.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::istringstream model(line.substr(bar + 3));
        for (f32& v : mesh.model) model >> v;
        found.push_back(std::move(mesh));
    }
    return found;
}

f32 lua_number(TestContext& ctx, const std::string& expr) {
    (void)ctx.lua_state.do_string(fmt::format("rawset(_G, '__osc_mb_n', {})\n", expr));
    lua_pushstring(ctx.L, "__osc_mb_n");
    lua_rawget(ctx.L, LUA_GLOBALSINDEX);
    const f32 n = lua_isnumber(ctx.L, -1) ? static_cast<f32>(lua_tonumber(ctx.L, -1)) : -1.0f;
    lua_pop(ctx.L, 1);
    return n;
}

bool is_backdrop(const ImageRGBA8& image, size_t i) {
    const auto& b = OffscreenShots::kBackdrop;
    for (int c = 0; c < 3; ++c)
        if (std::abs(image.pixels[i * 4 + c] / 255.0f - b[c]) > 2.5f / 255.0f) return false;
    return true;
}

bool same_pixel(const ImageRGBA8& a, const ImageRGBA8& b, size_t i) {
    for (int c = 0; c < 3; ++c)
        if (std::abs(static_cast<int>(a.pixels[i * 4 + c]) -
                     static_cast<int>(b.pixels[i * 4 + c])) > 2)
            return false;
    return true;
}

} // namespace

void test_map_border(TestContext& ctx) {
    spdlog::info("=== Map border test ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    if (!terrain) {
        t.check(false, "a map");
        return;
    }
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    const u32 errors_before = renderer::Renderer::validation_error_count();
    const f32 w = static_cast<f32>(terrain->map_width());
    const f32 h = static_cast<f32>(terrain->map_height());

    // Draw until what the border loads has loaded, then capture the frame
    // and its scene (the glow in alpha).
    const auto settle = [&](renderer::Renderer::SceneImage* scene) {
        for (int f = 0; f < 600; ++f) {
            shots.redraw();
            if (r.texture_cache().loading() == 0) break;
        }
        shots.redraw();
        if (scene)
            r.request_scene_capture(
                [&](renderer::Renderer::SceneImage captured) { *scene = std::move(captured); });
        return shots.grab();
    };
    const auto run = [&](const std::string& code) {
        const auto ran = ctx.lua_state.do_string(code);
        t.check(static_cast<bool>(ran),
                ran ? code : fmt::format("{}: {}", code, ran.error().message));
    };

    // The whole map and its surroundings, at strategic zoom: none first
    // (the UI's skin may have added one)
    run("MapBorderClear()");
    const f32 whole_view = 2.0f * w;
    (void)shots.shoot_frame(*terrain, w * 0.5f, h * 0.5f, whole_view);
    const ImageRGBA8 none = settle(nullptr);
    const u32 units_none = r.mesh_instance_count();
    t.check(r.strategic_icons().is_strategic_zoom(),
            fmt::format("from {:.0f} the view is at strategic zoom", whole_view));
    t.check(border_meshes(r).empty(), "with none added, no border is drawn");

    run(fmt::format("MapBorderAdd('{}')", kUefBorder));
    renderer::Renderer::SceneImage scene;
    const ImageRGBA8 framed = settle(&scene);
    const u32 units_framed = r.mesh_instance_count();

    // Where Moho's MapImager puts it
    const std::vector<BorderMesh> drawn = border_meshes(r);
    t.check(drawn.size() == 1, fmt::format("one border mesh is drawn ({})", drawn.size()));
    if (!drawn.empty()) {
        const map::Heightmap& field = terrain->heightmap();
        f32 lo = field.get_height_at_grid(0, 0);
        f32 hi = lo;
        for (u32 z = 0; z < field.grid_height(); ++z)
            for (u32 x = 0; x < field.grid_width(); ++x) {
                lo = std::min(lo, field.get_height_at_grid(x, z));
                hi = std::max(hi, field.get_height_at_grid(x, z));
            }
        const f32 offset = terrain->cartographic().imager_elevation_offset;
        const f32 uniform =
            lua_number(ctx, fmt::format("__blueprints['{}'].UniformScale", kUefBorder));
        const auto& m = drawn[0].model;
        const auto close = [](f32 a, f32 b) {
            return std::abs(a - b) < 1e-3f * std::max(1.0f, std::abs(b));
        };
        t.check(
            uniform > 0.0f && close(m[0], w * uniform) && close(m[5], w * uniform) &&
                close(m[10], w * uniform),
            fmt::format("scaled by the map's width times its UniformScale: {:.4f} ({:.0f} x {})",
                        m[0], w, uniform));
        t.check(close(m[12], w * 0.5f) && close(m[14], h * 0.5f) &&
                    close(m[13], (lo + hi) * 0.5f + offset),
                fmt::format("over the map's centre at its heights' midpoint plus the offset: "
                            "({:.2f}, {:.2f}, {:.2f}), heights {:.2f}..{:.2f}, offset {:.2f}",
                            m[12], m[13], m[14], lo, hi, offset));
        t.check(m[1] == 0.0f && m[2] == 0.0f && m[4] == 0.0f && m[6] == 0.0f && m[8] == 0.0f &&
                    m[9] == 0.0f,
                "unrotated");
        t.check(drawn[0].texture.find("map-border_squ_uef") != std::string::npos,
                fmt::format("in the mesh blueprint's textures ({})", drawn[0].texture));
    }

    // The same view without it again, frames later: the map's pixels that
    // held still between the two (not the water's waves) are the ones to
    // judge it by
    run("MapBorderClear()");
    const ImageRGBA8 cleared = settle(nullptr);
    t.check(border_meshes(r).empty(), "MapBorderClear takes it away");

    // What it covers: past the map's edge (the backdrop without it), and
    // nothing on the map
    size_t outside = 0;
    size_t covered = 0;
    size_t lit = 0;
    size_t glowing = 0;
    size_t glow_bright = 0;
    size_t map_still = 0;
    size_t map_same = 0;
    size_t restored = 0;
    const size_t pixels = static_cast<size_t>(none.width) * none.height;
    const bool same_size = framed.width == none.width && framed.height == none.height &&
                           cleared.width == none.width && cleared.height == none.height &&
                           scene.width == none.width && scene.height == none.height;
    t.check(same_size, "the frames and the scene are the same size");
    for (size_t i = 0; same_size && i < pixels; ++i) {
        if (is_backdrop(none, i)) {
            ++outside;
            if (is_backdrop(cleared, i)) ++restored;
            if (is_backdrop(framed, i)) continue;
            ++covered;
            // MapImagerPS0's 0.02 at the least, in every channel
            const auto* px = &framed.pixels[i * 4];
            if (std::min({px[0], px[1], px[2]}) >= 4) ++lit;
            // MapImagerPS1's glowMinimum at the least
            const f32 glow = scene.rgba[i * 4 + 3];
            if (glow >= 0.009f) ++glowing;
            if (glow >= 0.3f) ++glow_bright;
        } else if (same_pixel(none, cleared, i)) {
            // Within the frame's middle, well inside the map: at the map's
            // edge the border, at its heights' midpoint, may overlap the
            // ground by parallax
            const size_t x = i % none.width;
            const size_t y = i / none.width;
            if (x < none.width * 35 / 100 || x >= none.width * 65 / 100 ||
                y < none.height * 35 / 100 || y >= none.height * 65 / 100)
                continue;
            ++map_still;
            if (same_pixel(framed, none, i)) ++map_same;
        }
    }
    t.check(outside > pixels / 10 && covered >= outside * 9 / 10,
            fmt::format("it covers what lies past the map: {} of {} pixels", covered, outside));
    t.check(map_still > pixels / 20 && map_same == map_still,
            fmt::format("and leaves the map as it was: {} of {} still pixels in the middle",
                        map_same, map_still));
    t.check(covered > 0 && lit == covered,
            fmt::format("lit at 0.02 or more ({} of {})", lit, covered));
    t.check(covered > 0 && glowing == covered && glow_bright > 0,
            fmt::format("its glow written: glowMinimum or more at {} of {}, 0.3 or more at {}",
                        glowing, covered, glow_bright));
    t.check(outside > 0 && restored == outside,
            fmt::format("cleared, the backdrop is back: {} of {} pixels", restored, outside));
    t.check(
        units_framed == units_none,
        fmt::format("not counted among the units' meshes ({} and {})", units_framed, units_none));

    // Near the map's edge, below strategic zoom: there too
    run(fmt::format("MapBorderAdd('{}')", kUefBorder));
    (void)shots.shoot_frame(*terrain, w * 0.5f, -60.0f, 150.0f);
    const Pixels edge_framed = centre_pixels(settle(nullptr));
    t.check(!r.strategic_icons().is_strategic_zoom() && border_meshes(r).size() == 1,
            "drawn below strategic zoom too");
    run("MapBorderClear()");
    const Pixels edge_none = centre_pixels(settle(nullptr));
    t.check(mean_abs_diff(edge_framed, edge_none) > 0.02f,
            fmt::format("past the edge it is what the view shows (differs by {:.4f})",
                        mean_abs_diff(edge_framed, edge_none)));

    // Two at once: a skin's border and its details
    run(fmt::format("MapBorderAdd('{}') MapBorderAdd('{}')", kAeonBorder, kAeonDetails));
    (void)settle(nullptr);
    const std::vector<BorderMesh> two = border_meshes(r);
    const auto has = [&](const char* albedo) {
        return std::any_of(two.begin(), two.end(), [&](const BorderMesh& b) {
            return b.texture.find(albedo) != std::string::npos;
        });
    };
    t.check(two.size() == 2 && has("map-border_squ_aeon_albedo") &&
                has("map-border_squ_aeon_a_albedo"),
            fmt::format("a border and its details: {} drawn", two.size()));
    run("MapBorderClear()");

    const u32 errors = renderer::Renderer::validation_error_count() - errors_before;
    t.check(errors == 0, fmt::format("no Vulkan validation errors ({})", errors));
    spdlog::info("Map border: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
