#include <catch2/catch_test_macros.hpp>
#include "sim/sim_callback_queue.hpp"
#include "core/preferences.hpp"
#include "core/localization.hpp"
#include <filesystem>

// ─── SimCallbackQueue ────────────────────────────────────────────────────────

TEST_CASE("SimCallbackQueue push and drain", "[phase1][simcallback]") {
    osc::sim::SimCallbackQueue queue;
    REQUIRE(queue.empty());

    osc::sim::SimCallbackEntry entry;
    entry.func_name = "TestCallback";
    entry.args["key1"] = std::string("value1");
    entry.args["key2"] = osc::f64(42.0);
    entry.args["key3"] = true;
    entry.unit_ids = {1, 2, 3};

    queue.push(entry);
    REQUIRE_FALSE(queue.empty());

    auto drained = queue.drain();
    REQUIRE(queue.empty());
    REQUIRE(drained.size() == 1);
    REQUIRE(drained[0].func_name == "TestCallback");
    REQUIRE(std::get<std::string>(drained[0].args.at("key1")) == "value1");
    REQUIRE(std::get<osc::f64>(drained[0].args.at("key2")) == 42.0);
    REQUIRE(std::get<bool>(drained[0].args.at("key3")) == true);
    REQUIRE(drained[0].unit_ids.size() == 3);
    REQUIRE(drained[0].unit_ids[0] == 1);
}

TEST_CASE("SimCallbackQueue drain clears queue", "[phase1][simcallback]") {
    osc::sim::SimCallbackQueue queue;

    osc::sim::SimCallbackEntry e1;
    e1.func_name = "First";
    queue.push(e1);

    osc::sim::SimCallbackEntry e2;
    e2.func_name = "Second";
    queue.push(e2);

    auto batch = queue.drain();
    REQUIRE(batch.size() == 2);
    REQUIRE(batch[0].func_name == "First");
    REQUIRE(batch[1].func_name == "Second");
    REQUIRE(queue.empty());

    // Second drain returns empty
    auto empty = queue.drain();
    REQUIRE(empty.empty());
}

TEST_CASE("SimCallbackQueue empty entry", "[phase1][simcallback]") {
    osc::sim::SimCallbackQueue queue;

    osc::sim::SimCallbackEntry entry;
    entry.func_name = "NoArgs";
    // args and unit_ids left empty
    queue.push(entry);

    auto drained = queue.drain();
    REQUIRE(drained.size() == 1);
    REQUIRE(drained[0].func_name == "NoArgs");
    REQUIRE(drained[0].args.empty());
    REQUIRE(drained[0].unit_ids.empty());
}

// ─── Preferences (phase1-tagged) ─────────────────────────────────────────────

TEST_CASE("Preferences typed get/set roundtrip", "[phase1][preferences]") {
    osc::core::Preferences prefs;

    prefs.set_string("str_key", "hello");
    REQUIRE(prefs.get_string("str_key", "") == "hello");

    prefs.set_float("float_key", 1.5f);
    REQUIRE(prefs.get_float("float_key", 0.0f) == 1.5f);

    prefs.set_bool("bool_key", false);
    REQUIRE(prefs.get_bool("bool_key", true) == false);

    prefs.set_int("int_key", 7);
    REQUIRE(prefs.get_int("int_key", 0) == 7);
}

TEST_CASE("Preferences defaults returned for missing keys", "[phase1][preferences]") {
    osc::core::Preferences prefs;
    REQUIRE(prefs.get_string("missing", "fallback") == "fallback");
    REQUIRE(prefs.get_float("missing", 3.14f) == 3.14f);
    REQUIRE(prefs.get_bool("missing", true) == true);
    REQUIRE(prefs.get_int("missing", -1) == -1);
}

TEST_CASE("Preferences save and load persistence", "[phase1][preferences]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "osc_test_phase1_prefs.json";
    fs::remove(tmp);

    {
        osc::core::Preferences prefs;
        prefs.set_string("map", "SCMP_009");
        prefs.set_float("zoom", 200.0f);
        prefs.set_bool("fog", true);
        prefs.set_int("army", 2);
        prefs.save(tmp.string());
    }
    {
        osc::core::Preferences prefs;
        prefs.load(tmp.string());
        REQUIRE(prefs.get_string("map", "") == "SCMP_009");
        REQUIRE(prefs.get_float("zoom", 0.0f) == 200.0f);
        REQUIRE(prefs.get_bool("fog", false) == true);
        REQUIRE(prefs.get_int("army", 0) == 2);
    }

    fs::remove(tmp);
}

// ─── Localization (phase1-tagged) ────────────────────────────────────────────

TEST_CASE("Localization basic lookup", "[phase1][localization]") {
    osc::core::Localization loc;
    loc.add("unit_factory", "Land Factory");
    loc.add("unit_engineer", "Engineer");

    REQUIRE(loc.lookup("unit_factory") == "Land Factory");
    REQUIRE(loc.lookup("unit_engineer") == "Engineer");
    // Missing key returns the key itself (FA convention)
    REQUIRE(loc.lookup("missing_key") == "missing_key");
}

TEST_CASE("Localization LOC wrapper format", "[phase1][localization]") {
    osc::core::Localization loc;
    loc.add("<LOC build_key>", "Build Unit");

    REQUIRE(loc.lookup("<LOC build_key>") == "Build Unit");
    // Unrecognised <LOC …> key returns itself
    REQUIRE(loc.lookup("<LOC unknown>") == "<LOC unknown>");
}

TEST_CASE("Localization format substitution", "[phase1][localization]") {
    osc::core::Localization loc;
    loc.add("greet_key", "Hello %s, you have %s items");

    std::vector<std::string> args = {"Brandon", "5"};
    auto result = loc.format("greet_key", args);
    REQUIRE(result == "Hello Brandon, you have 5 items");
}

TEST_CASE("Localization size tracks entries", "[phase1][localization]") {
    osc::core::Localization loc;
    REQUIRE(loc.size() == 0);
    loc.add("a", "Alpha");
    loc.add("b", "Beta");
    REQUIRE(loc.size() == 2);
}
