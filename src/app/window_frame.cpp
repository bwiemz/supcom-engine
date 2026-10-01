// The windowed loop's frame, in its phases (M192 step 2c): App::Window.

#include "app/window_loop.hpp"
#include "lua/gpgnet_session.hpp"
#include "app/window_commands.hpp"
#include "app/world_sounds.hpp"
#include "core/fixed_step.hpp"
#include "core/profiler.hpp"
#include "lua/net_lobby.hpp"
#include "lua/session_clients.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/sim_sync.hpp"
#include "renderer/frustum.hpp"
#include "sim/lockstep_session.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace osc::app {

bool App::Window::frame() {
    const double dt = begin_frame();

    update_audio(dt);

    count_fps(dt);

    // Pause, game speed and Escape come from retail's key map
    // (Pause, NumPlus/NumMinus/NumStar, Esc): see UIDispatch.

    note_game_over_if_ended(sim_state.get(), game_state_mgr, ui_lua_state.raw());

    // Fixed-timestep sim ticking (scaled by sim_speed)
    const osc::u32 beat_tick0 = sim_state ? sim_state->tick_count() : 0;
    advance_sim(dt);

    run_beats(dt, beat_tick0);

    // Process SimCallbacks from UI (M138a)
    if (sim_state && sim_lua_state) submit_sim_callbacks(sim_callback_queue, *sim_state);

    run_flows();

    update_ui(dt);

    // This frame's world, between the last two ticks: what is
    // drawn and what clicks pick.
    const osc::sim::FrameView frame_view = world_interp.view();
    update_input(dt, frame_view);
    render(frame_view);
    update_title(dt);

    if (exit_requested()) return false;
    handle_launch();
    handle_return_to_lobby();

    osc::Profiler::instance().end_frame();
    return true;
}

double App::Window::begin_frame() {
    osc::Profiler::instance().begin_frame();
    // A resized window: the UI's root frame follows it (M217h)
    if (renderer.take_resized()) {
        root_width = renderer.width();
        root_height = renderer.height();
        size_root_frame(ui_lua_state.raw(), root_width, root_height);
    }
    auto now = std::chrono::high_resolution_clock::now();
    double dt = std::chrono::duration<double>(now - prev_time).count();
    prev_time = now;
    if (!screenshot_path.empty() || render_bench) dt = kScreenshotFrameDt;
    if (opt.scripted_window) dt = kInterpFrameDt;
    // Clamp dt to avoid spiral of death
    if (dt > 0.25) dt = 0.25;
    return dt;
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
        sound.play_world({s.bank, s.cue, s.lod_cutoff, s.pos, s.underwater, s.tick}, hears);
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
            *sim_state, [&view](const osc::sim::Entity& e) { return view.position(e); },
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
    if (!game_state_mgr.paused() && !game_state_mgr.sim_stopped() && sim_state) {
        world_interp.clock.advance(dt * game_state_mgr.speed());
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
            sim_accumulator += dt * game_state_mgr.speed();
            int guard = 0;
            while (sim_accumulator >= osc::sim::SimState::SECONDS_PER_TICK && guard++ < 4) {
                sim_accumulator -= osc::sim::SimState::SECONDS_PER_TICK;
                session->send_frame();
                session->receive_and_advance();
                // A timed-out peer is defeated so the match resolves
                // instead of stalling in "waiting for players": the
                // survivors agree on its last frame, and the session
                // defeats its army on the same tick on every one of
                // them (a command, so replays keep it).
                for (osc::u32 src : session->take_dropped()) {
                    spdlog::warn("[mp] peer {} dropped — its army is defeated", src);
                    osc::lua::mp_disconnect_source(src); // M218e
                }
            }
        } else {
            // At most 8 ticks per frame; a slower-than-real-time
            // sim slows the game rather than stalling every frame.
            const int ticks = osc::consume_fixed_steps(sim_accumulator, dt * game_state_mgr.speed(),
                                                       osc::sim::SimState::SECONDS_PER_TICK, 8);
            for (int t = 0; t < ticks; ++t) {
                // A replay plays to its end, then holds there.
                if (active_playback && active_playback->finished(*sim_state)) break;
                sim_state->tick();
                if (active_playback) {
                    const bool diverged = active_playback->diverged_at() != 0;
                    if (!active_playback->check(*sim_state) && !diverged) {
                        spdlog::warn("Replay: diverged from the recording at tick {}",
                                     active_playback->diverged_at());
                    }
                }
            }
        }
    }
}

