// The run's boot: the engine, the game's sim, the UI state and the session
// (M192 step 2b, moved from run()).

#include "app/app_internal.hpp"
#include "core/profiler.hpp"
#include "lua/binding_coverage.hpp"
#include "lua/engine_bindings.hpp"
#include "lua/lan_dialog_ui.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/script_loader.hpp"
#include "lua/session_manager.hpp"
#include "lua/sim_bindings.hpp"
#include "lua/sim_loader.hpp"
#include "lua/smoke_test.hpp"
#include "lua/user_bindings.hpp"
#include "map/terrain.hpp"
#include "platform/paths.hpp"
#include "sim/anim_cache.hpp"
#include "sim/bone_cache.hpp"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <memory>
#include <random>
#include <spdlog/spdlog.h>

namespace osc::app {

std::optional<int> App::boot() {
    if (auto code = boot_engine()) return code;
    if (auto code = boot_game()) return code;
    if (auto code = boot_ui()) return code;
    return start();
}

std::optional<int> App::boot_engine() {
    // Phase 1: Init + VFS (sim Lua state)

    auto init_result = loader.execute_init(*sim_lua_state, config, vfs);
    if (!init_result) {
        spdlog::error("Init failed: {}", init_result.error().message);
        return 1;
    }

    // Phase 2: Blueprint loading (sim Lua state owns the store)

    auto bp_result = loader.load_blueprints(*sim_lua_state, vfs, store);
    if (!bp_result) {
        spdlog::error("Blueprint loading failed: {}", bp_result.error().message);
        return 1;
    }

    // Spot check
    auto* acu = store.find("uel0001");
    if (acu) {
        auto desc = store.get_string_field(*acu, "Description");
        spdlog::info("Spot check: uel0001 found ({})", desc.value_or("no description"));
    } else {
        spdlog::warn("Spot check: uel0001 (UEF ACU) not found");
    }

    spdlog::info("OpenSupCom initialization complete.");

    if (const auto trace = parse_string_arg(argc, argv, "--checksum-trace", ""); !trace.empty()) {
        checksum_trace.open(trace, std::ios::trunc);
        if (!checksum_trace) {
            spdlog::error("--checksum-trace: cannot write {}", trace);
            return 1;
        }
        g_checksum_trace = &checksum_trace;
    }
    if (const auto trace = parse_string_arg(argc, argv, "--entity-trace", ""); !trace.empty()) {
        entity_trace.open(trace, std::ios::trunc);
        if (!entity_trace) {
            spdlog::error("--entity-trace: cannot write {}", trace);
            return 1;
        }
        g_entity_trace = &entity_trace;
        const auto range = parse_string_arg(argc, argv, "--entity-trace-ticks", "");
        if (const auto dash = range.find('-'); dash != std::string::npos) {
            g_entity_trace_from = static_cast<osc::u32>(std::stoul(range.substr(0, dash)));
            g_entity_trace_to = static_cast<osc::u32>(std::stoul(range.substr(dash + 1)));
        }
    }
    if (const auto trace = parse_string_arg(argc, argv, "--rng-trace", ""); !trace.empty()) {
        rng_trace.open(trace, std::ios::trunc);
        if (!rng_trace) {
            spdlog::error("--rng-trace: cannot write {}", trace);
            return 1;
        }
        g_rng_trace = &rng_trace;
        const auto range = parse_string_arg(argc, argv, "--rng-trace-ticks", "");
        if (const auto dash = range.find('-'); dash != std::string::npos) {
            g_rng_trace_from = static_cast<osc::u32>(std::stoul(range.substr(0, dash)));
            g_rng_trace_to = static_cast<osc::u32>(std::stoul(range.substr(dash + 1)));
        }
    }
    sound.set_sim_clocked(opt.headless);
    return std::nullopt;
}

std::optional<int> App::boot_game() {
    // Phase 3: Map + Sim boot (only when --map provided)
    if (opt.replay_to_play) {
        game_setup = opt.replay_to_play->setup;
    } else if (opt.save_to_load) {
        game_setup = opt.save_to_load->game.setup;
    } else {
        game_setup.scenario = opt.map_path;
        game_setup.seed = new_game_seed(opt.seed_arg, opt.reproducible_run);
        if (opt.ai_skirmish) {
            // Every army the AI's (--ai-armies of them); listed once they exist.
            game_setup.army_count = static_cast<int>(opt.ai_army_count);
            game_setup.ai_personality = opt.ai_personality;
            // Cheat variants: a personality ending in "cheat"
            if (opt.ai_personality.size() > 5 &&
                opt.ai_personality.compare(opt.ai_personality.size() - 5, 5, "cheat") == 0) {
                game_setup.cheat_mult = 2.0;
                game_setup.build_mult = 2.0;
            }
        } else if (request.ai_army_2) {
            game_setup.ai_armies = {1}; // ARMY_2 (0-based index 1) is AI
        }
    }

    if (!opt.map_path.empty()) {
        sim_state = std::make_unique<osc::sim::SimState>(sim_lua_state->raw(), &store);
        sim_state->set_seed(game_setup.seed);
        sim_state->set_checksum_trace(g_checksum_trace);
        sim_state->set_entity_trace(g_entity_trace, g_entity_trace_from, g_entity_trace_to);
        sim_state->set_rng_trace(g_rng_trace, g_rng_trace_from, g_rng_trace_to);
        spdlog::info("Game seed {:#018x}", game_setup.seed);

        attach_sound(*sim_lua_state, *sim_state, &sound);
        // Only a drawn world needs its ticks captured.
        if (!opt.headless) world_interp.attach(*sim_state);

        // Bone cache (lazy-loaded per-blueprint SCM bone data)
        auto bone_cache = std::make_unique<osc::sim::BoneCache>(&vfs, &store);
        sim_state->set_bone_cache(std::move(bone_cache));

        // Animation cache (lazy-loaded SCA animation data)
        auto anim_cache = std::make_unique<osc::sim::AnimCache>(&vfs);
        sim_state->set_anim_cache(std::move(anim_cache));

        // Load scenario and map
        {
            osc::lua::ScenarioLoader scenario_loader;
            auto meta_result =
                scenario_loader.load_scenario(*sim_lua_state, vfs, opt.map_path, *sim_state);
            if (!meta_result) {
                spdlog::error("Scenario load failed: {}", meta_result.error().message);
                return 1;
            }
            scenario_meta = meta_result.value();

            // The setup's armies (all of the scenario's when it doesn't say)
            const size_t army_limit = game_setup.army_count > 0
                                          ? static_cast<size_t>(game_setup.army_count)
                                          : scenario_meta.armies.size();
            for (size_t i = 0; i < std::min(army_limit, scenario_meta.armies.size()); i++) {
                sim_state->add_army(scenario_meta.armies[i], scenario_meta.armies[i]);
            }
            if (opt.ai_skirmish && !opt.replay_to_play && !opt.save_to_load) {
                for (size_t a = 0; a < sim_state->army_count(); ++a)
                    game_setup.ai_armies.push_back(static_cast<int>(a));
            }
        }

        // Fallback: add a default army if none from scenario
        if (sim_state->army_count() == 0) {
            sim_state->add_army("ARMY_1", "Player");
        }

        osc::lua::SimLoader sim_loader;
        auto sim_result = sim_loader.boot_sim(*sim_lua_state, vfs, *sim_state);
        if (!sim_result) {
            spdlog::error("Sim boot failed: {}", sim_result.error().message);
            return 1;
        }

        // Register GiveResources SimCallback handler (M151b)
        // Use rawget/rawset to bypass config.lua global lock
        {
            lua_State* sL = sim_lua_state->raw();
            // Get or create SimCallbacks table
            lua_pushstring(sL, "SimCallbacks");
            lua_rawget(sL, LUA_GLOBALSINDEX);
            if (!lua_istable(sL, -1)) {
                lua_pop(sL, 1);
                lua_newtable(sL);
                lua_pushstring(sL, "SimCallbacks");
                lua_pushvalue(sL, -2);
                lua_rawset(sL, LUA_GLOBALSINDEX);
            }
            // Register GiveResources function
            auto give_res = sim_lua_state->do_string(R"(
            local sc = rawget(_G, 'SimCallbacks')
            sc.GiveResources = function(args)
                if not args or not args.From or not args.To then return end
                LOG('GiveResources: army ' .. tostring(args.From) .. ' -> army ' .. tostring(args.To) ..
                    ' mass=' .. tostring(args.Mass or 0) .. ' energy=' .. tostring(args.Energy or 0))
            end
        )");
            if (!give_res) {
                spdlog::warn("GiveResources SimCallback registration error: {}",
                             give_res.error().message);
            }
            lua_settop(sL, 0); // clean stack
        }
    } // end if (!map_path.empty()) — Phase 3
    return std::nullopt;
}

std::optional<int> App::boot_ui() {
    // === UI Lua State ===
    ui_lua_state.set_vfs(&vfs);
    ui_lua_state.set_blueprint_store(&store);
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "osc_sound_manager");
        lua_pushlightuserdata(uL, &sound);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Run init sequence on UI state (polyfills, config, class system, import)
    auto ui_init_result = loader.execute_init(ui_lua_state, config, vfs);
    if (!ui_init_result) {
        spdlog::error("UI Lua init failed: {}", ui_init_result.error().message);
        return 1;
    }

