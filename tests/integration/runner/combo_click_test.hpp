#pragma once

namespace osc::app {
struct Engine;
}

namespace osc::test {

/// --combo-click-test: in retail's options dialog a combo's list closes on a
/// click outside it, and Cancel still takes its click.
void run_combo_click_test(app::Engine& e);

} // namespace osc::test
