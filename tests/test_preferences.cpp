#include <catch2/catch_test_macros.hpp>
#include "core/preferences.hpp"
#include "lua/lua_state.hpp"

extern "C" {
#include <lua.h>
}

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

namespace fs = std::filesystem;

/// A script state with GetPreference/SetPreference-like helpers over `prefs`.
struct Script {
    osc::lua::LuaState state;
    lua_State* L = state.raw();

    /// Run Lua; fail the test on an error.
    void run(const char* code) {
        auto r = state.do_string(code);
        if (!r) FAIL(r.error().message);
    }
    /// prefs[key] = global `name`
    void set_from_global(osc::core::Preferences& prefs, const char* key, const char* name) {
        lua_pushstring(L, name);
        lua_rawget(L, LUA_GLOBALSINDEX);
        prefs.set(key, L, -1);
        lua_pop(L, 1);
    }
    /// global `name` = prefs[key]
    void get_to_global(const osc::core::Preferences& prefs, const char* key, const char* name) {
        lua_pushstring(L, name);
        prefs.push(key, L);
        lua_rawset(L, LUA_GLOBALSINDEX);
    }
    bool global_true(const char* name) {
        lua_pushstring(L, name);
        lua_rawget(L, LUA_GLOBALSINDEX);
        const bool v = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return v;
    }
};

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

fs::path temp_prefs(const char* name) {
    return fs::temp_directory_path() / name;
}

} // namespace

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

    // A differently typed value gives the default
    REQUIRE(prefs.get_bool("name", true) == true);
}

TEST_CASE("Preferences nested keys with dot notation", "[preferences]") {
    osc::core::Preferences prefs;
    prefs.set_string("options.graphics.quality", "high");
    REQUIRE(prefs.get_string("options.graphics.quality", "") == "high");
    // Setting through a non-table replaces it with a table
    prefs.set_int("options.graphics", 3);
    prefs.set_int("options.graphics.fidelity", 2);
    REQUIRE(prefs.get_int("options.graphics.fidelity", 0) == 2);
}

TEST_CASE("Preferences hold Lua tables, as retail's profiles need", "[preferences]") {
    osc::core::Preferences prefs;
    Script s;
    // What retail's Prefs.CreateProfile does
    s.run("profiles = { { Name = 'Brandon', options = { fidelity = 2 } } }");
    s.set_from_global(prefs, "profile.profiles", "profiles");
    lua_pushnumber(s.L, 1);
    prefs.set("profile.current", s.L, -1);
    lua_pop(s.L, 1);

    // Retail's GetCurrentProfile: GetPreference('profile').profiles[current]
    s.get_to_global(prefs, "profile", "profile");
    s.run("ok_profile = profile.profiles[profile.current].Name == 'Brandon'");
    CHECK(s.global_true("ok_profile"));

    // A digit segment indexes by number
    CHECK(prefs.get_string("profile.profiles.1.Name", "") == "Brandon");
    CHECK(prefs.current_profile_path() == "profile.profiles.1");

    // What a script gets back is a copy: changing it changes nothing until
    // SetPreference (retail's SetToCurrentProfile relies on writing back).
    s.run("profile.profiles[1].Name = 'Other'");
    CHECK(prefs.get_string("profile.profiles.1.Name", "") == "Brandon");
    s.set_from_global(prefs, "profile", "profile");
    CHECK(prefs.get_string("profile.profiles.1.Name", "") == "Other");

    // Moho's GetOptions: the current profile's option
    s.run("opt = nil");
    lua_pushstring(s.L, "opt");
    prefs.push_option("fidelity", s.L);
    lua_rawset(s.L, LUA_GLOBALSINDEX);
    s.run("ok_opt = opt == 2");
    CHECK(s.global_true("ok_opt"));

    // nil removes
    lua_pushnil(s.L);
    prefs.set("profile.profiles.1.options", s.L, -1);
    lua_pop(s.L, 1);
    CHECK(prefs.get_int("profile.profiles.1.options.fidelity", -1) == -1);
}

