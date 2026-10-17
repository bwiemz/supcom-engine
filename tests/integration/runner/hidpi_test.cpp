#include "hidpi_test.hpp"

#include "app/app.hpp"
#include "app/window_commands.hpp"
#include "core/image.hpp"
#include "core/test_status.hpp"
#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "render_probe.hpp"
#include "renderer/minimap_renderer.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"
#include "ui/ui_layout.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr u32 kWidth = 1512;
constexpr u32 kHeight = 982;
constexpr i32 kTextSize = 24;

struct Frame {
    u32 width = 0;
    u32 height = 0;
    u32 ui_width = 0;
    u32 ui_height = 0;
    f64 root_width = 0;
    f64 root_height = 0;
    u64 square = 0;
    u64 text = 0;
    bool raster = false;
};

f64 ui_number(lua::LuaState& state, const std::string& expr) {
    lua_State* L = state.raw();
    if (!state.do_string("rawset(_G, '__osc_hidpi_value', " + expr + ")")) {
        return -1.0;
    }
    lua_pushstring(L, "__osc_hidpi_value");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const f64 v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1.0;
    lua_pop(L, 1);
    return v;
}

u64 count_pixels(const ImageRGBA8& img, const std::function<bool(const u8*)>& is) {
    u64 n = 0;
    for (size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
        if (is(&img.pixels[i])) {
            ++n;
        }
    }
    return n;
}

Frame draw_at(app::Engine& e, f32 scale) {
    Frame f;
    lua_State* L = e.ui_lua_state.raw();
    renderer::Renderer r;
    r.set_offscreen_scale(scale);
    if (!r.init(kWidth, kHeight, "HiDPI Test", /*offscreen=*/true)) {
        return f;
    }
    r.init_ui_caches(&e.vfs);
    r.set_fixed_frame_dt(1.0f / 30.0f);
    app::size_root_frame(L, r.ui_width(), r.ui_height());
    if (auto res = e.ui_lua_state.do_string(
            fmt::format("local Bitmap = import('/lua/maui/bitmap.lua').Bitmap\n"
                        "local UIUtil = import('/lua/ui/uiutil.lua')\n"
                        "local LayoutHelpers = import('/lua/maui/layouthelpers.lua')\n"
                        "local panel = Bitmap(GetFrame(0))\n"
                        "panel:SetSolidColor('ff000000')\n"
                        "LayoutHelpers.AtLeftTopIn(panel, GetFrame(0), 100, 100)\n"
                        "panel.Width:Set(500)\n"
                        "panel.Height:Set(300)\n"
                        "panel.Depth:Set(100000)\n"
                        "local square = Bitmap(panel)\n"
                        "square:SetSolidColor('ffff00ff')\n"
                        "LayoutHelpers.AtLeftTopIn(square, panel, 300, 50)\n"
                        "square.Width:Set(150)\n"
                        "square.Height:Set(100)\n"
                        "square.Depth:Set(100001)\n"
                        "local text = UIUtil.CreateText(panel, 'Retina 1234', {}, 'Arial')\n"
                        "text:SetColor('ff00ff00')\n"
                        "LayoutHelpers.AtLeftTopIn(text, panel, 20, 50)\n"
                        "text.Depth:Set(100001)\n"
                        "rawset(_G, '__osc_hidpi', panel)\n",
                        kTextSize));
        !res) {
        osc::test_status::fail("[FAIL] hidpi-test Lua: {}", res.error().message);
    }
    ImageRGBA8 shot;
    for (int i = 0; i < 3; ++i) {
        if (i == 2) {
            r.request_capture([&](ImageRGBA8 captured) { shot = std::move(captured); });
        }
        r.render_ui_only(L, &e.ui_registry);
    }
    f.width = shot.width;
    f.height = shot.height;
    f.ui_width = r.ui_width();
    f.ui_height = r.ui_height();
    f.root_width = ui_number(e.ui_lua_state, "GetFrame(0).Width()");
    f.root_height = ui_number(e.ui_lua_state, "GetFrame(0).Height()");
    f.square = count_pixels(shot, [](const u8* p) { return p[0] > 250 && p[1] < 5 && p[2] > 250; });
    f.text = count_pixels(shot, [](const u8* p) { return p[0] < 60 && p[1] > 128 && p[2] < 60; });
    f.raster = r.font_cache().loaded("Arial", renderer::raster_pointsize(kTextSize, scale));
    (void)e.ui_lua_state.do_string("rawget(_G, '__osc_hidpi'):Destroy()");
    r.shutdown();
    return f;
}

