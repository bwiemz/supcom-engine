#include <catch2/catch_test_macros.hpp>
#include "core/localization.hpp"

TEST_CASE("LOC cache basic lookup", "[loc]") {
    osc::core::Localization loc;
    loc.add("test_key_001", "Hello World");
    loc.add("test_key_002", "Build Factory");

    REQUIRE(loc.lookup("test_key_001") == "Hello World");
    REQUIRE(loc.lookup("test_key_002") == "Build Factory");
    // Missing key returns the key itself (FA convention)
    REQUIRE(loc.lookup("missing_key") == "missing_key");
}

TEST_CASE("LOC cache from Lua string table", "[loc]") {
    osc::core::Localization loc;
    // Simulate loading: strings_db.lua sets global tables like:
    // usloc = { ["test_key"] = "Value" }
    loc.add("<LOC test_key>", "Formatted Value");
    REQUIRE(loc.lookup("<LOC test_key>") == "Formatted Value");
}

TEST_CASE("LOCF format substitution", "[loc]") {
    osc::core::Localization loc;
    loc.add("fmt_key", "Hello %s, you have %d items");
    // LOCF does sprintf-style substitution
    auto result = loc.format("fmt_key", {"World", "42"});
    REQUIRE(result == "Hello World, you have 42 items");
}
