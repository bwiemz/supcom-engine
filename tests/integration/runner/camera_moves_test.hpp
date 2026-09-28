#pragma once

#include <functional>

namespace osc::test {

struct TestContext;

/// --camera-moves-test (M217g), in a game with FA's interface: the camera's
/// timed moves, WaitFor(camera), UIZoomTo, tracking, and a SimCamera move
/// round trip. `ui` is the UI state's context, `sim` the sim's.
void test_camera_moves(TestContext& ui, TestContext& sim, const std::function<void(int)>& pump,
                       const std::function<void(int)>& play,
                       const std::function<bool(const char*)>& sim_lua);

} // namespace osc::test
