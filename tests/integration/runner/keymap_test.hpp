#pragma once

#include <functional>

namespace osc {
class GameStateManager;
namespace ui {
class UIControlRegistry;
}
} // namespace osc

namespace osc::test {

struct TestContext;

/// --keymap-test (M217c), in a game with FA's interface: retail's key names
/// and default mappings are loaded at boot, and its keys act through the
/// console as Moho's CUIKeyHandler has them -- Esc, the game speed keys,
/// Pause, an unbound Enter (chat), and a focused edit box keeping its keys.
/// `ctx` is the UI state's. Failures are recorded in test_status.
void test_keymap(TestContext& ctx, ui::UIControlRegistry& registry, GameStateManager& game,
                 const std::function<void(int)>& pump_frames);

/// --session-command-test (M217d), in a game with FA's interface: the
/// console's session commands -- UI_SelectByCategory's filters,
/// StartCommandMode's toggle and cap check, IssueCommand Stop -- and the
/// hotkeys that reach them. `ctx` is the UI state's; `sim_lua` runs sim Lua.
void test_session_commands(TestContext& ctx, ui::UIControlRegistry& registry,
                           const std::function<void(int)>& pump_frames,
                           const std::function<void(int)>& play,
                           const std::function<bool(const char*)>& sim_lua);

/// --keyboard-test (M217e), in a game with FA's interface: retail's keys own
/// the keyboard -- its control groups (Ctrl-1 sets, 1 recalls) where the
/// engine had its own.
void test_keyboard(TestContext& ctx, ui::UIControlRegistry& registry,
                   const std::function<void(int)>& pump_frames,
                   const std::function<void(int)>& play,
                   const std::function<bool(const char*)>& sim_lua);

} // namespace osc::test