    // Load blueprints into UI state (separate store — UI refs must not
    // overwrite sim_L registry refs in the main store)
    auto ui_bp_result = loader.load_blueprints(ui_lua_state, vfs, ui_store);
    if (!ui_bp_result) {
        spdlog::warn("UI blueprint load: {}", ui_bp_result.error().message);
    }

    // Register moho class tables on UI state (unit_methods, etc.)
    // When no map, create a temporary dummy SimState for registration.
    // Sim-dependent moho methods check get_sim(L) and return gracefully when null.
    {
        std::unique_ptr<osc::sim::SimState> dummy_sim;
        if (!sim_state) {
            dummy_sim = std::make_unique<osc::sim::SimState>(sim_lua_state->raw(), &store);
        }
        osc::lua::register_moho_bindings(ui_lua_state, sim_state ? *sim_state : *dummy_sim);
        // A drawn game's captured ticks serve the UI's unit objects too.
        if (!opt.headless) osc::lua::set_ui_world_source(ui_lua_state.raw(), &world_interp.history);
        // If we used a dummy, clear the sim pointer in UI registry so moho methods
        // return gracefully instead of dereferencing a dangling pointer.
        if (dummy_sim) {
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "osc_sim_state");
            lua_pushlightuserdata(uL, nullptr);
            lua_rawset(uL, LUA_REGISTRYINDEX);
        }
    }

    // Localization cache — load strings from VFS, then store pointer in UI registry
    loc_cache.load_from_vfs(ui_lua_state.raw(), &vfs);
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_loc_cache");
        lua_pushlightuserdata(uL, &loc_cache);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Preferences (Game.prefs). An interactive game keeps them in the user's
    // config dir. Tests and captures never touch that file, so the player's
    // settings cannot change a result: --prefs PATH seeds them instead, and
    // is written back only when interactive.
    {
        std::filesystem::path file = parse_string_arg(argc, argv, "--prefs", "");
        if (file.empty() && opt.interactive) {
            file = osc::platform::known_folder(osc::platform::KnownFolder::Config) / "opensupcom" /
                   "Game.prefs";
        }
        if (!file.empty()) prefs.load(file);
        if (opt.interactive) prefs.set_path(file);
        // Retail's menus need a current profile; it would ask for one in a
        // first-run dialog (and then offer the tutorial). The engine makes
        // "Player" instead.
        if (prefs.ensure_profile("Player")) {
            prefs.set_bool(prefs.current_profile_path() + ".MenuTutorialPrompt", true);
            prefs.save();
        }
    }
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_preferences");
        lua_pushlightuserdata(uL, &prefs);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Replays and saved games (FA's special files). An interactive game keeps
    // them in FA's user folder; other runs use --user-dir, else a temporary
    // folder of their own (removed at exit), never the player's.
    user_dir = parse_string_arg(argc, argv, "--user-dir", "");
    if (user_dir.empty() && opt.interactive) user_dir = osc::lua::SpecialFiles::default_root();
    if (user_dir.empty()) {
        std::random_device rd;
        temp_user_dir =
            std::filesystem::temp_directory_path() / fmt::format("opensupcom-user-{:08x}", rd());
        user_dir = temp_user_dir;
    }
    temp_user_dir_remover.dir = temp_user_dir;
    special_files.emplace(user_dir);
    osc::lua::register_special_file_bindings(ui_lua_state, &*special_files);

    // WldUIProvider — long-lived instance stored in registry for InternalCreateWldUIProvider
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_wld_ui_provider");
        lua_pushlightuserdata(uL, &wld_provider);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // UI control registry (M71)

    // Register file I/O bindings on ui_L for lobby map enumeration (M148c)
    // IMPORTANT: must come BEFORE register_ui_bindings so that the real
    // ForkThread/WaitSeconds/Sound overwrite the blueprint stubs.
    osc::lua::register_blueprint_bindings(ui_lua_state);

    osc::lua::register_ui_bindings(ui_lua_state, ui_registry);
    osc::lua::register_user_bindings(ui_lua_state);

    // Set root frame size to window dimensions (1600x900) so LazyVar layout
    // resolves correctly. Must happen BEFORE CreateUI() so FillParent etc. work.
    {
        ui_lua_state.do_string("local f = GetFrame(0)\n"
                               "if f then\n"
                               "  f.Width:Set(1600)\n"
                               "  f.Height:Set(900)\n"
                               "  f.Left:Set(0)\n"
                               "  f.Top:Set(0)\n"
                               "  f.Right:Set(1600)\n"
                               "  f.Bottom:Set(900)\n"
                               "end\n");
    }

    // UI-side thread manager (reuses ThreadManager with frame counts instead of sim ticks)
    ui_thread_manager.register_in_registry(ui_lua_state.raw());

    // Also store under __osc_ui_thread_manager for ForkThread lookup.
    // register_in_registry stores under "osc_thread_mgr" for Destroy() support.
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_ui_thread_manager");
        lua_pushlightuserdata(uL, &ui_thread_manager);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // ── Engine state machine ──
    // Store game state string in UI registry (source of truth for GetCurrentUIState)
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_game_state");
        lua_pushstring(uL, "game");
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Set focus army (0 = ARMY_1)
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_focus_army");
        lua_pushnumber(uL, 0);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Register engine globals on UI Lua state
    // Note: GetCurrentUIState and WorldIsLoading are registered by register_ui_bindings (M144c)
    register_session_ui_globals(ui_lua_state);

    // User-state init, as Moho runs it for every UI state (front end and
    // game): retail /lua/userInit.lua -- plus its /schook hook -- runs
    // globalInit (which turns the moho.* tables into Lua classes, so MAUI's
    // Class(moho.frame_methods, Control) works), and defines WaitSeconds,
    // FrontEndData and the Prefetcher. The engine globals it and the UI
    // bootstrap expect must exist first.
    osc::lua::register_prefetch_bindings(ui_lua_state);
    osc::lua::register_category_bindings(ui_lua_state);
    {
        lua_State* uL = ui_lua_state.raw();
        auto global_is_defined = [&](const char* name) {
            lua_pushstring(uL, name);
            lua_rawget(uL, LUA_GLOBALSINDEX);
            const bool defined = !lua_isnil(uL, -1);
            lua_pop(uL, 1);
            return defined;
        };
        auto set_stub = [&](const char* name) {
            if (global_is_defined(name)) return;
            lua_pushstring(uL, name);
            lua_pushcfunction(uL, [](lua_State*) -> int { return 0; });
            lua_rawset(uL, LUA_GLOBALSINDEX);
        };
        auto set_str = [&](const char* name, const char* val) {
            if (global_is_defined(name)) return;
            lua_pushstring(uL, name);
            lua_pushstring(uL, val);
            lua_rawset(uL, LUA_GLOBALSINDEX);
        };
        auto set_bool_fn = [&](const char* name, bool val) {
            if (global_is_defined(name)) return;
            lua_pushstring(uL, name);
            lua_pushcfunction(uL, val ?
                +[](lua_State* L) -> int { lua_pushboolean(L, 1); return 1; } :
                +[](lua_State* L) -> int { lua_pushboolean(L, 0); return 1; });
            lua_rawset(uL, LUA_GLOBALSINDEX);
        };
        set_stub("AudioSetLanguage");
        set_str("__language", "us");
        set_bool_fn("HasLocalizedVO", false);
        // Engine globals needed by FA's UI bootstrap chain
        // (GetOptions, GetVolume, SetVolume, etc.)
        auto set_nil_fn = [&](const char* name) {
            if (global_is_defined(name)) return;
            lua_pushstring(uL, name);
            lua_pushcfunction(uL, [](lua_State* L) -> int {
                lua_pushnil(L);
                return 1;
            });
            lua_rawset(uL, LUA_GLOBALSINDEX);
        };
        set_stub("ConExecute");         // console commands
        set_stub("ConExecuteSave");     // console commands
        set_stub("AddInputCapture");    // input system
        set_stub("RemoveInputCapture"); // input system
        set_bool_fn("AnyInputCapture", false);
        set_bool_fn("DebugFacilitiesEnabled", false);
        set_stub("ExitApplication");              // exit
        set_stub("PrefetchSession");              // loading optimization
        set_stub("SetFocusArmy");                 // army focus
        set_nil_fn("GetFocusArmy");               // army focus
        set_stub("ClearFrame");                   // UI cleanup
        set_stub("GpgNetSend");                   // multiplayer
        set_bool_fn("HasCommandLineArg2", false); // command line
        // Session functions
        set_bool_fn("SessionIsActive", false);
        set_bool_fn("SessionIsMultiplayer", false);
        set_bool_fn("SessionIsObservingAllowed", false);
        set_bool_fn("SessionIsBeingRecorded", false);
        set_bool_fn("SessionCanRestart", false);
        set_nil_fn("SessionGetCommandSourceNames");
        set_nil_fn("SessionGetLocalCommandSource");
        // System info
        set_nil_fn("GetMouseScreenPos");
        set_stub("SetOverlayFilter");
        set_stub("SetOverlayFilters");
        set_nil_fn("GetActiveBuildTemplate");
        set_nil_fn("GetHighlightCommand");
        set_nil_fn("GetInputCapture");
        set_stub("RemoveInputCapture");
        set_stub("RestartSession");
        set_nil_fn("GetAntiAliasingOptions");
        set_nil_fn("GetResolution");
        set_stub("SetResolution");
        // __installedlanguages — table of available language codes
        if (!global_is_defined("__installedlanguages")) {
            lua_pushstring(uL, "__installedlanguages");
            lua_newtable(uL);
            lua_pushstring(uL, "us");
            lua_rawseti(uL, -2, 1);
            lua_rawset(uL, LUA_GLOBALSINDEX);
        }
    }
    {
        const char* init_script =
            vfs.file_exists("/lua/userInit.lua") ? "/lua/userInit.lua" : "/lua/globalInit.lua";
        if (auto r = osc::lua::run_vfs_script(ui_lua_state.raw(), init_script)) {
            spdlog::info("Loaded {} on ui_L", init_script);
        } else {
            spdlog::warn("{} error: {}", init_script, r.error().message);
        }
    }

    // State transition: INIT → GAME or INIT → FRONT_END
    if (!opt.map_path.empty()) {
        osc::core::call_setup_ui(ui_lua_state.raw());
    } else {
        // No map: bootstrap front-end menu UI
        // 2. Call SetupUI() (creates cursor, sets skin)
        osc::core::call_setup_ui(ui_lua_state.raw());
        // 3. Call import('/lua/ui/menus/main.lua').CreateUI()
        {
            auto r = ui_lua_state.do_string("import('/lua/ui/menus/main.lua').CreateUI()");
            if (r) {
                spdlog::info("Front-end menu CreateUI() succeeded");
            } else {
                spdlog::warn("Front-end CreateUI error: {}", r.error().message);
            }
            // Add the LAN Game button + IP/Host/Join dialog to the front end.
            {
                auto lr = ui_lua_state.do_string(osc::lua::kLanDialogLua);
                if (!lr) spdlog::warn("LAN dialog UI error: {}", lr.error().message);
            }
        }
        // Auto-trigger Skirmish: bypass lobby UI, directly launch with sessionConfig
        if (parse_flag(argc, argv, "--auto-skirmish")) {
            ui_lua_state.do_string(R"(
                ForkThread(function()
                    WaitSeconds(2.0)
                    LOG('Auto-skirmish: launching with 1 human + 1 AI...')
                    LaunchSinglePlayerSession({
                        ScenarioFile = '/maps/SCMP_009/SCMP_009_scenario.lua',
                        GameOptions = {
                            ScenarioFile = '/maps/SCMP_009/SCMP_009_scenario.lua',
                        },
                        PlayerOptions = {
                            [1] = {
                                Human = true,
                                PlayerName = 'Player',
                                Faction = 1,
                                Team = 1,
                                StartSpot = 1,
                            },
                            [2] = {
                                Human = false,
                                PlayerName = 'AI: Adaptive',
                                AIPersonality = 'adaptive',
                                Faction = 2,
                                Team = 2,
                                StartSpot = 2,
                            },
                        },
                    })
                end)
            )");
        }
        // Auto-trigger lobby via ButtonSkirmish (tests full lobby flow)
        if (parse_flag(argc, argv, "--auto-lobby")) {
            ui_lua_state.do_string(R"(
                ForkThread(function()
                    WaitSeconds(2.0)
                    LOG('Auto-lobby: triggering ButtonSkirmish...')
                    import('/lua/ui/menus/main.lua').ButtonSkirmish()
                end)
            )");
        }
        spdlog::info("Front-end UI initialized (no --map)");
    }
    return std::nullopt;
}

