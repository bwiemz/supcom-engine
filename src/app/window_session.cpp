// The UI's requests between games (M192 step 2c): exit, a launch, and the
// return to the front end.

#include "app/window_loop.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/sim_sync.hpp"

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
    // Check for game launch request from lobby (M148a)
    {
        lua_State* uiL = ui_lua_state.raw();
        lua_pushstring(uiL, "__osc_launch_requested");
        lua_rawget(uiL, LUA_REGISTRYINDEX);
        if (lua_toboolean(uiL, -1)) {
            lua_pop(uiL, 1);

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
            lua_pushstring(uiL, "__osc_launch_replay");
            lua_rawget(uiL, LUA_REGISTRYINDEX);
            if (lua_type(uiL, -1) == LUA_TSTRING) {
                launch_replay = load_replay(lua_tostring(uiL, -1));
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
                osc::sim::SavedGame save;
                if (osc::lua::read_saved_game(lua_tostring(uiL, -1), save) ==
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

            if (!launch_scenario.empty()) {
                spdlog::info("Launch requested: {}{}", launch_scenario,
                             launch_replay ? " (replay)"
                             : launch_save ? " (saved game)"
                                           : "");
                save_last_game(); // the game being left, if any
                active_playback.reset();
                catch_up.reset();
                restored_at.reset();

                // The game gets a fresh UI state, as Moho gives each
                // game one (M191 step 4): the front end's, or the last
                // game's, goes with its controls and threads.
                wld_provider.destroy_game_interface(uiL);
                renderer.forget_ui_controls();
                // (with the game's mods, as its sim will have them)
                const std::string game_mods = launch_setup(uiL, recorded, launch_scenario, 0).mods;
                reset_ui_state(&game_mods);
                uiL = ui_lua_state.raw();
                publish_window_objects();
                // The new state's loading screen has no game yet, even
                // when the last one still runs until the reload (per
                // review), as from the front end.
                detach_ui_from_sim(uiL);

                // Transition to LOADING (SetupUI) and show the loading screen
                game_state_mgr.transition_to(osc::GameState::LOADING, uiL);
                begin_world_ui(uiL, wld_provider);

                // Pump one UI frame to display loading screen
                pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 1, ui_frame_count);
                renderer.render_ui_only(ui_lua_state.raw(), &ui_registry);

                // Execute reload in stages, pumping UI frames between each
                const auto reload = [&] {
                    execute_reload_sequence(sim_lua_state, sim_state, ui_lua_state, vfs, store,
                                            loader, config, scenario_meta, game_state_mgr,
                                            &renderer, &ui_store, &input_handler, &prev_selection,
                                            &world_interp,
                                            launch_seed(opt.seed_arg, opt.reproducible_run),
                                            sim_accumulator, launch_scenario, recorded);
                };
                reload();
                if (launch_replay && sim_state) {
                    // Watched as an observer, as the replay plays.
                    active_playback.emplace(std::move(*launch_replay));
                    active_playback->start(*sim_state);
                    lua_pushstring(uiL, "__osc_focus_army");
                    lua_pushnumber(uiL, -1);
                    lua_rawset(uiL, LUA_REGISTRYINDEX);
                }
                if (launch_save && sim_state) {
                    // The player's game, restored from its snapshot
                    // (M208c). Without one, or if the restore fails
                    // (the sim it spoiled booted again), caught up
                    // from its history instead; recorded from its
                    // start (once its orders and command delay are
                    // queued), so it saves again whole.
                    std::string why;
                    const Restore result = restore_save(*launch_save, why);
                    if (result == Restore::Failed) {
                        spdlog::warn("Saved game '{}': not restored ({}); catching up",
                                     launch_save->name, why);
                        reload();
                    }
                    if (result != Restore::Done && sim_state) {
                        catch_up.emplace(std::move(launch_save->game));
                        catch_up->resume(*sim_state);
                        sim_state->set_recording(true);
                        spdlog::info("Saved game '{}': catching up to tick {} ({})",
                                     launch_save->name, launch_save->tick, why);
                    }
                }

                // Reset per-session state for the new game
                first_update_fired = false;

                // Multiplayer: if the lobby stood up a network
                // transport (HostGame/JoinGame), build the lockstep
                // session over it now that the game's sim exists.
                // No-op in single-player.
                if (sim_state && !active_playback && osc::lua::mp_attach_session(*sim_state)) {
                    // Each peer plays its own source's army, and
                    // sees the world through its intel (M215a); an
                    // observer, all of it.
                    osc::lua::set_focus_army(sim_lua_state ? sim_lua_state->raw() : nullptr, uiL,
                                             osc::lua::mp_net_state().local_army());
                }

                // Re-install instrument harness on new sim VM (M166)
                if (instrument_harness && sim_lua_state) {
                    instrument_harness->install_panic_handler(sim_lua_state->raw());
                    instrument_harness->install_global_interceptor(sim_lua_state->raw());
                    instrument_harness->install_all_method_interceptors(sim_lua_state->raw());
                }

                // Build the game interface; the loading dialog fades out
                finish_world_ui(ui_lua_state.raw(), wld_provider, active_playback.has_value(),
                                sim_lua_state.get(), sim_state.get());
            }
        } else {
            lua_pop(uiL, 1);
        }
    }
}

void App::Window::handle_return_to_lobby() {
    // Check for return-to-lobby request (M156b — score screen "Continue")
    {
        lua_State* uiL = ui_lua_state.raw();
        lua_pushstring(uiL, "__osc_return_to_lobby");
        lua_rawget(uiL, LUA_REGISTRYINDEX);
        if (lua_toboolean(uiL, -1)) {
            lua_pop(uiL, 1);

            // Clear the flag
            lua_pushstring(uiL, "__osc_return_to_lobby");
            lua_pushnil(uiL);
            lua_rawset(uiL, LUA_REGISTRYINDEX);

            spdlog::info("Returning to lobby...");

            // Tear down any multiplayer session/transport before the
            // sim it references is destroyed.
            osc::lua::mp_teardown();
            save_last_game();
            active_playback.reset();
            catch_up.reset();

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
            game_state_mgr.set_paused(false, uiL);
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

            // Transition to FRONT_END (SetupUI) and show the main menu, as
            // Moho's UI_StartFrontEnd does (with the LAN dialog).
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