constexpr const char* kGameUiHidden = "import('/lua/ui/game/gamemain.lua').gameUIHidden and 1 or 0";
constexpr const char* kMiniMap = "import('/lua/ui/game/minimap.lua').controls.miniMap";
constexpr std::array<const char*, 12> kOverMiniMap = {"GlowTL", "GlowTR", "GlowBR", "GlowBL",
                                                      "GlowL",  "GlowR",  "GlowT",  "GlowB",
                                                      "DragTL", "DragTR", "DragBL", "DragBR"};

ui::ControlRect control_area(lua::LuaState& state, const std::string& control) {
    const auto edge = [&](const char* name) {
        return static_cast<f32>(ui_number(state, control + "." + name + "()"));
    };
    return {edge("Left"), edge("Top"), edge("Right") - edge("Left"), edge("Bottom") - edge("Top")};
}

struct Copied {
    u64 pixels = 0;
    u64 matched = 0;
};

Copied compare_copy(const ImageRGBA8& shot, u32 x0, u32 y0,
                    const renderer::Renderer::CartographicImages& drawn,
                    const std::vector<ui::ControlRect>& over) {
    Copied c;
    for (u32 j = 0; j < drawn.height; ++j) {
        for (u32 i = 0; i < drawn.width; ++i) {
            const u32 x = x0 + i;
            const u32 y = y0 + j;
            const f32 cx = static_cast<f32>(x) + 0.5f;
            const f32 cy = static_cast<f32>(y) + 0.5f;
            const bool covered =
                std::any_of(over.begin(), over.end(), [&](const ui::ControlRect& r) {
                    return cx >= r.x && cx < r.x + r.w && cy >= r.y && cy < r.y + r.h;
                });
            if (x >= shot.width || y >= shot.height || covered) {
                continue;
            }
            ++c.pixels;
            const size_t s = (static_cast<size_t>(y) * shot.width + x) * 4;
            const size_t d = (static_cast<size_t>(j) * drawn.width + i) * 4;
            if (std::equal(&shot.pixels[s], &shot.pixels[s] + 3, &drawn.output[d])) {
                ++c.matched;
            }
        }
    }
    return c;
}

} // namespace

void run_hidpi_test(app::Engine& e) {
    spdlog::info("=== HiDPI test ===");
    Tally t;
    const Frame one = draw_at(e, 1.0f);
    const Frame two = draw_at(e, 2.0f);
    if (one.width == 0 || two.width == 0) {
        osc::test_status::fail("[FAIL] hidpi-test: no Vulkan device");
        return;
    }

    t.check(two.width == 2 * kWidth && two.height == 2 * kHeight && two.ui_width == kWidth &&
                two.ui_height == kHeight && two.root_width == kWidth &&
                two.root_height == kHeight && one.root_width == kWidth,
            fmt::format("Test 1: a Retina frame of {}x{} pixels lays the UI out at {}x{} points "
                        "(the root frame {}x{}; {} at scale 1)",
                        two.width, two.height, two.ui_width, two.ui_height, two.root_width,
                        two.root_height, one.root_width));

    const f64 share_one = static_cast<f64>(one.square) / (static_cast<f64>(one.width) * one.height);
    const f64 share_two = static_cast<f64>(two.square) / (static_cast<f64>(two.width) * two.height);
    t.check(one.square == 150 * 100 && two.square == 4 * one.square,
            fmt::format("Test 2: a 150x100 square covers {} pixels at scale 1 and {} at scale 2, "
                        "the same share of the frame ({:.5f}, {:.5f})",
                        one.square, two.square, share_one, share_two));

    const f64 ink = static_cast<f64>(two.text) / (4.0 * static_cast<f64>(one.text));
    t.check(one.text > 100 && std::abs(ink - 1.0) < 0.25 && two.raster && one.raster,
            fmt::format("Test 3: the text's ink at scale 2 is {:.2f} of scale 1's four times ({} "
                        "and {} pixels), from a {}px raster of its {}pt font ({})",
                        ink, two.text, one.text, renderer::raster_pointsize(kTextSize, 2.0f),
                        kTextSize, two.raster));

    spdlog::info("HiDPI test: {}/{} passed", t.pass, t.pass + t.fail);
}

