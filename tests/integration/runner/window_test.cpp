// --window-test (M217h): the window's side of FA's video options.
//
// An offscreen renderer's hidden window takes SC_PrimaryAdapter "windowed"
// at the prefs' size: the swapchain is rebuilt at it, a frame renders at
// it, and the UI's root frame follows. SC_VerticalSync takes the vsync
// option. The options screen (optionsLogic.GetOptionsData) reads the
// display's modes back as SetupPrimaryAdapterSettings publishes them. The
// commands run on a console of the test's own, with prefs of its own.

#include "window_test.hpp"

#include "app/window_commands.hpp"
#include "core/image.hpp"
#include "core/preferences.hpp"
#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "render_probe.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"
#include "ui/console.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace osc::test {

namespace {

f64 ui_number(TestContext& ui, const std::string& expr) {
    lua_State* L = ui.L;
    auto r = ui.lua_state.do_string("__osc_window_test_value = " + expr);
    if (!r) return -1.0;
    lua_pushstring(L, "__osc_window_test_value");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const f64 v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1.0;
    lua_pop(L, 1);
    return v;
}

std::string ui_string(TestContext& ui, const std::string& expr) {
    lua_State* L = ui.L;
    auto r = ui.lua_state.do_string("__osc_window_test_value = " + expr);
    if (!r) return "(error: " + r.error().message + ")";
    lua_pushstring(L, "__osc_window_test_value");
    lua_rawget(L, LUA_GLOBALSINDEX);
    std::string v = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "(not a string)";
    lua_pop(L, 1);
    return v;
}

} // namespace

void test_window(TestContext& ui, TestContext& sim, const std::function<void(int)>& pump) {
    spdlog::info("=== Window test (M217h) ===");
    Tally t;
    lua_State* L = ui.L;
    const map::Terrain* terrain = sim.sim.terrain();
    if (!terrain) {
        t.check(false, "the map");
        return;
    }
    OffscreenShots shots(sim);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    (void)shots.shoot(*terrain, 512.0f, 512.0f, 200.0f, false);
    lua_pushstring(L, "__osc_renderer");
    lua_pushlightuserdata(L, &r);
    lua_rawset(L, LUA_REGISTRYINDEX);

    core::Preferences prefs;
    prefs.ensure_profile("window-test");
    const std::string options = prefs.current_profile_path() + ".options.";
    ui::Console console;
    app::register_window_commands(console, prefs, false);
    // A few frames, as the windowed loop runs them: the UI's root frame
    // follows a resized swapchain
    const auto frames = [&](int n) {
        for (int i = 0; i < n; ++i) {
            shots.redraw();
            if (r.take_resized()) app::size_root_frame(L, r.width(), r.height());
            pump(1);
        }
    };

    // Test 1: "windowed" is the window at the prefs' size: the swapchain is
    // rebuilt at it, a frame renders at it, and the root frame follows
    {
        prefs.set_int("Windows.Main.width", 1280);
        prefs.set_int("Windows.Main.height", 800);
        console.execute(L, "SC_PrimaryAdapter windowed");
        // The resize's own event marks the swapchain stale, before any frame
        // could find it out of date (Wayland never says so). The window
        // system sends it when it gets to it, later on a loaded machine:
        // wait for it a while, not for a number of polls
        bool stale = false;
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!stale && std::chrono::steady_clock::now() < give_up) {
            r.poll_events(0.0);
            stale = r.swapchain_stale();
            if (!stale) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        frames(5);
        const ImageRGBA8 shot = shots.grab();
        const f64 fw = ui_number(ui, "GetFrame(0).Width()");
        const f64 fh = ui_number(ui, "GetFrame(0).Height()");
        t.check(
            stale && r.width() == 1280 && r.height() == 800 && shot.width == 1280 &&
                shot.height == 800 && fw == 1280.0 && fh == 800.0 && !r.fullscreen(),
            fmt::format("Test 1: windowed at the prefs' 1280x800: the resize marks the "
                        "swapchain stale ({}); it is {}x{}, a frame {}x{}, the root frame {}x{}",
                        stale, r.width(), r.height(), shot.width, shot.height, fw, fh));
    }

    // Test 2: "overridden" and a junk mode leave it be
    {
        console.execute(L, "SC_PrimaryAdapter overridden");
        console.execute(L, "SC_PrimaryAdapter 12,x,60");
        frames(3);
        t.check(r.width() == 1280 && r.height() == 800 && !r.fullscreen(),
                fmt::format("Test 2: after \"overridden\" and a junk mode the window is still "
                            "{}x{}, windowed",
                            r.width(), r.height()));
    }

    // Test 3: SC_VerticalSync takes the vsync option: off, not FIFO; on, FIFO
    {
        prefs.set_int(options + "vsync", 0);
        console.execute(L, "SC_VerticalSync 0");
        frames(2);
        const VkPresentModeKHR off = r.present_mode();
        prefs.set_int(options + "vsync", 1);
        console.execute(L, "SC_VerticalSync 1");
        frames(2);
        const VkPresentModeKHR on = r.present_mode();
        t.check(r.vsync() && off != VK_PRESENT_MODE_FIFO_KHR && on == VK_PRESENT_MODE_FIFO_KHR,
                fmt::format("Test 3: vsync off presents in mode {}, on in mode {} (FIFO is {})",
                            static_cast<int>(off), static_cast<int>(on),
                            static_cast<int>(VK_PRESENT_MODE_FIFO_KHR)));
    }

    // Test 4: the options screen reads the display's modes back as Moho
    // publishes them: "Windowed", then the modes of at least 1024x768
    {
        std::vector<app::Resolution> modes;
        for (const auto& m : r.display_modes()) modes.push_back({m[0], m[1], m[2]});
        app::publish_adapter_options(L, modes, false);
        const auto [want, fallback] = app::adapter_states(modes, false);
        const std::string find =
            "(function() for _, s in import('/lua/options/optionsLogic.lua').GetOptionsData() do "
            "for _, i in s.items do if i.key == '{}' then return i end end end end)()";
        const std::string primary = fmt::format(fmt::runtime(find), "primary_adapter");
        const std::string secondary = fmt::format(fmt::runtime(find), "secondary_adapter");
        const f64 count = ui_number(ui, "table.getn(" + primary + ".custom.states)");
        const std::string first = ui_string(ui, primary + ".custom.states[1].key");
        const std::string last_key = ui_string(ui, primary + ".custom.states[table.getn(" +
                                                       primary + ".custom.states)].key");
        const std::string last_text = ui_string(ui, primary + ".custom.states[table.getn(" +
                                                        primary + ".custom.states)].text");
        const std::string default_value = ui_string(ui, primary + ".default");
        const std::string disabled = ui_string(ui, secondary + ".custom.states[1].key");
        t.check(count == static_cast<f64>(want.size()) && first == "windowed" &&
                    last_key == want.back().key && last_text == want.back().text &&
                    default_value == fallback && disabled == "disabled" && want.size() > 1,
                fmt::format("Test 4: the options screen lists {} states ({} wanted), first {}, "
                            "last {} \"{}\", default {}; the secondary adapter {}",
                            count, want.size(), first, last_key, last_text, default_value,
                            disabled));
    }

    // Back to the capture size for whatever runs after
    prefs.set_int("Windows.Main.width", 1600);
    prefs.set_int("Windows.Main.height", 900);
    console.execute(L, "SC_PrimaryAdapter windowed");
    frames(3);
    lua_pushstring(L, "__osc_renderer");
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    spdlog::info("Window test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
