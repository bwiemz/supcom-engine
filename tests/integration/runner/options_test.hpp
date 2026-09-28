#pragma once

#include <functional>

namespace osc::test {

struct TestContext;

/// --options-test (M217i), in a game with FA's interface: FA's options
/// reach the engine through the console's variables, applied as Moho does
/// at startup and typed as a player does; SC_ToggleCursorClip. `ui` is the
/// UI state's.
void test_options(TestContext& ui, TestContext& sim, const std::function<void(int)>& pump);

} // namespace osc::test
