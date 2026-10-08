// Moho's console variables (M217i; faf-re's TConVar<bool|int|float>
// handlers): what each command does to a variable, as FA's options screen
// and a player's console run them.

#include <catch2/catch_test_macros.hpp>

#include "app/window_commands.hpp"
#include "lua/lua_state.hpp"
#include "renderer/renderer.hpp"
#include "ui/console.hpp"

extern "C" {
#include <lua.h>
}

#include <string>
#include <vector>

using osc::ui::convar_bool;
using osc::ui::convar_float;
using osc::ui::convar_int;

namespace {

std::vector<std::string> cmd(std::initializer_list<const char*> tokens) {
    std::vector<std::string> args{"var"};
    for (const char* t : tokens) args.emplace_back(t);
    return args;
}

} // namespace

TEST_CASE("A bool console variable takes Moho's words (M217i)", "[console][convar]") {
    bool shown = false;
    CHECK(convar_bool(cmd({}), false, shown)); // no argument: toggled
    CHECK_FALSE(shown);
    CHECK_FALSE(convar_bool(cmd({}), true, shown));
    CHECK(convar_bool(cmd({"on"}), false, shown));
    CHECK(convar_bool(cmd({"TRUE"}), false, shown));
    CHECK_FALSE(convar_bool(cmd({"Off"}), true, shown));
    CHECK_FALSE(convar_bool(cmd({"false"}), true, shown));
    CHECK(convar_bool(cmd({"tog"}), false, shown));
    CHECK_FALSE(convar_bool(cmd({"=", "0"}), true, shown));
    CHECK(convar_bool(cmd({"=", "5"}), false, shown));
    CHECK(convar_bool(cmd({"2"}), false, shown));
    CHECK_FALSE(convar_bool(cmd({"junk"}), true, shown)); // atoi: 0
    // show: only shown, the value kept
    CHECK(convar_bool(cmd({"show"}), true, shown));
    CHECK(shown);
}

TEST_CASE("An int console variable takes Moho's operators (M217i)", "[console][convar]") {
    bool shown = false;
    CHECK(convar_int(cmd({}), 7, shown) == 7);
    CHECK(shown); // no argument: shown
    CHECK(convar_int(cmd({"=", "3"}), 7, shown) == 3);
    CHECK_FALSE(shown);
    CHECK(convar_int(cmd({"+=", "3"}), 7, shown) == 10);
    CHECK(convar_int(cmd({"-=", "3"}), 7, shown) == 4);
    CHECK(convar_int(cmd({"*=", "3"}), 7, shown) == 21);
    CHECK(convar_int(cmd({"/=", "2"}), 7, shown) == 3);
    CHECK(convar_int(cmd({"%=", "4"}), 7, shown) == 3);
    CHECK(convar_int(cmd({"&=", "5"}), 7, shown) == 5);
    CHECK(convar_int(cmd({"|=", "8"}), 7, shown) == 15);
    CHECK(convar_int(cmd({"^=", "5"}), 7, shown) == 2);
    // A division by 0 leaves it (Moho's would fault)
    CHECK(convar_int(cmd({"/=", "0"}), 7, shown) == 7);
    CHECK(convar_int(cmd({"%=", "0"}), 7, shown) == 7);
    CHECK(convar_int(cmd({"on"}), 7, shown) == 1);
    CHECK(convar_int(cmd({"false"}), 7, shown) == 0);
    CHECK(convar_int(cmd({"tog"}), 7, shown) == 0);
    CHECK(convar_int(cmd({"tog"}), 0, shown) == 1);
    CHECK(convar_int(cmd({"12"}), 7, shown) == 12);
    CHECK(convar_int(cmd({"-4"}), 7, shown) == -4);
    CHECK(convar_int(cmd({"junk"}), 7, shown) == 0);
    CHECK(convar_int(cmd({"="}), 7, shown) == 0); // "=" alone parses as 0
}

TEST_CASE("A float console variable takes Moho's operators (M217i)", "[console][convar]") {
    bool shown = false;
    CHECK(convar_float(cmd({}), 0.5f, shown) == 0.5f);
    CHECK(shown);
    CHECK(convar_float(cmd({"=", "0.25"}), 0.5f, shown) == 0.25f);
    CHECK_FALSE(shown);
    CHECK(convar_float(cmd({"+=", "0.25"}), 0.5f, shown) == 0.75f);
    CHECK(convar_float(cmd({"-=", "0.25"}), 0.5f, shown) == 0.25f);
    CHECK(convar_float(cmd({"*=", "4"}), 0.5f, shown) == 2.0f);
    CHECK(convar_float(cmd({"/=", "4"}), 0.5f, shown) == 0.125f);
    CHECK(convar_float(cmd({"/=", "0"}), 0.5f, shown) == 0.5f);
    CHECK(convar_float(cmd({"0.4"}), 0.5f, shown) == 0.4f);
    CHECK(convar_float(cmd({"junk"}), 0.5f, shown) == 0.0f);
    // No bitwise operators for a float: "&=" is a bare token (atof: 0)
    CHECK(convar_float(cmd({"&=", "1"}), 0.5f, shown) == 0.0f);
}

TEST_CASE("A console variable reads and writes through its binding (M217i)", "[console][convar]") {
    osc::ui::Console console;
    int fidelity = 1;
    float zoom = 0.05f;
    bool bloom = false;
    osc::ui::add_int_var(
        console, "graphics_Fidelity", [&](lua_State*) { return fidelity; },
        [&](lua_State*, int v) { fidelity = v; });
    osc::ui::add_float_var(
        console, "cam_ZoomAmount", [&](lua_State*) { return zoom; },
        [&](lua_State*, float v) { zoom = v; });
    osc::ui::add_bool_var(
        console, "ren_bloom", [&](lua_State*) { return bloom; },
        [&](lua_State*, bool v) { bloom = v; });
    console.execute(nullptr, "graphics_Fidelity += 1; cam_ZoomAmount 0.4; ren_bloom");
    CHECK(fidelity == 2);
    CHECK(zoom == 0.4f);
    CHECK(bloom); // toggled
    console.execute(nullptr, "graphics_fidelity; CAM_ZOOMAMOUNT *= 2; ren_bloom show");
    CHECK(fidelity == 2); // shown only
    CHECK(zoom == 0.8f);
    CHECK(bloom);
}

TEST_CASE("Console variables set before the renderer keep their values", "[console][convar]") {
    osc::ui::Console console;
    osc::app::HeldConVars held;
    osc::renderer::RangeOverlays overlays;
    osc::app::register_option_commands(console, held, overlays);
    osc::lua::LuaState state;
    lua_State* L = state.raw();
    console.execute(L, "range_RenderSelected true; ui_RenderUnitBars false; cam_ZoomAmount 0.3");
    console.execute(L, "range_RenderHighlighted; range_RenderBuild true");
    console.execute(L, "range_RenderBuild tog; ren_SelectBoxes false; ren_SelectBoxes tog");
    CHECK(overlays.settings().render_selected);
    CHECK(overlays.settings().render_highlighted);
    CHECK_FALSE(overlays.settings().render_build);

    osc::renderer::Renderer r;
    lua_pushstring(L, "__osc_renderer");
    lua_pushlightuserdata(L, &r);
    lua_rawset(L, LUA_REGISTRYINDEX);
    r.set_unit_bars(true);
    r.set_select_boxes(false);
    held.replay(console, L);
    CHECK_FALSE(r.unit_bars());
    CHECK(r.select_boxes());
    CHECK(r.camera().zoom_amount() == 0.3f);
}
