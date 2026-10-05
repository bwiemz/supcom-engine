#pragma once

namespace osc::app {
struct Engine;
}

namespace osc::test {

/// --edit-text-test, from the front end's start: retail's name dialog
/// (profile.lua CreationDialog) draws a name in accented Latin and Cyrillic letters.
/// Failures are recorded in test_status.
void run_edit_text_test(app::Engine& e);

} // namespace osc::test
