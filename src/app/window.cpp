// The windowed loop: the renderer, input, the sim at its tick rate and the
// UI's frames (M192 step 2b, moved from run()); its frame's phases are
// App::Window's (step 2c, window_frame.cpp and window_session.cpp).

#include "core/profiler.hpp"
#include "app/window_loop.hpp"
#include "lua/sim_bindings.hpp"
#include "app/window_commands.hpp"
#include "lua/mp_net_state.hpp"
#include "platform/paths.hpp"
#include "sim/lockstep_session.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace osc::app {

std::optional<int> App::run_window() {
    Window window(*this);
    return window.run();
}

App::Window::Window(App& app)
    : app(app), offscreen_capture(!parse_string_arg(argc, argv, "--screenshot", "").empty() ||
                                  !parse_string_arg(argc, argv, "--golden", "").empty() ||
                                  !opt.render_bench_report.empty() || opt.scripted_window) {}

std::optional<int> App::Window::run() {
    // The render benchmark's frames are 1920x1080 (M223b); captures keep
    // their 1600x900.
    const bool bench = !opt.render_bench_report.empty();
    if (renderer.init(bench ? 1920 : 1600, bench ? 1080 : 900, "OpenSupCom", offscreen_capture)) {
        if (auto code = set_up()) return code;
        while (running()) {
            if (!frame()) break;
        }
        if (auto code = finish()) return code;
    } else if (auto code = run_without_renderer()) {
        return code;
    }

    // Dump instrument report on exit (M166)
    if (instrument_harness) {
        instrument_harness->print_report(false);
        instrument_harness->write_report_to_file("smoke_report.txt", false);
        spdlog::info("Instrument report written to smoke_report.txt");
    }
    return std::nullopt;
}

