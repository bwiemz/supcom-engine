#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "renderer/unit_renderer.hpp"
#include "sim/world_snapshot.hpp"

using namespace osc;
using namespace osc::renderer;
using Catch::Matchers::WithinAbs;

namespace {

/// Retail's GameColors: ten army colours, as many player colours.
sim::GameColors retail() {
    sim::GameColors c;
    c.army_colors = {0xFFE80A0A, 0xFF006400, 0xFF131CD3, 0xFFDAA520, 0xFFA7A7A7,
                     0xFF2F4F4F, 0xFF202020, 0xFF4D0505, 0xFF2E8B57, 0xFF8A2BE2};
    c.player_colors = c.army_colors;
    return c;
}

sim::ArmyRecord army(u8 r, u8 g, u8 b) {
    sim::ArmyRecord a;
    a.valid = true;
    a.has_color = true;
    a.r = r;
    a.g = g;
    a.b = b;
    return a;
}

} // namespace

TEST_CASE("An army's lookup row is its colour's index in ArmyColors (M211c)", "[renderer]") {
    const sim::GameColors colors = retail();
    const sim::ArmyRecord blue = army(0x13, 0x1C, 0xD3); // the third
    CHECK_THAT(team_color_lookup(&blue, colors), WithinAbs(2.5 / 10, 1e-6));
    const sim::ArmyRecord violet = army(0x8A, 0x2B, 0xE2); // the last
    CHECK_THAT(team_color_lookup(&violet, colors), WithinAbs(9.5 / 10, 1e-6));
}

TEST_CASE("A colour not in ArmyColors takes row 3, no army row 0 (M211c)", "[renderer]") {
    const sim::GameColors colors = retail();
    const sim::ArmyRecord odd = army(1, 2, 3);
    CHECK_THAT(team_color_lookup(&odd, colors), WithinAbs(3.5 / 10, 1e-6));
    sim::ArmyRecord uncoloured = army(0x13, 0x1C, 0xD3);
    uncoloured.has_color = false;
    CHECK_THAT(team_color_lookup(&uncoloured, colors), WithinAbs(3.5 / 10, 1e-6));
    CHECK_THAT(team_color_lookup(nullptr, colors), WithinAbs(0.5 / 10, 1e-6));
}

TEST_CASE("The row is clamped below the player colours' count (M211c)", "[renderer]") {
    sim::GameColors colors = retail();
    colors.player_colors.resize(2);
    const sim::ArmyRecord violet = army(0x8A, 0x2B, 0xE2);
    CHECK_THAT(team_color_lookup(&violet, colors), WithinAbs(1.5 / 2, 1e-6));
    // Without GameColors, one row: Moho counts at least one.
    CHECK_THAT(team_color_lookup(&violet, sim::GameColors{}), WithinAbs(0.5, 1e-6));
}
