// SetUIControlsAlpha and GetUIControlsAlpha: Moho's UI manager keeps an
// alpha multiplier its UI passes give the controls' shader (primbatcher.fx's
// AlphaMultiplier, set by CD3DPrimBatcher::SetToViewport); each control
// draws with its alpha scaled by it.

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "renderer/renderer.hpp"
#include "renderer/ui_renderer.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <string>

namespace {

std::string run(osc::lua::LuaState& lua, const char* code) {
    auto r = lua.do_string(code);
    return r ? std::string() : r.error().message;
}

} // namespace

TEST_CASE("A control's quad draws with its alpha scaled by the UI controls' alpha", "[ui]") {
    using osc::renderer::ui_upload_instance;
    using osc::renderer::UIInstance;
    const UIInstance quad{{10, 20, 30, 40}, {0, 0, 1, 1}, {1.0f, 0.5f, 0.25f, 0.8f}};

    // A control's: the alpha scaled, the colour kept, the rect in pixels.
    const UIInstance faded = ui_upload_instance(quad, true, 2.0f, 0.5f);
    CHECK(faded.color[3] == 0.4f);
    CHECK(faded.color[0] == 1.0f);
    CHECK(faded.color[1] == 0.5f);
    CHECK(faded.color[2] == 0.25f);
    CHECK(faded.rect[0] == 20.0f);
    CHECK(faded.rect[3] == 80.0f);
    CHECK(faded.uv[2] == 1.0f);

    // The cursor and a minimap's map are not controls' quads: theirs stays.
    CHECK(ui_upload_instance(quad, false, 1.0f, 0.5f).color[3] == 0.8f);
    // At 1 (Moho's start) nothing changes.
    CHECK(ui_upload_instance(quad, true, 1.0f, 1.0f).color[3] == 0.8f);
}

TEST_CASE("SetUIControlsAlpha and GetUIControlsAlpha", "[ui]") {
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim(sim_lua.raw(), nullptr);
    osc::lua::LuaState ui;
    osc::lua::register_moho_bindings(ui, sim);
    osc::lua::register_user_bindings(ui);

    // No UI drawn (Moho: no UI manager): nothing to ask, nothing to set.
    CHECK(run(ui, "assert(GetUIControlsAlpha() == nil)\nSetUIControlsAlpha(0.5)").empty());

    osc::renderer::Renderer r;
    lua_State* L = ui.raw();
    lua_pushstring(L, "__osc_renderer");
    lua_pushlightuserdata(L, &r);
    lua_rawset(L, LUA_REGISTRYINDEX);
    CHECK(r.ui_controls_alpha() == 1.0f);
    CHECK(run(ui, "assert(GetUIControlsAlpha() == 1)\n"
                  "SetUIControlsAlpha(0.5)\n"
                  "assert(GetUIControlsAlpha() == 0.5)\n")
              .empty());
    CHECK(r.ui_controls_alpha() == 0.5f);

    CHECK(run(ui, "SetUIControlsAlpha()").find("expected 1 args, but got 0") != std::string::npos);
    CHECK(run(ui, "SetUIControlsAlpha('half')").find("attempt to use as number a string value") !=
          std::string::npos);
    CHECK(run(ui, "GetUIControlsAlpha(1)").find("expected 0 args, but got 1") != std::string::npos);
    CHECK(r.ui_controls_alpha() == 0.5f);
}