TEST_CASE("Preferences save as Lua and load back", "[preferences]") {
    const fs::path file = temp_prefs("osc_test_game.prefs");
    fs::remove(file);
    {
        osc::core::Preferences prefs;
        Script s;
        s.run(R"(
            data = {
                name = 'say "hi"\n\ttab\\',
                list = { 10, 20.5, -3 },
                flags = { stratview = true, legacy = false },
                ['not an identifier'] = 'x',
                [7] = 'seven',
            }
            data.list[4] = { nested = { deeper = 'yes' } }
        )");
        s.set_from_global(prefs, "profile", "data");
        prefs.set_int("zoom", 120);
        REQUIRE(prefs.save(file));
    }
    const std::string text = read_file(file);
    // Readable, sorted Lua
    CHECK(text.find("profile = {") != std::string::npos);
    CHECK(text.find("zoom = 120") != std::string::npos);
    CHECK(text.find("[\"not an identifier\"] = \"x\"") != std::string::npos);

    osc::core::Preferences loaded;
    REQUIRE(loaded.load(file));
    CHECK(loaded.get_string("profile.name", "") == "say \"hi\"\n\ttab\\");
    CHECK(loaded.get_float("profile.list.2", 0) == 20.5f);
    CHECK(loaded.get_int("profile.list.3", 0) == -3);
    CHECK(loaded.get_string("profile.list.4.nested.deeper", "") == "yes");
    CHECK(loaded.get_bool("profile.flags.stratview", false) == true);
    CHECK(loaded.get_bool("profile.flags.legacy", true) == false);
    CHECK(loaded.get_string("profile.7", "") == "seven");
    CHECK(loaded.get_int("zoom", 0) == 120);

    // Stable output: saving what was loaded writes the same text
    const fs::path again = temp_prefs("osc_test_game2.prefs");
    REQUIRE(loaded.save(again));
    CHECK(read_file(again) == text);
    fs::remove(file);
    fs::remove(again);
}

TEST_CASE("Preferences files hold data only", "[preferences]") {
    const fs::path file = temp_prefs("osc_test_bad.prefs");
    osc::core::Preferences prefs;
    prefs.set_string("keep", "me");

    // Code in the file has nothing to call: no os, io or print.
    { std::ofstream(file) << "x = 1\nos.execute('echo pwned')\n"; }
    CHECK_FALSE(prefs.load(file));
    { std::ofstream(file) << "profile = {"; } // syntax error
    CHECK_FALSE(prefs.load(file));
    CHECK_FALSE(prefs.load(temp_prefs("osc_test_missing.prefs")));
    // A failed load keeps what was there
    CHECK(prefs.get_string("keep", "") == "me");
    CHECK(prefs.get_int("x", -1) == -1);
    fs::remove(file);
}

TEST_CASE("Preferences write a cycle without hanging", "[preferences]") {
    osc::core::Preferences prefs;
    Script s;
    s.run("t = { name = 'loop' }; t.self = t");
    s.set_from_global(prefs, "cyclic", "t");
    const fs::path file = temp_prefs("osc_test_cycle.prefs");
    REQUIRE(prefs.save(file));
    osc::core::Preferences loaded;
    REQUIRE(loaded.load(file));
    CHECK(loaded.get_string("cyclic.name", "") == "loop");
    fs::remove(file);
}

TEST_CASE("Preferences make a current profile when there is none", "[preferences]") {
    osc::core::Preferences prefs;
    CHECK(prefs.current_profile_path().empty());
    CHECK(prefs.ensure_profile("Player"));
    CHECK(prefs.current_profile_path() == "profile.profiles.1");
    CHECK(prefs.get_string("profile.profiles.1.Name", "") == "Player");
    CHECK_FALSE(prefs.ensure_profile("Player")); // already has one

    // A dangling current falls back to the first profile
    prefs.set_int("profile.current", 5);
    CHECK(prefs.ensure_profile("Other"));
    CHECK(prefs.get_int("profile.current", 0) == 1);
    CHECK(prefs.get_string("profile.profiles.1.Name", "") == "Player");
}

TEST_CASE("Preferences without a path are not saved", "[preferences]") {
    osc::core::Preferences prefs;
    prefs.set_int("x", 1);
    CHECK_FALSE(prefs.save()); // in memory only (tests, captures)
}
