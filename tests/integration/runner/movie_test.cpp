#include "movie_test.hpp"

#include "app/app.hpp"
#include "audio/sound_manager.hpp"
#include "core/game_state.hpp"
#include "core/image.hpp"
#include "core/test_status.hpp"
#include "lua/beat_system.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "render_probe.hpp"
#include "renderer/renderer.hpp"
#include "sim/thread_manager.hpp"
#include "ui/ui_control.hpp"
#include "ui/ui_dispatch.hpp"
#include "video/movie_player.hpp"

#include <GLFW/glfw3.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <cmath>
#include <string>

namespace osc::test {

namespace {

constexpr f64 kFrame = 1.0 / 30.0;

/// The front end's UI state, pumped as the window loop pumps it.
struct MovieTest {
    app::Engine& e;
    lua_State* L;
    ui::UIDispatch dispatch;
    Tally t;

    explicit MovieTest(app::Engine& engine) : e(engine), L(engine.ui_lua_state.raw()) {}

    bool run(const std::string& code) {
        auto r = e.ui_lua_state.do_string(code);
        if (!r) osc::test_status::fail("[FAIL] movie-test Lua: {}", r.error().message);
        return static_cast<bool>(r);
    }
    f64 num(const std::string& expr) {
        if (!run("__movie_value = " + expr)) return -1;
        lua_getglobal(L, "__movie_value");
        const f64 v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
        lua_pop(L, 1);
        return v;
    }
    bool truth(const std::string& expr) {
        if (!run("__movie_value = " + expr)) return false;
        lua_getglobal(L, "__movie_value");
        const bool v = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return v;
    }
    /// The control behind a Lua expression.
    ui::UIControl* control(const std::string& expr) {
        if (!run("__movie_value = " + expr)) return nullptr;
        lua_getglobal(L, "__movie_value");
        ui::UIControl* c = nullptr;
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "_c_object");
            lua_rawget(L, -2);
            c = static_cast<ui::UIControl*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        return c;
    }
    /// The live Movie control playing `file`, if any.
    ui::UIControl* movie_of(const std::string& file) {
        for (const auto& c : e.ui_registry.all())
            if (c && !c->destroyed() && c->control_type() == ui::UIControl::ControlType::Movie &&
                c->movie_filename() == file)
                return c.get();
        return nullptr;
    }
    /// A number field of a control's Lua table (a movie's soundHandle).
    f64 field(const ui::UIControl* c, const char* name) {
        if (!c || c->lua_table_ref() < 0) return -1;
        lua_rawgeti(L, LUA_REGISTRYINDEX, c->lua_table_ref());
        lua_pushstring(L, name);
        lua_rawget(L, -2);
        const f64 v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -1;
        lua_pop(L, 2);
        return v;
    }
    /// UI frames as the window loop runs them: threads, frame updates,
    /// events, beats, sounds.
    void pump(int frames, f64 dt = kFrame) {
        for (int i = 0; i < frames; ++i) {
            ++e.ui_frame_count;
            osc::lua::advance_ui_clock(L, dt);
            e.ui_thread_manager.resume_all(e.ui_frame_count);
            dispatch.update_controls(L, e.ui_registry, dt);
            dispatch.dispatch_events(L, e.ui_registry);
            osc::core::call_on_beat(L, dt);
            e.beat_registry.fire_all(L);
            e.sound.update(static_cast<f32>(dt));
        }
    }
    void press(int key) {
        dispatch.on_key(key, GLFW_PRESS, 0);
        dispatch.on_key(key, GLFW_RELEASE, 0);
    }
};

/// The frame due on a movie's clock.
i32 due(const video::MoviePlayer& m) {
    return static_cast<i32>(std::floor(m.clock() * m.frame_rate()));
}

// FA's splash, from the front end's start: the logos play with their
// sounds, one after another; Escape skips to the intro, then leaves to the
// main menu.
void splash(MovieTest& m) {
    Tally& t = m.t;
    auto* thq = m.movie_of("/movies/thqlogo.sfd");
    t.check(thq != nullptr && m.truth("AnyInputCapture()"),
            "Test 1: the front end starts with the splash's THQ logo, capturing the input");
    if (!thq) return;
    const auto thq_sound = static_cast<audio::SoundHandle>(m.field(thq, "soundHandle"));
    t.check(!thq->movie_playing() && m.e.sound.is_prepared(thq_sound) &&
                !m.e.sound.is_playing(thq_sound),
            fmt::format("Test 2: before it loads, the logo waits and its sound is prepared, silent "
                        "(playing {}, prepared {})",
                        thq->movie_playing(), m.e.sound.is_prepared(thq_sound)));

    m.pump(2);
    const auto* player = thq->movie_player();
    t.check(thq->movie_playing() && m.e.sound.is_playing(thq_sound) && player &&
                player->clock() > 0 && player->frame_shown() == due(*player),
            fmt::format("Test 3: once loaded it plays, with its sound, on its clock (frame {} at "
                        "{:.3f} s)",
                        player ? player->frame_shown() : -1, player ? player->clock() : -1.0));

    // It ends after its 201 frames (6.7 s), and the GPG logo follows.
    int frames = 2;
    while (!m.movie_of("/movies/gpglogo.sfd") && frames < 400) {
        m.pump(1);
        ++frames;
    }
    t.check(frames >= 201 && frames <= 203 && !m.e.sound.is_playing(thq_sound),
            fmt::format("Test 4: the THQ logo ends after 6.7 s (frame {}), its sound stops, and "
                        "the GPG logo follows",
                        frames));

    // Escape skips to the intro, then leaves.
    m.pump(2);
    m.press(GLFW_KEY_ESCAPE);
    m.pump(1);
    auto* intro = m.movie_of("/movies/fmv_scx_intro.sfd");
    t.check(intro != nullptr && intro->movie_player() &&
                intro->movie_player()->frame_count() == 6211,
            "Test 5: Escape skips to FA's intro (6211 frames)");
    m.pump(3);
    t.check(intro && intro->movie_playing() && m.e.sound.is_cue_playing("FMV_BG", "X_FMV_Intro"),
            "Test 6: the intro plays, with its music");
    m.press(GLFW_KEY_ESCAPE);
    m.pump(2);
    auto* back = m.movie_of("/movies/main_menu.sfd");
    t.check(!m.truth("AnyInputCapture()") && intro && intro->destroyed() &&
                !m.e.sound.is_cue_playing("FMV_BG", "X_FMV_Intro") && back &&
                back->movie_playing() && back->movie_looping(),
            "Test 7: Escape again leaves for the main menu (EngineStartFrontEndUI): the capture "
            "and the intro are gone, the menu's movie loops behind it");

    // EngineStartFrontEndUI lets go of any capture, and starts the menu anew.
    m.run("AddInputCapture(GetFrame(0)) EngineStartFrontEndUI()");
    auto* again = m.movie_of("/movies/main_menu.sfd");
    t.check(!m.truth("AnyInputCapture()") && back && back->destroyed() && again && again != back &&
                again->movie_playing(),
            "Test 8: EngineStartFrontEndUI lets go of the input capture and builds the menu anew");
}

// Movies on their clock, as Moho's CMauiMovie::Frame runs them.
void playback(MovieTest& m) {
    Tally& t = m.t;
    // Nothing else plays over these checks.
    m.run("moho.control_methods.Destroy(GetFrame(0))");
    m.run(R"(
        local Movie = import('/lua/maui/movie.lua').Movie
        mv = Movie(GetFrame(0))
        mv_loaded = mv:InternalSet('/movies/gpglogo.sfd')
        mv_frames, mv_dt, mv_stopped, mv_finished = 0, 0, 0, 0
        mv.OnFrame = function(self, dt) mv_frames = mv_frames + 1 mv_dt = mv_dt + dt end
        mv.OnStopped = function(self) mv_stopped = mv_stopped + 1 end
        mv.OnFinished = function(self) mv_finished = mv_finished + 1 end
    )");
    auto* c = m.control("mv");
    const video::MoviePlayer* p = c ? c->movie_player() : nullptr;
    t.check(m.truth("mv_loaded and mv:IsLoaded()") && m.num("mv:GetNumFrames()") == 167 &&
                m.num("mv:GetFrameRate()") == 30 && m.num("mv.MovieWidth()") == 1024 &&
                m.num("mv.MovieHeight()") == 576 && p && p->frame_shown() == 0 &&
                !c->movie_playing(),
            fmt::format("Test 9: a movie opens paused on its first frame, with the header's 167 "
                        "frames at 30 and its size (frames {}, rate {}, {}x{})",
                        m.num("mv:GetNumFrames()"), m.num("mv:GetFrameRate()"),
                        m.num("mv.MovieWidth()"), m.num("mv.MovieHeight()")));
    if (!p) return;

    // 1/16 s frames: the clock lands on exact frame times.
    constexpr f64 dt = 0.0625;
    m.run("mv:Play()");
    m.pump(8, dt);
    t.check(m.num("mv_frames") == 8 && m.num("mv_dt") == 0.5 && p->clock() == 0.5 &&
                p->frame_shown() == 15,
            fmt::format("Test 10: playing, OnFrame runs each frame with its step, and frame 15 "
                        "shows at 0.5 s (OnFrame {}, frame {})",
                        m.num("mv_frames"), p->frame_shown()));

    m.run("mv:Stop()");
    m.pump(1, dt);
    const bool stopped_once = m.num("mv_stopped") == 1 && !c->needs_frame_update();
    m.pump(4, dt);
    t.check(stopped_once && m.num("mv_stopped") == 1 && m.num("mv_frames") == 9 &&
                p->clock() == 0.5 && p->frame_shown() == 15,
            "Test 11: Stop pauses it: OnStopped once, and no more frames");
    m.run("mv:Play()");
    m.pump(2, dt);
    t.check(p->clock() == 0.625 && p->frame_shown() == 18,
            fmt::format("Test 12: Play resumes it where it stopped (frame {})", p->frame_shown()));

    // It finishes once its clock passes its last frame: 167 / 30 = 5.567 s.
    f64 finished_at = -1;
    for (int i = 0; i < 200 && finished_at < 0; ++i) {
        m.pump(1, dt);
        if (m.num("mv_finished") > 0) finished_at = p->clock();
    }
    const i32 last = p->frame_shown();
    m.pump(10, dt);
    t.check(finished_at == 5.625 && last == 166 && m.num("mv_finished") == 1 &&
                !c->movie_playing() && !c->needs_frame_update(),
            fmt::format("Test 13: it finishes on the first frame past its end (at {:.4f} s, last "
                        "frame {}), and OnFinished runs once",
                        finished_at, last));

    m.run("mv:Loop(true) mv:Play()");
    m.pump(1, dt);
    const i32 restarted = p->frame_shown();
    m.pump(8, dt);
    t.check(restarted == 0 && p->frame_shown() == 15 && m.num("mv_finished") == 1 &&
                c->movie_playing(),
            fmt::format("Test 14: looping, it starts over at its end instead (frame {}, then {})",
                        restarted, p->frame_shown()));

    // Off screen (the UI renderer didn't draw it: retail's loading movie
    // plays on under the world view all game), a movie runs its clock and
    // decodes nothing; back on screen it catches up to the frame due, at
    // most kMaxDecodes a frame.
    {
        c->set_movie_on_screen(false);
        const i32 before = p->frame_shown();
        const u64 serial = p->frame_serial();
        m.pump(8, dt); // 0.5 s on: 15 frames due
        const bool held = p->frame_shown() == before && p->frame_serial() == serial;
        c->set_movie_on_screen(true);
        m.pump(1, dt);
        const i32 first = p->frame_shown();
        m.pump(3, dt);
        t.check(held && first == before + video::MoviePlayer::kMaxDecodes &&
                    p->frame_shown() == due(*p),
                fmt::format("Test 24: off screen it decodes nothing (frame {} held); back on, it "
                            "catches up (frame {}, then {} of {} due)",
                            before, first, p->frame_shown(), due(*p)));
    }

    // A new movie on a playing control waits, paused, for Play (Moho's
    // LoadFile leaves the control playing; CMovie opens paused).
    m.run("mv:InternalSet('/movies/thqlogo.sfd')");
    const video::MoviePlayer* thq = c->movie_player();
    m.pump(5, dt);
    const bool waited = thq && thq->frame_count() == 201 && c->movie_playing() &&
                        thq->clock() == 0 && thq->frame_shown() == 0 && m.num("mv_finished") == 1;
    m.run("mv:Play()");
    m.pump(8, dt);
    t.check(waited && thq->frame_shown() == 15,
            fmt::format("Test 15: a new movie on a playing control waits for Play, then plays "
                        "(frame {})",
                        thq ? thq->frame_shown() : -1));

    // Callbacks are found through the class, as Moho's RunScript finds them.
    m.run(R"(
        local Movie = import('/lua/maui/movie.lua').Movie
        sub_frames = 0
        local Sub = Class(Movie) { OnFrame = function(self, dt) sub_frames = sub_frames + 1 end }
        sub = Sub(GetFrame(0))
        sub:InternalSet('/movies/gpglogo.sfd')
        sub:Play()
    )");
    m.pump(3);
    t.check(m.num("sub_frames") == 3 && m.truth("rawget(sub, 'OnFrame') == nil"),
            "Test 16: a movie's OnFrame defined on its class runs each frame");
    m.run("sub:Destroy()");

    m.run(R"(
        local Movie = import('/lua/maui/movie.lua').Movie
        none = Movie(GetFrame(0))
        none_loaded = none:InternalSet('/movies/no_such_movie.sfd')
        none_finished = 0
        none.OnFinished = function(self) none_finished = none_finished + 1 end
        none:Play()
    )");
    auto* none = m.control("none");
    m.pump(3);
    t.check(m.truth("none_loaded == false and not none:IsLoaded()") &&
                m.num("none:GetNumFrames()") == 0 && m.num("none_finished") == 1 && none &&
                !none->needs_frame_update(),
            "Test 17: a movie that won't open finishes on its first frame when played");
    m.run("mv:Destroy() none:Destroy()");
}

// The sounds a movie starts with: prepared, then started.
void sounds(MovieTest& m) {
    Tally& t = m.t;
    auto& snd = m.e.sound;
    m.run(R"(
        prepared = PlaySound({ Bank = 'FMV_BG', Cue = 'THQ_Logo' }, true)
        voice = PlayVoice({ Bank = 'FMV_BG', Cue = 'GPG_introLogo_HD' }, false, true)
        at_once = PlaySound({ Bank = 'FMV_BG', Cue = 'NVIDIA' })
    )");
    const auto prepared = static_cast<audio::SoundHandle>(m.num("prepared"));
    const auto voice = static_cast<audio::SoundHandle>(m.num("voice"));
    const auto at_once = static_cast<audio::SoundHandle>(m.num("at_once"));
    m.pump(10);
    t.check(snd.is_prepared(prepared) && !snd.is_playing(prepared) && snd.is_prepared(voice) &&
                snd.is_playing(at_once) && m.truth("SoundIsPrepared(prepared)") &&
                m.truth("SoundIsPrepared(nil)"),
            "Test 18: PlaySound and PlayVoice with prepareOnly prepare their cue, silent, until "
            "started; without it they play at once");
    m.run("StartSound(prepared) StopSound(voice)");
    m.pump(1);
    t.check(snd.is_playing(prepared) && !snd.is_prepared(prepared) && !snd.is_prepared(voice) &&
                !snd.is_playing(voice),
            "Test 19: StartSound starts a prepared sound; a stop ends one at once");
    m.run("StopSound(prepared, true) StopSound(at_once, true)");
    m.pump(1);
}

// A playing movie is drawn over its control, in its alpha; one not
// playing is not.
void drawing(MovieTest& m) {
    Tally& t = m.t;
    renderer::Renderer r;
    if (!r.init(320, 180, "Movie Test", /*offscreen=*/true)) {
        osc::test_status::fail("[FAIL] movie-test: no Vulkan device for the drawing checks");
        return;
    }
    r.init_ui_caches(&m.e.vfs);
    r.set_fixed_frame_dt(static_cast<f32>(kFrame));
    // `before` frames, then one captured (and one more, which delivers it).
    const auto frame = [&](int before) {
        for (int i = 0; i < before; ++i) {
            r.render_ui_only(m.L, &m.e.ui_registry);
            r.poll_events(kFrame);
        }
        ImageRGBA8 shot;
        r.request_capture([&](ImageRGBA8 image) { shot = std::move(image); });
        r.render_ui_only(m.L, &m.e.ui_registry);
        r.render_ui_only(m.L, &m.e.ui_registry);
        return centre_pixels(shot);
    };
    const auto mean = [](const Pixels& px) {
        f64 sum = 0;
        for (const auto& p : px) sum += (p[0] + p[1] + p[2]) / 3.0;
        return px.empty() ? -1.0 : sum / static_cast<f64>(px.size());
    };

    // A movie not played is not drawn, though its first frame (the menu's:
    // not black) is loaded and uploaded.
    m.run(R"(
        moho.control_methods.Destroy(GetFrame(0))
        local Movie = import('/lua/maui/movie.lua').Movie
        still = Movie(GetFrame(0))
        still:InternalSet('/movies/main_menu.sfd')
        still.Left:Set(0) still.Top:Set(0) still.Width:Set(320) still.Height:Set(180)
    )");
    const f64 unplayed = mean(frame(3));
    m.run(R"(
        still:Destroy()
        local Movie = import('/lua/maui/movie.lua').Movie
        shown = Movie(GetFrame(0))
        shown:InternalSet('/movies/gpglogo.sfd')
        shown.Left:Set(0) shown.Top:Set(0) shown.Width:Set(320) shown.Height:Set(180)
    )");
    auto* c = m.control("shown");
    m.run("shown:Play()");
    frame(28); // 1 s in
    m.run("shown:Stop()");
    const Pixels drawn = frame(2);
    // The frame it shows, sampled where the capture's middle falls.
    Pixels expected;
    if (const auto* p = c ? c->movie_player() : nullptr) {
        const u32 w = 320, h = 180;
        for (u32 y = h * 35 / 100; y < h * 65 / 100; ++y)
            for (u32 x = w * 35 / 100; x < w * 65 / 100; ++x) {
                const u32 sx = std::min(p->width() - 1, (x * p->width() + p->width() / 2) / w);
                const u32 sy = std::min(p->height() - 1, (y * p->height() + p->height() / 2) / h);
                const u8* px = p->rgba() + (static_cast<size_t>(sy) * p->width() + sx) * 4;
                expected.push_back({px[0] / 255.0f, px[1] / 255.0f, px[2] / 255.0f});
            }
    }
    const f32 diff = drawn.size() == expected.size() ? mean_abs_diff(drawn, expected) : 1.0f;
    t.check(unplayed >= 0 && unplayed < 0.01 && mean(drawn) > 0.2 && diff < 0.03,
            fmt::format("Test 20: a playing movie draws its current frame over its control, and "
                        "one not played draws nothing (mean {:.3f}, {:.4f} from the frame; {:.3f} "
                        "for the one not played)",
                        mean(drawn), diff, unplayed));
    m.run("shown:SetAlpha(0.5)");
    const f64 half = mean(frame(1));
    t.check(std::abs(half / mean(drawn) - 0.5) < 0.03,
            fmt::format("Test 21: in its alpha ({:.3f} of it at 0.5)", half / mean(drawn)));
    m.run("shown:Destroy()");
    r.shutdown();
}

// GetMovieDuration (Moho's MOV_GetDuration): a movie's length from its
// Sofdec header; 0 for a file that is missing or not a movie. Scenario
// scripts fall back to AllyCom.sfd on 0.
void durations(MovieTest& m) {
    Tally& t = m.t;
    const f64 gpg = m.num("GetMovieDuration('/movies/gpglogo.sfd')");
    const f64 ally = m.num("GetMovieDuration('/movies/AllyCom.sfd')");
    t.check(std::abs(gpg - 167.0 / 30.0) < 1e-4 && std::abs(ally - 150.0 / 29.97) < 1e-4,
            fmt::format("Test 22: GetMovieDuration reads a movie's frames and rate from its "
                        "header (gpglogo {:.4f} s, AllyCom {:.4f} s)",
                        gpg, ally));
    const f64 missing = m.num("GetMovieDuration('/movies/no_such_movie.sfd')");
    const f64 not_movie = m.num("GetMovieDuration('/lua/maui/movie.lua')");
    const bool arity = m.truth("not pcall(GetMovieDuration)");
    t.check(missing == 0 && not_movie == 0 && arity,
            fmt::format("Test 23: a missing file or one not a movie is 0 long ({} and {}), and "
                        "the file must be given (error {})",
                        missing, not_movie, arity));
}

} // namespace

void run_movie_test(app::Engine& e) {
    spdlog::info("=== Movie test (M216a) ===");
    MovieTest m(e);
    splash(m);
    playback(m);
    sounds(m);
    drawing(m);
    durations(m);
    spdlog::info("Movie test: {}/{} passed", m.t.pass, m.t.pass + m.t.fail);
}

} // namespace osc::test
