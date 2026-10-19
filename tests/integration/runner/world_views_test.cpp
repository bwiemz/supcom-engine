// --world-views-test: FA's main world views, one and split (borders.lua's
// SplitMapGroup), each drawn through its own camera.

#include "world_views_test.hpp"

#include "app/window_commands.hpp"
#include "core/image.hpp"
#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "render_probe.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>
#include <spdlog/spdlog.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::test {

namespace {

int off_backdrop(const ImageRGBA8& image, u32 x, u32 y0, u32 y1) {
    int worst = 0;
    for (u32 y = y0; y < y1; ++y) {
        const size_t p = (static_cast<size_t>(y) * image.width + x) * 4;
        for (int c = 0; c < 3; ++c) {
            const int want = static_cast<int>(std::lround(OffscreenShots::kBackdrop[c] * 255.0f));
            worst = std::max(worst, std::abs(static_cast<int>(image.pixels[p + c]) - want));
        }
    }
    return worst;
}

Pixels patch(const ImageRGBA8& image, u32 x, u32 y) {
    Pixels out;
    for (u32 j = y - 30; j <= y + 30; ++j) {
        for (u32 i = x - 30; i <= x + 30; ++i) {
            const size_t p = (static_cast<size_t>(j) * image.width + i) * 4;
            out.push_back({image.pixels[p] / 255.0f, image.pixels[p + 1] / 255.0f,
                           image.pixels[p + 2] / 255.0f});
        }
    }
    return out;
}

f32 number(TestContext& ui, const char* expr) {
    lua_State* L = ui.L;
    const int top = lua_gettop(L);
    const std::string code = fmt::format("return {}", expr);
    f32 v = std::numeric_limits<f32>::quiet_NaN();
    if (luaL_loadbuffer(L, code.data(), code.size(), "number") == 0 && lua_pcall(L, 0, 1, 0) == 0 &&
        lua_isnumber(L, -1)) {
        v = static_cast<f32>(lua_tonumber(L, -1));
    }
    lua_settop(L, top);
    return v;
}

} // namespace

