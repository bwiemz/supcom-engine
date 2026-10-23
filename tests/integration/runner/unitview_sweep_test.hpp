#pragma once

#include <functional>

namespace osc::test {

struct TestContext;

/// --unitview-sweep-test, in a game with FA's interface: every unit
/// blueprint's build button hovered, as a player's pointer does it. The
/// commander selected, the unit view's detail Show(bp, builder, id) and the
/// construction panel's rollover run for each, and none may fail (a failure
/// there leaves the button unarmed, and its click is lost). Run with
/// fixtures/armament_detail.prefs: FAF's detail then formats each weapon's
/// ranges. `ui` is the UI state's.
void test_unitview_sweep(TestContext& ui, const std::function<void(int)>& pump,
                         const std::function<void(int)>& play);

} // namespace osc::test
