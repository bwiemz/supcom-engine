// The windowed loop's frame, in its phases (M192 step 2c): App::Window.

#include "app/window_loop.hpp"
#include "lua/gpgnet_session.hpp"
#include "app/window_commands.hpp"
#include "app/world_sounds.hpp"
#include "core/fixed_step.hpp"
#include "core/game_state.hpp"
#include "core/fullscreen.hpp"
#include "core/profiler.hpp"
#include "core/test_status.hpp"
#include "lua/lua_thread_check.hpp"
#include "lua/match_records.hpp"
#include "lua/net_lobby.hpp"
#include "lua/session_clients.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/sim_sync.hpp"
#include "lua/user_bindings.hpp"
#include "renderer/frustum.hpp"
#include "sim/lockstep_session.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

namespace osc::app {

bool App::Window::frame() {
    const auto frame_started = std::chrono::high_resolution_clock::now();
    const double dt = begin_frame();

    // A launched game loading: the loading screen's frame alone, the world
    // loading on the loader's thread meanwhile (as Moho's frames go on
    // through WLD_DoLoading)
    if (loading) {
        loading_frame(dt, frame_started);
        osc::Profiler::instance().end_frame();
        return true;
    }

    // The ticks the last frame started on the sim thread still running (a
    // game sped up past what its sim keeps up with): this frame draws and
    // doesn't wait for them, so the game slows and the frame rate holds.
    // (A --fast-match wants its sim's speed alone, and waits.)
    const bool threaded = sim_threaded();
    if (threaded && sim_thread->busy() && !sim_thread->done() && !fast_match_in_game()) {
        draw_while_ticking(dt, frame_started);
        return true;
    }

    // The ticks the last frame started on the sim thread, done (OE-134):
    // from here until this frame starts its own, nothing else runs the sim
    collect_sim();
    world_interp.staged = threaded;
    sim_held = sim_state && (sim_state->paused() || sim_state->game_over());
    if (tracking_notice && sim_state) {
        osc::core::call_on_track_unit(ui_lua_state.raw(), *tracking_notice);
        tracking_notice.reset();
    }
    if (opt.play_for > 0 && game_state_mgr.current() == osc::GameState::GAME) {
        // --play-rate: a hosted game's speed is its lockstep's
        if (auto* session = osc::lua::mp_net_state().session.get()) {
            if (session->speed() != opt.play_rate) session->set_speed_option(opt.play_rate, true);
        } else if (game_state_mgr.sim_rate() != opt.play_rate) {
            game_state_mgr.set_sim_rate(opt.play_rate);
        }
    }

    update_audio(dt);

    count_fps(dt);
    count_game_frame();

    // Pause, game speed and Escape come from retail's key map
    // (Pause, NumPlus/NumMinus/NumStar, Esc): see UIDispatch.

    note_game_over_if_ended(sim_state.get(), game_state_mgr, ui_lua_state.raw());

    // Fixed-timestep sim ticking (scaled by sim_speed), here, or on the sim
    // thread as the frame draws (start_sim)
    if (!threaded) {
        PROFILE_ZONE("Frame::sim");
        const osc::u32 beat_tick0 = sim_state ? sim_state->tick_count() : 0;
        const auto ticks_started = std::chrono::steady_clock::now();
        advance_sim(session_dt);
        collected_tick = sim_state ? sim_state->tick_count() : 0;
        if (collected_tick > beat_tick0)
            time_ticks(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                 ticks_started)
                           .count(),
                       collected_tick - beat_tick0);
    }
    app.deliver_network_save_notices(); // a network save the ticks wrote
    write_due_saves();

    app.begin_match_record();
    if (app.telemetry && sim_state) {
        PROFILE_ZONE("Frame::telemetry");
        app.telemetry->after_tick(*sim_state);
    }

    {
        PROFILE_ZONE("Frame::beats");
        run_beats();
    }

    finish_world_ui_after_post_load();

    // Process SimCallbacks from UI (M138a)
    if (sim_state && sim_lua_state) submit_sim_callbacks(sim_callback_queue, *sim_state);

    run_flows();

    {
        PROFILE_ZONE("Frame::ui");
        update_ui(dt);
    }

    if (sim_state) {
        world_interp.history.refresh_pending(*sim_state);
    }
    // The drawn time moves on by this frame (before the view it draws:
    // moved on as its ticks start, after, the frame that collects a job
    // drew where the last one did)
    if (threaded && sim_state && !game_state_mgr.paused() && !game_state_mgr.sim_stopped())
        if (!sim_held) world_interp.clock.advance(drawn_dt() * game_state_mgr.speed());
    // This frame's world, between two published ticks: what is drawn and
    // what clicks pick.
    const osc::sim::FrameView frame_view = world_interp.view();
    {
        PROFILE_ZONE("Frame::input");
        update_input(dt, frame_view);
    }
    update_title(dt);
    {
        PROFILE_ZONE("Frame::render");
        render(frame_view, [&](bool drawn) {
            if (!threaded) return;
            // The UI's frame callbacks and events run their scripts, which
            // may read the live sim: here, before its tick starts, not in
            // the draw
            if (drawn)
                renderer.run_ui_frame(ui_lua_state.raw(), &ui_registry, sim_state != nullptr);
            // Every Lua-backed renderer preparation finishes before the next job.
        });
    }

    if (threaded) start_sim(session_dt);
    pace_play_for(frame_started);

