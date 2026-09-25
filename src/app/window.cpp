// The windowed loop: the renderer, input, the sim at its tick rate and the
// UI's frames (M192 step 2b, moved from run()).

#include "app/app_internal.hpp"
#include "core/fixed_step.hpp"
#include "core/image.hpp"
#include "core/profiler.hpp"
#include "core/test_status.hpp"
#include "lua/factory_queue.hpp"
#include "lua/lan_dialog_ui.hpp"
#include "lua/lan_lobby.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/smoke_test.hpp"
#include "platform/paths.hpp"
#include "renderer/input_handler.hpp"
#include "renderer/renderer.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/sim_callback_queue.hpp"

extern "C" {
#include <lua.h>
}

#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <spdlog/spdlog.h>

namespace osc::app {

std::optional<int> App::run_window() {
    osc::renderer::Renderer renderer;
    // Scripted captures and checks render offscreen: no window to show,
    // focus to steal, or compositor to wait for.
    const bool offscreen_capture = !parse_string_arg(argc, argv, "--screenshot", "").empty() ||
                                   !parse_string_arg(argc, argv, "--golden", "").empty() ||
                                   opt.scripted_window;
    if (renderer.init(1600, 900, "OpenSupCom", offscreen_capture)) {
        // Build 3D scene if we have a sim state (--map was provided)
        if (sim_state) {
            renderer.build_scene(sim_state->terrain(), sim_state->blueprint_store(),
                                 osc::sim::world_blueprints(*sim_state), &vfs, ui_lua_state.raw());
        }

        // Initialize texture/font caches for UI rendering (normally done in build_scene)
        if (!sim_state) {
            renderer.init_ui_caches(&vfs);
        }

        // Player input handler (ARMY_1 = index 0)
        osc::renderer::InputHandler input_handler;
        input_handler.set_player_army(0);
        renderer.set_player_army(0);

        // Factory queue display (M140c)
        osc::lua::FactoryQueueDisplay factory_queue;

        // SimCallback queue (UI→Sim bridge, M138a)
        osc::sim::SimCallbackQueue sim_callback_queue;

        // The window's objects in the UI state's registry: the renderer
        // (WorldView, GetCamera), the input handler (selection), the factory
        // queue display (M140c), the SimCallback queue (M138a), and no hover
        // yet (M142a). Again for each new UI state (M191 step 4).
        const auto publish_window_objects = [&] {
            lua_State* uL = ui_lua_state.raw();
            const auto publish = [&](const char* key, void* object) {
                lua_pushstring(uL, key);
                lua_pushlightuserdata(uL, object);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            };
            publish("__osc_renderer", &renderer);
            publish("__osc_input_handler", &input_handler);
            publish("__osc_factory_queue", &factory_queue);
            publish("__osc_sim_callback_queue", &sim_callback_queue);
            lua_pushstring(uL, "__osc_hover_entity_id");
            lua_pushnumber(uL, 0);
            lua_rawset(uL, LUA_REGISTRYINDEX);
        };
        publish_window_objects();

        // Store scenario path for SessionGetScenarioInfo (M145c2)
        if (!opt.map_path.empty()) {
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "__osc_scenario_path");
            lua_pushstring(uL, opt.map_path.c_str());
            lua_rawset(uL, LUA_REGISTRYINDEX);
        }

        // keymap_registry already stored in registry at outer scope

        // GameStateManager and BeatFunctionRegistry already declared at outer scope (M144b, M145b)

        if (opt.no_fog) renderer.set_fog_enabled(false);
        if (opt.no_decals) renderer.set_decals_enabled(false);
        if (opt.legacy_hud) renderer.set_legacy_hud(true);

        double sim_accumulator = 0.0;
        std::optional<osc::sim::ReplayPlayback> active_playback; // a replay being watched
        double paused_beat_accumulator = 0.0;

        // FA's command mode drives world clicks (read once per frame).
        osc::renderer::CommandMode current_command_mode;
        bool ghost_from_mode = false;
        input_handler.set_command_mode_hooks({[&] { return current_command_mode; },
                                              [&](const osc::renderer::IssuedCommand& c) {
                                                  report_command_issued(ui_lua_state.raw(), c);
                                              },
                                              [&] { cancel_command_mode(ui_lua_state.raw()); }});
        auto prev_time = std::chrono::high_resolution_clock::now();
        bool p_was_pressed = false;
        bool plus_was_pressed = false;
        bool minus_was_pressed = false;
        bool esc_was_pressed = false;
        double title_update_timer = 0.0;
        double fps_accum = 0.0;
        int fps_frames = 0;
        double display_fps = 0.0;
        std::unordered_set<osc::u32> prev_selection;

        // Windowed LAN entry: create the transport up front so the front-end
        // can host/join while it renders; the game loop then drives the lobby
        // handshake to launch. Same networking path as --lan-host/--lan-join
        // (verified headless). Absent these flags, single-player is untouched.
        bool lan_launch_fired = false;
        bool lan_host_cfg_set = false;
        {
            std::string lwp = parse_string_arg(argc, argv, "--mp-port", "47624");
            auto lport = static_cast<osc::u16>(std::strtoul(lwp.c_str(), nullptr, 10));
            if (parse_flag(argc, argv, "--lan-window-host")) {
                osc::lua::mp_begin_host(lport);
            }
            std::string lwj = parse_string_arg(argc, argv, "--lan-window-join", "");
            if (!lwj.empty()) osc::lua::mp_begin_join(lwj, lport);
        }

        // --screenshot <png> [--screenshot-frame N]: render N frames on a
        // fixed 60 Hz clock (so frame N is identical run to run), capture
        // the presented image, write it, and exit. Used for golden-image
        // tests and documentation shots.
        // --golden <name> [--golden-update]: capture like --screenshot and
        // compare against <golden dir>/<name>.png (OSC_GOLDEN_DIR, else the
        // user state dir). Goldens contain game art, so they live outside
        // the repository; a missing golden exits 77 (CTest "skipped").
        const std::string golden_name = parse_string_arg(argc, argv, "--golden", "");
        const bool golden_update = parse_flag(argc, argv, "--golden-update");
        osc::fs::path golden_path;
        if (!golden_name.empty()) {
            auto env_dir = osc::platform::system_env()("OSC_GOLDEN_DIR");
            osc::fs::path dir =
                env_dir && !env_dir->empty()
                    ? osc::fs::path(*env_dir)
                    : osc::platform::known_folder(osc::platform::KnownFolder::State) /
                          "opensupcom" / "golden";
            std::error_code ec;
            osc::fs::create_directories(dir, ec);
            golden_path = dir / (golden_name + ".png");
        }
        const std::string screenshot_path =
            !golden_name.empty()
                ? (golden_update
                       ? golden_path.string()
                       : (golden_path.parent_path() / (golden_name + ".actual.png")).string())
                : parse_string_arg(argc, argv, "--screenshot", "");
        const osc::u32 screenshot_frame = static_cast<osc::u32>(std::strtoul(
            parse_string_arg(argc, argv, "--screenshot-frame", "120").c_str(), nullptr, 10));
        constexpr double kScreenshotFrameDt = 1.0 / 60.0;
        // Scripted windowed runs (a test mode's, --replay-flow-test): four
        // frames per sim tick, on a fixed clock.
        constexpr double kInterpFrameDt = osc::sim::SimState::SECONDS_PER_TICK / 4.0;
        // A loaded game catching up ticks for this long each frame.
        constexpr auto kCatchUpFrameBudget = std::chrono::milliseconds(100);
        if (opt.scripted_window) {
            renderer.set_fixed_frame_dt(static_cast<osc::f32>(kInterpFrameDt));
            renderer.camera().set_input_enabled(false);
        }
        osc::u32 frames_rendered = 0;
        bool screenshot_done = false;
        bool screenshot_ok = false;
        if (!screenshot_path.empty()) {
            renderer.set_fixed_frame_dt(static_cast<osc::f32>(kScreenshotFrameDt));
            // Golden images must not depend on where the mouse happens to be.
            renderer.camera().set_input_enabled(false);
        }
        // --camera <x>,<z>,<distance>: initial camera placement (world units).
        {
            const std::string cam = parse_string_arg(argc, argv, "--camera", "");
            float cx = 0, cz = 0, dist = 0;
            if (!cam.empty()) {
                if (std::sscanf(cam.c_str(), "%f,%f,%f", &cx, &cz, &dist) == 3 && dist > 0) {
                    renderer.camera().set_target(cx, cz);
                    renderer.camera().set_distance(dist);
                } else {
                    spdlog::error("--camera expects <x>,<z>,<distance>, got '{}'", cam);
                    return 1;
                }
            }
        }

        // Open the replay (--watch, --replay-flow-test) through the same
        // globals retail's replay dialog calls; the loop then launches it.
        bool replay_flow_done = false;
        osc::u32 replay_flow_frames = 0;
        if (!opt.watch_path.empty() || opt.replay_flow_test) {
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "__osc_watch_file");
            lua_pushstring(uL, opt.watch_path.c_str());
            lua_rawset(uL, LUA_GLOBALSINDEX);
            auto opened = ui_lua_state.do_string(opt.replay_flow_test ? R"(
                    local data = GetSpecialFiles('Replay')
                    local profile, name
                    for p, names in data.files do
                        if names[1] then
                            profile, name = p, names[1]
                            break
                        end
                    end
                    if not name then error('GetSpecialFiles lists no replay') end
                    local info = GetSpecialFileInfo(profile, name, 'Replay')
                    if not info or not info.WriteTime or not info.TimeStamp then
                        error('GetSpecialFileInfo does not describe ' .. name)
                    end
                    __osc_watch_file = data.directory .. profile .. '/' .. name .. '.' ..
                                       data.extension
                    if LaunchReplaySession(__osc_watch_file) ~= true then
                        error('LaunchReplaySession refused ' .. __osc_watch_file)
                    end
                )"
                                                                      : R"(
                    if LaunchReplaySession(__osc_watch_file) ~= true then
                        error('cannot play ' .. __osc_watch_file)
                    end
                )");
            if (!opened) {
                spdlog::error("Replay: {}", opened.error().message);
                if (opt.replay_flow_test) {
                    osc::test_status::fail("[FAIL] replay-flow: {}", opened.error().message);
                    return finish_test_run("replay-flow-test");
                }
            }
        }

        // Open the saved game (--load, --load-flow-test) through the global
        // retail's Load dialog calls; the loop then loads it.
        // --load-flow-test, as a player might go (M208a, M191 step 4):
        //  0. the listed save loads from the front end, catches up (never a
        //     replay meanwhile), plays on, and is saved again;
        //  1. that save loads from inside the game, as the game menu's Load
        //     dialog loads it, into a fresh UI state;
        //  2. back to the front end (ReturnToLobby), a fresh state again;
        //  3. the first save loads from there once more.
        // Each game starts in a UI state of its own, so what the flow needs
        // (the save's profile and file) is kept here, not in Lua globals.
        bool load_flow_done = false;
        int load_flow_phase = 0;
        bool load_flow_saw_catch_up = false;          // this load began catching up
        std::optional<osc::u32> load_flow_resumed_at; // ...and was the player's from here
        std::vector<osc::u32> load_flow_resumes;      // every load's resume tick
        osc::u32 load_flow_frames = 0;
        osc::u32 load_flow_front_end_frames = 0;
        std::string load_flow_profile;
        std::string load_flow_file;
        const auto load_flow_globals = [&] { // into the current UI state
            lua_State* uL = ui_lua_state.raw();
            for (const auto& [name, value] : {std::pair{"__osc_load_profile", &load_flow_profile},
                                              std::pair{"__osc_load_file", &load_flow_file}}) {
                lua_pushstring(uL, name);
                lua_pushstring(uL, value->c_str());
                lua_rawset(uL, LUA_GLOBALSINDEX);
            }
        };
        if (!opt.load_path.empty() || opt.load_flow_test) {
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "__osc_load_file");
            lua_pushstring(uL, opt.load_path.c_str());
            lua_rawset(uL, LUA_GLOBALSINDEX);
            auto opened = ui_lua_state.do_string(opt.load_flow_test ? R"(
                    local data = GetSpecialFiles('SaveGame')
                    for p, names in data.files do
                        if names[1] then
                            __osc_load_profile, __osc_load_name = p, names[1]
                            break
                        end
                    end
                    if not __osc_load_name then error('GetSpecialFiles lists no saved game') end
                    local folder = data.directory .. __osc_load_profile .. '/'
                    local worked, err = LoadSavedGame(folder .. 'missing.' .. data.extension)
                    if worked or err ~= 'CantOpen' then
                        error('a missing save: ' .. tostring(worked) .. ', ' .. tostring(err))
                    end
                    __osc_load_file = folder .. __osc_load_name .. '.' .. data.extension
                    local detail
                    worked, err, detail = LoadSavedGame(__osc_load_file)
                    if not worked then
                        error('LoadSavedGame refused ' .. __osc_load_file .. ': ' .. tostring(err) ..
                              ' ' .. tostring(detail))
                    end
                )"
                                                                    : R"(
                    local worked, err = LoadSavedGame(__osc_load_file)
                    if not worked then
                        error('cannot load ' .. __osc_load_file .. ': ' .. tostring(err))
                    end
                )");
            if (opened && opt.load_flow_test) {
                const auto global = [&](const char* name) {
                    lua_pushstring(uL, name);
                    lua_rawget(uL, LUA_GLOBALSINDEX);
                    std::string value = lua_type(uL, -1) == LUA_TSTRING ? lua_tostring(uL, -1) : "";
                    lua_pop(uL, 1);
                    return value;
                };
                load_flow_profile = global("__osc_load_profile");
                load_flow_file = global("__osc_load_file");
            }
            if (!opened) {
                spdlog::error("Saved game: {}", opened.error().message);
                if (opt.load_flow_test) {
                    osc::test_status::fail("[FAIL] load-flow: {}", opened.error().message);
                    return finish_test_run("load-flow-test");
                }
            }
        }

        // --load-flow-test, once the loaded game has played on: save it again
        // as retail's Save dialog does, and check what saving refuses.
        auto save_again_in_load_flow = [&] {
            load_flow_globals();
            auto saved = ui_lua_state.do_string(R"(
                if SessionIsReplay() then error('a loaded game is a replay') end
                local data = GetSpecialFiles('SaveGame')
                local folder = data.directory .. __osc_load_profile .. '/'
                __osc_again_file = folder .. 'again.' .. data.extension
                local result
                InternalSaveGame(__osc_again_file, 'again', function(worked, errmsg)
                    result = {worked = worked, errmsg = errmsg}
                end)
                if not result then error('InternalSaveGame never called back') end
                if not result.worked then
                    error('InternalSaveGame failed: ' .. tostring(result.errmsg))
                end
                if not GetSpecialFileInfo(__osc_load_profile, 'again', 'SaveGame') then
                    error('the new save is not listed')
                end
                local refused
                InternalSaveGame(folder .. '../../outside.' .. data.extension, 'outside',
                                 function(worked) refused = not worked end)
                if not refused then error('InternalSaveGame saved outside its folder') end
            )");
            if (!saved) {
                osc::test_status::fail("[FAIL] load-flow: {}", saved.error().message);
                return;
            }
            // The new save holds the whole game, as it stands.
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "__osc_again_file");
            lua_rawget(uL, LUA_GLOBALSINDEX);
            const std::string again = lua_type(uL, -1) == LUA_TSTRING ? lua_tostring(uL, -1) : "";
            lua_pop(uL, 1);
            osc::sim::SavedGame save;
            const osc::u32 tick = sim_state->tick_count();
            if (osc::lua::read_saved_game(again, save) != osc::sim::SaveLoadError::None) {
                osc::test_status::fail("[FAIL] load-flow: the new save {} does not load", again);
            } else if (save.tick != tick || save.game.checksums.size() != tick) {
                osc::test_status::fail("[FAIL] load-flow: the new save holds tick {} and {} "
                                       "checksums, not the game's {}",
                                       save.tick, save.game.checksums.size(), tick);
            }
            if (osc::fs::exists(special_files->root() / "outside.oscsave"))
                osc::test_status::fail("[FAIL] load-flow: a save was written outside its folder");
        };

        while (!renderer.should_close() && !screenshot_done && !(tests && tests->frames_done()) &&
               !replay_flow_done && !load_flow_done) {
            osc::Profiler::instance().begin_frame();
            auto now = std::chrono::high_resolution_clock::now();
            double dt = std::chrono::duration<double>(now - prev_time).count();
            prev_time = now;
            if (!screenshot_path.empty()) dt = kScreenshotFrameDt;
            if (opt.scripted_window) dt = kInterpFrameDt;
            // Clamp dt to avoid spiral of death
            if (dt > 0.25) dt = 0.25;

            // Audio: the camera is the listener, and FA's zoom and angle
            // curves read its distance and pitch.
            {
                const auto& cam = renderer.camera();
                osc::f32 ex = 0, ey = 0, ez = 0;
                cam.eye_position(ex, ey, ez);
                const osc::f32 fx = cam.target_x() - ex;
                const osc::f32 fy = -ey;
                const osc::f32 fz = cam.target_z() - ez;
                const osc::f32 len = std::max(1e-3f, std::sqrt(fx * fx + fy * fy + fz * fz));
                sound.set_listener({ex, ey, ez}, {fx / len, fy / len, fz / len});
                sound.set_global_variable("CameraDistance", cam.distance());
                const osc::f32 zoom_span = std::max(1.0f, cam.max_zoom() - cam.min_zoom());
                sound.set_global_variable("ZoomPercent",
                                          100.0f * (cam.distance() - cam.min_zoom()) / zoom_span);
                sound.set_global_variable("Angle", cam.pitch() * 57.29578f);
                sound.update(static_cast<osc::f32>(dt));
            }

            // FPS tracking
            fps_accum += dt;
            fps_frames++;
            if (fps_accum >= 0.5) {
                display_fps = fps_frames / fps_accum;
                fps_accum = 0.0;
                fps_frames = 0;
            }

            // Pause toggle (P key, edge-triggered)
            bool p_pressed = renderer.is_key_pressed(GLFW_KEY_P);
            if (p_pressed && !p_was_pressed) {
                game_state_mgr.set_paused(!game_state_mgr.paused(), ui_lua_state.raw());
                spdlog::info("Sim {}", game_state_mgr.paused() ? "PAUSED" : "RESUMED");
            }
            p_was_pressed = p_pressed;

            // Speed control (+/- keys, edge-triggered)
            bool plus_pressed =
                renderer.is_key_pressed(GLFW_KEY_EQUAL) || renderer.is_key_pressed(GLFW_KEY_KP_ADD);
            if (plus_pressed && !plus_was_pressed) {
                game_state_mgr.set_speed(std::min(game_state_mgr.speed() * 2.0, 10.0));
                spdlog::info("Sim speed: {:.1f}x", game_state_mgr.speed());
            }
            plus_was_pressed = plus_pressed;

            bool minus_pressed = renderer.is_key_pressed(GLFW_KEY_MINUS) ||
                                 renderer.is_key_pressed(GLFW_KEY_KP_SUBTRACT);
            if (minus_pressed && !minus_was_pressed) {
                game_state_mgr.set_speed(std::max(game_state_mgr.speed() * 0.5, 0.125));
                spdlog::info("Sim speed: {:.1f}x", game_state_mgr.speed());
            }
            minus_was_pressed = minus_pressed;

            // ESC key — call escape handler (M146c)
            bool esc_pressed = renderer.is_key_pressed(GLFW_KEY_ESCAPE);
            if (esc_pressed && !esc_was_pressed) {
                lua_State* uiL = ui_lua_state.raw();
                lua_pushstring(uiL, "__osc_escape_handler");
                lua_rawget(uiL, LUA_REGISTRYINDEX);
                if (lua_isfunction(uiL, -1)) {
                    if (lua_pcall(uiL, 0, 0, 0) != 0) {
                        spdlog::warn("ESC handler error: {}", lua_tostring(uiL, -1));
                        lua_pop(uiL, 1);
                    }
                } else {
                    lua_pop(uiL, 1);
                }
            }
            esc_was_pressed = esc_pressed;

            note_game_over_if_ended(sim_state.get(), game_state_mgr, ui_lua_state.raw());

            // Fixed-timestep sim ticking (scaled by sim_speed)
            const osc::u32 beat_tick0 = sim_state ? sim_state->tick_count() : 0;
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
                    sim_accumulator += dt * game_state_mgr.speed();
                    auto* session = osc::lua::mp_net_state().session.get();
                    int guard = 0;
                    while (sim_accumulator >= osc::sim::SimState::SECONDS_PER_TICK && guard++ < 4) {
                        sim_accumulator -= osc::sim::SimState::SECONDS_PER_TICK;
                        osc::lua::mp_pump(); // drain mux game channel + peers
                        session->send_frame();
                        session->receive_and_advance();
                        // A timed-out peer is defeated so the match resolves
                        // instead of stalling in "waiting for players": the
                        // survivors agree on its last frame, and the session
                        // defeats its army on the same tick on every one of
                        // them (a command, so replays keep it).
                        for (osc::u32 src : session->take_dropped())
                            spdlog::warn("[mp] peer {} dropped — its army is defeated", src);
                    }
                } else {
                    // At most 8 ticks per frame; a slower-than-real-time
                    // sim slows the game rather than stalling every frame.
                    const int ticks =
                        osc::consume_fixed_steps(sim_accumulator, dt * game_state_mgr.speed(),
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

            // Process SimCallbacks from UI (M138a)
            if (sim_state && sim_lua_state) submit_sim_callbacks(sim_callback_queue, *sim_state);

            // --replay-flow-test ends when the replay has played out.
            if (opt.replay_flow_test) {
                ++replay_flow_frames;
                if (active_playback && sim_state && active_playback->finished(*sim_state))
                    replay_flow_done = true;
                else if (replay_flow_frames > 40000)
                    replay_flow_done = true; // stuck: reported below
            }

            // --load-flow-test: the saved game catches up -- never a replay
            // meanwhile -- then plays on a little and is saved again.
            if (opt.load_flow_test && !load_flow_done) {
                ++load_flow_frames;
                if (sim_state && sim_state->resuming() && !load_flow_saw_catch_up) {
                    load_flow_saw_catch_up = true;
                    auto r = ui_lua_state.do_string(
                        "if SessionIsReplay() then error('SessionIsReplay() is true while a "
                        "saved game catches up') end");
                    if (!r) osc::test_status::fail("[FAIL] load-flow: {}", r.error().message);
                }
                if (sim_state && load_flow_saw_catch_up && !catch_up && !load_flow_resumed_at) {
                    load_flow_resumed_at = sim_state->tick_count();
                    load_flow_resumes.push_back(*load_flow_resumed_at);
                }
                const bool played_on = sim_state && load_flow_resumed_at &&
                                       sim_state->tick_count() >= *load_flow_resumed_at + 50;
                const auto next_load = [&] {
                    load_flow_saw_catch_up = false;
                    load_flow_resumed_at.reset();
                };
                const auto run = [&](const char* code) {
                    load_flow_globals();
                    if (auto r = ui_lua_state.do_string(code); !r)
                        osc::test_status::fail("[FAIL] load-flow: {}", r.error().message);
                };
                if (load_flow_phase == 0 && played_on) {
                    save_again_in_load_flow();
                    // The game menu's Load dialog: load, then destroy the
                    // control it was opened over, GetFrame(0) in a game.
                    run(R"(
                        local worked, err = LoadSavedGame(__osc_again_file)
                        if not worked then error('LoadSavedGame in a game: ' .. tostring(err)) end
                        GetFrame(0):Destroy()
                    )");
                    next_load();
                    load_flow_phase = 1;
                } else if (load_flow_phase == 1 && played_on) {
                    run("ReturnToLobby()");
                    load_flow_front_end_frames = 0;
                    load_flow_phase = 2;
                } else if (load_flow_phase == 2 && !sim_state &&
                           ++load_flow_front_end_frames > 20) {
                    run(R"(
                        local worked, err = LoadSavedGame(__osc_load_file)
                        if not worked then error('LoadSavedGame from the front end: ' ..
                                                 tostring(err)) end
                    )");
                    next_load();
                    load_flow_phase = 3;
                } else if (load_flow_phase == 3 && played_on) {
                    load_flow_done = true;
                } else if (load_flow_frames > 60000) {
                    load_flow_done = true; // stuck: reported below
                }
            }

            // OnFirstUpdate — fire once after first sim tick
            static bool first_update_fired = false;
            if (!first_update_fired && sim_state && sim_state->tick_count() > 0) {
                osc::core::call_on_first_update(ui_lua_state.raw());
                first_update_fired = true;
            }

            renderer.poll_events(dt);

            // Resume UI coroutines
            ++ui_frame_count;
            osc::lua::advance_ui_clock(ui_lua_state.raw(), dt);
            ui_thread_manager.resume_all(ui_frame_count);

            // OnBeat — UI heartbeat each frame
            osc::core::call_on_beat(ui_lua_state.raw(), dt);

            // Fire beat functions each frame (M145b)
            beat_registry.fire_all(ui_lua_state.raw());

            // This frame's world, between the last two ticks: what is
            // drawn and what clicks pick.
            const osc::sim::FrameView frame_view = world_interp.view();
            input_handler.set_frame_view(frame_view);
            if (tests && sim_state) {
                Frame frame{frame_view, renderer, input_handler};
                tests->frame_view(engine, frame);
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
                dispatch_selection_change(ui_lua_state.raw(), prev_selection,
                                          input_handler.selected(),
                                          input_handler.take_selection_event());
            } else {
                (void)input_handler.take_selection_event();
            }

            const auto& sel = input_handler.selected();
            if (!screenshot_path.empty() &&
                ++frames_rendered == std::max<osc::u32>(screenshot_frame, 1)) {
                const bool requested = renderer.request_capture([&](osc::ImageRGBA8 image) {
                    screenshot_ok = osc::write_png(screenshot_path, image);
                    screenshot_done = true;
                    spdlog::info("Screenshot {}x{} -> {} ({})", image.width, image.height,
                                 screenshot_path, screenshot_ok ? "written" : "WRITE FAILED");
                });
                if (!requested) {
                    spdlog::error("Screenshot: swapchain readback unsupported");
                    screenshot_done = true;
                }
            }
            if (sim_state) {
                const auto ghost = input_handler.build_ghost(renderer, *sim_state);
                renderer.render(frame_view, world_interp.history.events(),
                                ghost ? &*ghost : nullptr, ui_lua_state.raw(), &ui_registry,
                                sel.empty() ? nullptr : &sel);
                if (tests) {
                    Frame frame{frame_view, renderer, input_handler};
                    tests->frame_rendered(engine, frame);
                }
            } else {
                // No sim state (front-end/lobby) — render UI only
                renderer.render_ui_only(ui_lua_state.raw(), &ui_registry);
            }

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
                    std::snprintf(
                        title, sizeof(title),
                        "OpenSupCom | %s%.1fx | T:%u (%.1fs) | %zu entities | %zu sel | %.0f FPS",
                        status_str, game_state_mgr.speed(), sim_state->tick_count(),
                        sim_state->game_time(), sim_state->entity_registry().count(), sel.size(),
                        display_fps);
                } else {
                    std::snprintf(title, sizeof(title), "OpenSupCom | Lobby | %.0f FPS",
                                  display_fps);
                }
                renderer.set_window_title(title);
            }

            // Check for exit request (M146d)
            {
                lua_State* uiL = ui_lua_state.raw();
                lua_pushstring(uiL, "__osc_exit_requested");
                lua_rawget(uiL, LUA_REGISTRYINDEX);
                if (lua_toboolean(uiL, -1)) {
                    lua_pop(uiL, 1);
                    break;
                }
                lua_pop(uiL, 1);
            }

            // LAN lobby: drive the host/client handshake during the
            // front-end and fire the launch barrier. Inert unless a LAN
            // transport exists (only when the LAN window flags were set).
            {
                auto& mpn = osc::lua::mp_net_state();
                auto* lob = osc::lua::mp_lobby();
                if (lob && !mpn.active() && !lan_launch_fired) {
                    if (lob->role() == osc::lua::LanLobby::Role::Host && !lan_host_cfg_set) {
                        lob->set_host_config(osc::lua::LanSessionConfig{
                            "/maps/SCMP_009/SCMP_009_scenario.lua", mpn.seed});
                        lan_host_cfg_set = true;
                    }
                    osc::lua::mp_pump();
                    lob->poll();
                    if (lob->role() == osc::lua::LanLobby::Role::Host &&
                        lob->state() == osc::lua::LanLobby::State::Ready) {
                        lob->request_launch();
                    }
                    if (lob->launch_ready()) {
                        lan_launch_fired = true;
                        mpn.seed = lob->config().seed;
                        lan_launch_session(ui_lua_state.raw(), lob->config().scenario);
                    }
                }
            }

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

                        // The game gets a fresh UI state, as Moho gives each
                        // game one (M191 step 4): the front end's, or the last
                        // game's, goes with its controls and threads.
                        wld_provider.destroy_game_interface(uiL);
                        renderer.forget_ui_controls();
                        reset_ui_state();
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
                        pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 1,
                                       ui_frame_count);
                        renderer.render_ui_only(ui_lua_state.raw(), &ui_registry);

                        // Execute reload in stages, pumping UI frames between each
                        execute_reload_sequence(sim_lua_state, sim_state, ui_lua_state, vfs, store,
                                                loader, config, scenario_meta, game_state_mgr,
                                                &renderer, &input_handler, &prev_selection,
                                                &world_interp,
                                                launch_seed(opt.seed_arg, opt.reproducible_run),
                                                sim_accumulator, launch_scenario, recorded);
                        if (launch_replay && sim_state) {
                            // Watched as an observer, as the replay plays.
                            active_playback.emplace(std::move(*launch_replay));
                            active_playback->start(*sim_state);
                            lua_pushstring(uiL, "__osc_focus_army");
                            lua_pushnumber(uiL, -1);
                            lua_rawset(uiL, LUA_REGISTRYINDEX);
                        }
                        if (launch_save && sim_state) {
                            // The player's game, caught up first; recorded
                            // from its start (once its orders and command
                            // delay are queued), so it saves again whole.
                            catch_up.emplace(std::move(launch_save->game));
                            catch_up->resume(*sim_state);
                            sim_state->set_recording(true);
                            spdlog::info("Saved game '{}': catching up to tick {}",
                                         launch_save->name, launch_save->tick);
                        }

                        // Reset per-session state for the new game
                        first_update_fired = false;

                        // Multiplayer: if the lobby stood up a network
                        // transport (HostGame/JoinGame), build the lockstep
                        // session over it now that the game's sim exists.
                        // No-op in single-player.
                        if (sim_state && !active_playback) {
                            osc::lua::mp_attach_session(*sim_state);
                        }

                        // Re-install instrument harness on new sim VM (M166)
                        if (instrument_harness && sim_lua_state) {
                            instrument_harness->install_panic_handler(sim_lua_state->raw());
                            instrument_harness->install_global_interceptor(sim_lua_state->raw());
                            instrument_harness->install_all_method_interceptors(
                                sim_lua_state->raw());
                        }

                        // Build the game interface; the loading dialog fades out
                        finish_world_ui(ui_lua_state.raw(), wld_provider,
                                        active_playback.has_value());
                    }
                } else {
                    lua_pop(uiL, 1);
                }
            }

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

                    // Transition to FRONT_END (SetupUI) and show the main menu
                    game_state_mgr.transition_to(osc::GameState::FRONT_END, uiL);
                    if (auto shown =
                            ui_lua_state.do_string("import('/lua/ui/menus/main.lua').CreateUI()");
                        !shown)
                        spdlog::warn("Front-end CreateUI error: {}", shown.error().message);

                    if (!notice.empty()) {
                        lua_pushstring(uiL, "__osc_notice");
                        lua_pushstring(uiL, notice.c_str());
                        lua_rawset(uiL, LUA_GLOBALSINDEX);
                        auto shown = ui_lua_state.do_string(
                            "import('/lua/ui/uiutil.lua').ShowInfoDialog(GetFrame(0), "
                            "__osc_notice, '<LOC _Ok>')");
                        if (!shown) spdlog::warn("Front-end notice: {}", shown.error().message);
                    }

                    // The LAN dialog on the fresh front end.
                    if (auto lr = ui_lua_state.do_string(osc::lua::kLanDialogLua); !lr)
                        spdlog::warn("LAN dialog UI (relobby) error: {}", lr.error().message);

                    spdlog::info("=== Returned to lobby ===");
                } else {
                    lua_pop(uiL, 1);
                }
            }

            osc::Profiler::instance().end_frame();
        }

        save_last_game(); // quitting leaves the game being played
        renderer.shutdown();
        if (opt.load_flow_test) {
            // The first save resumes at its tick, the one saved in the game
            // 50 ticks later, then the first again.
            const bool all = load_flow_resumes.size() == 3 &&
                             load_flow_resumes[1] == load_flow_resumes[0] + 50 &&
                             load_flow_resumes[2] == load_flow_resumes[0];
            if (!all) {
                std::string got;
                for (const osc::u32 t : load_flow_resumes) got += fmt::format(" {}", t);
                osc::test_status::fail("[FAIL] load-flow: stopped in phase {}; the loads "
                                       "resumed at:{}",
                                       load_flow_phase, got.empty() ? " none" : got);
            } else if (osc::test_status::failure_count() == 0) {
                spdlog::info("[PASS] load-flow: loaded from the front end (tick {}), saved "
                             "again, loaded that from the game (tick {}), back to the lobby, "
                             "and loaded again (tick {}), each in a fresh UI state",
                             load_flow_resumes[0], load_flow_resumes[1], load_flow_resumes[2]);
            }
            return finish_test_run("load-flow-test");
        }
        if (opt.replay_flow_test) {
            auto is_replay = ui_lua_state.do_string(
                "if not SessionIsReplay() then error('SessionIsReplay() is false') end");
            if (!active_playback || !sim_state) {
                osc::test_status::fail("[FAIL] replay-flow: the replay never launched");
            } else if (!active_playback->finished(*sim_state)) {
                osc::test_status::fail("[FAIL] replay-flow: stopped at tick {} of {}",
                                       sim_state->tick_count(),
                                       active_playback->replay().final_tick);
            } else if (active_playback->diverged_at() != 0) {
                osc::test_status::fail("[FAIL] replay-flow: diverged at tick {}",
                                       active_playback->diverged_at());
            } else if (!is_replay) {
                osc::test_status::fail("[FAIL] replay-flow: {}", is_replay.error().message);
            } else {
                spdlog::info("[PASS] replay-flow: {} ticks watched in the game, matching "
                             "the recording",
                             sim_state->tick_count());
            }
            return finish_test_run("replay-flow-test");
        }
        if (tests) {
            if (auto code = tests->after_window()) return code;
        }
        if (!screenshot_path.empty() && osc::renderer::Renderer::validation_error_count() > 0) {
            spdlog::error("{} Vulkan validation error(s) during the capture run",
                          osc::renderer::Renderer::validation_error_count());
            return 1;
        }
        if (!golden_name.empty() && !golden_update) {
            if (!screenshot_ok) return 1;
            auto golden = osc::read_png(golden_path);
            if (!golden) {
                spdlog::warn("No golden image at {} -- record one with "
                             "--golden {} --golden-update",
                             golden_path.string(), golden_name);
                return kExitSkippedNoData;
            }
            auto actual = osc::read_png(screenshot_path);
            const double tolerance = std::strtod(
                parse_string_arg(argc, argv, "--golden-tolerance", "0.01").c_str(), nullptr);
            auto diff = osc::compare_images(*actual, *golden, 16);
            const bool pass = diff.same_size && diff.fraction_over_threshold <= tolerance;
            spdlog::info("Golden '{}': {:.3f}% of pixels differ (tolerance "
                         "{:.3f}%), mean abs error {:.2f} -> {}",
                         golden_name, diff.fraction_over_threshold * 100.0, tolerance * 100.0,
                         diff.mean_abs_error, pass ? "PASS" : "FAIL");
            if (!diff.same_size) {
                spdlog::error("Golden '{}': size {}x{} != golden {}x{}", golden_name, actual->width,
                              actual->height, golden->width, golden->height);
            }
            return pass ? 0 : 1;
        }
        if (!screenshot_path.empty()) return screenshot_ok ? 0 : 1;
    } else {
        if (parse_flag(argc, argv, "--screenshot") ||
            !parse_string_arg(argc, argv, "--screenshot", "").empty() ||
            !parse_string_arg(argc, argv, "--golden", "").empty()) {
            spdlog::error("Screenshot requested but the renderer failed to "
                          "initialize");
            return 1;
        }
        spdlog::warn("Vulkan init failed — falling back to headless "
                     "(100 ticks)");
        if (sim_state) {
            for (osc::u32 i = 0; i < 100; i++) sim_state->tick();
        }
    }

    // Dump instrument report on exit (M166)
    if (instrument_harness) {
        instrument_harness->print_report(false);
        instrument_harness->write_report_to_file("smoke_report.txt", false);
        spdlog::info("Instrument report written to smoke_report.txt");
    }
    return std::nullopt;
}

} // namespace osc::app