void run_hidpi_minimap_test(TestContext& ui, const std::function<void(int)>& pump) {
    spdlog::info("=== HiDPI test: the minimap window ===");
    Tally t;
    lua_State* L = ui.L;
    const map::Terrain* terrain = ui.sim.terrain();
    lua_pushstring(L, "osc_ui_registry");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* registry = static_cast<ui::UIControlRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!terrain || !registry) {
        t.check(false, "a map is loaded and FA's game interface is up");
        return;
    }
    constexpr u32 kScale = 2;
    OffscreenShots shots(ui, static_cast<f32>(kScale));
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.init_ui_caches(&ui.vfs);
    app::size_root_frame(L, r.ui_width(), r.ui_height());
    int frames = 0;
    while (frames < 600 && ui_number(ui.lua_state, kGameUiHidden) == 1.0) {
        pump(1);
        ++frames;
    }
    t.check(ui_number(ui.lua_state, "import('/lua/ui/game/minimap.lua').controls.displayGroup:"
                                    "IsHidden() and 0 or 1") == 1.0,
            fmt::format("Test 4: once the game interface shows ({} frames), so does the minimap "
                        "window, as the profile's stratview asks",
                        frames));

    const auto map_w = static_cast<f32>(terrain->map_width());
    const auto map_h = static_cast<f32>(terrain->map_height());
    (void)shots.shoot_frame(*terrain, map_w * 0.5f, map_h * 0.5f, 200.0f);
    shots.set_ui(registry);
    const ui::ControlRect view = control_area(ui.lua_state, kMiniMap);
    const auto points =
        renderer::cartographic_minimap(view.x, view.y, view.w, view.h, map_w, map_h);
    if (!points) {
        t.check(false, "the minimap view has a cartographic frame");
        shots.set_ui(nullptr);
        return;
    }
    const auto to_pixels = [](const ui::ControlRect& p) {
        constexpr auto s = static_cast<f32>(kScale);
        return ui::ControlRect{p.x * s, p.y * s, p.w * s, p.h * s};
    };
    const ui::ControlRect pixels =
        to_pixels({points->rect.x, points->rect.y, points->rect.w, points->rect.h});
    renderer::CartographicRenderer::View frame = points->frame;
    frame.width *= kScale;
    frame.height *= kScale;

    std::array<ImageRGBA8, 2> images;
    std::array<std::vector<renderer::UIQuad>, 2> painted;
    for (size_t f = 0; f < images.size(); ++f) {
        images[f] = shots.grab();
        painted[f] = r.painted_minimap();
    }
    const renderer::CartographicRenderer& carto = r.cartographic();
    t.check(carto.width(0) == frame.width && carto.height(0) == frame.height &&
                carto.width(1) == frame.width && carto.height(1) == frame.height,
            fmt::format("Test 5: the {}x{} point view's cartographic frame is its {}x{} pixels "
                        "({}x{} and {}x{} drawn)",
                        points->rect.w, points->rect.h, frame.width, frame.height, carto.width(0),
                        carto.height(0), carto.width(1), carto.height(1)));

    std::vector<ui::ControlRect> decorations;
    decorations.reserve(kOverMiniMap.size());
    for (const char* name : kOverMiniMap) {
        decorations.push_back(
            to_pixels(control_area(ui.lua_state, std::string(kMiniMap) + "." + name)));
    }
    const auto drawn = r.capture_cartographic(*terrain, frame);
    bool placed = true;
    Copied copied;
    for (size_t f = 0; f < images.size(); ++f) {
        const std::vector<renderer::UIQuad>& ps = painted[f];
        const auto quad = [&](size_t q) {
            const auto& rect = ps[q].inst.rect;
            return to_pixels({rect[0], rect[1], rect[2], rect[3]});
        };
        placed = placed && !ps.empty() &&
                 (ps[0].texture_ds == carto.ui_descriptor(0) ||
                  ps[0].texture_ds == carto.ui_descriptor(1)) &&
                 quad(0).x == pixels.x && quad(0).y == pixels.y && quad(0).w == pixels.w &&
                 quad(0).h == pixels.h;
        std::vector<ui::ControlRect> over = decorations;
        for (size_t q = 1; q < ps.size(); ++q) {
            over.push_back(quad(q));
        }
        if (drawn && !images[f].pixels.empty()) {
            const Copied c = compare_copy(images[f], static_cast<u32>(pixels.x),
                                          static_cast<u32>(pixels.y), *drawn, over);
            copied.pixels += c.pixels;
            copied.matched += c.matched;
        }
    }
    t.check(placed, fmt::format("Test 6: in both frames the view's first quad is the "
                                "cartographic frame, over pixels ({}, {}) {}x{}",
                                pixels.x, pixels.y, pixels.w, pixels.h));
    t.check(copied.pixels > 0 && copied.matched == copied.pixels,
            fmt::format("Test 7: {} of {} pixels the frame's, copied pixel for pixel (the "
                        "window's glows and handles, the icons and the camera's outline aside)",
                        copied.matched, copied.pixels));
    shots.set_ui(nullptr);
    spdlog::info("HiDPI minimap test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