    if (exit_requested()) {
        quiesce_sim();
        return false;
    }
    handle_launch();
    handle_return_to_lobby();

    osc::Profiler::instance().end_frame();
    return true;
}

void App::Window::draw_while_ticking(double dt,
                                     std::chrono::high_resolution_clock::time_point started) {
    count_fps(dt);
    count_game_frame();
    // The window's events (its callbacks only queue the UI's, for the next
    // full frame) and the camera's keys and mouse
    renderer.poll_events(dt);
    // The world moves on between the ticks it has (held at the newest), and
    // the game's time owed to the sim: the next ticks started make it up, as
    // many as consume_fixed_steps lets a frame start (the rest is let go: the
    // game slows to what its sim keeps up with)
    if (!sim_held) world_interp.clock.advance(drawn_dt() * game_state_mgr.speed());
    // (at most a frame's worth of ticks: a network game's rounds take what
    // is owed without a limit of their own)
    if (!game_state_mgr.paused() && !game_state_mgr.sim_stopped())
        sim_accumulator = std::min(sim_accumulator + (sim_held ? dt : dt * game_state_mgr.speed()),
                                   8 * osc::sim::SimState::SECONDS_PER_TICK);
    {
        PROFILE_ZONE("Frame::render");
        render(world_interp.view(), [this](bool) { renderer.skip_ui_frame(); });
    }
    pace_play_for(started);
    osc::Profiler::instance().end_frame();
}

void App::Window::count_game_frame() {
    if (!sim_state || game_state_mgr.current() != osc::GameState::GAME) return;
    // (the live sim's tick count is the sim thread's while it ticks: the
    // collected one is the frame's)
    if (settled_tick() == 0) return;
    const double ms = wall_dt * 1000.0;
    ++game_frames.frames;
    game_frames.total_ms += ms;
    game_frames.worst_ms = std::max(game_frames.worst_ms, ms);
    game_frames.over_20 += ms > 20.0 ? 1 : 0;
    game_frames.over_33 += ms > 33.0 ? 1 : 0;
    game_frames.over_50 += ms > 50.0 ? 1 : 0;
    game_frames.in_game_s += wall_dt;
    perf_stretch.frame_ms.push_back(static_cast<float>(ms));
    perf_stretch.over_33 += ms > 33.0 ? 1 : 0;
    perf_stretch.wall_s += wall_dt;
    write_perf_sample();
}

bool App::Window::at_save_tick() const {
    return next_save < opt.save_ticks.size() &&
           sim_state->tick_count() >= opt.save_ticks[next_save];
}