std::optional<int> App::start() {
    spdlog::info("Dual Lua states initialized (sim_L + ui_L)");

    // Terrain query test
    if (sim_state && sim_state->terrain()) {
        auto* t = sim_state->terrain();
        osc::f32 cx = static_cast<osc::f32>(t->map_width()) / 2;
        osc::f32 cz = static_cast<osc::f32>(t->map_height()) / 2;
        spdlog::info("Terrain test:");
        spdlog::info("  Center ({}, {}): terrain={:.1f}, surface={:.1f}", cx, cz,
                     t->get_terrain_height(cx, cz), t->get_surface_height(cx, cz));
    }

    // Phase 4: Session lifecycle
    if (!opt.map_path.empty()) {
        osc::lua::SessionManager session_mgr;
        session_mgr.configure(game_setup);
        auto session_result =
            session_mgr.start_session(*sim_lua_state, vfs, *sim_state, scenario_meta);
        if (!session_result) {
            spdlog::error("Session start failed: {}", session_result.error().message);
            return 1;
        }
        sim_state->set_game_setup(game_setup);
        // Recorded for --record, an interactive game for its LastGame, and
        // a game that will be saved (a loaded one too: its recording grows
        // back to the whole game as it catches up).
        // A save first: its orders queue, with its command delay, before
        // the recording starts from the sim's.
        if (opt.save_to_load) {
            catch_up.emplace(opt.save_to_load->game);
            catch_up->resume(*sim_state);
            spdlog::info("Saved game '{}': catching up to tick {}", opt.save_to_load->name,
                         opt.save_to_load->tick);
        }
        if (!g_record_path.empty() || opt.interactive || !opt.save_path.empty() || opt.save_to_load)
            sim_state->set_recording(true);
        if (opt.replay_to_play) return play_replay(*sim_state, *opt.replay_to_play);
    }

    // Binding-coverage report (roadmap M184): runs on the fully booted sim and
    // UI states, then exits.
    if (const std::string coverage_out = parse_string_arg(argc, argv, "--binding-coverage", "");
        !coverage_out.empty()) {
        if (!sim_lua_state) {
            spdlog::error("--binding-coverage needs --map <scenario>");
            return 1;
        }
        return osc::lua::coverage::run_coverage_report(
            sim_lua_state->raw(), ui_lua_state.raw(), vfs, coverage_out,
            parse_string_arg(argc, argv, "--binding-baseline", ""));
    }

    // Enable profiler if requested
    if (opt.profile_enabled) {
        osc::Profiler::instance().set_enabled(true);
        spdlog::info("Performance profiling enabled");
    }

    // BeatFunctionRegistry for per-frame Lua callbacks (M145b) — outer scope for headless test
    // access
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_beat_registry");
        lua_pushlightuserdata(uL, &beat_registry);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Key map registry for hotkey dispatch (M150b) — outer scope for headless test access
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_keymap_registry");
        lua_pushlightuserdata(uL, &keymap_registry);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // FrontEndData — cross-state key-value store (M147c)
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_front_end_data");
        lua_pushlightuserdata(uL, &front_end_data);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Command-line args for HasCommandLineArg (M147d)
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_cmdline_args");
        lua_pushlightuserdata(uL, &opt.cmdline_args);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // GameStateManager — outer scope for headless test access (M144b)
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_game_state_mgr");
        lua_pushlightuserdata(uL, &game_state_mgr);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }
    // SetupUI already ran during the UI state's boot above; the initial
    // transitions pass nullptr so it does not run a second time.
    if (!opt.map_path.empty()) {
        game_state_mgr.transition_to(osc::GameState::GAME, nullptr);
        if (sim_lua_state) {
            lua_State* sL = sim_lua_state->raw();
            lua_pushstring(sL, "__osc_game_state_mgr");
            lua_pushlightuserdata(sL, &game_state_mgr);
            lua_rawset(sL, LUA_REGISTRYINDEX);
        }
        // FA's game interface. Headless test modes other than those asking
        // for it keep a bare root frame: they build and inspect their own
        // controls.
        if (!opt.headless || request.world_ui) {
            begin_world_ui(ui_lua_state.raw(), wld_provider);
            finish_world_ui(ui_lua_state.raw(), wld_provider, false);
        }
    } else {
        // No map: start in FRONT_END state (main menu)
        game_state_mgr.transition_to(osc::GameState::FRONT_END, nullptr);
    }

    if (tests) {
        if (auto code = tests->front_end(engine)) return code;
    }

    // Instrumented mode: install SmokeTestHarness for interactive play (M166)
    if (opt.instrument) {
        instrument_harness = std::make_unique<osc::lua::SmokeTestHarness>();
        instrument_harness->activate();
        // Install on ui_L (persistent)
        instrument_harness->install_panic_handler(ui_lua_state.raw());
        instrument_harness->install_global_interceptor(ui_lua_state.raw());
        instrument_harness->install_all_method_interceptors(ui_lua_state.raw());
        // Install on sim_L if it exists
        if (sim_lua_state) {
            instrument_harness->install_panic_handler(sim_lua_state->raw());
            instrument_harness->install_global_interceptor(sim_lua_state->raw());
            instrument_harness->install_all_method_interceptors(sim_lua_state->raw());
        }
        spdlog::info("Instrumented mode active — smoke report on exit");
    }
    return std::nullopt;
}

} // namespace osc::app
