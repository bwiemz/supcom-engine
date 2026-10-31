#pragma once

#include <functional>

namespace osc::test {

struct TestContext;

/// --bp-reflect-test, in a game with FA's interface: on retail's data both
/// Lua states read the original game's flag enums and footprints, and its
/// projectile, prop and mesh blueprints; on FAF's, its unit view and
/// formations read them.
void test_bp_reflect(TestContext& ui, TestContext& sim, const std::function<void(int)>& play);

} // namespace osc::test
