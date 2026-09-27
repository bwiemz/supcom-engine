// --colors-test / --army-colors-test: colours decode as Moho's
// SCR_DecodeColor does, and armies take FA's GameColors.
//
// The engine had read UI colours as hex alone: a name ("Goldenrod",
// "black") or a six-digit colour came out as garbage or transparent, and
// EnumColorNames was empty. Armies took colours from a table of the
// engine's own rather than /lua/GameColors.lua, which a session's colour
// index names.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "lua/session_manager.hpp"
#include "sim/army_brain.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace osc::test {

namespace {

/// Run `body` after making text control `t` and bitmap `b`; it returns one
/// value, left on the stack. False (with a warning) on a Lua error.
bool run_ui(TestContext& ctx, const std::string& body) {
    const std::string script = "local Frame = import('/lua/maui/frame.lua').Frame\n"
                               "local f = Frame('ColorsFrame')\n"
                               "local t = {}\n"
                               "setmetatable(t, {__index = moho.text_methods})\n"
                               "InternalCreateText(t, f)\n"
                               "local b = {}\n"
                               "setmetatable(b, {__index = moho.bitmap_methods})\n"
                               "InternalCreateBitmap(b, f)\n" +
                               body;
    const auto result = ctx.lua_state.do_string(script);
    if (!result) spdlog::warn("colors test Lua error: {}", result.error().message);
    return static_cast<bool>(result);
}

} // namespace

void test_colors(TestContext& ctx) {
    spdlog::info("=== COLORS TEST: colours as Moho decodes them ===");
    Tally t;
    lua_State* L = ctx.L;

    // Test 1: EnumColorNames lists Moho's 141 names.
    {
        bool ok = false;
        if (run_ui(ctx, "local n = EnumColorNames()\n"
                        "return table.getn(n) == 141 and n[1] == 'AliceBlue' and "
                        "n[141] == 'transparent'\n")) {
            ok = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
        t.check(ok, "Test 1: EnumColorNames lists 141 names, AliceBlue to transparent");
    }

    // Tests 2-4: named, lower-case and six-digit colours reach the controls.
    const auto color_of = [&](const std::string& setup, const char* which) -> u32 {
        if (!run_ui(ctx, setup + "\nreturn " + which + "._c_object\n")) return 0;
        auto* ctrl = static_cast<ui::UIControl*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        if (!ctrl) return 0;
        return std::string(which) == "t" ? ctrl->text_color() : ctrl->solid_color();
    };
    {
        const u32 c = color_of("t:SetNewColor('Goldenrod')", "t");
        t.check(c == 0xFFDAA520u, fmt::format("Test 2: SetNewColor('Goldenrod') is {:08x}", c));
    }
    {
        const u32 c = color_of("b:InternalSetSolidColor('black')", "b");
        t.check(c == 0xFF000000u, fmt::format("Test 3: SetSolidColor('black') is {:08x}", c));
    }
    {
        const u32 c = color_of("t:SetNewColor('ff8000')", "t");
        t.check(c == 0xFFFF8000u, fmt::format("Test 4: SetNewColor('ff8000') is {:08x}", c));
    }

    // Test 5: an unknown colour is a Lua error, as in Moho.
    {
        bool ok = false;
        if (run_ui(ctx, "local good, msg = pcall(t.SetNewColor, t, 'NotAColor')\n"
                        "return (not good) and string.find(msg, 'Unknown color') ~= nil\n")) {
            ok = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
        }
        t.check(ok, "Test 5: SetNewColor('NotAColor') raises 'Unknown color'");
    }

    spdlog::info("Colors test: {}/{} passed", t.pass, t.pass + t.fail);
}

void test_army_colors(TestContext& ctx) {
    spdlog::info("=== ARMY COLORS TEST: armies take FA's GameColors ===");
    Tally t;

    // Test 1: GameColors.ArmyColors, decoded: ten, named ones included.
    const std::vector<u32> colors = lua::game_army_colors(ctx.L);
    t.check(colors.size() == 10 && colors[0] == 0xFFE80A0Au && colors[1] == 0xFF006400u &&
                colors[3] == 0xFFDAA520u && colors[9] == 0xFF8A2BE2u,
            fmt::format("Test 1: GameColors has {} army colours ({:08x}, {:08x}, ...)",
                        colors.size(), colors.empty() ? 0u : colors[0],
                        colors.size() > 1 ? colors[1] : 0u));

    // Test 2: a slot's colour index names ArmyColors[index + 1] (Moho's
    // ResolveArmyColorByIndex): index 1 is DarkGreen.
    {
        auto* brain = ctx.sim.get_army(0);
        lua::ArmySlotConfig slot;
        slot.army_color = 1;
        lua::apply_config_to_brain(&slot, brain, colors);
        const bool ok = brain && brain->has_color() && brain->color_r() == 0 &&
                        brain->color_g() == 100 && brain->color_b() == 0;
        t.check(ok, fmt::format("Test 2: colour index 1 gives ({}, {}, {})",
                                brain ? brain->color_r() : 0, brain ? brain->color_g() : 0,
                                brain ? brain->color_b() : 0));
    }

    spdlog::info("Army colors test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
