#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "audio/sound_manager.hpp"
#include "xact_fixtures.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

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
/// allows one instance and replaces the oldest. Each case gets its own
/// directory, as ctest runs them in parallel.
struct Sounds {
    fs::path dir;
    /// Optionally: Music's limit and behaviour, and Click's category and
    /// priority (Shot's priority is 7).
    explicit Sounds(u8 music_limit = 1, u8 music_behavior = 2, u16 click_category = 0,
                    u8 click_priority = 0, bool new_variation_on_loop = false) {
        std::random_device rd;
        dir = fs::temp_directory_path() /
              ("osc_sound_engine_test_" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(dir);
        u32 rpc = 0;
        write(dir / "Game.xgs", make_xgs(&rpc, music_limit, music_behavior));
        write(dir / "Test.xsb", make_xsb("TestWaves", rpc, click_category, click_priority, 0x40,
                                         new_variation_on_loop));
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
    sm.set_position(h, {0, 0, 500}); // ahead, still centred: the curve's -1000 mB at 500
    sm.update(0.0f);
    CHECK_THAT(sm.current_gain(h) / at_listener, WithinAbs(0.316, 0.005));
}

TEST_CASE("Sound engine: X3DAudio's stereo matrix, linear between +-90 degrees",
          "[audio][engine]") {
    // F3DAudio's stereo law: L + R = 1, centre [0.5, 0.5], hard right at
    // 90 degrees, and back across behind.
    const sim::Vector3 forward{0, 0, 1}, right{1, 0, 0};
    const auto gains = [&](sim::Vector3 d) {
        return SoundManager::stereo_gains(d, forward, right);
    };
    const auto near = [](std::array<f32, 2> g, f32 l, f32 r) {
        return std::abs(g[0] - l) < 1e-4f && std::abs(g[1] - r) < 1e-4f;
    };
    CHECK(near(gains({0, 0, 10}), 0.5f, 0.5f));     // ahead
    CHECK(near(gains({10, 0, 0}), 0.0f, 1.0f));     // right
    CHECK(near(gains({-10, 0, 0}), 1.0f, 0.0f));    // left
    CHECK(near(gains({10, 0, 10}), 0.25f, 0.75f));  // 45 degrees right
    CHECK(near(gains({0, 0, -10}), 0.5f, 0.5f));    // behind
    CHECK(near(gains({10, 0, -10}), 0.25f, 0.75f)); // 135: back toward centre
    CHECK(near(gains({0, 50, 0}), 0.5f, 0.5f));     // straight up: no azimuth
    CHECK(near(gains({0, 0, 0}), 0.5f, 0.5f));      // at the listener
}

TEST_CASE("Sound engine: a cue's Angle is its elevation off straight up", "[audio][engine]") {
    const sim::Vector3 listener{0, 100, 0};
    CHECK_THAT(SoundManager::cue_angle_degrees({0, 0, 0}, listener), WithinAbs(180.0, 1e-3));
    CHECK_THAT(SoundManager::cue_angle_degrees({100, 100, 0}, listener), WithinAbs(90.0, 1e-3));
    CHECK_THAT(SoundManager::cue_angle_degrees({100, 0, 0}, listener), WithinAbs(135.0, 1e-3));
    CHECK_THAT(SoundManager::cue_angle_degrees({0, 200, 0}, listener), WithinAbs(0.0, 1e-3));
}

TEST_CASE("Sound engine: a world sound is panned; centred it takes half each side",
          "[audio][engine]") {
    // Moho's X3DAudio matrix puts a centred mono emitter at [0.5, 0.5],
    // where a 2D sound plays at [1, 1]: world sounds sit 6 dB under UI.
    Sounds s;
    SoundManager sm(s.dir, false);
    sm.set_listener({0, 0, 0}, {0, 0, 1}, {1, 0, 0});
    const sim::Vector3 here{0, 0, 0};
    const auto flat = sm.play("Test", "Shot");
    const auto world = sm.play("Test", "Shot", &here); // Music holds one: replaces flat
    REQUIRE(world != INVALID_SOUND);
    sm.update(0.6f);
    f32 l = 0, r = 0;
    REQUIRE(sm.stereo(world, l, r));
    CHECK(l == 0.5f);
    CHECK(r == 0.5f);
    CHECK_FALSE(sm.stereo(flat, l, r)); // 2D, and replaced
    const f32 centred = sm.current_gain(world);
    sm.set_position(world, {1, 0, 0}); // hard right, 1 unit off: the curve is ~0 there
    sm.update(0.0f);
    REQUIRE(sm.stereo(world, l, r));
    CHECK(r == 1.0f);
    CHECK_THAT(sm.current_gain(world) / centred, WithinAbs(2.0, 0.01));
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

TEST_CASE("Sound engine: a world one-shot is filtered as Moho's FilterSound does",
          "[audio][engine]") {
    // Its LodCutoff against CameraDistance (not the emitter's distance),
    // then the player's hearing; one of each cue a beat; none while world
    // sounds are off.
    Sounds s;
    SoundManager sm(s.dir, false);
    using WS = SoundManager::WorldSound;
    const sim::Vector3 far{5000, 0, 0};
    // CameraDistance 100, TestCutoff 100: not beyond.
    CHECK(sm.play_world(WS{"Test", "Click", "TestCutoff", far, false, 1}) != INVALID_SOUND);
    sm.set_global_variable("CameraDistance", 150);
    CHECK(sm.play_world(WS{"Test", "Click", "TestCutoff", far, false, 2}) == INVALID_SOUND);
    sm.set_global_variable("TestCutoff", -1); // never culls
    CHECK(sm.play_world(WS{"Test", "Click", "TestCutoff", far, false, 3}) != INVALID_SOUND);

    const SoundManager::Hearing deaf_west = [](const sim::Vector3& p, bool) { return p.x >= 0; };
    CHECK(sm.play_world(WS{"Test", "Click", "", {-10, 0, 0}, false, 4}, deaf_west) ==
          INVALID_SOUND);
    CHECK(sm.play_world(WS{"Test", "Click", "", {10, 0, 0}, false, 4}, deaf_west) != INVALID_SOUND);
    // Again in beat 4 (a bank named in another case): deduped; beat 5 plays.
    CHECK(sm.play_world(WS{"test", "Click", "", {20, 0, 0}, false, 4}) == INVALID_SOUND);
    CHECK(sm.play_world(WS{"Test", "Click", "", {20, 0, 0}, false, 5}) != INVALID_SOUND);

    sm.set_world_enabled(false);
    CHECK(sm.play_world(WS{"Test", "Click", "", {0, 0, 0}, false, 6}) == INVALID_SOUND);
    sm.set_world_enabled(true);
    CHECK(sm.play_world(WS{"Test", "Click", "", {0, 0, 0}, false, 6}) != INVALID_SOUND);
}

TEST_CASE("Sound engine: entity loops start in view and near, and stop as Moho's do",
          "[audio][engine]") {
    Sounds s;
    SoundManager sm(s.dir, false);
    using EL = SoundManager::EntityLoop;
    std::vector<EL> want{EL{1, "Test", "Shot", "", {0, 0, 0}, false, /*in_view=*/false}};
    sm.sync_entity_loops(want);
    CHECK(sm.entity_loop(1) == INVALID_SOUND); // out of view: not started
    want[0].in_view = true;
    sm.set_global_variable("CameraDistance", 250); // too far out to start one
    sm.sync_entity_loops(want);
    CHECK(sm.entity_loop(1) == INVALID_SOUND);
    sm.set_global_variable("CameraDistance", 150);
    sm.sync_entity_loops(want);
    const auto h = sm.entity_loop(1);
    REQUIRE(h != INVALID_SOUND);
    sm.update(0.2f); // Shot's wave starts at 150 ms

    // Playing, it follows its entity, in view or not, near or far.
    want[0].pos = {30, 0, 0};
    want[0].in_view = false;
    sm.set_global_variable("CameraDistance", 400);
    sm.sync_entity_loops(want);
    sim::Vector3 at{};
    REQUIRE(sm.position(h, at));
    CHECK(at.x == 30.0f);
    CHECK(sm.entity_loop(1) == h);

    // Out of hearing: released (Shot's 300 ms fade), and forgotten.
    const SoundManager::Hearing deaf = [](const sim::Vector3&, bool) { return false; };
    sm.sync_entity_loops(want, deaf);
    CHECK(sm.entity_loop(1) == INVALID_SOUND);
    sm.update(0.1f);
    CHECK(sm.is_playing(h));
    sm.update(0.3f);
    CHECK_FALSE(sm.is_playing(h));

    // No longer wanted: released.
    sm.set_global_variable("CameraDistance", 50);
    want[0].in_view = true;
    sm.sync_entity_loops(want);
    const auto again = sm.entity_loop(1);
    REQUIRE(again != INVALID_SOUND);
    sm.sync_entity_loops({});
    CHECK(sm.entity_loop(1) == INVALID_SOUND);
    sm.update(0.4f);
    CHECK_FALSE(sm.is_playing(again));

    // Beyond its LodCutoff: stopped at once.
    want[0].lod_cutoff = "TestCutoff"; // 100
    sm.sync_entity_loops(want);
    const auto culled = sm.entity_loop(1);
    REQUIRE(culled != INVALID_SOUND);
    sm.set_global_variable("CameraDistance", 150);
    sm.sync_entity_loops(want);
    sm.update(0.0f);
    CHECK_FALSE(sm.is_playing(culled));

    // A one-shot cue in a loop slot plays out, then starts again in view.
    sm.set_global_variable("CameraDistance", 50);
    const std::vector<EL> once{EL{2, "Test", "Click", "", {0, 0, 0}, false, true}};
    sm.sync_entity_loops(once);
    const auto first = sm.entity_loop(2);
    REQUIRE(first != INVALID_SOUND);
    sm.update(0.11f); // Click's 0.1 s wave ends
    sm.sync_entity_loops(once);
    const auto second = sm.entity_loop(2);
    CHECK(second != INVALID_SOUND);
    CHECK(second != first);

    // World sounds off: loops are left as they are.
    sm.set_world_enabled(false);
    sm.sync_entity_loops({});
    CHECK(sm.entity_loop(2) == second);
}

TEST_CASE("Sound engine: stop_all stops every sound as a plain stop does", "[audio][engine]") {
    // Moho stops the Global category without IMMEDIATE (the score screen):
    // Shot fades over its cue's 300 ms, Click (no fade) ends at once.
    Sounds s;
    SoundManager sm(s.dir, false);
    sm.play("Test", "Shot");
    sm.update(0.2f); // Shot's wave starts at 150 ms
    sm.play("Test", "Click");
    sm.push_duck();
    CHECK(sm.active_count() == 2);
    sm.stop_all();
    CHECK(sm.active_count() == 1);
    CHECK(sm.global_variable("Duck") == 0.0f);
    sm.update(0.31f);
    CHECK(sm.active_count() == 0);
}

TEST_CASE("Sound engine: a paused category holds its subtree's sounds", "[audio][engine]") {
    Sounds s;
    SoundManager sm(s.dir, false);
    const auto click = sm.play("Test", "Click"); // Global, a 0.1 s wave
    const auto shot = sm.play("Test", "Shot");   // Music, under Global
    sm.update(0.05f);
    sm.pause_category("Music", true); // a child: Click plays on
    CHECK(sm.is_paused(shot));
    CHECK_FALSE(sm.is_paused(click));
    sm.update(0.06f);
    CHECK_FALSE(sm.is_playing(click));

    const auto held = sm.play("Test", "Click");
    sm.pause_category("Global", true); // the root: everything under it
    CHECK(sm.is_paused(held));
    sm.update(1.0f);
    CHECK(sm.is_playing(held));                 // its wave's time waits
    const auto late = sm.play("Test", "Click"); // started into the pause: waits
    CHECK(sm.is_paused(late));
    sm.update(1.0f);
    CHECK(sm.is_playing(late));

    sm.pause_category("Global", false);
    CHECK_FALSE(sm.is_paused(held));
    CHECK(sm.is_paused(shot)); // Music is still paused itself
    sm.update(0.05f);
    CHECK(sm.is_playing(held));
    sm.update(0.06f);
    CHECK_FALSE(sm.is_playing(held));
    CHECK_FALSE(sm.is_playing(late));
    sm.pause_category("Music", false);
    CHECK(sm.is_playing(shot));
    sm.pause_category("NoSuchCategory", true); // ignored
}

TEST_CASE("Sound engine: a paused sound stopped, or replaced, ends at once", "[audio][engine]") {
    // Its fade could never run while its clock is held.
    Sounds s;
    SoundManager sm(s.dir, false);
    const auto shot = sm.play("Test", "Shot"); // a 300 ms cue fade-out
    sm.update(0.2f);
    sm.pause_category("Music", true);
    sm.stop(shot, /*immediate=*/false);
    sm.update(0.0f);
    CHECK_FALSE(sm.is_playing(shot));

    const auto a = sm.play("Test", "Shot");
    sm.update(0.2f);
    const auto click = sm.play("Test", "Click");
    CHECK(sm.is_paused(a));
    sm.stop_all();
    sm.update(0.0f);
    CHECK_FALSE(sm.is_playing(a));
    CHECK_FALSE(sm.is_playing(click));
    CHECK(sm.active_count() == 0);

    // Music is still paused; a new game resumes everything.
    const auto b = sm.play("Test", "Shot");
    CHECK(sm.is_paused(b));
    sm.resume_all();
    CHECK_FALSE(sm.is_paused(b));
    sm.update(0.2f);
    CHECK(sm.is_playing(b));
}

TEST_CASE("Sound engine: the duck ramps over DuckLength, and a volume drops it",
          "[audio][engine]") {
    // Moho's PushDuck/PopDuck/UpdateDuck: 0 -> 1 over DuckLength (0.5 s)
    // for the first ducking voice, 1 -> 0 after the last; SetVolume resets.
    Sounds s;
    SoundManager sm(s.dir, false);
    const auto duck = [&] { return sm.global_variable("Duck"); };
    sm.push_duck();
    CHECK(duck() == 0.0f);
    sm.update(0.25f);
    CHECK(duck() == 0.5f);
    sm.push_duck(); // a second voice: no new ramp
    sm.update(0.5f);
    CHECK(duck() == 1.0f);
    sm.pop_duck(); // one still ducking
    sm.update(0.1f);
    CHECK(duck() == 1.0f);
    sm.pop_duck();
    sm.update(0.125f);
    CHECK(duck() == 0.75f);
    sm.update(1.0f);
    CHECK(duck() == 0.0f);

    sm.push_duck();
    sm.update(1.0f);
    const u32 generation = sm.duck_generation();
    sm.set_category_volume("Music", 0.5f);
    CHECK(duck() == 0.0f);
    CHECK(sm.duck_generation() != generation);
    sm.pop_duck(); // nothing left to pop
    sm.update(1.0f);
    CHECK(duck() == 0.0f);
}

TEST_CASE("Sound engine: only a replacement fades in", "[audio][engine]") {
    // XACT's category and cue fade-ins are the instance-limit crossfade:
    // retail's Ambient category (1 s) must not fade in every play.
    Sounds s;
    u32 rpc = 0;
    write(s.dir / "Game.xgs", make_xgs(&rpc, 1, 2, /*music_fade_in_ms=*/400));
    SoundManager sm(s.dir, false);
    const auto first = sm.play("Test", "Shot");
    sm.update(0.2f);
    const f32 full = sm.current_gain(first);
    REQUIRE(full > 0.0f);
    const auto second = sm.play("Test", "Shot"); // replaces first (Music holds one)
    sm.update(0.2f);                             // half of the 400 ms fade-in
    const f32 half = sm.current_gain(second);
    CHECK(half > 0.25f * full);
    CHECK(half < 0.75f * full);
    sm.update(0.3f);
    CHECK(sm.current_gain(second) == full);
}

TEST_CASE("Sound engine: a prepared sound waits, silent, until started", "[audio][engine]") {
    // Retail's movies prepare their sound (PlaySound(sound, true)) and
    // start it with the movie (StartSound).
    Sounds s;
    SoundManager sm(s.dir, false);
    int finished = 0;
    const auto h = sm.prepare("Test", "Click");
    REQUIRE(h != INVALID_SOUND);
    sm.on_finished(h, [&] { ++finished; });
    CHECK(sm.is_prepared(h));
    CHECK_FALSE(sm.is_playing(h));
    CHECK_FALSE(sm.is_cue_playing("Test", "Click"));
    sm.update(1.0f); // time passes; it waits
    CHECK(sm.is_prepared(h));
    CHECK(finished == 0);

    sm.start(h);
    CHECK(sm.is_playing(h));
    CHECK_FALSE(sm.is_prepared(h));
    CHECK(sm.is_cue_playing("Test", "Click"));
    sm.update(0.05f);
    CHECK(sm.is_playing(h));
    sm.update(0.06f); // 0.11 s from its start: past the 0.1 s wave
    CHECK_FALSE(sm.is_playing(h));
    CHECK(finished == 1);
    sm.start(h); // over: nothing to start
    CHECK_FALSE(sm.is_playing(h));

    // A stop ends a prepared sound at once, though the cue fades (300 ms).
    const auto shot = sm.prepare("Test", "Shot");
    REQUIRE(shot != INVALID_SOUND);
    sm.stop(shot, /*immediate=*/false);
    CHECK_FALSE(sm.is_prepared(shot));
    CHECK_FALSE(sm.is_playing(shot));
    sm.update(0.0f);
    CHECK(sm.active_count() == 0);
}

TEST_CASE("Sound engine: a voice language loads sounds/Voice/<la> and its tutorials",
          "[audio][engine]") {
    // FA's VO banks (EVA's XGG, the campaign's, the movies' X_FMV) sit in
    // sounds/Voice/US, Windows-cased; Moho loads them on AudioSetLanguage.
    Sounds s;
    u32 rpc = 0;
    (void)make_xgs(&rpc);
    const fs::path voice = s.dir / "Voice" / "US";
    fs::create_directories(voice / "Tutorials");
    write(voice / "VO.xsb", make_xsb("VOWaves", rpc));
    write(voice / "VOWaves.xwb", make_xwb("VOWaves", 4, 2205));
    write(voice / "Tutorials" / "Tut.xsb", make_xsb("VOWaves", rpc));
    SoundManager sm(s.dir, /*output=*/false);
    REQUIRE(sm.has_data());
    CHECK(sm.play("VO", "Click") == INVALID_SOUND); // not loaded before
    CHECK(sm.has_voice_language("us"));
    CHECK(sm.has_voice_language("US"));
    CHECK_FALSE(sm.has_voice_language("de"));
    CHECK_FALSE(sm.set_voice_language("de"));
    REQUIRE(sm.set_voice_language("us"));
    CHECK(sm.set_voice_language("US")); // the same, again
    CHECK(sm.play("VO", "Click") != INVALID_SOUND);
    CHECK(sm.play("Tut", "Click") != INVALID_SOUND);
    CHECK(sm.play("Test", "Click") != INVALID_SOUND); // the game's banks still there
}

TEST_CASE("Sound engine: a loop that picks a new wave each time keeps playing", "[audio][engine]") {
    // FA's Music/Base_Building and Battle: loop forever, a new wave per loop.
    Sounds s(1, 2, 0, 0, /*new_variation_on_loop=*/true);
    SoundManager sm(s.dir, /*output=*/false);
    const auto h = sm.play("Test", "Shot");
    REQUIRE(h != INVALID_SOUND);
    for (int i = 0; i < 40; ++i) sm.update(0.05f); // 2 s: some 18 of its 0.1 s waves
    CHECK(sm.is_playing(h));
    CHECK(sm.active_count() == 1);
    sm.stop(h, /*immediate=*/true);
    sm.update(0.0f);
    CHECK_FALSE(sm.is_playing(h));

    // Stopped with its 300 ms fade, it plays on across wave ends to the
    // fade's end, not the current wave's.
    const auto faded = sm.play("Test", "Shot");
    REQUIRE(faded != INVALID_SOUND);
    for (int i = 0; i < 10; ++i) sm.update(0.05f);
    sm.stop(faded, /*immediate=*/false);
    for (int i = 0; i < 5; ++i) sm.update(0.05f); // 0.25 s: two or more waves end
    CHECK(sm.is_playing(faded));
    sm.update(0.1f);
    CHECK_FALSE(sm.is_playing(faded));
}

TEST_CASE("Sound engine: a long wave streams and plays for its length", "[audio][engine]") {
    // Retail's music and movie voices run 34-43 MB; from 2 MB a PCM wave
    // streams from its bank rather than being read whole when it starts.
    Sounds s;
    write(s.dir / "TestWaves.xwb", make_xwb("TestWaves", 4, 1100000)); // 2.2 MB, ~49.9 s
    SoundManager sm(s.dir, false);
    const auto h = sm.play("Test", "Click");
    REQUIRE(h != INVALID_SOUND);
    sm.update(49.8f);
    CHECK(sm.is_playing(h));
    sm.update(0.2f);
    CHECK_FALSE(sm.is_playing(h));
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
    Sounds s(/*music_limit=*/2, /*music_behavior=*/4 /* ReplaceLowestPriority */,
             /*click_category=*/1,
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
