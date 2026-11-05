#pragma once

namespace osc::test {

struct TestContext;

/// --shore-test: a hover, an amphibious and a land tank sent across a bay
/// cross it on their own layers (the water's surface, the seabed, round it).
void test_shore(TestContext& ctx);

/// --choke-test: a crowd of Strikers through a wall's gap, and a Monkeylord,
/// whose class ignores structures, over the wall with Moho's pathing.
void test_choke(TestContext& ctx);

/// --crowdgap-test: twenty Strikers through a wall's gap to twenty places
/// beyond it all arrive, as in retail FA.
void test_crowdgap(TestContext& ctx);

/// --ship-shore-test: ships sent at points on X1MP_012's shores stay on the
/// water.
void test_ship_shore(TestContext& ctx);

} // namespace osc::test
