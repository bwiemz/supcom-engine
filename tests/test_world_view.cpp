#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
