#include "hidpi_test.hpp"

#include "app/app.hpp"
#include "app/window_commands.hpp"
#include "core/image.hpp"
#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "render_probe.hpp"
#include "renderer/renderer.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <functional>

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

f64 ui_number(app::Engine& e, const char* expr) {
    lua_State* L = e.ui_lua_state.raw();
    if (!e.ui_lua_state.do_string(std::string("rawset(_G, '__osc_hidpi_value', ") + expr + ")")) {
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
    f.root_width = ui_number(e, "GetFrame(0).Width()");
    f.root_height = ui_number(e, "GetFrame(0).Height()");
    f.square = count_pixels(shot, [](const u8* p) { return p[0] > 250 && p[1] < 5 && p[2] > 250; });
    f.text = count_pixels(shot, [](const u8* p) { return p[0] < 60 && p[1] > 128 && p[2] < 60; });
    f.raster = r.font_cache().loaded("Arial", renderer::raster_pointsize(kTextSize, scale));
    (void)e.ui_lua_state.do_string("rawget(_G, '__osc_hidpi'):Destroy()");
    r.shutdown();
    return f;
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

} // namespace osc::test
