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

} // namespace osc::test