void App::Window::write_due_saves() {
    if (opt.save_path.empty() || !sim_state) return;
    while (next_save < opt.save_ticks.size() &&
           sim_state->tick_count() >= opt.save_ticks[next_save]) {
        const osc::u32 tick = opt.save_ticks[next_save++];
        if (sim_state->tick_count() != tick) continue; // (passed: a load began later)
        std::string path = opt.save_path;
        if (const auto at = path.find("{tick}"); at != std::string::npos)
            path.replace(at, 6, std::to_string(tick));
        const auto start = std::chrono::steady_clock::now();
        auto save = osc::sim::save_game(*sim_state, fmt::format("tick {}", tick));
        if (app.special_files) (void)app.special_files->sign_snapshot(save);
        if (osc::lua::write_saved_game(save, path))
            spdlog::info(
                "Saved game: {} at tick {}, written in {:.0f} ms", path, tick,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
        else osc::test_status::fail("[FAIL] saved game: cannot write {}", path);
    }
}

void App::Window::time_ticks(double ms, u32 ticks) {
    if (ticks == 0) return;
    // --bench: the ticks' time, and the units they ran (a cost per unit's
    // divisor); read here, the sim not running
    if (app.bench && sim_state) {
        if (app.bench->tick_ms().empty())
            app.bench->set_scenario(opt.bench_scenario, sim_state->tick_count() - ticks);
        app.bench->record_ms(ms, ticks);
        size_t units = 0;
        sim_state->entity_registry().for_each_unit([&](const osc::sim::Entity& e) {
            if (!e.destroyed()) ++units;
        });
        app.bench->note_units(units);
    }
    perf_stretch.tick_ms += ms;
    perf_stretch.ticks_timed += ticks;
    perf_stretch.tick_ms_worst = std::max(perf_stretch.tick_ms_worst, ms / ticks);
    job_ms = 0;
    job_ticks = 0;
}

void App::Window::write_perf_sample() {
    if (!app.telemetry) {
        osc::lua::pending_marks().clear();
        return;
    }
    const osc::u32 tick = settled_tick();
    for (const std::string& note : osc::lua::pending_marks()) app.telemetry->write_mark(tick, note);
    osc::lua::pending_marks().clear();
    PerfStretch& s = perf_stretch;
    if (s.wall_s < 10.0 || s.frame_ms.empty()) return;
    osc::app::PerfSample p;
    p.wall_seconds = s.wall_s;
    p.tick = tick;
    // Count the work timed in this interval, including a game loaded at a later tick.
    p.ticks = s.ticks_timed;
    p.speed = game_state_mgr.speed();
    p.frames = static_cast<u32>(s.frame_ms.size());
    double total = 0;
    for (const float ms : s.frame_ms) total += ms;
    p.frame_ms_mean = total / p.frames;
    std::sort(s.frame_ms.begin(), s.frame_ms.end());
    p.frame_ms_p95 = s.frame_ms[std::min(s.frame_ms.size() - 1, s.frame_ms.size() * 95 / 100)];
    p.frame_ms_worst = s.frame_ms.back();
    p.frames_over_33 = s.over_33;
    p.held = s.held;
    p.pace_change = s.paces > 0 ? s.pace_change / s.paces : 0.0;
    p.tick_ms_mean = s.ticks_timed > 0 ? s.tick_ms / s.ticks_timed : 0.0;
    p.tick_ms_worst = s.tick_ms_worst;
    // From the published world: the live one may be ticking on its thread
    const auto& world = world_interp.history.cur();
    p.entities = static_cast<u32>(world.entities.size());
    for (const auto& e : world.entities) p.units += e.is_unit ? 1 : 0;
    app.telemetry->write_perf(p);
    s.clear();
}

void App::Window::note_drawn_time(const sim::FrameView& view) {
    // How evenly the drawn game time runs. A frame it stands still in while
    // the game runs is a held frame (units stop); and its pace, the step
    // over the frame's time at the game's speed, changing from one frame
    // to the next is units lurching (a steady pace a little off the
    // game's speed is not seen).
    const bool running = sim_state && game_state_mgr.current() == osc::GameState::GAME &&
                         !game_state_mgr.paused() && !game_state_mgr.sim_stopped() &&
                         view.cur() != nullptr && settled_tick() > 0;
    const double drawn = view.time();
    if (running && game_frames.last_drawn >= 0.0) {
        const double step = drawn - game_frames.last_drawn;
        const double expected =
            wall_dt * game_state_mgr.speed() / osc::sim::SimState::SECONDS_PER_TICK;
        game_frames.held += step <= 0.0 ? 1 : 0;
        perf_stretch.held += step <= 0.0 ? 1 : 0;
        if (expected > 0.0) {
            const double pace = step / expected;
            if (game_frames.last_pace >= 0.0) {
                game_frames.uneven += std::abs(pace - game_frames.last_pace);
                game_frames.expected += 1.0;
                perf_stretch.pace_change += std::abs(pace - game_frames.last_pace);
                ++perf_stretch.paces;
            }
            game_frames.last_pace = pace;
        }
    }
    if (!running) game_frames.last_pace = -1.0;
    game_frames.last_drawn = running ? drawn : -1.0;
}

void App::Window::pace_play_for(std::chrono::high_resolution_clock::time_point started) const {
    // --play-for: the rest of a 60 Hz display's frame, waited out as a
    // present would wait for it (the sim thread's tick runs meanwhile)
    // (Windows sleeps in its timer's steps, about 15.6 ms: the last of the
    // wait is spun, so the frame ends on time)
    if (opt.play_for <= 0) return;
    const auto frame_end = started + std::chrono::microseconds(16667);
    while (std::chrono::high_resolution_clock::now() < frame_end) std::this_thread::yield();
}

double App::Window::begin_frame() {
    osc::Profiler::instance().begin_frame();
    // A resized window: the UI's root frame follows it (M217h)
    root_resize_pending = renderer.take_resized() || root_resize_pending;
    const bool sim_busy = sim_thread && sim_thread->busy();
    if (root_resize_pending && !sim_busy) {
        root_resize_pending = false;
        root_width = renderer.ui_width();
        root_height = renderer.ui_height();
        size_root_frame(ui_lua_state.raw(), root_width, root_height);
        osc::core::call_on_application_resize(ui_lua_state.raw(), 0, root_width, root_height);
    }
    if (!sim_busy && osc::core::kNativeFullscreen && !offscreen_capture && !adapter_overridden &&
        renderer.fullscreen() != shown_fullscreen) {
        shown_fullscreen = renderer.fullscreen();
        keep_adapter_option(ui_lua_state.raw(), prefs, shown_fullscreen);
    }
    auto now = std::chrono::high_resolution_clock::now();
    double dt = std::chrono::duration<double>(now - prev_time).count();
    prev_time = now;
    if (!screenshot_path.empty() || render_bench) dt = kScreenshotFrameDt;
    if (opt.scripted_window) dt = kInterpFrameDt;
    // --fast-match: the front end and the load on the capture clock (their
    // scripts wait on it), then a sim tick each frame.
    if (opt.fast_match_ticks > 0)
        dt = fast_match_in_game() ? osc::sim::SimState::SECONDS_PER_TICK : kScreenshotFrameDt;
    // The session runs on the median of the last frame times (Moho's
    // CScApp::Main and RollingFrameRates), or on a fixed step as Moho's
    // /framerate gives one: a slow frame adds only a usual frame's time to
    // the game, so a hitch leaves its ticks behind the wall clock, which the
    // UI's WaitSeconds keeps. (Retail's commander is selected at the start
    // only if that leaves the UI's tick short of the warp's unselectable 30.)
    frame_times.push(dt);
    session_dt = (fixed_step() || opt.fast_match_ticks > 0) ? dt : frame_times.median();
    wall_dt = dt;

    // Clamp dt to avoid spiral of death
    if (dt > 0.25) dt = 0.25;
    return dt;
}

double App::Window::drawn_dt() const {
    if (fixed_step() || opt.fast_match_ticks > 0) return session_dt;
    return std::min(wall_dt, 0.25);
}

bool App::Window::fixed_step() const {
    return !screenshot_path.empty() || render_bench || opt.scripted_window ||
           opt.fast_match_ticks > 0;
}

void App::Window::update_audio(double dt) {
    // Audio: the camera is the listener, and FA's zoom curves read
    // its CameraDistance (the LOD metric at its focus: the zoom) and
    // ZoomPercent (the zoom over the farthest), as Moho's
    // CUserSoundManager sets them.
    {
        const auto& cam = renderer.camera();
        osc::f32 ex = 0, ey = 0, ez = 0;
        cam.eye_position(ex, ey, ez);
        const osc::f32 fx = cam.focus_x() - ex;
        const osc::f32 fy = cam.focus_y() - ey;
        const osc::f32 fz = cam.focus_z() - ez;
        const osc::f32 len = std::max(1e-3f, std::sqrt(fx * fx + fy * fy + fz * fz));
        // Moho's listener stands over the focus at the target zoom's height,
        // less 4, facing the view; its right is the screen's (the view
        // matrix's first row). Each cue's Angle is its own (SoundManager).
        const auto view = cam.view();
        sound.set_listener({cam.focus_x(), cam.focus_y() + cam.zoom() - 4.0f, cam.focus_z()},
                           {fx / len, fy / len, fz / len}, {view[0], view[4], view[8]});
        sound.set_global_variable("CameraDistance", cam.zoom());
        sound.set_global_variable("ZoomPercent", cam.zoom() / cam.max_zoom() * 100.0f);
        update_world_sounds();
        sound.update(static_cast<osc::f32>(dt));
    }
}

void App::Window::update_world_sounds() {
    // What the player hears of the world: Moho's CUserSoundManager, once a
    // frame. A point sounds where the player's army (or an ally) sees it
    // (FilterSound; everywhere for an observer or with the fog off). Moho
    // asks its water-vision grid for a sub's or seabed unit's sound; the
    // engine paints WaterVision into its one Vision grid, so that test is
    // this one (a little kinder: surface sight counts too).
    const osc::sim::FrameView view = world_interp.view();
    const auto& recon = renderer.recon();
    const osc::audio::SoundManager::Hearing hears = [&recon, &view](const osc::sim::Vector3& p,
                                                                    bool /*underwater*/) {
        return recon.sees_at(view, -1, p.x, p.z);
    };
    // The ticks' one-shots, in order.
    auto& events = world_interp.history.events();
    for (const auto& s : events.sounds)
        sound.play_world({s.bank, s.cue, s.lod_cutoff, s.pos, s.underwater, s.tick, s.entity_id},
                         hears);
    events.sounds.clear();

    // The entities' wanted loops: where they are drawn, and whether they're
    // in the world camera's view.
    std::vector<osc::audio::SoundManager::EntityLoop> loops;
    if (sim_state) {
        const auto& cam = renderer.camera();
        const osc::f32 aspect = renderer.height() > 0 ? static_cast<osc::f32>(renderer.width()) /
                                                            static_cast<osc::f32>(renderer.height())
                                                      : 1.0f;
        const osc::renderer::Frustum frustum(cam.view_proj(aspect));
        loops = gather_entity_loops(
            *sim_state, loop_sources.ids(*sim_state),
            [&view](const osc::sim::Entity& e) { return view.position(e); },
            [&frustum](const osc::sim::Vector3& at, osc::f32 radius) {
                return frustum.is_sphere_visible(at.x, at.y, at.z, radius);
            });
    }
    sound.sync_entity_loops(loops, hears);
}

void App::Window::count_fps(double dt) {
    // FPS tracking
    fps_accum += dt;
    fps_frames++;
    if (fps_accum >= 0.5) {
        display_fps = fps_frames / fps_accum;
        fps_accum = 0.0;
        fps_frames = 0;
    }
}

void App::Window::advance_sim(double dt) {
    if (sim_state && game_state_mgr.paused()) {
        world_interp.one_tick_next = true;
    }
    if (fixed_step()) {
        world_interp.one_tick_next = false;
    }
    if (!game_state_mgr.paused() && !game_state_mgr.sim_stopped() && sim_state) {
        const bool held = sim_state->paused() || sim_state->game_over();
        const double sim_dt = held ? dt : dt * game_state_mgr.speed();
        if (!held) {
            if (!sim_held) world_interp.clock.advance(drawn_dt() * game_state_mgr.speed());
        }

        if (catch_up) {
            // A loaded game catches up to its saved tick as fast as
            // the sim runs, a frame's budget of ticks at a time: the
            // window stays live, showing the game fast-forward.
            const auto until = std::chrono::steady_clock::now() + kCatchUpFrameBudget;
            bool matched = true;
            while (catch_up && matched && std::chrono::steady_clock::now() < until) {
                sim_state->tick();
                matched = check_catch_up();
            }
            sim_accumulator = 0.0;
            if (!matched) {
                // Not the game that was saved: back to the front end.
                lua_State* uiL = ui_lua_state.raw();
                lua_pushstring(uiL, "__osc_front_end_notice");
                lua_pushstring(uiL, "This saved game did not load as it was played.");
                lua_rawset(uiL, LUA_REGISTRYINDEX);
                lua_pushstring(uiL, "__osc_return_to_lobby");
                lua_pushboolean(uiL, 1);
                lua_rawset(uiL, LUA_REGISTRYINDEX);
                if (opt.load_flow_test) {
                    osc::test_status::fail("[FAIL] load-flow: the game diverged from "
                                           "its save as it caught up");
                    load_flow_done = true;
                }
            }
        } else if (osc::lua::mp_net_state().active()) {
            // Multiplayer: advance in lockstep. Pace command frames
            // at the sim tick rate; the session only advances the
            // sim once every peer has confirmed the next frame
            // (the classic "waiting for players" stall otherwise).
            // A round is a tick's time at the game's speed, which
            // is every peer's (M218i): the local speed follows it
            auto* session = osc::lua::mp_net_state().session.get();
            if (game_state_mgr.sim_rate() != session->speed())
                game_state_mgr.set_sim_rate(session->speed());
            const int rounds = osc::session_ticks(sim_accumulator, world_interp.one_tick_next,
                                                  sim_dt, osc::sim::SimState::SECONDS_PER_TICK, 4);
            for (int r = 0; r < rounds; ++r) {
                session->send_frame();
                session->receive_and_advance();
                // A timed-out peer leaves so the match resolves instead
                // of stalling in "waiting for players": the survivors
                // agree on its last frame, and its command source ends in
                // the same beat on every one of them (a command, so
                // replays keep it) -- an army it leaves with no player is
                // abandoned to its brain, which retail's defeats.
                for (osc::u32 src : session->take_dropped()) {
                    spdlog::warn("[mp] peer {} dropped — its source ends", src);
                    osc::lua::mp_disconnect_source(src); // M218e
                }
            }
        } else {
            // At most 8 beats per frame; a slower-than-real-time
            // sim slows the game rather than stalling every frame.
            const int beats = osc::session_ticks(sim_accumulator, world_interp.one_tick_next,
                                                 sim_dt, osc::sim::SimState::SECONDS_PER_TICK, 8);
            for (int b = 0; b < beats; ++b) {
                // A replay plays to its end, then holds there.
                if (active_playback && active_playback->finished(*sim_state)) {
                    break;
                }
                if (at_save_tick()) break;
                sim_state->tick();
                if (active_playback) {
                    const bool diverged = active_playback->diverged_at() != 0;
                    if (!active_playback->check(*sim_state) && !diverged) {
                        spdlog::warn("Replay: diverged from the recording at beat {}",
                                     active_playback->diverged_at());
                    }
                }
            }
        }
    }
}

bool App::Window::sim_threaded() const {
    // (--profile times the sim's zones on this thread; --profile-frames
    // leaves it on its own)
    // Fixed-frame captures must not accrue extra sim time from busy draws.
    // Fast matches collect each job; scripted network flows exercise the
    // real threaded session path and synchronize their ticks over lockstep.
    const bool network_flow = opt.lan_game_test() || opt.gpgnet_scripted;
    return sim_thread && sim_state && !catch_up && !tests &&
           (!fixed_step() || opt.fast_match_ticks > 0 || network_flow) &&
           (!osc::Profiler::instance().enabled() || opt.profile_frames);
}

osc::u32 App::Window::collect_sim() {
    if (!sim_thread || !sim_thread->busy()) return sim_state ? sim_state->tick_count() : 0;
    sim_thread->wait();
    osc::lua::watch_lua_state(sim_lua_state ? sim_lua_state->raw() : nullptr, {}); // ours again
    if (sim_state) sim_state->set_ticking_elsewhere(false);
    world_interp.publish();
    collected_tick = sim_state ? sim_state->tick_count() : 0;
    time_ticks(job_ms, job_ticks);
    // A timed-out peer, defeated by the lockstep's agreement, is let go
    for (const osc::u32 src : job_dropped) {
        spdlog::warn("[mp] peer {} dropped — its army is defeated", src);
        osc::lua::mp_disconnect_source(src); // M218e
    }
    job_dropped.clear();
    return job_tick0;
}

void App::Window::quiesce_sim() {
    if (sim_thread && sim_thread->busy()) collect_sim();
}

osc::u32 App::Window::settled_tick() const {
    if (sim_thread && sim_thread->busy()) return collected_tick;
    return sim_state ? sim_state->tick_count() : 0;
}

void App::Window::start_sim(double dt) {
    if (game_state_mgr.paused() || game_state_mgr.sim_stopped() || !sim_state) {
        world_interp.one_tick_next = true;
        return;
    }
    // (a --fast-match's last tick is its last: none past it)
    if (opt.fast_match_ticks > 0 && sim_state->tick_count() >= opt.fast_match_ticks) return;
    job_tick0 = sim_state->tick_count();
    world_interp.sync.prepare_threaded(sim_lua_state ? sim_lua_state->raw() : nullptr,
                                       ui_lua_state.raw());
    sim_held = sim_state->paused() || sim_state->game_over();
    const double sim_dt = sim_held ? dt : dt * game_state_mgr.speed();
    if (fixed_step()) world_interp.one_tick_next = false;
    if (osc::lua::mp_net_state().active()) {
        // A lockstep game's rounds, as advance_sim paces them
        auto* session = osc::lua::mp_net_state().session.get();
        if (game_state_mgr.sim_rate() != session->speed())
            game_state_mgr.set_sim_rate(session->speed());
        const int rounds = osc::session_ticks(sim_accumulator, world_interp.one_tick_next, sim_dt,
                                              sim::SimState::SECONDS_PER_TICK, 4);
        if (rounds == 0) return;
        osc::lua::watch_lua_state(sim_lua_state ? sim_lua_state->raw() : nullptr, sim_thread->id());
        sim_state->set_ticking_elsewhere(true);
        sim_thread->start([this, session, rounds] {
            const auto started = std::chrono::steady_clock::now();
            const osc::u32 tick0 = sim_state->tick_count();
            for (int r = 0; r < rounds; ++r) {
                session->send_frame();
                session->receive_and_advance();
                for (const osc::u32 src : session->take_dropped()) job_dropped.push_back(src);
            }
            world_interp.history.stage_world(*sim_state);
            job_ticks = sim_state->tick_count() - tick0;
            job_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                               started)
                         .count();
        });
        return;
    }
    const int ticks = osc::session_ticks(sim_accumulator, world_interp.one_tick_next, sim_dt,
                                         osc::sim::SimState::SECONDS_PER_TICK, 8);
    if (ticks == 0) return;
    osc::lua::watch_lua_state(sim_lua_state ? sim_lua_state->raw() : nullptr, sim_thread->id());
    sim_state->set_ticking_elsewhere(true);
    sim_thread->start([this, ticks] {
        const auto started = std::chrono::steady_clock::now();
        const osc::u32 tick0 = sim_state->tick_count();
        for (int t = 0; t < ticks; ++t) {
            // A replay plays to its end, then holds there.
            if (active_playback && active_playback->finished(*sim_state)) break;
            if (opt.fast_match_ticks > 0 && sim_state->tick_count() >= opt.fast_match_ticks) break;
            if (at_save_tick()) break;
            sim_state->tick();
            if (active_playback) {
                const bool diverged = active_playback->diverged_at() != 0;
                if (!active_playback->check(*sim_state) && !diverged) {
                    spdlog::warn("Replay: diverged from the recording at tick {}",
                                 active_playback->diverged_at());
                }
            }
        }
        // The world is captured once, as the job's last tick left it (each
        // tick's events as it ran): no frame sees the ticks between
        world_interp.history.stage_world(*sim_state);
        job_ticks = sim_state->tick_count() - tick0;
        job_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
    });
}

