#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "audio/sound_manager.hpp"
#include "xact_fixtures.hpp"

#include <filesystem>

using namespace osc;
using namespace osc::test::xact_fixtures;
using osc::audio::INVALID_SOUND;
using osc::audio::SoundManager;
using Catch::Matchers::WithinAbs;

namespace {

namespace fs = std::filesystem;

/// A sounds directory with the fixture banks: "Click" (0.1 s one-shot,
/// Global) and "Shot" (Music: loops forever from 150 ms, cue limit 2, a
/// 300 ms fade-out, a Distance falloff to -2000 mB at 1000 units). Music
/// allows one instance and replaces the oldest.
struct Sounds {
    fs::path dir = fs::temp_directory_path() / "osc_sound_engine_test";
    /// Optionally: Music's limit and behaviour, and Click's category and
    /// priority (Shot's priority is 7).
    explicit Sounds(u8 music_limit = 1, u8 music_behavior = 2, u16 click_category = 0,
                    u8 click_priority = 0) {
        fs::remove_all(dir);
        fs::create_directories(dir);
        u32 rpc = 0;
        write(dir / "Game.xgs", make_xgs(&rpc, music_limit, music_behavior));
        write(dir / "Test.xsb", make_xsb("TestWaves", rpc, click_category, click_priority));
        write(dir / "TestWaves.xwb", make_xwb("TestWaves", 4, 2205)); // 0.1 s waves
    }
    ~Sounds() { fs::remove_all(dir); }
};

} // namespace

TEST_CASE("Sound engine: a one-shot plays for its wave's length", "[audio][engine]") {
    Sounds s;
    SoundManager sm(s.dir, /*output=*/false);
    REQUIRE(sm.has_data());
    int finished = 0;
    const auto h = sm.play("Test", "Click");
    REQUIRE(h != INVALID_SOUND);
    sm.on_finished(h, [&] { ++finished; });
    CHECK(sm.is_cue_playing("test", "Click"));
    sm.update(0.05f);
    CHECK(sm.is_playing(h));
    sm.update(0.06f); // 0.11 s: past the 0.1 s wave
    CHECK_FALSE(sm.is_playing(h));
    CHECK(finished == 1);
    CHECK(sm.active_count() == 0);
    int late = 0;
    sm.on_finished(h, [&] { ++late; }); // already over: at once
    CHECK(late == 1);

    CHECK(sm.play("Test", "Nope") == INVALID_SOUND);
    CHECK(sm.play("NoSuchBank", "Click") == INVALID_SOUND);
}

TEST_CASE("Sound engine: a looping cue plays until stopped, then fades out", "[audio][engine]") {
    Sounds s;
    SoundManager sm(s.dir, false);
    const auto h = sm.play("Test", "Shot");
    REQUIRE(h != INVALID_SOUND);
    sm.update(0.2f); // its play event fires at 150 ms
    sm.update(5.0f);
    CHECK(sm.is_playing(h));
    const f32 before = sm.current_gain(h);
    CHECK(before > 0.0f);

    sm.stop(h, /*immediate=*/false); // the cue's 300 ms fade-out
    sm.update(0.15f);
    CHECK(sm.is_playing(h));
    CHECK(sm.current_gain(h) < before);
    sm.update(0.2f);
    CHECK_FALSE(sm.is_playing(h));

    const auto h2 = sm.play("Test", "Shot");
    sm.stop(h2); // immediate
    sm.update(0.0f);
    CHECK_FALSE(sm.is_playing(h2));
}

TEST_CASE("Sound engine: a full category replaces its oldest sound", "[audio][engine]") {
    Sounds s;
    SoundManager sm(s.dir, false);
    const auto first = sm.play("Test", "Shot");
    sm.update(0.2f);
    const auto second = sm.play("Test", "Shot"); // Music holds one: first fades
    REQUIRE(second != INVALID_SOUND);
    sm.update(0.1f);
    CHECK(sm.is_playing(first));
    CHECK(sm.is_playing(second));
    sm.update(0.3f);
    CHECK_FALSE(sm.is_playing(first));
    CHECK(sm.is_playing(second));

    // Global has no limit
    CHECK(sm.play("Test", "Click") != INVALID_SOUND);
    CHECK(sm.play("Test", "Click") != INVALID_SOUND);
    CHECK(sm.play("Test", "Click") != INVALID_SOUND);
}

