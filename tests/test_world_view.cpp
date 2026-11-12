#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/game_state.hpp"
#include "lua/engine_bindings.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "renderer/camera.hpp"
#include "sim/bone_data.hpp"
#include "sim/manipulator.hpp"
#include "sim/pose.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"
#include "ui/ui_control.hpp"
#include "ui/world_view.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("WorldView:GetScreenPos and ProjectMultiple put world points on the view, as Moho's",
          "[ui][worldview][lua]") {
    osc::renderer::Camera camera;
    camera.init(512.0f, 512.0f);
    camera.set_target(200.0f, 300.0f);
    camera.set_zoom(150.0f);
    osc::ui::WorldView view;
    view.register_camera("WorldCamera", &camera);
    view.set_viewport(1280, 720);

    // Three units: one in view with a mesh, one far off it, one without a mesh.
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    sim.add_army("ARMY_1", "ARMY_1");
    osc::sim::BoneData model;
    model.mesh_bounds = osc::sim::MeshBounds{{-1.0f, 0.0f, -1.0f}, {1.0f, 2.0f, 1.0f}};
    std::vector<osc::sim::EntityId> ids;
    for (const auto& [at, meshed] : {std::pair{osc::sim::Vector3{230.0f, 0.0f, 280.0f}, true},
                                     std::pair{osc::sim::Vector3{4000.0f, 0.0f, 4000.0f}, true},
                                     std::pair{osc::sim::Vector3{215.0f, 0.0f, 310.0f}, false}}) {
        auto unit = std::make_unique<osc::sim::Unit>();
        unit->set_army(0);
        unit->set_position(at);
        if (meshed) unit->set_bone_data(&model);
        ids.push_back(sim.entity_registry().register_entity(std::move(unit)));
    }

    osc::lua::LuaState ui;
    osc::ui::UIControlRegistry registry;
    osc::lua::register_blueprint_bindings(ui);
    osc::lua::register_moho_bindings(ui, sim);
    osc::lua::register_ui_bindings(ui, registry);
    osc::lua::register_user_bindings(ui);
    lua_State* L = ui.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &view);
    lua_rawset(L, -3);
    lua_setglobal(L, "view");
    osc::lua::push_units_for_ui(L, ids);
    lua_setglobal(L, "units");

    float sx = 0.0f, sy = 0.0f;
    REQUIRE(view.project(230.0f, 0.0f, 280.0f, sx, sy));
    const auto number = [L](const char* name) {
        lua_getglobal(L, name);
        const double v = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return v;
    };

    // In view: its position's pixel, floored, as a Vector2.
    REQUIRE(ui.do_string("local p = moho.UIWorldView.GetScreenPos(view, units[1]) "
                         "gx, gy = p.x, p[2]")
                .ok());
    CHECK(number("gx") == std::floor(sx));
    CHECK(number("gy") == std::floor(sy));
    // Out of the view's frustum, or without a mesh: nil.
    CHECK(ui.do_string("assert(moho.UIWorldView.GetScreenPos(view, units[2]) == nil)").ok());
    CHECK(ui.do_string("assert(moho.UIWorldView.GetScreenPos(view, units[3]) == nil)").ok());

    // ProjectMultiple: each point under its own key, a plain {x, y}.
    REQUIRE(ui.do_string("local r = moho.UIWorldView.ProjectMultiple(view, "
                         "{a = Vector(230, 0, 280), [3] = {230, 0, 280}}) "
                         "assert(getmetatable(r.a) == nil and r.b == nil) "
                         "mx, my, nx = r.a[1], r.a[2], r[3][1]")
                .ok());
    CHECK(number("mx") == Approx(sx));
    CHECK(number("my") == Approx(sy));
    CHECK(number("nx") == Approx(sx));

    auto r = ui.do_string("moho.UIWorldView.GetScreenPos(view)");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("(vector2f|nil) = GetScreenPos(unit)\n  expected 2 args, but "
                                 "got 1") != std::string::npos);
    r = ui.do_string("moho.UIWorldView.ProjectMultiple(view, 5)");
    REQUIRE_FALSE(r.ok());
    CHECK(r.error().message.find("table expected.") != std::string::npos);
}

