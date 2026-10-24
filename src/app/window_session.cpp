// The UI's requests between games (M192 step 2c): exit, a launch and its
// load, and the return to the front end.

#include "app/window_loop.hpp"
#include "core/profiler.hpp"
#include "lua/lua_thread_check.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/sim_sync.hpp"

#include <chrono>
#include <thread>
#include <utility>

namespace osc::app {

bool App::Window::exit_requested() {
    // Check for exit request (M146d)
    {
        lua_State* uiL = ui_lua_state.raw();
        lua_pushstring(uiL, "__osc_exit_requested");
        lua_rawget(uiL, LUA_REGISTRYINDEX);
        if (lua_toboolean(uiL, -1)) {
            lua_pop(uiL, 1);
            return true;
        }
        lua_pop(uiL, 1);
    }
    return false;
}

void App::Window::handle_launch() {
    // RestartSession: the game in play launched again, from its own launch
    // (a new game from its own setup, seed and all), as Moho's CreateSession
    // frame action starts the session again from its launch info
    std::optional<osc::sim::GameSetup> restart;
    if (take_restart_request(ui_lua_state.raw(), session_launch)) restart = session_launch->setup;
    // Check for game launch request from lobby (M148a)
    {
        lua_State* uiL = ui_lua_state.raw();
        lua_pushstring(uiL, "__osc_launch_requested");
        lua_rawget(uiL, LUA_REGISTRYINDEX);
        if (lua_toboolean(uiL, -1)) {
            lua_pop(uiL, 1);
            quiesce_sim(); // the game being left finishes its tick first

            // Clear the flag
            lua_pushstring(uiL, "__osc_launch_requested");
            lua_pushnil(uiL);
            lua_rawset(uiL, LUA_REGISTRYINDEX);

            // Read scenario path
            lua_pushstring(uiL, "__osc_launch_scenario");
            lua_rawget(uiL, LUA_REGISTRYINDEX);
            const char* sc = lua_tostring(uiL, -1);
            std::string launch_scenario = sc ? sc : "";
            lua_pop(uiL, 1);

            // A replay to play (LaunchReplaySession), if any
            std::optional<osc::sim::Replay> launch_replay;
            SessionLaunch launch{launch_scenario, {}, {}, std::nullopt};
            lua_pushstring(uiL, "__osc_launch_replay");
            lua_rawget(uiL, LUA_REGISTRYINDEX);
            if (lua_type(uiL, -1) == LUA_TSTRING) {
                launch.replay_file = lua_tostring(uiL, -1);
                launch_replay = load_replay(launch.replay_file);
                if (!launch_replay) launch_scenario.clear();
            }
            lua_pop(uiL, 1);
            lua_pushstring(uiL, "__osc_launch_replay");
            lua_pushnil(uiL);
            lua_rawset(uiL, LUA_REGISTRYINDEX);

            // A saved game to load (LoadSavedGame), if any
            std::optional<osc::sim::SavedGame> launch_save;
            lua_pushstring(uiL, "__osc_launch_save");
            lua_rawget(uiL, LUA_REGISTRYINDEX);
            if (lua_type(uiL, -1) == LUA_TSTRING) {
                launch.save_file = lua_tostring(uiL, -1);
                osc::sim::SavedGame save;
                if (osc::lua::read_saved_game(launch.save_file, save) ==
                    osc::sim::SaveLoadError::None)
                    launch_save = std::move(save);
                else launch_scenario.clear();
            }
            lua_pop(uiL, 1);
            lua_pushstring(uiL, "__osc_launch_save");
            lua_pushnil(uiL);
            lua_rawset(uiL, LUA_REGISTRYINDEX);
            const osc::sim::Replay* recorded = launch_replay ? &*launch_replay
                                               : launch_save ? &launch_save->game
                                                             : nullptr;
            if (recorded) restart.reset(); // a replay's or a save's is its file's

            if (!launch_scenario.empty()) {
                spdlog::info("Launch requested: {}{}{}", launch_scenario,
                             launch_replay ? " (replay)"
                             : launch_save ? " (saved game)"
                                           : "",
                             restart ? ", restarted" : "");
                save_last_game(); // the game being left, if any
                perf_stretch.clear();
                game_frames.last_drawn = game_frames.last_pace = -1.0;
                active_playback.reset();
                catch_up.reset();
                restored_at.reset();

                // The game gets a fresh UI state, as Moho gives each
                // game one (M191 step 4): the front end's, or the last
                // game's, goes with its controls and threads.
                wld_provider.destroy_game_interface(uiL);
                renderer.forget_ui_controls();
                // (with the game's mods, as its sim will have them)
                const std::string game_mods =
                    restart ? restart->mods : launch_setup(uiL, recorded, launch_scenario, 0).mods;
                reset_ui_state(&game_mods);
                uiL = ui_lua_state.raw();
                publish_window_objects();
                // The new state's loading screen has no game yet, even
                // when the last one still runs until the reload (per
                // review), as from the front end.
                detach_ui_from_sim(uiL);

                // Transition to LOADING (SetupUI, the game's UI state's)
                // and show the loading screen
                osc::set_ui_state(osc::UIState::Game);
                game_state_mgr.transition_to(osc::GameState::LOADING, uiL);
                begin_world_ui(uiL, wld_provider);

                // The world loads as Moho's loader loads it, off the frame
                // loop: the frames go on drawing the loading screen until
                // it is in (loading_frame), then the launch finishes
                // (finish_launch).
                loading.emplace();
                loading->scenario = std::move(launch_scenario);
                loading->launch_replay = std::move(launch_replay);
                loading->launch_save = std::move(launch_save);
                loading->restart = std::move(restart);
                loading->launch = std::move(launch);
                start_world_load();
            }
        } else {
            lua_pop(uiL, 1);
        }
    }
}

bool App::Window::load_threaded() const {
    return opt.sim_thread && (!osc::Profiler::instance().enabled() || opt.profile_frames);
}

App::Window::Loading::Loading() = default;

void App::Window::start_world_load() {
    Loading& l = *loading;
    const osc::sim::Replay* recorded = l.launch_replay ? &*l.launch_replay
                                       : l.launch_save ? &l.launch_save->game
                                                       : nullptr;
    l.stage = Loading::Stage::World;
    l.world =
        begin_world_load(sim_lua_state, sim_state, ui_lua_state, vfs, store, loader, config,
                         scenario_meta, &renderer, launch_seed(opt.seed_arg, opt.reproducible_run),
                         l.scenario, recorded, l.restart ? &*l.restart : nullptr);
    start_load_job([this, &world = l.world] {
        load_world(world, vfs, store, loader, config, game_state_mgr);
    });
}

void App::Window::start_load_job(std::function<void()> job) {
    if (!load_threaded()) {
        loading->job_here = std::move(job);
        return;
    }
    if (!load_thread) load_thread = std::make_unique<SimThread>();
    // No other thread may touch the sim's Lua meanwhile (counted in the
    // runs that check)
    if (loading->world.sim_lua)
        osc::lua::watch_lua_state(loading->world.sim_lua->raw(), load_thread->id());
    load_thread->start(std::move(job));
}

bool App::Window::load_job_done() {
    if (loading->job_here) return false;
    if (load_thread && load_thread->busy()) {
        if (!load_thread->done()) return false;
        load_thread->wait();
        osc::lua::watch_lua_state(nullptr, {}); // ours again
    }
    return true;
}

void App::Window::loading_frame(double dt, std::chrono::high_resolution_clock::time_point started) {
    if (load_job_done()) {
        advance_load();
        if (!loading) return; // the game is in: the next frame is its
    }
    count_fps(dt);
    // The window's events (resized, closed; the UI's input, which the draw
    // dispatches), the UI's scripts, and the loading screen drawn
    renderer.poll_events(dt);
    run_ui_scripts(dt);
    renderer.render_ui_only(ui_lua_state.raw(), &ui_registry);
    if (loading_screen)
        loading_screen->loading_frame_drawn(ui_registry, loading->stage == Loading::Stage::Restore);
    if (loading->job_here) {
        std::exchange(loading->job_here, nullptr)();
    } else if (offscreen_capture) {
        // No display paces an offscreen frame: the rest of a 60 Hz one is
        // the loader's
        std::this_thread::sleep_until(started + std::chrono::microseconds(16667));
    }
}

void App::Window::advance_load() {
    Loading& l = *loading;
    if (l.stage == Loading::Stage::World) {
        l.world_ok = l.world.ok;
        if (l.launch_save && l.world.sim && !l.reloaded) {
            // The player's game, restored from its snapshot (M208c) into
            // the sim just loaded for it, on the loader's thread too.
            // Without one, with --load-by-replay, or if the restore fails
            // (the sim it spoiled loaded again), caught up from its
            // history instead (finish_launch).
            l.restore = Restore::Skipped;
            if (app.may_restore(*l.launch_save, l.why)) {
                l.stage = Loading::Stage::Restore;
                l.restore_started = std::chrono::steady_clock::now();
                start_load_job(
                    [&l] { l.restore = restore_snapshot(*l.world.sim, *l.launch_save, l.why); });
                return;
            }
        }
    }
    // The world is the game's
    adopt_world_load(l.world, sim_lua_state, sim_state, scenario_meta, ui_lua_state, &world_interp);
    const bool loading_save = l.launch_save && sim_state;
    if (loading_save) {
        // Its interface waits for the load's post-load (see
        // finish_world_ui_after_post_load).
        world_ui_after_post_load = kPostLoadUnscheduled;
        if (l.restore == Restore::Done) {
            app.restored(*l.launch_save, l.restore_started);
        } else if (l.restore == Restore::Failed && !l.reloaded &&
                   !osc::lua::mp_net_state().transport_ready) {
            spdlog::warn("Saved game '{}': not restored ({}); catching up", l.launch_save->name,
                         l.why);
            l.reloaded = true;
            start_world_load();
            return;
        }
    }
    if (l.world_ok && sim_state && sim_lua_state)
        finish_world_load(*sim_state, *sim_lua_state, ui_lua_state, vfs, game_state_mgr, &renderer,
                          &ui_store, &input_handler, &prev_selection, sim_accumulator, l.scenario);
    finish_launch();
    loading.reset();
}

void App::Window::finish_launch() {
    Loading& l = *loading;
    lua_State* uiL = ui_lua_state.raw();
    // What it was launched from, for RestartSession: a new game's own setup
    // (its seed too), or its replay or save
    if (sim_state && !l.launch_replay && !l.launch_save)
        l.launch.setup = sim_state->recorded_replay().setup;
    session_launch = sim_state ? std::optional<SessionLaunch>(std::move(l.launch)) : std::nullopt;
    if (l.launch_replay && sim_state) {
        // Watched as an observer, as the replay plays.
        active_playback.emplace(std::move(*l.launch_replay));
        active_playback->start(*sim_state);
        lua_pushstring(uiL, "__osc_focus_army");
        lua_pushnumber(uiL, -1);
        lua_rawset(uiL, LUA_REGISTRYINDEX);
    }
    const bool loading_save = l.launch_save && sim_state;
    if (loading_save) {
        // Not restored: caught up from its history, recorded from its
        // start (once its orders and command delay are queued), so it saves
        // again whole.
        if (l.restore != Restore::Done && osc::lua::mp_net_state().transport_ready) {
            // A network game resumes only from each player's
            // own restored copy (OE-128): no catching up alone
            spdlog::error("Saved game '{}': can't resume the network game ({})",
                          l.launch_save->name, l.why);
            lua_pushstring(uiL, "__osc_front_end_notice");
            lua_pushstring(uiL, ("The saved game couldn't be resumed: " + l.why).c_str());
            lua_rawset(uiL, LUA_REGISTRYINDEX);
            lua_pushstring(uiL, "__osc_return_to_lobby");
            lua_pushboolean(uiL, 1);
            lua_rawset(uiL, LUA_REGISTRYINDEX);
            return;
        }
        if (l.restore != Restore::Done) {
            catch_up.emplace(std::move(l.launch_save->game));
            catch_up->resume(*sim_state);
            sim_state->set_recording(true);
            spdlog::info("Saved game '{}': catching up to beat {} ({})", l.launch_save->name,
                         l.launch_save->beat, l.why);
        }
    }

    // Reset per-session state for the new game
    first_update_fired = false;

    // Multiplayer: if the lobby stood up a network
    // transport (HostGame/JoinGame), build the lockstep
    // session over it now that the game's sim exists.
    // No-op in single-player.
    // Each player's copy of a save the players agree to
    // (OE-128), as this player's profile keeps its saves
    app.network_profile = osc::lua::current_profile(uiL);
    osc::lua::mp_set_save_handler([this](osc::sim::SimState& sim, const std::string& name) {
        app.save_network_game(sim, name);
    });
    if (sim_state && !active_playback && osc::lua::mp_attach_session(*sim_state)) {
        // Each peer plays its own source's army, and
        // sees the world through its intel (M215a); an
        // observer, all of it.
        osc::lua::set_focus_army(sim_lua_state ? sim_lua_state->raw() : nullptr, uiL,
                                 osc::lua::mp_net_state().local_army());
    }

    if (!osc::lua::mp_net_state().resume_error.empty()) {
        lua_pushstring(uiL, "__osc_front_end_notice");
        lua_pushstring(uiL, osc::lua::mp_net_state().resume_error.c_str());
        lua_rawset(uiL, LUA_REGISTRYINDEX);
        lua_pushstring(uiL, "__osc_return_to_lobby");
        lua_pushboolean(uiL, 1);
        lua_rawset(uiL, LUA_REGISTRYINDEX);
        return;
    }

    // Re-install instrument harness on new sim VM (M166)
    if (instrument_harness && sim_lua_state) {
        instrument_harness->install_panic_handler(sim_lua_state->raw());
        instrument_harness->install_global_interceptor(sim_lua_state->raw());
        instrument_harness->install_all_method_interceptors(sim_lua_state->raw());
    }

    // Build the game interface; the loading dialog fades out. A
    // loaded game's waits for its post-load (the next tick of a
    // restore, the end of a catch-up): see
    // finish_world_ui_after_post_load.
    if (!loading_save) {
        world_ui_after_post_load.reset(); // a load left before its post-load
        finish_world_ui(uiL, wld_provider, active_playback.has_value(), sim_lua_state.get(),
                        sim_state.get(), world_interp.sync);
    }
}

void App::Window::abandon_load() {
    if (!loading) return;
    loading->job_here = nullptr;
    if (load_thread) load_thread->wait();
    osc::lua::watch_lua_state(nullptr, {});
    // A restore's sim is still the load's: the App's, to go as a game's
    // does (its store is bound to its Lua state)
    adopt_world_load(loading->world, sim_lua_state, sim_state, scenario_meta, ui_lua_state,
                     &world_interp);
    loading.reset();
}

void App::Window::handle_return_to_lobby() {
    // (asked for while a game loads: once it is in)
    if (loading) return;
    // Check for return-to-lobby request (M156b — score screen "Continue")
    {
        lua_State* uiL = ui_lua_state.raw();
        lua_pushstring(uiL, "__osc_return_to_lobby");
        lua_rawget(uiL, LUA_REGISTRYINDEX);
        if (lua_toboolean(uiL, -1)) {
            lua_pop(uiL, 1);
            quiesce_sim(); // the game being left finishes its tick first

            // Clear the flag
            lua_pushstring(uiL, "__osc_return_to_lobby");
            lua_pushnil(uiL);
            lua_rawset(uiL, LUA_REGISTRYINDEX);

            spdlog::info("Returning to lobby...");
            world_ui_after_post_load.reset(); // a load abandoned before its post-load

            // Tear down any multiplayer session/transport before the
            // sim it references is destroyed.
            osc::lua::mp_teardown();
            save_last_game();
            perf_stretch.clear();
            game_frames.last_drawn = game_frames.last_pace = -1.0;
            active_playback.reset();
            catch_up.reset();
            session_launch.reset();

            // Tear down game state
            wld_provider.destroy_game_interface(uiL);
            renderer.clear_scene();
            detach_ui_from_sim(uiL);
            sim_state.reset();
            sim_lua_state.reset();
            // Its blueprints' Lua references went with it: the store holds
            // none until the next game rebinds it. (It had kept the freed
            // state, and releasing its references there at exit crashed a
            // game quit from the front end after a game.)
            store.rebind(nullptr);

            // Reset game state
            game_state_mgr.set_game_over(false);
            game_state_mgr.set_paused(false);
            sim_accumulator = 0.0;
            input_handler.set_selected({});
            prev_selection.clear();

            // Why the game ended, when it wasn't the player's doing
            // (a saved game that didn't load as it was played): read
            // before the game's UI state goes.
            std::string notice;
            lua_pushstring(uiL, "__osc_front_end_notice");
            lua_rawget(uiL, LUA_REGISTRYINDEX);
            if (lua_type(uiL, -1) == LUA_TSTRING) notice = lua_tostring(uiL, -1);
            lua_pop(uiL, 1);

            // The front end gets a fresh UI state, as in Moho (M191
            // step 4), built as at boot.
            renderer.forget_ui_controls();
            reset_ui_state();
            uiL = ui_lua_state.raw();
            publish_window_objects();

            // Transition to FRONT_END (SetupUI, the front end's UI state's)
            // and show the main menu, as Moho's UI_StartFrontEnd does (with
            // the LAN dialog).
            osc::set_ui_state(osc::UIState::FrontEnd);
            game_state_mgr.transition_to(osc::GameState::FRONT_END, uiL);
            if (auto shown = ui_lua_state.do_string("EngineStartFrontEndUI()"); !shown)
                spdlog::warn("Front end: {}", shown.error().message);

            if (!notice.empty()) {
                lua_pushstring(uiL, "__osc_notice");
                lua_pushstring(uiL, notice.c_str());
                lua_rawset(uiL, LUA_GLOBALSINDEX);
                auto shown = ui_lua_state.do_string(
                    "import('/lua/ui/uiutil.lua').ShowInfoDialog(GetFrame(0), "
                    "__osc_notice, '<LOC _Ok>')");
                if (!shown) spdlog::warn("Front-end notice: {}", shown.error().message);
            }

            spdlog::info("=== Returned to lobby ===");
        } else {
            lua_pop(uiL, 1);
        }
    }
}

} // namespace osc::app
