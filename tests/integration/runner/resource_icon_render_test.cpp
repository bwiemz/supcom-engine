// --resource-icon-render-test: CWldSession::RenderResources, TResourceIcon.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "map/terrain.hpp"
#include "renderer/dds_decode.hpp"
#include "renderer/dds_parser.hpp"
#include "renderer/renderer.hpp"
#include "renderer/resource_icon_renderer.hpp"
#include "sim/sim_state.hpp"
#include "ui/world_view.hpp"
#include "vfs/virtual_file_system.hpp"

#include <GLFW/glfw3.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace osc::test {

namespace {

std::optional<sim::ResourceDeposit> lonely_deposit(sim::SimState& sim,
                                                   sim::ResourceDeposit::Type type) {
    const map::Terrain& terrain = *sim.terrain();
    std::vector<std::array<f32, 2>> units;
    sim.entity_registry().for_each([&](const sim::Entity& e) {
        if (e.is_unit() && !e.destroyed()) {
            units.push_back({e.position().x, e.position().z});
        }
    });
    std::optional<sim::ResourceDeposit> best;
    f32 best_d2 = 0.0f;
    for (const sim::ResourceDeposit& d : sim.resource_deposits()) {
        if (d.type != type) {
            continue;
        }
        bool dry = true;
        for (int iz = -5; iz <= 5 && dry; ++iz) {
            for (int ix = -5; ix <= 5 && dry; ++ix) {
                const f32 x = d.x + 4.0f * static_cast<f32>(ix);
                const f32 z = d.z + 4.0f * static_cast<f32>(iz);
                dry = !terrain.has_water() ||
                      terrain.get_terrain_height(x, z) > terrain.water_elevation() + 0.5f;
            }
        }
        if (!dry) {
            continue;
        }
        f32 near2 = std::numeric_limits<f32>::max();
        for (const auto& u : units) {
            near2 = std::min(near2, (u[0] - d.x) * (u[0] - d.x) + (u[1] - d.z) * (u[1] - d.z));
        }
        if (!best || near2 > best_d2) {
            best = d;
            best_d2 = near2;
        }
    }
    return best;
}

const std::array<f32, 4>* icon_quad(renderer::Renderer& r, const sim::ResourceDeposit& d,
                                    const map::Terrain& terrain, f32 size) {
    const auto [x, z] = renderer::deposit_centre(d);
    const auto at = screen_of(r, {x, terrain.get_terrain_height(x, z), z});
    if (!at) {
        return nullptr;
    }
    for (const auto& q : r.resource_icons().quads()) {
        if (q[0] == std::floor((*at)[0]) - size * 0.5f &&
            q[1] == std::floor((*at)[1]) - size * 0.5f && q[2] == size && q[3] == size) {
            return &q;
        }
    }
    return nullptr;
}

struct Texels {
    u32 w = 0, h = 0;
    std::vector<u8> rgba;
};

Texels decode(TestContext& ctx, const char* path) {
    Texels out;
    const auto file = ctx.vfs.read_file(path);
    const auto dds = file ? renderer::parse_dds(*file) : std::nullopt;
    if (!dds || dds->format != VK_FORMAT_BC3_UNORM_BLOCK || dds->mips.empty()) {
        return out;
    }
    out.w = dds->width;
    out.h = dds->height;
    out.rgba =
        renderer::decode_bc3_to_rgba(reinterpret_cast<const u8*>(dds->mips[0].data), out.w, out.h);
    return out;
}

} // namespace