TEST_CASE("A world view's script hears OnIconsVisible as its icons turn off and on, and only then",
          "[ui][worldview][lua]") {
    // CUIWorldView::Frame, while the view's input isn't locked: OnIconsVisible(self,
    // !mCameraRotationActive && !cam_Free) when that differs from what it last told the
    // script (true as made, so nothing at creation). A minimap's own view takes no spin.
    osc::renderer::Camera camera;
    camera.set_viewport(1280.0f, 720.0f);
    camera.init(512.0f, 512.0f);
    camera.set_target(200.0f, 300.0f);
    camera.set_zoom(60.0f); // near: Space spins it

    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    osc::lua::LuaState ui;
    osc::ui::UIControlRegistry registry;
    osc::lua::register_blueprint_bindings(ui);
    osc::lua::register_moho_bindings(ui, sim);
    osc::lua::register_ui_bindings(ui, registry);
    osc::lua::register_user_bindings(ui);
    lua_State* L = ui.raw();
    const auto made = ui.do_string(R"(
        told = ''
        views = {}
        local function view_named(name)
            local view = {}
            view.OnIconsVisible = function(...)
                told = told .. name .. '=' .. tostring(arg[2])
                if arg[1] ~= view then told = told .. '(not self)' end
                if arg.n ~= 2 then told = told .. '(' .. arg.n .. ' args)' end
                told = told .. ';'
            end
            views[name] = view
        end
        view_named('main')
        view_named('mini')
    )");
    INFO((made.ok() ? std::string() : made.error().message));
    REQUIRE(made.ok());

    const auto add_view = [&](const char* name, bool minimap) {
        auto owned = std::make_unique<osc::ui::WorldView>();
        osc::ui::WorldView* view = owned.get();
        registry.add(std::move(owned));
        view->set_minimap(minimap);
        view->register_camera("WorldCamera", &camera);
        lua_getglobal(L, "views");
        lua_pushstring(L, name);
        lua_gettable(L, -2);
        view->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
        lua_pop(L, 1); // views
        return view;
    };
    osc::ui::WorldView* main_view = add_view("main", false);
    add_view("mini", true);

    // What the views' scripts were told since the last look.
    const auto told = [L] {
        lua_getglobal(L, "told");
        const char* text = lua_tostring(L, -1);
        std::string out = text ? text : "";
        lua_pop(L, 1);
        lua_pushstring(L, "");
        lua_setglobal(L, "told");
        return out;
    };
    const auto frame = [L] { osc::lua::update_world_view_icons(L); };

    // Made, its icons showing: nothing to tell
    frame();
    CHECK(told().empty());

    // The Space spin's first motion: the main view's icons go; the minimap's
    // stay. Told once, not each frame after.
    osc::renderer::CameraInput in;
    in.space = true;
    camera.apply(in, 0.0);
    in.mouse_x = 100.0f;
    camera.apply(in, 0.0);
    REQUIRE(camera.spinning());
    frame();
    CHECK(told() == "main=false;");
    frame();
    CHECK(told().empty());

    // While its input is locked, nothing: the spin's end is told once it's
    // unlocked
    main_view->set_input_locked(true);
    in.space = false;
    camera.apply(in, 0.0);
    REQUIRE_FALSE(camera.spinning());
    frame();
    CHECK(told().empty());
    main_view->set_input_locked(false);
    frame();
    CHECK(told() == "main=true;");

    // cam_Free turns every view's icons off, the minimap's too, and back
    camera.set_free(true);
    frame();
    CHECK(told() == "main=false;mini=false;");
    camera.set_free(false);
    frame();
    CHECK(told() == "main=true;mini=true;");
    frame();
    CHECK(told().empty());
}
