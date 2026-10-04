#include <catch2/catch_test_macros.hpp>

#include "lua/engine_bindings.hpp"

#include <string>

using osc::lua::utf8_substring;

TEST_CASE("STR_Utf8SubString counts characters from 1", "[lua][strings]") {
    CHECK(utf8_substring("Gunship.", 1, 4) == "Guns");
    CHECK(utf8_substring("Gunship.", 5, 4) == "hip.");
    CHECK(utf8_substring("Gunship.", 5, 100) == "hip.");
    const std::string accented = std::string("a\xC3\xA9") + "b";
    CHECK(utf8_substring(accented, 2, 1) == "\xC3\xA9");
    CHECK(utf8_substring(accented, 3, 1) == "b");
    CHECK(utf8_substring("abc", 0, 2) == "ab");
    CHECK(utf8_substring("abc", 4, 1).empty());
    CHECK(utf8_substring("abc", 1, 0).empty());
}
