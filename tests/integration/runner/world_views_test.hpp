#pragma once

#include <functional>

namespace osc::ui {
class UIControlRegistry;
}

namespace osc::test {

struct TestContext;

/// --world-views-test; `ui` is the UI state's.
void test_world_views(TestContext& ui, TestContext& sim, ui::UIControlRegistry& registry,
                      const std::function<void(int)>& pump);

} // namespace osc::test