void App::Window::run_beats(double dt, u32 beat_tick0) {
    // Moho's sim beat reaches the user side once per tick, and
    // keeps running at the tick rate while paused.
    if (sim_state && sim_lua_state) {
        osc::u32 beats = sim_state->tick_count() - beat_tick0;
        if (game_state_mgr.paused()) {
            paused_beat_accumulator += dt;
            while (paused_beat_accumulator >= osc::sim::SimState::SECONDS_PER_TICK) {
                paused_beat_accumulator -= osc::sim::SimState::SECONDS_PER_TICK;
                ++beats;
            }
        } else {
            paused_beat_accumulator = 0.0;
        }
        for (osc::u32 b = 0; b < beats; ++b)
            world_beat(sim_lua_state.get(), sim_state.get(), ui_lua_state.raw());
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
    if (ui_clicks) {
        ui_clicks->frame(ui_lua_state, renderer.ui_dispatch(), ui_registry);
    }

    load_flow_frame();
}

void App::Window::update_ui(double dt) {
    // OnFirstUpdate — fire once after first sim tick
    if (!first_update_fired && sim_state && sim_state->tick_count() > 0) {
        osc::core::call_on_first_update(ui_lua_state.raw());
        first_update_fired = true;
    }

    // The world view's camera (M217f): its keys only while no control
    // has the keyboard, its mouse only off the UI's controls; it
    // keeps to the sim's playable rect.
    renderer.camera().set_keys_enabled(ui_registry.keyboard_focus() == nullptr);
    {
        osc::f64 mx = 0, my = 0;
        renderer.mouse_position(mx, my);
        renderer.camera().set_mouse_enabled(!mouse_over_ui(ui_lua_state.raw(), mx, my));
    }
    if (sim_state && sim_state->has_playable_rect()) {
        renderer.camera().set_playable_rect(sim_state->playable_x0(), sim_state->playable_z0(),
                                            sim_state->playable_x1(), sim_state->playable_z1());
    }
    renderer.poll_events(dt);

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
    }

    // Resume UI coroutines
    ++ui_frame_count;
    osc::lua::advance_ui_clock(ui_lua_state.raw(), dt);
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
    }

    // Player input: selection + commands
    if (sim_state) {
        current_command_mode = read_command_mode(ui_lua_state.raw());
        sync_build_ghost(*sim_state, current_command_mode, ghost_from_mode);
        input_handler.update(renderer, *sim_state, dt, [&] {
            osc::f64 mx = 0, my = 0;
            renderer.mouse_position(mx, my);
            return mouse_over_ui(ui_lua_state.raw(), mx, my);
        });
    }

    // Selections are a game's: at the front end (after a return to the
    // lobby cleared the selection) there is no game UI to tell.
    if (sim_state) {
        dispatch_selection_change(ui_lua_state.raw(), prev_selection, input_handler.selected(),
                                  input_handler.take_selection_event());
    } else {
        (void)input_handler.take_selection_event();
    }
}

void App::Window::render(const sim::FrameView& frame_view) {
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
        const auto ghost = input_handler.build_ghost(renderer, *sim_state);
        // The render benchmark's scene, once the loaded game is in (M223b)
        RenderBench* bench = render_bench && game_state_mgr.current() == osc::GameState::GAME
                                 ? render_bench.get()
                                 : nullptr;
        if (bench) bench->before_frame(renderer, *sim_state);
        const auto render_start = std::chrono::steady_clock::now();
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
            const char* status_str = game_state_mgr.game_over() ? "GAME OVER "
                                     : sim_state->resuming()    ? "LOADING "
                                     : game_state_mgr.paused()  ? "PAUSED "
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
