#pragma once

#include <functional>

namespace osc::app {
struct Engine;
}

namespace osc::test {

struct TestContext;

void run_hidpi_test(app::Engine& e);

void run_hidpi_minimap_test(TestContext& ui, const std::function<void(int)>& pump);

} // namespace osc::test
