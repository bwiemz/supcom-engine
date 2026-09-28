// --camera-moves-test (M217g): the camera's moves through FA's scripts.
//
// In a game with FA's interface, the world camera (an offscreen renderer's,
// on a clock the test steps) takes Lua's moves:
// - the game's opening zoom (gamemain's UIZoomTo over a second);
// - MoveTo eases over its seconds;
// - WaitFor(camera) parks a UI thread until a move ends;
// - UIZoomTo frames a unit;
// - TrackEntities follows one as it walks;
// - a sim script's SimCamera move goes through Sync.CameraRequests to
//   usercamera.lua, which moves the camera, waits for it, and calls the sim
//   back, so the script resumes.

#include "camera_moves_test.hpp"

#include "core/test_status.hpp"
#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "map/terrain.hpp"
#include "render_probe.hpp"
#include "renderer/camera.hpp"
#include "renderer/renderer.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <string>

namespace osc::test {

void test_camera_moves(TestContext& ui, TestContext& sim, const std::function<void(int)>& pump,
                       const std::function<void(int)>& play,
                       const std::function<bool(const char*)>& sim_lua) {
    spdlog::info("=== Camera moves test (M217g) ===");
    Tally t;
    lua_State* L = ui.L;
    const map::Terrain* terrain = sim.sim.terrain();
    const u32 acu = army_acu_id(sim.sim, 0);
    if (!terrain || acu == 0) {
        t.check(false, "the map and ARMY_1's ACU");
        return;
    }
    OffscreenShots shots(sim);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    (void)shots.shoot(*terrain, 512.0f, 512.0f, 200.0f, false);
    renderer::Camera& cam = r.camera();
    cam.set_free(false);
    cam.reset();
    // The UI's camera bindings reach this renderer's camera
    lua_pushstring(L, "__osc_renderer");
    lua_pushlightuserdata(L, &r);
    lua_rawset(L, LUA_REGISTRYINDEX);
    // The camera's frames, on a clock the test steps (the renderer's poll
    // would read the wall's)
    f64 clock = 0.0;
    const auto step = [&](f64 dt) {
        clock += dt;
        cam.set_clocks(clock, 0.0);
        cam.frame(dt);
    };
    step(0.0);
    const auto run = [&](const std::string& code) {
        auto result = ui.lua_state.do_string(code);
        if (!result)
            osc::test_status::fail("[FAIL] camera-moves-test Lua: {}", result.error().message);
        return static_cast<bool>(result);
    };
    // Raw reads: config.lua's lock makes an unset global's read an error
    const auto ui_number = [&](const char* name) {
        lua_pushstring(L, name);
        lua_rawget(L, LUA_GLOBALSINDEX);
        const f64 v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1.0;
        lua_pop(L, 1);
        return v;
    };
    const auto ui_true = [&](const char* name) {
        lua_pushstring(L, name);
        lua_rawget(L, LUA_GLOBALSINDEX);
        const bool v = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return v;
    };

    // Test 0: the game's opening (gamemain's OnFirstUpdate): 1.5 s in,
    // UIZoomTo(the commander, 1) closes on the ACU over a second
    {
        play(40); // 4 s: the opening's zoom has begun, the input unlocked
        const auto* e = sim.sim.entity_registry().find(acu);
        const bool began = cam.moving() && std::abs(cam.requested_zoom() - 40.0f) < 1e-3f;
        step(1.1);
        const bool framed = e && !cam.moving() && std::abs(cam.zoom() - 40.0f) < 1e-3f &&
                            std::abs(cam.target_x() - e->position().x) < 0.5f &&
                            std::abs(cam.target_z() - e->position().z) < 0.5f;
        t.check(began && framed,
                fmt::format("Test 0: the opening's UIZoomTo glides to the ACU over a second "
                            "({}), framing it at zoom {:.1f} ({})",
                            began ? "began" : "didn't begin", cam.zoom(),
                            framed ? "there" : "not there"));
    }

    // Test 1: MoveTo over a second eases there: half way at half the time
    {
        const f32 x = 400.0f;
        const f32 z = 300.0f;
        const f32 y = terrain->get_surface_height(x, z);
        const f32 zoom0 = cam.zoom();
        const f32 pitch0 = cam.pitch();
        run(fmt::format(
            "GetCamera('WorldCamera'):MoveTo({{{}, {}, {}}}, {{3.0, 0.9, 0}}, 120, 1.0)", x, y, z));
        const bool moving = cam.moving();
        step(0.5);
        const f32 half_zoom = cam.zoom();
        const f32 half_pitch = cam.pitch();
        step(0.5);
        run("__cam_zoom = GetCamera('WorldCamera'):GetZoom() "
            "__cam_heading = GetCamera('WorldCamera'):GetHeading()");
        const bool half = std::abs(half_zoom - (zoom0 + (120.0f - zoom0) * 0.5f)) < 0.01f &&
                          std::abs(half_pitch - (pitch0 + (0.9f - pitch0) * 0.5f)) < 1e-3f;
        const bool there = !cam.moving() && std::abs(ui_number("__cam_zoom") - 120.0) < 1e-3 &&
                           std::abs(ui_number("__cam_heading") - 3.0) < 1e-4 &&
                           std::abs(cam.pitch() - 0.9f) < 1e-4f &&
                           std::abs(cam.target_x() - x) < 1e-3f;
        t.check(moving && half && there,
                fmt::format("Test 1: MoveTo over 1 s is half way at 0.5 s (zoom {:.2f} from "
                            "{:.2f} to 120, pitch {:.3f}) and there at 1 s (zoom {:.2f}, "
                            "heading {:.3f})",
                            half_zoom, zoom0, half_pitch, ui_number("__cam_zoom"),
                            ui_number("__cam_heading")));
    }

    // Test 2: WaitFor(camera) parks a UI thread until the move ends; with
    // no move under way it returns at once
    {
        run("__cam_waited = nil ForkThread(function() WaitFor(GetCamera('WorldCamera')) "
            "__cam_waited = true end)");
        pump(2);
        const bool idle = ui_true("__cam_waited");
        run("__cam_waited = nil GetCamera('WorldCamera'):SetZoom(300, 1.0) "
            "ForkThread(function() WaitFor(GetCamera('WorldCamera')) "
            "__cam_waited = GetCamera('WorldCamera'):GetZoom() end)");
        pump(2);
        step(0.5);
        pump(2);
        const bool parked = ui_number("__cam_waited") < 0.0;
        step(0.6);
        pump(2);
        const f64 woke = ui_number("__cam_waited");
        t.check(idle && parked && std::abs(woke - 300.0) < 1e-3,
                fmt::format("Test 2: WaitFor(camera) returns at once with no move ({}), waits "
                            "out a move ({}), and wakes at its end (the zoom then {:.1f})",
                            idle, parked ? "parked" : "didn't park", woke));
    }

    // Test 3: UIZoomTo frames a unit: its place, 20 about it each way
    {
        const auto* e = sim.sim.entity_registry().find(acu);
        run(fmt::format("UIZoomTo({{GetUnitById('{}')}}, 0)", acu));
        const bool ok = e && std::abs(cam.target_x() - e->position().x) < 1e-2f &&
                        std::abs(cam.target_z() - e->position().z) < 1e-2f &&
                        std::abs(cam.zoom() - 40.0f) < 1e-3f &&
                        cam.target_type() == renderer::CameraTarget::Location;
        t.check(ok, fmt::format("Test 3: UIZoomTo frames the ACU at ({:.1f}, {:.1f}), zoom "
                                "{:.1f} (40: its place, 20 each way)",
                                cam.target_x(), cam.target_z(), cam.zoom()));
    }

    // Test 4: TrackEntities follows the ACU as it walks
    {
        // A frame of the world: the camera finds its targets in it
        shots.recapture();
        shots.redraw();
        run(fmt::format("GetCamera('WorldCamera'):TrackEntities({{'{}'}}, 60, 0)", acu));
        const bool tracking = cam.target_type() == renderer::CameraTarget::Entity;
        const auto* e = sim.sim.entity_registry().find(acu);
        const f32 x0 = e ? e->position().x : 0.0f;
        sim_lua(fmt::format("IssueMove({{GetEntityById('{}')}}, {{{}, 0, {}}})", acu, x0 + 30.0f,
                            e ? e->position().z : 0.0f)
                    .c_str());
        play(50);
        shots.recapture();
        shots.redraw(); // the frame the camera follows the world through
        step(1.0 / 60.0);
        const auto* moved = sim.sim.entity_registry().find(acu);
        const f32 x1 = moved ? moved->position().x : 0.0f;
        const bool still = cam.target_type() == renderer::CameraTarget::Entity;
        t.check(tracking && still && x1 - x0 > 3.0f && std::abs(cam.target_x() - x1) < 1.0f,
                fmt::format("Test 4: tracked ({}, still {}), the camera's target is at x {:.1f} "
                            "with the ACU walked from {:.1f} to {:.1f}",
                            tracking, still, cam.target_x(), x0, x1));
        cam.target_nothing();
    }

    // Test 5: a sim script's SimCamera move: through Sync to usercamera.lua,
    // which moves the camera over a second, waits for it and calls the sim
    // back; the script resumes after
    {
        sim_lua("__cam_done = false "
                "local cam = import('/lua/SimCamera.lua').SimCamera('WorldCamera') "
                "ForkThread(function() cam:MoveTo(Rect(400, 300, 480, 380), 1.0) cam:WaitFor() "
                "__cam_done = true end)");
        play(1); // the request goes up with the beat; the UI starts the move
        const bool moving = cam.moving();
        // The rect's corners on their cells' middles: 399.5 and 479.5
        const f32 want_x = (399.5f + 479.5f) * 0.5f;
        lua_State* sL = sim.L;
        const auto sim_done = [&] {
            lua_pushstring(sL, "__cam_done");
            lua_rawget(sL, LUA_GLOBALSINDEX);
            const bool v = lua_toboolean(sL, -1) != 0;
            lua_pop(sL, 1);
            return v;
        };
        play(2);
        const bool waiting = !sim_done();
        step(1.1); // the move ends: usercamera.lua's thread wakes
        pump(2);
        play(3); // its SimCallback, then the sim's thread
        t.check(moving && waiting && sim_done() && std::abs(cam.target_x() - want_x) < 0.01f,
                fmt::format("Test 5: a SimCamera move reached the camera ({}), the script "
                            "waited ({}) and resumed once it ended ({}); the target x {:.2f} "
                            "({:.2f})",
                            moving, waiting, sim_done(), cam.target_x(), want_x));
    }

    lua_pushstring(L, "__osc_renderer");
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    spdlog::info("Camera moves test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