std::optional<int> App::Window::set_up() {
    // A player's window opens as FA's command line and options say
    // (M217h); captures and scripted checks keep their fixed 1600x900.
    // The UI's root frame: the size the window was asked for, until its
    // swapchain has it (the resize lands a frame or more later), then the
    // swapchain's (review: the boot laid the front end out at 1600x900)
    root_width = renderer.width();
    root_height = renderer.height();
    if (!offscreen_capture) {
        const std::vector<std::string> args(argv, argv + argc);
        const WindowMode mode = open_window(renderer, args, prefs);
        adapter_overridden = mode.overridden;
        root_width = mode.size.width;
        root_height = mode.size.height;
    }
    register_window_commands(console, prefs, adapter_overridden);
    register_option_commands(console);
    for (const auto& m : renderer.display_modes()) display_modes.push_back({m[0], m[1], m[2]});

    // Build 3D scene if we have a sim state (--map was provided)
    if (sim_state) {
        // Blueprints through the UI state's store, whose tables are the
        // state the renderer is given (the sim's store's are the sim's).
        renderer.build_scene(sim_state->terrain(), &ui_store,
                             osc::sim::world_blueprints(*sim_state), &vfs, ui_lua_state.raw());
    }

    // Initialize texture/font caches for UI rendering (normally done in build_scene)
    if (!sim_state) {
        renderer.init_ui_caches(&vfs);
    }

    // Player input handler (ARMY_1 = index 0)
    input_handler.set_player_army(0);
    renderer.set_player_army(0);
    input_handler.set_recon(&renderer.recon()); // clicks pick what it shows

    publish_window_objects();
    // The saved options, as Moho applies them once the window is up
    // (OPTIONS_Apply, M217i). Captures and checks keep the engine's own.
    if (!offscreen_capture) apply_options(ui_lua_state.raw());
    // Moho's /nomusic: its audio engine starts with Music at 0.
    if (std::any_of(opt.cmdline_args.begin(), opt.cmdline_args.end(), [](const std::string& a) {
            return a.size() == 8 && std::equal(a.begin(), a.end(), "/nomusic", [](char x, char y) {
                       return std::tolower(static_cast<unsigned char>(x)) == y;
                   });
        }))
        sound.set_category_volume("Music", 0.0f);

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

    input_handler.set_command_mode_hooks(
        {[this] { return current_command_mode; },
         [this](const osc::renderer::IssuedCommand& c) {
             report_command_issued(ui_lua_state.raw(), c);
         },
         [this] { cancel_command_mode(ui_lua_state.raw()); },
         [this](osc::i32 army, const std::string& bp, osc::f32 x, osc::f32 z, osc::u32 moving) {
             return sim_state && osc::lua::can_build_structure(ui_lua_state.raw(), *sim_state, army,
                                                               bp, x, z, moving);
         },
         [this] { call_command_graph(ui_lua_state.raw(), "OnCommandDragBegin", 0); },
         [this](osc::u32 command, osc::f32 mx, osc::f32 my) {
             lua_State* L = ui_lua_state.raw();
             lua_newtable(L);
             lua_pushstring(L, "Type");
             lua_pushstring(L, "ButtonRelease");
             lua_rawset(L, -3);
             lua_pushstring(L, "MouseX");
             lua_pushnumber(L, mx);
             lua_rawset(L, -3);
             lua_pushstring(L, "MouseY");
             lua_pushnumber(L, my);
             lua_rawset(L, -3);
             lua_pushnumber(L, command);
             call_command_graph(L, "OnCommandDragEnd", 2);
         }});
    prev_time = std::chrono::high_resolution_clock::now();

    // --screenshot <png> [--screenshot-frame N]: render N frames on a
    // fixed 60 Hz clock (so frame N is identical run to run), capture
    // the presented image, write it, and exit. Used for golden-image
    // tests and documentation shots.
    // --golden <name> [--golden-update]: capture like --screenshot and
    // compare against <golden dir>/<name>.png (OSC_GOLDEN_DIR, else the
    // user state dir). Goldens contain game art, so they live outside
    // the repository; a missing golden exits 77 (CTest "skipped").
    golden_name = parse_string_arg(argc, argv, "--golden", "");
    golden_update = parse_flag(argc, argv, "--golden-update");
    if (!golden_name.empty()) {
        auto env_dir = osc::platform::system_env()("OSC_GOLDEN_DIR");
        osc::fs::path dir = env_dir && !env_dir->empty()
                                ? osc::fs::path(*env_dir)
                                : osc::platform::known_folder(osc::platform::KnownFolder::State) /
                                      "opensupcom" / "golden";
        std::error_code ec;
        osc::fs::create_directories(dir, ec);
        golden_path = dir / (golden_name + ".png");
    }
    screenshot_path =
        !golden_name.empty()
            ? (golden_update ? golden_path.string()
                             : (golden_path.parent_path() / (golden_name + ".actual.png")).string())
            : parse_string_arg(argc, argv, "--screenshot", "");
    screenshot_frame = static_cast<osc::u32>(std::strtoul(
        parse_string_arg(argc, argv, "--screenshot-frame", "120").c_str(), nullptr, 10));
    if (opt.scripted_window) {
        renderer.set_fixed_frame_dt(static_cast<osc::f32>(kInterpFrameDt));
        renderer.camera().set_input_enabled(false);
    }
    if (!opt.render_bench_report.empty()) {
        const auto scene = RenderBench::scene_named(opt.render_scene);
        if (!scene) {
            spdlog::error("--render-scene expects battle, late or strategic, got '{}'",
                          opt.render_scene);
            return 1;
        }
        if (opt.load_path.empty()) {
            spdlog::error("--render-bench renders a saved game's scene: pass --load <file>");
            return 1;
        }
        render_bench = std::make_unique<RenderBench>(opt.render_bench_report, *scene,
                                                     opt.render_warmup, opt.render_frames);
        // A fixed clock (the same frames each run), the camera the scene's,
        // and no wait for the display
        renderer.set_fixed_frame_dt(static_cast<osc::f32>(kScreenshotFrameDt));
        renderer.camera().set_input_enabled(false);
        renderer.set_vsync(false);
        // Its zones split each frame's CPU time; its overlay would add to it
        osc::Profiler::instance().set_enabled(true);
        renderer.set_profile_overlay_hidden(true);
    }
    if (!screenshot_path.empty()) {
        renderer.set_fixed_frame_dt(static_cast<osc::f32>(kScreenshotFrameDt));
        // Golden images must not depend on where the mouse happens to be.
        renderer.camera().set_input_enabled(false);
    }
    // --camera <x>,<z>,<zoom>: the camera's first target and zoom (the
    // world's extent across the view)
    {
        const std::string cam = parse_string_arg(argc, argv, "--camera", "");
        float cx = 0, cz = 0, zoom = 0;
        if (!cam.empty()) {
            if (std::sscanf(cam.c_str(), "%f,%f,%f", &cx, &cz, &zoom) == 3 && zoom > 0) {
                renderer.camera().set_zoom(zoom);
                renderer.camera().set_target(cx, cz);
            } else {
                spdlog::error("--camera expects <x>,<z>,<zoom>, got '{}'", cam);
                return 1;
            }
        }
    }

    if (auto code = open_replay()) return code;
    if (auto code = open_saved_game()) return code;
    return start_flows();
}

void App::Window::publish_window_objects() {
    lua_State* uL = ui_lua_state.raw();
    const auto publish = [&](const char* key, void* object) {
        lua_pushstring(uL, key);
        lua_pushlightuserdata(uL, object);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    };
    publish("__osc_renderer", &renderer);
    publish("__osc_range_overlays", &renderer.range_overlays());
    publish("__osc_input_handler", &input_handler);
    publish("__osc_factory_queue", &factory_queue);
    publish("__osc_sim_callback_queue", &sim_callback_queue);
    // The UI's root frame is the window's, and the options screen
    // lists the display's modes (M217h)
    size_root_frame(uL, root_width, root_height);
    publish_adapter_options(uL, display_modes, adapter_overridden);
    publish_fidelity_options(uL);
}

std::optional<int> App::Window::open_replay() {
    // Open the replay (--watch, --replay-flow-test) through the same
    // globals retail's replay dialog calls; the loop then launches it.
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
    return std::nullopt;
}