void App::Window::finish_world_ui_after_post_load() {
    if (!world_ui_after_post_load || !sim_state) return;
    if (sim_state->post_loads_run() <= *world_ui_after_post_load) return;
    world_ui_after_post_load.reset();
    finish_world_ui(ui_lua_state.raw(), wld_provider, active_playback.has_value(),
                    sim_lua_state.get(), sim_state.get(), world_interp.sync);
}

void App::Window::run_beats() {
    // CWldSession::DoBeat: each of the sim's beats reaches the user side with its own Sync.
    if (sim_state && sim_lua_state) {
        const auto refresh = [this] {
            if (!world_ui_after_post_load) {
                refresh_selection(ui_lua_state.raw(), input_handler, *sim_state, prev_selection,
                                  prev_selection_upgrades);
            }
        };
        while (world_interp.sync.pending()) {
            world_beat(sim_lua_state.get(), sim_state.get(), ui_lua_state.raw(), world_interp.sync,
                       refresh);
        }
    }
}

void App::Window::run_flows() {
    // --replay-flow-test ends when the replay has played out.
    if (opt.replay_flow_test) {
        ++replay_flow_frames;
        if (active_playback && sim_state && active_playback->finished(*sim_state)) {
            replay_flow_done = true;
            // Watched from the front end, its units are drawn: as their
            // meshes, or as their icons at strategic zoom, which draws no
            // meshes.
            const auto& icons = renderer.strategic_icons();
            const bool shown = icons.is_strategic_zoom() ? !icons.quads().empty()
                                                         : renderer.mesh_instance_count() > 0;
            if (!shown || renderer.cube_instance_count() != 0)
                osc::test_status::fail(
                    "[FAIL] replay-flow: {} meshes, {} icons (strategic zoom: {}) and {} cubes "
                    "drawn",
                    renderer.mesh_instance_count(), icons.quads().size(), icons.is_strategic_zoom(),
                    renderer.cube_instance_count());
        } else if (replay_flow_frames > 40000) replay_flow_done = true; // stuck: reported below
    }

    if (lan_game) lan_game->frame(ui_lua_state, sim_state.get());
    // A test's GPGNet game says how far it has got, for its client
    // to see it play (M220b)
    if (opt.gpgnet_scripted && sim_state && sim_state->tick_count() % 50 == 0 &&
        sim_state->tick_count() != gpgnet_logged_tick) {
        gpgnet_logged_tick = sim_state->tick_count();
        spdlog::info("[gpgnet] tick {}", gpgnet_logged_tick);
    }
    if (mods_flow)
        mods_flow->frame(ui_lua_state, sim_lua_state.get(), sim_state.get(), renderer.ui_dispatch(),
                         ui_registry);
    if (self_destruct_flow) self_destruct_flow->frame(ui_lua_state, sim_state.get());
    if (restart_flow) restart_flow->frame(ui_lua_state, sim_state.get());
    if (loading_screen)
        loading_screen->frame(ui_lua_state, sim_state.get(), !world_ui_after_post_load,
                              restored_at);
    if (campaign_flow)
        campaign_flow->frame(ui_lua_state, sim_lua_state.get(), sim_state.get(),
                             renderer.ui_dispatch(), ui_registry);
    if (ui_clicks) {
        ui_clicks->frame(ui_lua_state, renderer.ui_dispatch(), ui_registry);
    }
    if (scripted_mouse) {
        drive_scripted_mouse();
    }

    load_flow_frame();
}

