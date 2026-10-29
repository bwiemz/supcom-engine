#pragma once

#include "core/types.hpp"

#include <functional>

namespace osc::test {

struct TestContext;

/// --command-issued-test, in a game with FA's interface: the UI's orders reach
/// commandmode.OnCommandIssued with their LuaParams, which FAF's enhancement
/// queue and attack-move read. `ui` is the UI state's.
void test_command_issued(TestContext& ui, const std::function<void(int)>& pump,
                         const std::function<void(int)>& play,
                         const std::function<bool(f32, f32, bool)>& click,
                         const std::function<bool(const char*)>& sim_lua);

} // namespace osc::test