void test_world_views(TestContext& ui, TestContext& sim, ui::UIControlRegistry& registry,
                      const std::function<void(int)>& pump) {
    spdlog::info("=== World views test ===");
    Tally t;
    lua_State* L = ui.L;
    const map::Terrain* terrain = sim.sim.terrain();
    if (!terrain) {
        t.check(false, "the map");
        return;
    }
    OffscreenShots shots(sim, 2.0f);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    const u32 errors_before = renderer::Renderer::validation_error_count();
    (void)shots.shoot(*terrain, 512.0f, 512.0f, 200.0f);
    lua_pushstring(L, "__osc_renderer");
    lua_pushlightuserdata(L, &r);
    lua_rawset(L, LUA_REGISTRYINDEX);
    shots.draw_ui(L, &registry);
    app::size_root_frame(L, r.ui_width(), r.ui_height());
    // gamemain.lua's StopLoadingDialog: its background over the game fades out
    pump(600);

    const auto run = [&](const char* code) {
        auto result = ui.lua_state.do_string(code);
        if (!result) {
            t.check(false, fmt::format("Lua: {}", result.error().message));
        }
        return static_cast<bool>(result);
    };
    const auto frame = [&] {
        pump(3);
        shots.redraw();
        return shots.grab();
    };

    {
        if (!run(R"(import('/lua/ui/game/borders.lua').SplitMapGroup(true, true))")) {
            return;
        }
        const ImageRGBA8 shot = frame();
        const f32 left_end = number(ui, "import('/lua/ui/game/worldview.lua').viewLeft.Right()");
        const f32 right_start = number(ui, "import('/lua/ui/game/worldview.lua').viewRight.Left()");
        const auto gap = static_cast<u32>((left_end + right_start) / 2.0f * r.ui_scale());
        const bool split = right_start - left_end >= 2.0f && gap < shot.width;
        const int off = split ? off_backdrop(shot, gap, shot.height / 6, shot.height / 3) : 255;
        t.check(split && off <= 2,
                fmt::format("Test 1: split, the gap between the views at x {} is the cleared "
                            "frame: off it by {} at most",
                            gap, off));
    }

    const auto snap = [&](const char* camera, f32 x, f32 z, f32 zoom = 120.0f) {
        return run(fmt::format("GetCamera('{}'):SnapTo({{{}, {}, {}}}, {{3.14159, 1.1, 0}}, {})",
                               camera, x, terrain->get_terrain_height(x, z), z, zoom)
                       .c_str());
    };
    const auto sim_lua = [&](const char* code) {
        const bool ok = static_cast<bool>(sim.lua_state.do_string(code));
        sim.sim.tick();
        shots.recapture();
        return ok;
    };
    f32 land_x = 0.0f;
    f32 land_z = 0.0f;
    f32 land_y = -1.0f;
    const f32 size = static_cast<f32>(terrain->map_width());
    for (int iz = 0; iz < 20; ++iz) {
        const f32 z = size * 0.2f + static_cast<f32>(iz) * size / 32.0f;
        for (int ix = 0; ix < 20; ++ix) {
            const f32 x = size * 0.2f + static_cast<f32>(ix) * size / 32.0f;
            if (terrain->get_terrain_height(x, z) > land_y) {
                land_x = x;
                land_z = z;
                land_y = terrain->get_terrain_height(x, z);
            }
        }
    }

    // Each split view draws its own camera: the right one's is WorldCamera2
    // (worldview.lua's CreateMainWorldView), which GetCamera finds and moves
    {
        const bool found = number(ui, "GetCamera('WorldCamera2') and 1 or 0") == 1.0f;
        t.check(found, "Test 2: split, GetCamera('WorldCamera2') finds the right view's camera");
        if (!found || !snap("WorldCamera", land_x, land_z) ||
            !snap("WorldCamera2", land_x, land_z)) {
            return;
        }
        const ImageRGBA8 same = frame();
        const u32 w = same.width;
        const u32 h = same.height;
        const f32 twins = mean_abs_diff(patch(same, w / 4, h / 2), patch(same, w * 3 / 4, h / 2));
        t.check(w > 0 && twins < 0.03f,
                fmt::format("Test 3: two cameras on one spot draw their views alike: the views' "
                            "centres differ by {:.3f}",
                            twins));

        if (!snap("WorldCamera2", 300.0f, 260.0f)) {
            return;
        }
        const ImageRGBA8 moved = frame();
        const f32 left = mean_abs_diff(patch(same, w / 4, h / 2), patch(moved, w / 4, h / 2));
        const f32 right =
            mean_abs_diff(patch(same, w * 3 / 4, h / 2), patch(moved, w * 3 / 4, h / 2));
        t.check(left < 0.01f && right > 0.05f,
                fmt::format("Test 4: moving WorldCamera2 moves the right view alone: the left "
                            "view's centre changed by {:.3f}, the right's by {:.3f}",
                            left, right));

        const f32 focus_x = number(ui, "GetCamera('WorldCamera'):GetFocusPosition()[1]");
        const f32 focus2_x = number(ui, "GetCamera('WorldCamera2'):GetFocusPosition()[1]");
        const f32 focus2_z = number(ui, "GetCamera('WorldCamera2'):GetFocusPosition()[3]");
        run(R"(
            local view = import('/lua/ui/game/worldview.lua').viewRight
            __osc_right_ground = UnProject(view, Vector2(view.Width() / 2, view.Height() / 2))
        )");
        const f32 ground_x = number(ui, "__osc_right_ground[1]");
        const f32 ground_z = number(ui, "__osc_right_ground[3]");
        t.check(std::abs(focus_x - land_x) < 1.0f && std::abs(focus2_x - 300.0f) < 1.0f &&
                    std::abs(ground_x - focus2_x) < 2.0f && std::abs(ground_z - focus2_z) < 2.0f,
                fmt::format("Test 5: the right view picks through its own camera: the middle of "
                            "it is the ground at ({:.1f}, {:.1f}), WorldCamera2 looks at ({:.1f}, "
                            "{:.1f}) and WorldCamera at x {:.1f}",
                            ground_x, ground_z, focus2_x, focus2_z, focus_x));

        if (!sim_lua("__osc_wv_acu = GetArmyBrain('ARMY_1'):GetListOfUnits(categories.COMMAND, "
                     "false)[1]\n"
                     "__osc_wv_at = __osc_wv_acu:GetPosition()")) {
            t.check(false, "Test 6: ARMY_1's commander");
            return;
        }
        const f32 acu_x = number(sim, "__osc_wv_at[1]");
        const f32 acu_y = number(sim, "__osc_wv_at[2]");
        const f32 acu_z = number(sim, "__osc_wv_at[3]");
        const auto zoomed_out = [&](const char* camera) {
            return run(
                fmt::format("GetCamera('{}'):SnapTo({{{}, {}, {}}}, {{3.14159, 1.1, 0}}, 500)",
                            camera, acu_x, acu_y, acu_z)
                    .c_str());
        };
        if (!zoomed_out("WorldCamera") || !zoomed_out("WorldCamera2")) {
            return;
        }
        const ImageRGBA8 with_acu = frame();
        sim_lua("local x, z = __osc_wv_at[1] + 60, __osc_wv_at[3] + 60\n"
                "Warp(__osc_wv_acu, {x, GetTerrainHeight(x, z), z})");
        const ImageRGBA8 without = frame();
        const f32 left_icon =
            mean_abs_diff(patch(with_acu, w / 4, h / 2), patch(without, w / 4, h / 2));
        const f32 right_icon =
            mean_abs_diff(patch(with_acu, w * 3 / 4, h / 2), patch(without, w * 3 / 4, h / 2));
        t.check(left_icon > 0.004f && right_icon > 0.004f,
                fmt::format("Test 6: zoomed out, each view draws the commander's strategic icon: "
                            "the left view's centre changed by {:.3f} as it left, the right's by "
                            "{:.3f}",
                            left_icon, right_icon));
    }

    {
        if (!run(R"(import('/lua/ui/game/borders.lua').SplitMapGroup(false, true))")) {
            return;
        }
        const ImageRGBA8 shot = frame();
        const int off = off_backdrop(shot, shot.width / 2, shot.height / 6, shot.height / 3);
        t.check(shot.width > 0 && off > 10,
                fmt::format("Test 7: joined again, the world is drawn where the gap was: off the "
                            "cleared frame by {}",
                            off));
    }

    if (!run(R"(import('/lua/ui/game/borders.lua').SplitMapGroup(true, true))")) {
        return;
    }
    const auto view_rects = [&] {
        std::vector<renderer::SceneView> views = r.drawn_views();
        return views.size() == 2 ? views : std::vector<renderer::SceneView>{};
    };
    const auto scene = [&] {
        pump(3);
        shots.redraw();
        renderer::Renderer::SceneImage image;
        r.request_scene_capture(
            [&](renderer::Renderer::SceneImage captured) { image = std::move(captured); });
        shots.redraw();
        return image;
    };
    const auto frame_alike = [&](const renderer::Renderer::SceneImage& image,
                                 const renderer::SceneView& v, const renderer::Camera& camera) {
        std::array<size_t, 2> counts{};
        const auto vw = static_cast<u32>(v.w);
        const auto vh = static_cast<u32>(v.h);
        const f32 zoom = camera.zoom();
        const f32 extent = zoom / (v.w / v.h);
        renderer::CartographicRenderer::View frame_view;
        frame_view.width = vw;
        frame_view.height = vh;
        frame_view.x0 = camera.focus_x() - zoom * 0.5f + zoom * 0.5f / std::floor(zoom);
        frame_view.z0 = camera.focus_z() - extent * 0.5f + extent * 0.5f / std::floor(extent);
        frame_view.x1 = frame_view.x0 + zoom;
        frame_view.z1 = frame_view.z0 + extent;
        const auto drawn = r.capture_cartographic(*terrain, frame_view);
        if (!drawn || drawn->output.size() != static_cast<size_t>(vw) * vh * 4 ||
            image.width == 0) {
            return counts;
        }
        const auto vx = static_cast<u32>(v.x);
        const auto vy = static_cast<u32>(v.y);
        for (u32 y = 0; y < vh; ++y) {
            for (u32 x = 0; x < vw; ++x) {
                const size_t at = (static_cast<size_t>(y + vy) * image.width + x + vx) * 4;
                const size_t in = (static_cast<size_t>(y) * vw + x) * 4;
                bool alike = true;
                for (size_t c = 0; c < 3; ++c) {
                    const auto got = std::lround(image.rgba[at + c] * 255.0f);
                    alike = alike && std::abs(got - drawn->output[in + c]) <= 1;
                }
                counts[0] += alike ? 1 : 0;
                ++counts[1];
            }
        }
        return counts;
    };

    {
        if (!snap("WorldCamera", land_x, land_z, 300.0f) ||
            !snap("WorldCamera2", land_x, land_z, 300.0f)) {
            return;
        }
        const renderer::Renderer::SceneImage before = scene();
        run(R"(import('/lua/ui/game/worldview.lua').viewRight:SetCartographic(true))");
        const renderer::Renderer::SceneImage after = scene();
        const auto views = view_rects();
        const renderer::Camera* right = r.cameras().find("WorldCamera2");
        const bool ortho = right && right->ortho() && !r.camera().ortho();
        size_t same = 0;
        size_t pixels = 0;
        f32 left_changed = 1.0f;
        if (right && views.size() == 2 && after.rgba.size() == before.rgba.size() &&
            after.width > 0) {
            const auto counts = frame_alike(after, views[1], *right);
            same = counts[0];
            pixels = counts[1];
            const renderer::SceneView& l = views[0];
            f32 sum = 0.0f;
            size_t n = 0;
            for (u32 y = static_cast<u32>(l.y); y < static_cast<u32>(l.y + l.h); ++y) {
                for (u32 x = static_cast<u32>(l.x); x < static_cast<u32>(l.x + l.w); ++x) {
                    const size_t at = (static_cast<size_t>(y) * after.width + x) * 4;
                    for (size_t c = 0; c < 3; ++c) {
                        sum += std::abs(after.rgba[at + c] - before.rgba[at + c]);
                        ++n;
                    }
                }
            }
            left_changed = n > 0 ? sum / static_cast<f32>(n) : 1.0f;
        }
        run(R"(import('/lua/ui/game/worldview.lua').viewRight:SetCartographic(false))");
        t.check(ortho && pixels > 0 && same * 1000 >= pixels * 995 && left_changed < 0.01f,
                fmt::format("Test 8: the right view made cartographic ({}) draws the map's frame "
                            "through WorldCamera2: {} of {} pixels; the left view's changed by "
                            "{:.3f}",
                            ortho, same, pixels, left_changed));
    }

    sim_lua("__osc_wv_at = __osc_wv_acu:GetPosition()");
    const f32 acu_x = number(sim, "__osc_wv_at[1]");
    const f32 acu_z = number(sim, "__osc_wv_at[3]");
    {
        if (!snap("WorldCamera", acu_x, acu_z) || !snap("WorldCamera2", acu_x, acu_z)) {
            return;
        }
        const auto views = view_rects();
        if (views.size() != 2) {
            t.check(false, "Test 9: split into two views");
            return;
        }
        const auto w = static_cast<u32>(std::min(views[0].w, views[1].w));
        const auto h = static_cast<u32>(std::min(views[0].h, views[1].h));
        const auto pixel = [&](const renderer::Renderer::SceneImage& image, size_t view, u32 x,
                               u32 y) {
            const renderer::SceneView& v = views[view];
            const size_t at = (static_cast<size_t>(y + static_cast<u32>(v.y)) * image.width + x +
                               static_cast<u32>(v.x)) *
                              4;
            return &image.rgba[at];
        };
        const auto mean_over = [&](const auto& f) {
            f32 sum = 0.0f;
            for (u32 y = 0; y < h; ++y) {
                for (u32 x = 0; x < w; ++x) {
                    sum += f(x, y);
                }
            }
            return sum / static_cast<f32>(std::max<u32>(w * h, 1));
        };
        const auto twins = [&](const renderer::Renderer::SceneImage& image) {
            if (image.rgba.empty()) {
                return 1.0f;
            }
            return mean_over([&](u32 x, u32 y) {
                const f32* a = pixel(image, 0, x, y);
                const f32* b = pixel(image, 1, x, y);
                return (std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2])) /
                       3.0f;
            });
        };
        const renderer::Renderer::SceneImage clear = scene();
        r.set_fog_enabled(true);
        sim_lua("");
        const renderer::Renderer::SceneImage fogged = scene();
        sim_lua("local x, z = __osc_wv_at[1] + 30, __osc_wv_at[3]\n"
                "Warp(__osc_wv_acu, {x, GetTerrainHeight(x, z), z})");
        renderer::Renderer::SceneImage moved;
        r.request_scene_capture(
            [&](renderer::Renderer::SceneImage captured) { moved = std::move(captured); });
        shots.redraw();
        r.set_fog_enabled(false);
        f32 dimmed = 0.0f;
        if (clear.rgba.size() == fogged.rgba.size() && !clear.rgba.empty()) {
            dimmed = mean_over([&](u32 x, u32 y) {
                const f32* a = pixel(clear, 0, x, y);
                const f32* b = pixel(fogged, 0, x, y);
                return std::max(0.0f, (a[0] + a[1] + a[2] - b[0] - b[1] - b[2]) / 3.0f);
            });
        }
        const f32 alike = twins(fogged);
        const f32 alike_moved = twins(moved);
        t.check(dimmed > 0.01f && alike < 0.01f && alike_moved < 0.01f,
                fmt::format("Test 9: with the fog on, two cameras on one spot fog their views "
                            "alike: the views differ by {:.4f} (the fog dims them by {:.3f}), and "
                            "by {:.4f} in the frame after the commander moved",
                            alike, dimmed, alike_moved));
    }

    {
        const f32 far_x = acu_x < size * 0.5f ? size * 0.8f : size * 0.2f;
        if (!snap("WorldCamera", far_x, size - far_x) || !snap("WorldCamera2", acu_x, acu_z)) {
            return;
        }
        run(R"(
            local view = import('/lua/ui/game/worldview.lua').viewRight
            view.OnRenderWorld = function(self)
                rawset(_G, '__osc_wv_drawn', (rawget(_G, '__osc_wv_drawn') or 0) + 1)
                UI_DrawCircle(GetCamera('WorldCamera2'):GetFocusPosition(), 15, 'ffff00ff', 2)
            end
            view:SetCustomRender(true)
        )");
        const ImageRGBA8 shot = frame();
        run(R"(import('/lua/ui/game/worldview.lua').viewRight:SetCustomRender(false))");
        const auto views = view_rects();
        std::array<int, 2> drawn{};
        for (size_t i = 0; i < views.size() && shot.width > 0; ++i) {
            const renderer::SceneView& v = views[i];
            for (u32 y = static_cast<u32>(v.y); y < static_cast<u32>(v.y + v.h); ++y) {
                for (u32 x = static_cast<u32>(v.x); x < static_cast<u32>(v.x + v.w); ++x) {
                    const u8* p = &shot.pixels[(static_cast<size_t>(y) * shot.width + x) * 4];
                    drawn[i] += p[0] >= 247 && p[1] <= 8 && p[2] >= 247 ? 1 : 0;
                }
            }
        }
        const f32 calls = number(ui, "rawget(_G, '__osc_wv_drawn') or 0");
        t.check(calls > 0.0f && drawn[1] > 50 && drawn[0] == 0,
                fmt::format("Test 10: the right view's OnRenderWorld ({} calls) draws its ring in "
                            "it: {} pixels, {} in the left view",
                            calls, drawn[1], drawn[0]));
    }

    {
        if (!snap("WorldCamera", land_x, land_z) || !snap("WorldCamera2", land_x, land_z)) {
            return;
        }
        run(R"(
            import('/lua/ui/game/worldview.lua').viewLeft:SetCartographic(true)
            import('/lua/ui/game/worldview.lua').viewRight:SetCartographic(true)
        )");
        const renderer::Renderer::SceneImage near = scene();
        run(R"(
            import('/lua/ui/game/worldview.lua').viewLeft:SetCartographic(false)
            import('/lua/ui/game/worldview.lua').viewRight:SetCartographic(false)
        )");
        const auto views = view_rects();
        const renderer::Camera* right = r.cameras().find("WorldCamera2");
        std::array<f32, 2> meshes{};
        f32 apart = 1.0f;
        if (right && views.size() == 2 && views[0].w == views[1].w && views[0].h == views[1].h &&
            near.width > 0) {
            const std::array<const renderer::Camera*, 2> cameras = {&r.camera(), right};
            for (size_t i = 0; i < 2; ++i) {
                const auto counts = frame_alike(near, views[i], *cameras[i]);
                if (counts[1] > 0) {
                    meshes[i] = 1.0f - static_cast<f32>(counts[0]) / static_cast<f32>(counts[1]);
                }
            }
            size_t differ = 0;
            size_t pixels = 0;
            const auto w = static_cast<u32>(views[1].w);
            const auto h = static_cast<u32>(views[1].h);
            const auto lx = static_cast<u32>(views[0].x);
            const auto ly = static_cast<u32>(views[0].y);
            const auto rx = static_cast<u32>(views[1].x);
            const auto ry = static_cast<u32>(views[1].y);
            for (u32 y = 0; y < h; ++y) {
                for (u32 x = 0; x < w; ++x) {
                    const size_t a = (static_cast<size_t>(y + ly) * near.width + x + lx) * 4;
                    const size_t b = (static_cast<size_t>(y + ry) * near.width + x + rx) * 4;
                    f32 worst = 0.0f;
                    for (size_t c = 0; c < 3; ++c) {
                        worst = std::max(worst, std::abs(near.rgba[a + c] - near.rgba[b + c]));
                    }
                    differ += worst * 255.0f > 1.0f ? 1 : 0;
                    ++pixels;
                }
            }
            apart = static_cast<f32>(differ) / static_cast<f32>(pixels);
        }
        t.check(meshes[0] > 0.01f && meshes[1] > 0.01f && apart < 0.001f,
                fmt::format("Test 11: near, both views cartographic on one spot draw the meshes "
                            "alike: {:.2f}% and {:.2f}% of their pixels off the meshless frame, "
                            "{:.3f}% of them apart",
                            meshes[0] * 100.0f, meshes[1] * 100.0f, apart * 100.0f));
    }

    lua_pushstring(L, "__osc_renderer");
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    t.check(renderer::Renderer::validation_error_count() == errors_before,
            "no Vulkan validation errors drawing the views");
    spdlog::info("World views test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