TEST_CASE("Sound engine: distance falloff comes from the sound's RPC curve", "[audio][engine]") {
    Sounds s;
    SoundManager sm(s.dir, false);
    sm.set_listener({0, 0, 0}, {0, 0, 1});
    const sim::Vector3 near{0, 0, 0};
    const auto h = sm.play("Test", "Shot", &near);
    REQUIRE(h != INVALID_SOUND);
    sm.update(0.2f);
    const f32 at_listener = sm.current_gain(h);
    REQUIRE(at_listener > 0.0f);
    sm.set_position(h, {500, 0, 0}); // the curve: -1000 mB at 500
    sm.update(0.0f);
    CHECK_THAT(sm.current_gain(h) / at_listener, WithinAbs(0.316, 0.005));
}

TEST_CASE("Sound engine: the player's category volumes scale their subtree", "[audio][engine]") {
    Sounds s;
    SoundManager sm(s.dir, false);
    const auto h = sm.play("Test", "Shot");
    sm.update(0.2f);
    const f32 full = sm.current_gain(h);
    REQUIRE(full > 0.0f);

    sm.set_category_volume("Music", 0.5f);
    CHECK(sm.category_volume("Music") == 0.5f);
    sm.update(0.0f);
    CHECK_THAT(sm.current_gain(h) / full, WithinAbs(0.5, 1e-4));

    sm.set_category_volume("Global", 0.0f); // Music's parent
    sm.update(0.0f);
    CHECK(sm.current_gain(h) == 0.0f);
    CHECK(sm.category_volume("NoSuchCategory") == 1.0f);
    sm.set_category_volume("Music", 7.0f); // clamped
    CHECK(sm.category_volume("Music") == 1.0f);
}

TEST_CASE("Sound engine: a LOD cutoff culls distant sounds", "[audio][engine]") {
    Sounds s;
    SoundManager sm(s.dir, false);
    sm.set_listener({0, 0, 0}, {0, 0, 1});
    const sim::Vector3 far{200, 0, 0}, near{50, 0, 0};
    CHECK(sm.play("Test", "Click", &far, "TestCutoff") == INVALID_SOUND); // cutoff 100
    CHECK(sm.play("Test", "Click", &near, "TestCutoff") != INVALID_SOUND);
    sm.set_global_variable("TestCutoff", 500);
    CHECK(sm.global_variable("TestCutoff") == 500.0f);
    CHECK(sm.play("Test", "Click", &far, "TestCutoff") != INVALID_SOUND);
    CHECK(sm.play("Test", "Click", nullptr, "TestCutoff") != INVALID_SOUND); // 2D: never culled
}

TEST_CASE("Sound engine: stop_all ends every sound", "[audio][engine]") {
    Sounds s;
    SoundManager sm(s.dir, false);
    sm.play("Test", "Shot");
    sm.play("Test", "Click");
    CHECK(sm.active_count() == 2);
    sm.stop_all();
    CHECK(sm.active_count() == 0);
}

TEST_CASE("Sound engine: no sound data plays nothing", "[audio][engine]") {
    SoundManager sm(fs::temp_directory_path() / "osc_no_such_sounds_dir", false);
    CHECK_FALSE(sm.has_data());
    CHECK(sm.play("Test", "Click") == INVALID_SOUND);
    sm.update(1.0f);
    CHECK(sm.category_volume("Music") == 1.0f);
}

TEST_CASE("Sound engine: replace-lowest-priority stops the lowest priority", "[audio][engine]") {
    // Music holds two and replaces its lowest priority; Click (priority 3)
    // and Shot (7) both play in it.
    Sounds s(/*music_limit=*/2, /*ReplaceLowestPriority=*/4, /*click_category=*/1,
             /*click_priority=*/3);
    SoundManager sm(s.dir, false);
    const auto shot = sm.play("Test", "Shot");
    REQUIRE(shot != INVALID_SOUND);
    sm.update(0.2f); // Shot's play event fires at 150 ms
    const auto click = sm.play("Test", "Click"); // a 0.1 s wave
    REQUIRE(click != INVALID_SOUND);
    const f32 click_gain = sm.current_gain(click);
    const f32 shot_gain = sm.current_gain(shot);
    REQUIRE(click_gain > 0.0f);
    const auto shot2 = sm.play("Test", "Shot"); // Music is full: Click gives way
    REQUIRE(shot2 != INVALID_SOUND);
    sm.update(0.05f); // a quarter into Music's 200 ms fade-out
    CHECK(sm.current_gain(click) < click_gain * 0.9f); // fading
    CHECK_THAT(sm.current_gain(shot), WithinAbs(shot_gain, 1e-5)); // untouched
    CHECK(sm.is_playing(shot2));
}