void App::Window::drive_scripted_mouse() {
    auto& input = renderer.ui_dispatch();
    MouseSink sink;
    sink.cursor = [&](f64 x, f64 y) { input.on_cursor_pos(x, y); };
    sink.button = [&](i32 button, i32 action, i32 mods) {
        input.on_mouse_button(button, action, mods);
    };
    sink.key = [&](i32 key, i32 action, i32 mods) { input.on_key(key, action, mods); };
    sink.world_to_screen = [&](f64 x, f64 z) -> std::optional<std::array<f64, 2>> {
        if (!sim_state || !sim_state->terrain()) {
            return std::nullopt;
        }
        const auto w = static_cast<f32>(renderer.ui_width());
        const auto h = static_cast<f32>(renderer.ui_height());
        const sim::Vector3 at{
            static_cast<f32>(x),
            sim_state->terrain()->get_surface_height(static_cast<f32>(x), static_cast<f32>(z)),
            static_cast<f32>(z)};
        const auto p =
            renderer::screen_point(renderer.camera().view_proj(h > 0 ? w / h : 1.0f), at, w, h);
        if (!p) {
            return std::nullopt;
        }
        return std::array<f64, 2>{(*p)[0], (*p)[1]};
    };
    scripted_mouse->frame(sim_state ? std::optional<u32>(sim_state->tick_count()) : std::nullopt,
                          sink);
    renderer.set_scripted_pointer(scripted_mouse->pointer());
}

