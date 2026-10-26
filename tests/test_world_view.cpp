#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/game_state.hpp"
#include "lua/engine_bindings.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "renderer/camera.hpp"
#include "sim/sim_state.hpp"
#include "ui/world_view.hpp"

extern "C" {
#include <lua.h>
}

#include <array>
#include <string>
#include <utility>

using Catch::Approx;

TEST_CASE("A world view's unprojection undoes its projection", "[ui][worldview]") {
    // UnProject (a dragged ping's new place) is Project's inverse: a point on
    // the ground projected to the screen comes back where it was.
    osc::renderer::Camera camera;
    camera.init(512.0f, 512.0f);
    camera.set_target(200.0f, 300.0f);
    camera.set_zoom(150.0f);
    osc::ui::WorldView view;
    view.register_camera("WorldCamera", &camera);
    view.set_viewport(1280, 720);

    const std::array<std::pair<float, float>, 4> points{
        {{200.0f, 300.0f}, {230.0f, 280.0f}, {170.0f, 335.0f}, {215.0f, 310.0f}}};
    for (const auto& [x, z] : points) {
        float sx = 0.0f, sy = 0.0f;
        REQUIRE(view.project(x, 0.0f, z, sx, sy));
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        REQUIRE(view.get_mouse_world_pos(sx, sy, wx, wy, wz));
        CHECK(wx == Approx(x).margin(0.05));
        CHECK(wz == Approx(z).margin(0.05));
        CHECK(wy == Approx(0.0f).margin(0.05));
    }
}

TEST_CASE("A world view without a camera unprojects nothing", "[ui][worldview]") {
    osc::ui::WorldView view;
    view.set_viewport(1280, 720);
    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    CHECK_FALSE(view.get_mouse_world_pos(640.0f, 360.0f, wx, wy, wz));
}

TEST_CASE("WorldView:Project takes a Moho vector and gives a Vector2", "[ui][worldview][lua]") {
    osc::renderer::Camera camera;
    camera.init(512.0f, 512.0f);
    camera.set_target(200.0f, 300.0f);
    camera.set_zoom(150.0f);
    osc::ui::WorldView view;
    view.register_camera("WorldCamera", &camera);
    view.set_viewport(1280, 720);
    float sx = 0.0f, sy = 0.0f;
    REQUIRE(view.project(230.0f, 0.0f, 280.0f, sx, sy));

    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    osc::lua::LuaState ui;
    osc::lua::register_blueprint_bindings(ui);
    osc::lua::register_moho_bindings(ui, sim);
    osc::lua::register_user_bindings(ui);
    lua_State* L = ui.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &view);
    lua_rawset(L, -3);
    lua_setglobal(L, "view");
    REQUIRE(ui.do_string("local p = moho.UIWorldView.Project(view, Vector(230, 0, 280)) "
                         "projected = {p[1], p[2], p.x, p.y}")
                .ok());
    lua_getglobal(L, "projected");
    const std::array<float, 4> expected{sx, sy, sx, sy};
    for (int i = 0; i < 4; ++i) {
        lua_rawgeti(L, -1, i + 1);
        CHECK(lua_tonumber(L, -1) == Approx(expected[static_cast<size_t>(i)]));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

TEST_CASE("WorldView:CameraReset resets its own camera, a minimap's not the one it tracks",
          "[ui][worldview][lua]") {
    // CUIWorldView:CameraReset resets the view's own camera (its mCamera, the
    // one of its name) and answers the view. minimap.lua's, on its first
    // frame, leaves the WorldCamera it tracks where the player has it.
    osc::renderer::Camera world;
    world.init(512.0f, 512.0f);
    world.set_target(200.0f, 300.0f);
    world.set_zoom(150.0f);
    osc::renderer::Camera mini;
    mini.init(512.0f, 512.0f);
    mini.set_target(100.0f, 120.0f);
    mini.set_zoom(150.0f);
    osc::ui::WorldView view;
    view.set_minimap(true);
    view.register_camera("WorldCamera", &world);
    view.set_own_camera("MiniMap", &mini);

    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    osc::lua::LuaState ui;
    osc::lua::register_blueprint_bindings(ui);
    osc::lua::register_moho_bindings(ui, sim);
    osc::lua::register_user_bindings(ui);
    lua_State* L = ui.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &view);
    lua_rawset(L, -3);
    lua_setglobal(L, "view");
    REQUIRE(ui.do_string("answered = moho.UIWorldView.CameraReset(view) == view").ok());
    lua_getglobal(L, "answered");
    CHECK(lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);

    // Its own camera: the whole map from above
    CHECK(mini.target_x() == Approx(256.0f));
    CHECK(mini.target_z() == Approx(256.0f));
    CHECK(mini.zoom() == Approx(mini.max_zoom()));
    // The tracked one untouched
    CHECK(world.target_x() == Approx(200.0f));
    CHECK(world.target_z() == Approx(300.0f));
    CHECK(world.zoom() == Approx(150.0f));
}

TEST_CASE("GetMouseScreenPos and GetAntiAliasingOptions answer Moho's types", "[ui][lua]") {
    // GetMouseScreenPos answers a Vector2 (Moho's CursorScreenPos), and
    // raises "No session started." without a session; GetAntiAliasingOptions
    // a list of option keys, 0 (off) the engine's only one. FAF's reticles
    // and the fidelity presets index what they answer: nil broke both.
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    osc::lua::LuaState ui;
    osc::lua::register_blueprint_bindings(ui);
    osc::lua::register_moho_bindings(ui, sim);
    osc::lua::register_user_bindings(ui);
    lua_State* L = ui.raw();

    auto result = ui.do_string(R"(
        local ok, err = pcall(GetMouseScreenPos)
        no_session = not ok and string.find(err, 'No session started.', 1, true) ~= nil
        local aa = GetAntiAliasingOptions()
        aa_off_only = type(aa) == 'table' and table.getn(aa) == 1 and aa[1] == 0
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());
    lua_getglobal(L, "no_session");
    CHECK(lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    lua_getglobal(L, "aa_off_only");
    CHECK(lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);

    osc::GameStateManager game;
    game.transition_to(osc::GameState::GAME, nullptr);
    lua_pushstring(L, "__osc_game_state_mgr");
    lua_pushlightuserdata(L, &game);
    lua_rawset(L, LUA_REGISTRYINDEX);
    result = ui.do_string(R"(
        local p = GetMouseScreenPos()
        at_cursor = type(p) == 'table' and p[1] == 0 and p[2] == 0 and p.x == 0 and p.y == 0
        local ok, err = pcall(GetMouseScreenPos, 1)
        args_checked = not ok and string.find(err, 'expected 0 args', 1, true) ~= nil
    )");
    INFO((result.ok() ? std::string() : result.error().message));
    REQUIRE(result.ok());
    lua_getglobal(L, "at_cursor"); // no renderer: the cursor at 0, 0
    CHECK(lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
    lua_getglobal(L, "args_checked");
    CHECK(lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);
}