std::optional<int> App::Window::open_saved_game() {
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
    return std::nullopt;
}

std::optional<int> App::Window::start_flows() {
    // --lan-game-host / --lan-game-join: retail's LAN lobby, from here
    // to a game in lockstep with the other process (M218c).
    if (opt.lan_game_test()) {
        lan_game.emplace(opt.lan_game_host,
                         opt.lan_game_join.empty() ? "127.0.0.1" : opt.lan_game_join,
                         opt.lan_game_port, opt.lan_game_quit_at);
        if (!lan_game->start(ui_lua_state)) {
            lan_game->finish(nullptr);
            return finish_test_run("lan-game-test");
        }
    }

    // --mods-flow-test: a skirmish with mods (M221b), launched from the
    // front end, through retail's lobby and its mod manager (M221c), or
    // a recorded one watched
    if (opt.mods_flow_test) {
        using Launch = osc::app::ModsFlowTest::Launch;
        mods_flow.emplace(!opt.watch_path.empty() ? Launch::Watch
                          : opt.mods_flow_lobby   ? Launch::Lobby
                                                  : Launch::Direct);
        if (!mods_flow->start(ui_lua_state)) {
            mods_flow->finish();
            return finish_test_run("mods-flow-test");
        }
    }
    // --campaign-flow-test: the campaign through retail's screens (M209b)
    if (opt.campaign_flow_test) {
        using Route = osc::app::CampaignFlowTest::Route;
        const Route route = opt.tutorial_flow_test ? Route::Tutorial
                            : opt.outro_flow_test  ? Route::Outro
                                                   : Route::Campaign;
        spdlog::info("=== {} flow test ===", route == Route::Tutorial ? "tutorial"
                                             : route == Route::Outro  ? "outro"
                                                                      : "campaign");
        campaign_flow.emplace(route);
    }
    if (!opt.clicks.empty()) {
        ui_clicks.emplace(opt.clicks);
    }
    return std::nullopt;
}

bool App::Window::running() const {
    return !renderer.should_close() && !screenshot_done && !(tests && tests->frames_done()) &&
           !(render_bench && (render_bench->done() || render_bench->gave_up())) &&
           !replay_flow_done && !load_flow_done && !(lan_game && lan_game->done()) &&
           !gpgnet_done && !(mods_flow && mods_flow->done()) &&
           !(campaign_flow && campaign_flow->done());
}

std::optional<int> App::Window::finish() {
    // The render benchmark's report, while the renderer and the game are up
    if (render_bench) {
        const bool complete = render_bench->done() && sim_state;
        const bool written = complete && render_bench->write(renderer, *sim_state);
        if (written) spdlog::info("Render bench report: {}", render_bench->report_path());
        else
            spdlog::error("--render-bench: {}", complete ? "cannot write the report"
                                                : render_bench->gave_up()
                                                    ? "the saved game never loaded"
                                                    : "the scene never finished");
        renderer.shutdown();
        return written ? 0 : 1;
    }
    save_last_game(); // quitting leaves the game being played
    if (!offscreen_capture) {
        // Where and how big the window was, for the next start (M217h)
        save_window_geometry(prefs, renderer);
        prefs.save();
    }
    renderer.shutdown();
    if (opt.load_flow_test) return finish_load_flow();
    if (lan_game) {
        lan_game->finish(sim_state.get());
        return finish_test_run("lan-game-test");
    }
    if (opt.gpgnet_scripted) {
        // A game the client launched must have played in step (M220b)
        if (sim_state) {
            const auto* session = osc::lua::mp_net_state().session.get();
            spdlog::info("[gpgnet] the game reached tick {}{}", sim_state->tick_count(),
                         session ? ", in lockstep" : "");
            if (session && session->desynced())
                osc::test_status::fail("[FAIL] gpgnet: the game desynced at tick {}",
                                       session->desync_tick());
        }
        return finish_test_run("gpgnet");
    }
    if (mods_flow) {
        mods_flow->finish();
        return finish_test_run("mods-flow-test");
    }
    if (campaign_flow) {
        campaign_flow->finish();
        return finish_test_run(opt.tutorial_flow_test ? "tutorial-flow-test"
                               : opt.outro_flow_test  ? "outro-flow-test"
                                                      : "campaign-flow-test");
    }
    if (opt.replay_flow_test) {
        auto is_replay = ui_lua_state.do_string(
            "if not SessionIsReplay() then error('SessionIsReplay() is false') end");
        if (!active_playback || !sim_state) {
            osc::test_status::fail("[FAIL] replay-flow: the replay never launched");
        } else if (!active_playback->finished(*sim_state)) {
            osc::test_status::fail("[FAIL] replay-flow: stopped at tick {} of {}",
                                   sim_state->tick_count(), active_playback->replay().final_tick);
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
    if (ui_clicks && !ui_clicks->done()) {
        spdlog::error("{}", ui_clicks->never_clicked());
        return 1;
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
    return std::nullopt;
}

std::optional<int> App::Window::run_without_renderer() {
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
    return std::nullopt;
}

} // namespace osc::app