void App::Window::update_ui(double dt) {
    // OnFirstUpdate — fire once after first sim tick
    if (!first_update_fired && sim_state && sim_state->tick_count() > 0) {
        osc::core::call_on_first_update(ui_lua_state.raw());
        first_update_fired = true;
    }

    // The world view's camera (M217f): its keys unless the focused control
    // blocks them, its mouse only off the UI's controls; it
    // keeps to the sim's playable rect.
    renderer.camera().set_keys_enabled(!ui_registry.key_down_blocked());
    renderer.set_ui_keys_blocked(ui_registry.key_down_blocked());
    {
        osc::f64 mx = 0, my = 0;
        renderer.ui_mouse_position(mx, my);
        const bool over_ui = mouse_over_ui(ui_lua_state.raw(), mx, my);
        renderer.set_mouse_over_ui(over_ui);
        renderer.camera().set_mouse_enabled(!over_ui);
    }
    if (sim_state && sim_state->has_playable_rect()) {
        renderer.camera().set_playable_rect(sim_state->playable_x0(), sim_state->playable_z0(),
                                            sim_state->playable_x1(), sim_state->playable_z1());
    }
    renderer.set_nis_mode(osc::core::ui_nis_mode(ui_lua_state.raw()));
    renderer.poll_events(dt);
    // While the pointer is pinned FA's cursor is hidden, every frame
    // (func_ProcessMouseScrubbing), and let go, it is shown again
    // (func_StartMouseScrubbing)
    if (const auto scrub = renderer.take_scrub_change(); renderer.mouse_scrubbing() || scrub) {
        show_ui_cursor(ui_lua_state.raw(), !renderer.mouse_scrubbing());
    }
    if (renderer.should_close() && !renderer.iconified() &&
        osc::core::call_show_escape_dialog(ui_lua_state.raw())) {
        renderer.keep_open();
    }
    const bool graph_held = sim_state && renderer.command_graph_held();
    if (graph_held != command_graph_shown) {
        command_graph_shown = graph_held;
        osc::core::call_on_command_graph_show(ui_lua_state.raw(), graph_held);
    }

    // The lobbies' networks: what has come, into their callbacks
    // (M218a)
    osc::lua::pump_net_lobbies(ui_lua_state.raw(), osc::lua::net_lobby_clock_ms());
    // The matchmaking client's commands (M220a)
    osc::lua::pump_gpgnet(ui_lua_state.raw());
    // (A test's run ends with the link in a game too: its client is done)
    if (osc::lua::gpgnet_state() == osc::lua::GpgNetState::Closed &&
        (!sim_state || opt.gpgnet_scripted))
        gpgnet_done = true;
    osc::lua::pump_session_chat(ui_lua_state.raw()); // M218d
    if (sim_state) {
        osc::lua::pump_disconnect_dialog(ui_lua_state.raw()); // M218e
        osc::lua::pump_pause_state(ui_lua_state.raw());       // M218f
        osc::lua::pump_speed_changes(ui_lua_state.raw());     // M218i
        osc::lua::pump_desyncs(ui_lua_state.raw());
    }

    // The engine statistics the UI reads this frame (Moho publishes them
    // every UI tick, the front end's too)
    osc::lua::update_engine_stats(ui_lua_state.raw(), wall_dt, sim_state.get());

    run_ui_scripts(dt);
}

