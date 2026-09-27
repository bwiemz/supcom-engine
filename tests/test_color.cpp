#include <catch2/catch_test_macros.hpp>

#include "core/color.hpp"

using namespace osc;

TEST_CASE("Colours decode as Moho's SCR_DecodeColor: names in any case", "[color]") {
    CHECK(decode_color("DarkGreen") == 0xFF006400u);
    CHECK(decode_color("darkgreen") == 0xFF006400u);
    CHECK(decode_color("DARKGREEN") == 0xFF006400u);
    CHECK(decode_color("Darkslategray") == 0xFF2F4F4Fu); // as GameColors spells it
    CHECK(decode_color("Goldenrod") == 0xFFDAA520u);
    CHECK(decode_color("transparent") == 0x00000000u);
    CHECK(decode_color("black") == 0xFF000000u);
    // Moho's table is the .NET web colours', where DarkSeaGreen is 8FBC8B
    // (CSS: 8FBC8F).
    CHECK(decode_color("DarkSeaGreen") == 0xFF8FBC8Bu);
}

TEST_CASE("Colours decode as Moho's SCR_DecodeColor: 6 or 8 hex digits", "[color]") {
    CHECK(decode_color("ff0000") == 0xFFFF0000u);   // six: opaque
    CHECK(decode_color("80ff0000") == 0x80FF0000u); // eight: as written
    CHECK(decode_color("FFe80a0a") == 0xFFE80A0Au);
}

TEST_CASE("Anything else is no colour", "[color]") {
    CHECK_FALSE(decode_color(""));
    CHECK_FALSE(decode_color("ff00"));       // too short
    CHECK_FALSE(decode_color("ff00ff00ff")); // too long
    CHECK_FALSE(decode_color("zzzzzz"));     // not hex
    CHECK_FALSE(decode_color("NotAColor"));
}

TEST_CASE("The colour names are Moho's, in its order", "[color]") {
    const auto& names = color_names();
    REQUIRE(names.size() == 141);
    CHECK(names.front() == "AliceBlue");
    CHECK(names.back() == "transparent");
}
