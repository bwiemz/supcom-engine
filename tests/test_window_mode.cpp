// The window as FA opens and sets it (M217h): Moho's startup head (CScApp),
// SC_PrimaryAdapter's "w,h,fps", the primary adapter option's states
// (SetupPrimaryAdapterSettings), and the cursor on a scaled display.

#include <catch2/catch_test_macros.hpp>

#include "app/window_mode.hpp"
#include "core/cursor.hpp"
#include "core/fullscreen.hpp"

#include <string>
#include <vector>

using namespace osc;
using namespace osc::app;

TEST_CASE("A primary_adapter mode parses as CFG_ParseResolutionTriple (M217h)", "[window]") {
    CHECK(parse_resolution("1920,1080,144") == Resolution{1920, 1080, 144});
    CHECK(parse_resolution("1280,800") == Resolution{1280, 800, 60});
    CHECK(parse_resolution(" 1024 , 768 , 60 ") == Resolution{1024, 768, 60});
    CHECK_FALSE(parse_resolution("windowed"));
    CHECK_FALSE(parse_resolution("overridden"));
    CHECK_FALSE(parse_resolution("1024"));
    CHECK_FALSE(parse_resolution("1024,x,60"));
    CHECK_FALSE(parse_resolution("0,768,60"));
    CHECK_FALSE(parse_resolution("1,2,3,4"));
    CHECK_FALSE(parse_resolution(""));
}

TEST_CASE("The window opens as the options and prefs say (M217h)", "[window]") {
    // A mode: full screen at it (FA's first launch: its default mode)
    WindowPrefs prefs;
    WindowMode m = startup_window_mode({}, prefs);
    CHECK(m.fullscreen);
    CHECK(m.size == Resolution{1024, 768, 60});
    CHECK_FALSE(m.overridden);
    prefs.primary_adapter = "2560,1440,120";
    m = startup_window_mode({"opensupcom"}, prefs);
    CHECK(m.fullscreen);
    CHECK(m.size == Resolution{2560, 1440, 120});

    // Windowed: the size, place and maximized state it last had, else the
    // default
    prefs.primary_adapter = "windowed";
    m = startup_window_mode({}, prefs);
    CHECK_FALSE(m.fullscreen);
    CHECK(m.size == Resolution{1024, 768, 60});
    CHECK_FALSE(m.position);
    prefs.width = 1500;
    prefs.height = 900;
    prefs.x = -20;
    prefs.y = 40;
    prefs.maximized = true;
    m = startup_window_mode({}, prefs);
    CHECK(m.size == Resolution{1500, 900, 60});
    REQUIRE(m.position);
    CHECK((*m.position)[0] == -20);
    CHECK((*m.position)[1] == 40);
    CHECK(m.maximized);
}

TEST_CASE("The command line overrides the options, with Moho's prefixes (M217h)", "[window]") {
    WindowPrefs prefs; // a full-screen mode
    WindowMode m = startup_window_mode({"opensupcom", "/windowed", "1280", "800"}, prefs);
    CHECK_FALSE(m.fullscreen);
    CHECK(m.size == Resolution{1280, 800, 60});
    CHECK(m.overridden);
    // Its least, 1024x720; any prefix and alias, in any case
    m = startup_window_mode({"-SIZE", "800", "600"}, prefs);
    CHECK(m.size == Resolution{1024, 720, 60});
    m = startup_window_mode({"--window", "1200", "700"}, prefs);
    CHECK(m.size == Resolution{1200, 720, 60});
    CHECK(startup_window_mode({"+windowed", "1300", "900"}, prefs).size.width == 1300);
    CHECK(startup_window_mode({"\\windowed", "1300", "900"}, prefs).size.width == 1300);
    // Too few numbers after it: not the option
    CHECK(startup_window_mode({"/windowed", "1300"}, prefs).fullscreen);
    // Full screen at a size, at least the default
    m = startup_window_mode({"/fullscreen", "1600", "500"}, prefs);
    CHECK(m.fullscreen);
    CHECK(m.size == Resolution{1600, 768, 60});
    CHECK(m.overridden);
    // Maximized and placed (a window only)
    prefs.primary_adapter = "windowed";
    m = startup_window_mode({"/maximize", "/position", "10", "-5"}, prefs);
    CHECK(m.maximized);
    REQUIRE(m.position);
    CHECK((*m.position)[0] == 10);
    CHECK((*m.position)[1] == -5);
    // The engine's own flags don't read as Moho's
    CHECK_FALSE(
        startup_window_mode({"--map", "/maps/SCMP_009/SCMP_009_scenario.lua"}, prefs).overridden);
}

