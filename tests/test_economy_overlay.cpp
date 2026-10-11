#include <catch2/catch_test_macros.hpp>

#include "renderer/economy_overlay_renderer.hpp"

using osc::renderer::economy_rate_text;

TEST_CASE("The economy overlay prints one decimal inside 10 and whole numbers past it",
          "[renderer]") {
    CHECK(economy_rate_text(2.0f, false) == "+2.0");
    CHECK(economy_rate_text(-2.0f, true) == "-2.0 ");
    CHECK(economy_rate_text(0.0f, false) == "+0.0");
    CHECK(economy_rate_text(9.94f, false) == "+9.9");
    CHECK(economy_rate_text(20.0f, true) == " +20 ");
    CHECK(economy_rate_text(-15.7f, false) == " -15");
    CHECK(economy_rate_text(-10.0f, false) == " -10");
    CHECK(economy_rate_text(1234.5f, false) == "+1234");
}