void App::Window::run_ui_scripts(double dt) {
    // Resume UI coroutines
    ++ui_frame_count;
    osc::lua::advance_ui_clock(ui_lua_state.raw(), wall_dt);
    ui_thread_manager.resume_all(ui_frame_count);

    // OnBeat — UI heartbeat each frame
    osc::core::call_on_beat(ui_lua_state.raw(), dt);

    // Fire beat functions each frame (M145b)
    beat_registry.fire_all(ui_lua_state.raw());
}

void App::Window::update_input(double dt, const sim::FrameView& frame_view) {
    input_handler.set_frame_view(frame_view);
    if (tests && sim_state) {
        Frame frame{frame_view, renderer, input_handler};
        tests->frame_view(engine, frame);
    }

    // The player's army is the UI's focus army (SetFocusArmy; -1 an
    // observer): what it selects, and what its intel shows (M215a).
    if (sim_state) {
        const osc::i32 focus = osc::lua::focus_army(ui_lua_state.raw());
        renderer.set_player_army(focus);
        input_handler.set_player_army(focus);
        world_interp.history.set_sight_army(focus, sim_state.get()); // its sight, from now
    }

    // Player input: selection + commands. Not while a loaded game's
    // interface waits for its post-load: there is no game UI to tell yet.
    const bool interface_up = !world_ui_after_post_load;
    if (sim_state && interface_up) {
        current_command_mode = read_command_mode(ui_lua_state.raw());
        sync_build_ghost(*sim_state, current_command_mode, ghost_from_mode);
        input_handler.update(renderer, *sim_state, dt, [&] {
            osc::f64 mx = 0, my = 0;
            renderer.ui_mouse_position(mx, my);
            return mouse_over_ui(ui_lua_state.raw(), mx, my);
        });
        // (not while the pointer is pinned: CUIWorldView::Frame skips it)
        if (input_handler.cursor_world() && !renderer.mouse_scrubbing()) {
            osc::lua::update_world_view_cursor(ui_lua_state.raw());
        }
    }

    // Selections are a game's: at the front end (after a return to the
    // lobby cleared the selection) there is no game UI to tell.
    if (sim_state && interface_up) {
        dispatch_selection_change(ui_lua_state.raw(), prev_selection, prev_selection_upgrades,
                                  *sim_state, input_handler.selected(),
                                  input_handler.take_selection_event());
    } else {
        (void)input_handler.take_selection_event();
    }
}