TEST_CASE("A native full screen settles before it toggles or resizes", "[window]") {
    using core::native_fullscreen_step;
    using Step = core::NativeFullscreenStep;
    CHECK(native_fullscreen_step(true, false, false, false) == Step{.toggle = true});
    CHECK(native_fullscreen_step(true, false, true, false) == Step{});
    CHECK(native_fullscreen_step(true, true, false, false) == Step{.settled = true});
    CHECK(native_fullscreen_step(false, true, false, true) == Step{.toggle = true});
    CHECK(native_fullscreen_step(false, true, true, true) == Step{});
    CHECK(native_fullscreen_step(false, false, true, true) == Step{});
    CHECK(native_fullscreen_step(false, false, false, true) ==
          Step{.apply_windowed = true, .settled = true});
    CHECK(native_fullscreen_step(std::nullopt, false, false, false) == Step{.settled = true});
    CHECK(native_fullscreen_step(std::nullopt, true, false, false) == Step{.settled = true});
#ifdef __APPLE__
    CHECK(core::kNativeFullscreen);
#else
    CHECK_FALSE(core::kNativeFullscreen);
#endif
}

TEST_CASE("The adapter option lists the display's modes as Moho does (M217h)", "[window]") {
    const std::vector<Resolution> modes = {{800, 600, 60},    {1024, 768, 60}, {1920, 1080, 60},
                                           {1920, 1080, 144}, {1024, 768, 60}, {1280, 720, 60},
                                           {1600, 900, 60}};
    auto [states, fallback] = adapter_states(modes, false);
    REQUIRE(states.size() == 5);
    CHECK(states[0].text == "<LOC OPTIONS_0070>Windowed");
    CHECK(states[0].key == "windowed");
    // In the display's order; under 1024x768 (800x600, 1280x720) and
    // repeats left out
    CHECK(states[1].key == "1024,768,60");
    CHECK(states[1].text == "1024x768(60)");
    CHECK(states[2].key == "1920,1080,60");
    CHECK(states[3].key == "1920,1080,144");
    CHECK(states[3].text == "1920x1080(144)");
    CHECK(states[4].key == "1600,900,60");
    CHECK(fallback == "1024,768,60");
    // Overridden: the override alone, and its default
    auto [only, overridden] = adapter_states(modes, true);
    REQUIRE(only.size() == 1);
    CHECK(only[0].key == "overridden");
    CHECK(only[0].text == "<LOC _Command_Line_Override>");
    CHECK(overridden == "overridden");
}

TEST_CASE("A native full screen is one adapter state the option keeps", "[window]") {
    CHECK(native_fullscreen_key("1920,1200,120") == "1920,1200,120");
    CHECK(native_fullscreen_key("1024,768,60") == "1024,768,60");
    CHECK(native_fullscreen_key("windowed") == "1024,768,60");
    CHECK(native_fullscreen_key("overridden") == "1024,768,60");

    auto [states, fallback] = native_adapter_states("1920,1200,120", false);
    REQUIRE(states.size() == 2);
    CHECK(states[0].key == "windowed");
    CHECK(states[1].key == "1920,1200,120");
    CHECK(states[1].text == "Full Screen");
    CHECK(fallback == "1024,768,60");
    auto [first, first_fallback] = native_adapter_states("windowed", false);
    REQUIRE(first.size() == 2);
    CHECK(first[1].key == "1024,768,60");
    auto [only, overridden] = native_adapter_states("1920,1200,120", true);
    REQUIRE(only.size() == 1);
    CHECK(only[0].key == "overridden");
    CHECK(overridden == "overridden");

    CHECK(adapter_option_for(false, "1920,1200,120") == "windowed");
    CHECK(adapter_option_for(true, "1920,1200,120") == "1920,1200,120");
    CHECK(adapter_option_for(true, "windowed") == "1024,768,60");
}

TEST_CASE("A cursor in window units maps to framebuffer pixels (M217h)", "[window]") {
    // 150% scale: a 1000x600 window drawn at 1500x900
    const auto p = core::to_framebuffer(100.0, 200.0, 1000, 600, 1500, 900);
    CHECK(p[0] == 150.0);
    CHECK(p[1] == 300.0);
    const auto same = core::to_framebuffer(100.0, 200.0, 1600, 900, 1600, 900);
    CHECK(same[0] == 100.0);
    CHECK(same[1] == 200.0);
    const auto minimized = core::to_framebuffer(5.0, 6.0, 0, 0, 0, 0);
    CHECK(minimized[0] == 5.0);
    CHECK(minimized[1] == 6.0);
}
