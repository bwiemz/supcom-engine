#include <catch2/catch_test_macros.hpp>
#include "core/preferences.hpp"
#include <filesystem>

TEST_CASE("Preferences get/set with defaults", "[preferences]") {
    osc::core::Preferences prefs;
    // No file loaded — should return defaults
    REQUIRE(prefs.get_string("nonexistent", "fallback") == "fallback");
    REQUIRE(prefs.get_float("nonexistent", 3.14f) == 3.14f);
    REQUIRE(prefs.get_bool("nonexistent", true) == true);

    prefs.set_string("name", "test");
    REQUIRE(prefs.get_string("name", "") == "test");

    prefs.set_float("volume", 0.8f);
    REQUIRE(prefs.get_float("volume", 0.0f) == 0.8f);

    prefs.set_bool("fullscreen", false);
    REQUIRE(prefs.get_bool("fullscreen", true) == false);
}

TEST_CASE("Preferences save and load roundtrip", "[preferences]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "osc_test_prefs.json";

    {
        osc::core::Preferences prefs;
        prefs.set_string("map", "SCMP_009");
        prefs.set_float("zoom", 120.5f);
        prefs.set_bool("fog", true);
        prefs.save(tmp.string());
    }
    {
        osc::core::Preferences prefs;
        prefs.load(tmp.string());
        REQUIRE(prefs.get_string("map", "") == "SCMP_009");
        REQUIRE(prefs.get_float("zoom", 0.0f) == 120.5f);
        REQUIRE(prefs.get_bool("fog", false) == true);
    }

    fs::remove(tmp);
}

TEST_CASE("Preferences nested keys with dot notation", "[preferences]") {
    osc::core::Preferences prefs;
    prefs.set_string("options.graphics.quality", "high");
    REQUIRE(prefs.get_string("options.graphics.quality", "") == "high");
}
