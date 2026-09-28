#include "keymap_test.hpp"

#include "core/game_state.hpp"
#include "core/test_status.hpp"
#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "render_probe.hpp"
#include "ui/keymap.hpp"
#include "ui/ui_control.hpp"
#include "ui/ui_dispatch.hpp"

#include <GLFW/glfw3.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <string>

namespace osc::test {

void test_keymap(TestContext& ctx, ui::UIControlRegistry& registry, GameStateManager& game,
                 const std::function<void(int)>& pump_frames) {
    spdlog::info("=== Key map test (M217c) ===");
    Tally t;
    lua_State* L = ctx.L;
    ui::UIDispatch dispatch;
    const auto run = [&](const char* code) {
        auto r = ctx.lua_state.do_string(code);
        if (!r) osc::test_status::fail("[FAIL] keymap-test Lua: {}", r.error().message);
        return static_cast<bool>(r);
    };
    const auto num = [&](const char* expr) {
        if (!run(fmt::format("__keymap_value = {}", expr).c_str())) return -1.0;
        lua_getglobal(L, "__keymap_value");
        const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1.0;
        lua_pop(L, 1);
        return v;
    };
    // A key pressed and let go, as the window's callbacks give it.
    const auto press = [&](int key, int mods = 0) {
        dispatch.on_key(key, GLFW_PRESS, mods);
        dispatch.on_key(key, GLFW_RELEASE, mods);
        dispatch.dispatch_events(L, registry);
        pump_frames(1);
    };

    lua_pushstring(L, "__osc_keymap_registry");
    lua_rawget(L, LUA_REGISTRYINDEX);
    const auto* km = static_cast<const ui::KeyMapRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    const std::string* esc = km ? km->action(km->parse("Esc")) : nullptr;
    t.check(km && km->size() >= 120 && km->key_name(0x1B) == "Esc" &&
                km->key_name(0x6B) == "NumPlus" && esc &&
                esc->find("EscapeHandler") != std::string::npos,
            fmt::format("Test 1: retail's key names and default mappings are loaded ({} bindings; "
                        "Esc runs '{}')",
                        km ? km->size() : 0, esc ? *esc : std::string("nothing")));
    if (!km) return;

    // Retail locks the input as a game starts (worldview.LockInput: a modal
    // group over the screen) and lets go 3 s after the first update.
    const bool locked = registry.input_capture() != nullptr;
    int frames = 0;
    while (registry.input_capture() && frames < 600) {
        pump_frames(1);
        ++frames;
    }
    t.check(locked && !registry.input_capture() && frames >= 180,
            fmt::format("Test 2: the game starts with its input locked, and unlocks it after the "
                        "opening zoom ({} frames)",
                        frames));

    // Esc: uimain.EscapeHandler, through the escape handler a screen sets.
    run("esc_hits = 0 import('/lua/ui/uimain.lua').SetEscapeHandler(function() esc_hits = esc_hits "
        "+ 1 end)");
    press(GLFW_KEY_ESCAPE);
    t.check(num("esc_hits") == 1, "Test 3: Esc runs uimain.EscapeHandler");

    // The game speed keys step the sim rate: 10^(1/10) times normal a step.
    game.set_sim_rate(0);
    press(GLFW_KEY_KP_ADD);
    const int up = game.sim_rate();
    const double up_speed = game.speed();
    press(GLFW_KEY_KP_SUBTRACT);
    press(GLFW_KEY_KP_SUBTRACT);
    const int down = game.sim_rate();
    press(GLFW_KEY_KP_MULTIPLY);
    t.check(up == 1 && std::abs(up_speed - std::pow(10.0, 0.1)) < 1e-9 && down == -1 &&
                game.sim_rate() == 0 && num("GetGameSpeed()") == 0,
            fmt::format("Test 4: NumPlus, NumMinus and NumStar step and reset the game speed "
                        "(+{}, {}, {})",
                        up, down, game.sim_rate()));

    // Pause: tabs.lua's TogglePause, through the pause button.
    const bool was_paused = game.paused();
    press(GLFW_KEY_PAUSE);
    const bool paused = game.paused();
    press(GLFW_KEY_PAUSE);
    t.check(!was_paused && paused && !game.paused(),
            fmt::format("Test 5: Pause pauses the game, and again resumes it ({} then {})", paused,
                        game.paused()));

    // An unbound Enter opens the chat (UI_ActivateChat), with its modifiers.
    run(R"(
        chat_hits, chat_shift = 0, false
        local chat = import('/lua/ui/game/chat.lua')
        chat.ActivateChat = function(mods) chat_hits = chat_hits + 1 chat_shift = mods.Shift == true end
    )");
    press(GLFW_KEY_ENTER, GLFW_MOD_SHIFT);
    t.check(num("chat_hits") == 1 && num("chat_shift and 1 or 0") == 1,
            "Test 6: an unbound Enter opens the chat, with its modifiers");

    // A focused edit box keeps its keys: the key map never sees them.
    run(R"(
        focused = import('/lua/maui/edit.lua').Edit(GetFrame(0))
        focused:AcquireFocus()
    )");
    press(GLFW_KEY_KP_ADD);
    press(GLFW_KEY_ESCAPE);
    const bool kept = game.sim_rate() == 0 && num("esc_hits") == 1;
    run("focused:AbandonFocus() focused:Destroy()");
    press(GLFW_KEY_KP_ADD);
    t.check(kept && game.sim_rate() == 1,
            "Test 7: a focused edit box keeps its keys from the key map; without focus they act");
    game.set_sim_rate(0);

    // Out of a game (the score screen here), an unbound Enter opens nothing.
    game.transition_to(GameState::SCORE, nullptr);
    press(GLFW_KEY_ENTER);
    t.check(num("chat_hits") == 1, "Test 8: out of a game, Enter opens no chat");
    game.transition_to(GameState::GAME, nullptr);

    spdlog::info("Key map test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
