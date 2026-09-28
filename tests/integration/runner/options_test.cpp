// --options-test (M217i): FA's options, applied.
//
// A profile's saved options reach the engine as Moho applies them at
// startup (optionsLogic.Apply(true), each option's ConExecute through the
// console's variables): the camera's wheel zoom and keyboard speeds, the
// scroll switches, the strategic icons, the bloom and the video options.
// Then a player's console commands, with Moho's operators, and the cursor
// clip on a hidden window.

#include "options_test.hpp"

#include "app/window_commands.hpp"
#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "render_probe.hpp"
#include "renderer/camera.hpp"
#include "renderer/renderer.hpp"
#include "renderer/strategic_icon_renderer.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"
#include "ui/console.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <cmath>

namespace osc::test {

namespace {

bool near(f32 a, f32 b) {
    return std::abs(a - b) < 1e-5f;
}

} // namespace

void test_options(TestContext& ui, TestContext& sim, const std::function<void(int)>& pump) {
    spdlog::info("=== Options test (M217i) ===");
    Tally t;
    lua_State* L = ui.L;
    const map::Terrain* terrain = sim.sim.terrain();
    lua_pushstring(L, "__osc_console");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* console = static_cast<ui::Console*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!terrain || !console) {
        t.check(false, "the map and the UI state's console");
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
    app::register_option_commands(*console);
    const renderer::Camera& cam = r.camera();

    // Test 1: a profile's saved options apply as at startup
    {
        const auto saved = ui.lua_state.do_string(R"(
            local Prefs = import('/lua/user/prefs.lua')
            if not Prefs.GetCurrentProfile() then Prefs.CreateProfile('options-test') end
            local o = import('/lua/options/optionsLogic.lua').GetCurrent()
            o.wheel_sensitivity = 25
            o.keyboard_pan_speed = 120
            o.keyboard_pan_accelerate_multiplier = 3
            o.keyboard_rotate_speed = 20
            o.keyboard_rotate_accelerate_multiplier = 5
            o.screen_edge_pans_main_view = 0
            o.arrow_keys_pan_main_view = 0
            o.strat_icons_always_on = 1
            o.bloom_render = 0
            o.render_skydome = 0
            o.fidelity = 2
            o.shadow_quality = 3
            o.texture_level = 2
            o.level_of_detail = 2
            o.lock_fullscreen_cursor_to_window = 1
            Prefs.SetToCurrentProfile('options', o)
        )");
        app::apply_options(L);
        pump(1);
        const auto& v = r.video_options();
        t.check(static_cast<bool>(saved) && near(cam.zoom_amount(), 0.25f) &&
                    near(cam.keyboard_pan_speed(), 120.0f) &&
                    near(cam.keyboard_pan_accelerate(), 3.0f) &&
                    near(cam.keyboard_rotate_speed(), 20.0f) &&
                    near(cam.keyboard_rotate_accelerate(), 5.0f) && !cam.edge_scroll() &&
                    !cam.arrow_scroll() && r.icons_always() && !r.bloom_enabled() && !v.skydome &&
                    v.graphics_fidelity == 2 && v.shadow_fidelity == 3 && v.mip_skip_levels == 2 &&
                    near(v.camera_scale_lod, 2.0f) && r.cursor_clipped(),
                fmt::format("Test 1: applied, the wheel's zoom is {:.2f} (0.25), the keys pan "
                            "{} (x{}) and turn {} (x{}); edges {}, arrows {}; icons always {}; "
                            "bloom {}, sky {}; fidelity {}, shadows {}, mips skipped {}, LOD {}; "
                            "the cursor held {}",
                            cam.zoom_amount(), cam.keyboard_pan_speed(),
                            cam.keyboard_pan_accelerate(), cam.keyboard_rotate_speed(),
                            cam.keyboard_rotate_accelerate(), cam.edge_scroll(), cam.arrow_scroll(),
                            r.icons_always(), r.bloom_enabled(), v.skydome, v.graphics_fidelity,
                            v.shadow_fidelity, v.mip_skip_levels, v.camera_scale_lod,
                            r.cursor_clipped()));
    }

    // Test 2: a player's console, through ConExecute, with Moho's operators
    {
        const auto typed =
            ui.lua_state.do_string("ConExecute('cam_ZoomAmount *= 2') "
                                   "ConExecute('ui_ArrowKeysScrollView tog') "
                                   "ConExecute('ren_bloom on') "
                                   "ConExecute('graphics_Fidelity -= 1') "
                                   "ConExecute('ui_KeyboardPanSpeed')"); // shown, not changed
        t.check(static_cast<bool>(typed) && near(cam.zoom_amount(), 0.5f) && cam.arrow_scroll() &&
                    r.bloom_enabled() && r.video_options().graphics_fidelity == 1 &&
                    near(cam.keyboard_pan_speed(), 120.0f),
                fmt::format("Test 2: typed, the wheel's zoom is {:.2f}, arrows {}, bloom {}, "
                            "fidelity {}, the pan speed still {}",
                            cam.zoom_amount(), cam.arrow_scroll(), r.bloom_enabled(),
                            r.video_options().graphics_fidelity, cam.keyboard_pan_speed()));
    }

    // Test 3: SC_ToggleCursorClip holds a window's cursor, and "0" lets go
    {
        console->execute(L, "SC_ToggleCursorClip");
        const bool held = r.cursor_clipped();
        console->execute(L, "SC_ToggleCursorClip 0");
        const bool let_go = !r.cursor_clipped();
        t.check(held && let_go,
                fmt::format("Test 3: the cursor held ({}), then let go ({})", held, let_go));
    }

    // Test 4: ui_AlwaysRenderStrategicIcons draws the units' icons at a zoom
    // their meshes show at (inside their IconFadeInZoom), and off, not
    {
        const u32 acu = army_acu_id(sim.sim, 0);
        const auto* e = sim.sim.entity_registry().find(acu);
        u32 on = 0;
        u32 off = 0;
        if (e) {
            console->execute(L, "ui_AlwaysRenderStrategicIcons on");
            (void)shots.shoot(*terrain, e->position().x, e->position().z, 40.0f);
            shots.redraw();
            on = r.strategic_icons().quad_count();
            console->execute(L, "ui_AlwaysRenderStrategicIcons off");
            shots.redraw();
            off = r.strategic_icons().quad_count();
        }
        t.check(e && on > 0 && off == 0,
                fmt::format("Test 4: near the ACU, {} icons drawn with the icons always on, {} "
                            "with them off",
                            on, off));
    }

    // Test 5: ren_Skydome draws the sky dome, and off, leaves the clear
    // (WRenViewport::Render's ren_SkyDome): looking north over the map's
    // edge, nearly level, the scene's top tenth (under the UI) is sky
    {
        const auto top = [&](const char* command) {
            console->execute(L, command);
            r.camera().set_heading(std::atan2(0.0f, -1.0f)); // north: toward -z
            r.camera().set_pitch(0.1f);
            (void)shots.shoot(*terrain, 512.0f, 4.0f, 150.0f, false);
            renderer::Renderer::SceneImage scene;
            r.request_scene_capture(
                [&](renderer::Renderer::SceneImage image) { scene = std::move(image); });
            shots.redraw();
            // Its distance from the clear, per channel
            f32 off_clear = 0.0f;
            u32 n = 0;
            for (u32 y = 0; y < scene.height / 10; ++y)
                for (u32 x = 0; x < scene.width; x += 4) {
                    const size_t i = (static_cast<size_t>(y) * scene.width + x) * 4;
                    for (size_t c = 0; c < 3; ++c)
                        off_clear += std::abs(scene.rgba[i + c] - OffscreenShots::kBackdrop[c]);
                    ++n;
                }
            return n ? off_clear / static_cast<f32>(n * 3) : -1.0f;
        };
        const f32 sky = top("ren_Skydome on");
        const f32 clear = top("ren_Skydome off");
        t.check(!r.video_options().skydome && sky > 0.02f && clear >= 0.0f && clear < 0.005f,
                fmt::format("Test 5: the scene's top is {:.4f} off the clear with the sky dome, "
                            "{:.4f} without",
                            sky, clear));
    }

    lua_pushstring(L, "__osc_renderer");
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    spdlog::info("Options test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
