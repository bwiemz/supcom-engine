#pragma once

#include <functional>

namespace osc::test {

struct TestContext;

/// --window-test (M217h), in a game with FA's interface: the window's side
/// of FA's video options. SC_PrimaryAdapter "windowed" resizes the window
/// to the prefs' size (the swapchain and the UI's root frame follow);
/// SC_VerticalSync takes the vsync option; the options screen reads the
/// display's modes back as Moho publishes them. Never full screen: that
/// would change the display under the test. `ui` is the UI state's.
void test_window(TestContext& ui, TestContext& sim, const std::function<void(int)>& pump);

} // namespace osc::test