void test_resource_icon_render(TestContext& ctx) {
    spdlog::info("=== RESOURCE ICON RENDER TEST: the deposits' icons ===");
    Tally t;

    const auto mass = lonely_deposit(ctx.sim, sim::ResourceDeposit::Mass);
    const auto hydro = lonely_deposit(ctx.sim, sim::ResourceDeposit::Hydrocarbon);
    if (!mass || !hydro) {
        t.check(false, "the map has a mass and a hydrocarbon deposit on dry ground");
        return;
    }
    const Texels texels = decode(ctx, renderer::kMassIconTexture);
    if (texels.rgba.empty()) {
        t.check(false, "the mass icon's texture decodes");
        return;
    }
    const f32 size = static_cast<f32>(texels.w) / 2.0f;
    const map::Terrain& terrain = *ctx.sim.terrain();

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.camera().set_free(false);
    const auto scene = [&] {
        renderer::Renderer::SceneImage image;
        r.request_scene_capture(
            [&](renderer::Renderer::SceneImage captured) { image = std::move(captured); });
        shots.redraw();
        return image;
    };

    const auto [mx, mz] = renderer::deposit_centre(*mass);
    spdlog::info("Resource icon test: the mass deposit at ({:.1f}, {:.1f})", mx, mz);
    (void)shots.shoot(terrain, mx, mz, 300.0f);
    const f32 cx = static_cast<f32>(r.width()) * 0.5f;
    const f32 cy = static_cast<f32>(r.height()) * 0.5f;
    glfwSetTime(10.0f * 0.00277f * std::sqrt(cx * cx + cy * cy) + 1.5708f);
    const renderer::Renderer::SceneImage with = scene();
    const f32 time = r.resource_icon_time();
    const std::array<f32, 4>* quad = icon_quad(r, *mass, terrain, size);
    t.check(quad != nullptr,
            fmt::format("Test 1: at strategic zoom the mass deposit shows a {}x{} icon at its "
                        "pixel ({} icons in all)",
                        size, size, r.resource_icons().quads().size()));
    if (!quad) {
        return;
    }
    const std::array<f32, 4> q = *quad;

    r.camera().set_free(true);
    const renderer::Renderer::SceneImage without = scene();
    t.check(r.resource_icons().quads().empty(), "Test 2: a free camera (cam_Free) draws none");
    r.camera().set_free(false);

    {
        ui::WorldView view;
        view.set_resource_rendering(false);
        const auto publish = [&](ui::WorldView* wv) {
            lua_pushstring(ctx.L, "__osc_world_view");
            if (wv) {
                lua_pushlightuserdata(ctx.L, wv);
            } else {
                lua_pushnil(ctx.L);
            }
            lua_rawset(ctx.L, LUA_REGISTRYINDEX);
        };
        publish(&view);
        shots.redraw();
        const size_t off = r.resource_icons().quads().size();
        view.set_resource_rendering(true);
        shots.redraw();
        const size_t on = r.resource_icons().quads().size();
        publish(nullptr);
        t.check(off == 0 && on > 0,
                fmt::format("Test 3: the world view's EnableResourceRendering(false) draws none "
                            "({}), true draws them ({})",
                            off, on));
    }

    // Point sampling at a texel edge may take either side.
    int pixels = 0;
    int colour_ok = 0;
    int glow_ok = 0;
    int glowing = 0;
    f32 worst_colour = 0.0f;
    f32 worst_glow = 0.0f;
    const auto qx = static_cast<i32>(q[0]);
    const auto qy = static_cast<i32>(q[1]);
    const auto n = static_cast<i32>(size);
    for (i32 j = 0; j < n; ++j) {
        for (i32 i = 0; i < n; ++i) {
            const i32 px = qx + i;
            const i32 py = qy + j;
            if (px < 0 || py < 0 || px >= static_cast<i32>(with.width) ||
                py >= static_cast<i32>(with.height)) {
                continue;
            }
            const size_t at = (static_cast<size_t>(py) * with.width + static_cast<size_t>(px)) * 4;
            f32 best_colour = std::numeric_limits<f32>::max();
            f32 best_glow = std::numeric_limits<f32>::max();
            f32 expected_glow = 0.0f;
            for (i32 dv = 0; dv < 2; ++dv) {
                for (i32 du = 0; du < 2; ++du) {
                    const i32 u = std::min(2 * i + du, static_cast<i32>(texels.w) - 1);
                    const i32 v = std::min(2 * j + dv, static_cast<i32>(texels.h) - 1);
                    const u8* texel =
                        &texels.rgba[(static_cast<size_t>(v) * texels.w + static_cast<size_t>(u)) *
                                     4];
                    const f32 a = texel[3] / 255.0f;
                    f32 colour = 0.0f;
                    for (int c = 0; c < 3; ++c) {
                        const f32 expected =
                            a > 0.0f ? texel[c] / 255.0f * a + without.rgba[at + c] * (1.0f - a)
                                     : without.rgba[at + c];
                        colour = std::max(colour, std::abs(with.rgba[at + c] - expected));
                    }
                    best_colour = std::min(best_colour, colour);
                    const f32 g = renderer::resource_icon_glow(a, time, static_cast<f32>(px) + 0.5f,
                                                               static_cast<f32>(py) + 0.5f);
                    const f32 glow = g * g + without.rgba[at + 3] * (1.0f - g);
                    if (std::abs(with.rgba[at + 3] - glow) < best_glow) {
                        best_glow = std::abs(with.rgba[at + 3] - glow);
                        expected_glow = g;
                    }
                }
            }
            ++pixels;
            colour_ok += best_colour < 0.02f ? 1 : 0;
            glow_ok += best_glow < 0.02f ? 1 : 0;
            glowing += expected_glow > 0.05f ? 1 : 0;
            worst_colour = std::max(worst_colour, best_colour);
            worst_glow = std::max(worst_glow, best_glow);
        }
    }
    t.check(pixels > 0 && colour_ok == pixels,
            fmt::format("Test 4: the icon's colour is its texture over the frame ({}/{} pixels, "
                        "worst {:.4f})",
                        colour_ok, pixels, worst_colour));
    t.check(pixels > 0 && glow_ok == pixels && glowing > 0,
            fmt::format("Test 5: its glow is 0.6 sin^2(t - 10 R) of its alpha at t {:.2f} ({}/{} "
                        "pixels, {} glowing, worst {:.4f})",
                        time, glow_ok, pixels, glowing, worst_glow));

    (void)shots.shoot(terrain, mx, mz, 20.0f);
    t.check(icon_quad(r, *mass, terrain, size) == nullptr,
            fmt::format("Test 6: zoomed in past UI_ResourceLODCutoff, no icon ({} in all)",
                        r.resource_icons().quads().size()));

    const auto [hx, hz] = renderer::deposit_centre(*hydro);
    (void)shots.shoot(terrain, hx, hz, 300.0f);
    t.check(icon_quad(r, *hydro, terrain, size) != nullptr,
            "Test 7: the hydrocarbon deposit shows its icon");
}

} // namespace osc::test