void App::Window::render(const sim::FrameView& frame_view,
                         const std::function<void(bool)>& before_draw) {
    if (fast_match_in_game()) {
        if (fast_match_frames++ % kFastMatchDrawEvery != 0) {
            before_draw(false);
            return;
        }
        ++fast_match_drawn;
    }
    note_drawn_time(frame_view);
    const auto& sel = input_handler.selected();
    // The render benchmark waits for its save to load, front end included
    if (render_bench && !(sim_state && game_state_mgr.current() == osc::GameState::GAME))
        render_bench->waiting();
    if (!screenshot_path.empty() && ++frames_rendered == std::max<osc::u32>(screenshot_frame, 1)) {
        const bool requested = renderer.request_capture([&](osc::ImageRGBA8 image) {
            screenshot_ok = osc::write_png(screenshot_path, image);
            screenshot_done = true;
            spdlog::info("Screenshot {}x{} -> {} ({})", image.width, image.height, screenshot_path,
                         screenshot_ok ? "written" : "WRITE FAILED");
        });
        if (!requested) {
            spdlog::error("Screenshot: swapchain readback unsupported");
            screenshot_done = true;
        }
    }
    if (sim_state) {
        const bool busy = sim_thread && sim_thread->busy();
        if (!busy) cached_ghost = input_handler.build_ghost(renderer, *sim_state);
        const auto& ghost = cached_ghost;
        // The render benchmark's scene, once the loaded game is in (M223b)
        RenderBench* bench = render_bench && game_state_mgr.current() == osc::GameState::GAME
                                 ? render_bench.get()
                                 : nullptr;
        if (bench) bench->before_frame(renderer, *sim_state);
        const auto render_start = std::chrono::steady_clock::now();
        renderer.set_selection_marks(input_handler.hovered(), input_handler.drag_box());
        renderer.set_command_highlight(input_handler.hovered_command());
        if (!busy) renderer.set_formation_ghosts(input_handler.formation_ghosts(*sim_state));
        const auto drag = input_handler.order_drag();
        renderer.set_command_preview(drag ? drag->command_id : 0,
                                     drag ? drag->at : osc::sim::Vector3{}, !drag || drag->valid,
                                     drag && drag->held);
        // (from here the draw reads only the snapshots and what was set above)
        before_draw(true);
        renderer.render(frame_view, world_interp.history.events(), ghost ? &*ghost : nullptr,
                        ui_lua_state.raw(), &ui_registry, sel.empty() ? nullptr : &sel);
        if (bench)
            bench->after_frame(renderer, std::chrono::duration<osc::f64, std::milli>(
                                             std::chrono::steady_clock::now() - render_start)
                                             .count());
        if (tests) {
            Frame frame{frame_view, renderer, input_handler};
            tests->frame_rendered(engine, frame);
        }
    } else {
        // No sim state (front-end/lobby) — render UI only
        before_draw(true);
        renderer.render_ui_only(ui_lua_state.raw(), &ui_registry);
    }
}

void App::Window::update_title(double dt) {
    const auto& sel = input_handler.selected();
    // Update window title periodically
    title_update_timer += dt;
    if (title_update_timer >= 0.25) {
        title_update_timer = 0.0;
        char title[256];
        if (sim_state) {
            const char* status_str = game_state_mgr.game_over()                       ? "GAME OVER "
                                     : sim_state->resuming()                          ? "LOADING "
                                     : game_state_mgr.paused() || sim_state->paused() ? "PAUSED "
                                                                                      : "";
            std::snprintf(title, sizeof(title),
                          "OpenSupCom | %s%.1fx | T:%u (%.1fs) | %zu entities | %zu sel | %.0f FPS",
                          status_str, game_state_mgr.speed(), sim_state->tick_count(),
                          sim_state->game_time(), sim_state->entity_registry().count(), sel.size(),
                          display_fps);
        } else {
            std::snprintf(title, sizeof(title), "OpenSupCom | Lobby | %.0f FPS", display_fps);
        }
        renderer.set_window_title(title);
    }
}

} // namespace osc::app
