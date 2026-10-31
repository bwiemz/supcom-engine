// The reload: a new game's sim from a launch (M192 step 2, moved from
// app.cpp).

#include "app/app_internal.hpp"
#include "core/front_end_data.hpp"
#include "core/game_state.hpp"
#include "lua/game_mods.hpp"
#include "lua/lua_state.hpp"
#include "lua/session_manager.hpp"
#include "lua/sim_sync.hpp"
#include "lua/sim_loader.hpp"
#include "blueprints/blueprint_store.hpp"
#include "sim/bone_cache.hpp"
#include "sim/anim_cache.hpp"
#include "renderer/renderer.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/lua_sim.hpp"
#include "lua/mp_net_state.hpp"

namespace osc::app {

osc::sim::GameSetup launch_setup(lua_State* uiL, const osc::sim::Replay* replay,
                                 const std::string& scenario, osc::u64 seed) {
    if (replay) { // the recorded game's own
        osc::sim::GameSetup setup = replay->setup;
        setup.moho_pathing = recorded_moho_pathing(*replay);
        return setup;
    }
    osc::sim::GameSetup setup;
    lua_pushstring(uiL, "__osc_front_end_data");
    lua_rawget(uiL, LUA_REGISTRYINDEX);
    auto* fed = static_cast<osc::FrontEndData*>(lua_touserdata(uiL, -1));
    lua_pop(uiL, 1);
    if (fed) {
        const int top = lua_gettop(uiL);
        fed->get(uiL, "sessionConfig");
        if (lua_istable(uiL, -1)) setup = osc::lua::read_session_config(uiL, lua_gettop(uiL));
        lua_settop(uiL, top);
    }
    setup.scenario = scenario;
    setup.seed = seed;
    setup.moho_random = g_moho_random;
    setup.moho_pathing = g_moho_pathing;
    return setup;
}

bool take_restart_request(lua_State* uiL, const std::optional<SessionLaunch>& launch) {
    lua_pushstring(uiL, osc::core::kRestartRequestKey);
    lua_rawget(uiL, LUA_REGISTRYINDEX);
    const bool asked = lua_toboolean(uiL, -1) != 0;
    lua_pop(uiL, 1);
    if (!asked) return false;
    lua_pushstring(uiL, osc::core::kRestartRequestKey);
    lua_pushnil(uiL);
    lua_rawset(uiL, LUA_REGISTRYINDEX);
    if (!launch) {
        spdlog::warn("RestartSession: the game in play has no launch to repeat");
        return false;
    }
    const auto set = [&](const char* key, const std::string& value) {
        lua_pushstring(uiL, key);
        if (value.empty()) lua_pushnil(uiL);
        else lua_pushstring(uiL, value.c_str());
        lua_rawset(uiL, LUA_REGISTRYINDEX);
    };
    set("__osc_launch_scenario", launch->scenario);
    set("__osc_launch_replay", launch->replay_file);
    set("__osc_launch_save", launch->save_file);
    lua_pushstring(uiL, "__osc_launch_requested");
    lua_pushboolean(uiL, 1);
    lua_rawset(uiL, LUA_REGISTRYINDEX);
    spdlog::info("RestartSession: {} again", launch->scenario);
    return true;
}

int human_army(lua_State* simL, osc::sim::SimState& sim) {
    if (!simL) {
        return 0;
    }
    const int top = lua_gettop(simL);
    lua_pushstring(simL, "ScenarioInfo");
    lua_rawget(simL, LUA_GLOBALSINDEX);
    int setup = 0;
    if (lua_istable(simL, -1)) {
        lua_pushstring(simL, "ArmySetup");
        lua_gettable(simL, -2);
        if (lua_istable(simL, -1)) {
            setup = lua_gettop(simL);
        }
    }
    int found = -1;
    for (size_t i = 0; i < sim.army_count() && found < 0; ++i) {
        const auto* army = sim.get_army(static_cast<osc::i32>(i));
        if (!army) {
            continue;
        }
        bool human = true;
        if (setup != 0) {
            lua_pushstring(simL, army->name().c_str());
            lua_gettable(simL, setup);
            if (lua_istable(simL, -1)) {
                lua_pushstring(simL, "Human");
                lua_gettable(simL, -2);
                human = lua_toboolean(simL, -1) != 0;
                lua_pop(simL, 1);
            }
            lua_pop(simL, 1);
        }
        if (human) {
            found = static_cast<int>(i);
        }
    }
    lua_settop(simL, top);
    return found < 0 ? 0 : found;
}

// ── Reload sequence: tears down old sim, creates fresh Lua VM + SimState,
//    reloads blueprints/scenario, boots sim, rebuilds renderer scene. In
//    stages, so a window loads the sim's part off its frames (see
//    WorldLoad). ──

WorldLoad begin_world_load(std::unique_ptr<osc::lua::LuaState>& sim_lua_state,
                           std::unique_ptr<osc::sim::SimState>& sim_state,
                           osc::lua::LuaState& ui_lua_state, osc::vfs::VirtualFileSystem& vfs,
                           osc::blueprints::BlueprintStore& store, osc::lua::InitLoader& loader,
                           const osc::lua::InitConfig& config,
                           const osc::lua::ScenarioMetadata& scenario_meta,
                           osc::renderer::Renderer* renderer, osc::u64 seed,
                           const std::string& launch_scenario, const osc::sim::Replay* replay,
                           const osc::sim::GameSetup* restart) {
    lua_State* uiL = ui_lua_state.raw();
    WorldLoad load;
    load.scenario = launch_scenario;
    load.replay = replay;
    load.scenario_meta = scenario_meta; // (kept if the scenario doesn't load)

    // 1. GPU fence — ensure no in-flight work
    if (renderer) renderer->clear_scene();

    // 2. Destroy old SimState and sim Lua state. A multiplayer LockstepSession
    // holds a reference to the SimState, so drop any stale session first (but
    // keep the transport — HostGame/JoinGame created it before launch and
    // mp_attach_session rebuilds the session over it once the fresh sim exists).
    osc::lua::mp_net_state().session.reset();
    detach_ui_from_sim(uiL);
    sim_state.reset();
    sim_lua_state.reset();
    // Its blueprints' Lua references went with it (rebound below, unless
    // the reload fails first: then no destructor may release them)
    store.rebind(nullptr);

    // 3. Create fresh sim Lua state
    load.sim_lua = std::make_unique<osc::lua::LuaState>();
    load.sim_lua->set_vfs(&vfs);
    load.sim_lua->set_blueprint_store(&store);

    // 4. Run init sequence on new sim state (polyfills, config, class,
    // import). Here, on the window's thread: it mounts the VFS its frames
    // read.
    auto reinit_result = loader.execute_init(*load.sim_lua, config, vfs);
    if (!reinit_result) {
        spdlog::error("Reload init failed: {}", reinit_result.error().message);
        return load;
    }

    // The game's setup. Moho creates only the filled slots' armies, and
    // retail InitializeArmies spawns an ACU for every army ListArmies()
    // returns -- an army without a brain then runs its commander's scripts
    // against no brain at all.
    // A restart's is its game's own, the seed too: Moho starts the session
    // again from its launch info, which holds the seed it was launched with.
    load.seed = restart ? restart->seed : replay ? replay->setup.seed : seed;
    load.setup = restart ? *restart : launch_setup(uiL, replay, launch_scenario, load.seed);
    // The audio engine (the application's, kept in the UI state)
    lua_pushstring(uiL, "osc_sound_manager");
    lua_rawget(uiL, LUA_REGISTRYINDEX);
    load.sound = static_cast<osc::audio::SoundManager*>(lua_touserdata(uiL, -1));
    lua_pop(uiL, 1);
    load.ok = true;
    return load;
}

void load_world(WorldLoad& load, osc::vfs::VirtualFileSystem& vfs,
                osc::blueprints::BlueprintStore& store, osc::lua::InitLoader& loader,
                const osc::lua::InitConfig& config, osc::GameStateManager& game_state_mgr) {
    if (!load.ok) return;
    load.ok = false; // until the sim boots
    osc::lua::LuaState& sim_lua = *load.sim_lua;
    osc::sim::GameSetup& setup = load.setup;

    // 5. Rebind BlueprintStore to new Lua state and reload blueprints, with
    // the game's mods (theirs, and their hooks of Blueprints.lua)
    osc::lua::set_active_mods(sim_lua.raw(), setup.mods);
    store.rebind(sim_lua.raw());
    auto rebp_result = loader.load_blueprints(sim_lua, vfs, store);
    if (!rebp_result) {
        spdlog::error("Reload blueprint load failed: {}", rebp_result.error().message);
        return;
    }

    // 6. Create fresh SimState
    load.sim = std::make_unique<osc::sim::SimState>(sim_lua.raw(), &store);
    osc::sim::SimState& sim = *load.sim;
    sim.set_seed(load.seed);
    sim.set_checksum_trace(g_checksum_trace);
    sim.set_entity_trace(g_entity_trace, g_entity_trace_from, g_entity_trace_to);
    sim.set_rng_trace(g_rng_trace, g_rng_trace_from, g_rng_trace_to);
    sim.set_steer_log(g_steer_log);
    sim.set_moho_pathing(setup.moho_pathing);
    sim.set_moho_random(setup.moho_random);
    sim.set_faf_exe(!config.faf_data_path.empty());
    sim.set_run_opponent_ai(!g_no_ai);
    spdlog::info("Game seed {:#018x}{}", load.seed,
                 setup.moho_random ? ", Moho's random stream" : "");

    // 7. Audio (the sim's way to it; a saved game's snapshot names it),
    // bone cache, anim cache
    attach_sound(sim_lua, sim, load.sound);
    sim.set_bone_cache(std::make_unique<osc::sim::BoneCache>(&vfs, &store));
    sim.set_anim_cache(std::make_unique<osc::sim::AnimCache>(&vfs));

    // 8. Load scenario from selected map
    osc::lua::ScenarioLoader new_scenario_loader;
    auto new_meta_result = new_scenario_loader.load_scenario(sim_lua, vfs, load.scenario, sim);
    if (!new_meta_result) {
        spdlog::error("Reload scenario failed: {}", new_meta_result.error().message);
    } else {
        load.scenario_meta = new_meta_result.value();
        // The setup's armies: a lobby's slots taken, each its slot's army
        for (const std::string& name :
             osc::sim::session_army_names(setup, load.scenario_meta.armies))
            sim.add_army(name, name);
    }
    if (sim.army_count() == 0) {
        sim.add_army("ARMY_1", "Player");
    }

    // 9. Register GiveResources SimCallback on new state
    {
        lua_State* sL = sim_lua.raw();
        lua_pushstring(sL, "SimCallbacks");
        lua_rawget(sL, LUA_GLOBALSINDEX);
        if (!lua_istable(sL, -1)) {
            lua_pop(sL, 1);
            lua_newtable(sL);
            lua_pushstring(sL, "SimCallbacks");
            lua_pushvalue(sL, -2);
            lua_rawset(sL, LUA_GLOBALSINDEX);
        }
        sim_lua.do_string(R"(
            local sc = rawget(_G, 'SimCallbacks')
            sc.GiveResources = function(args)
                if not args or not args.From or not args.To then return end
                LOG('GiveResources: army ' .. tostring(args.From) .. ' -> army ' .. tostring(args.To) ..
                    ' mass=' .. tostring(args.Mass or 0) .. ' energy=' .. tostring(args.Energy or 0))
            end
        )");
        lua_settop(sL, 0);
    }

    // 10. Store game state manager in new sim registry
    {
        lua_State* sL = sim_lua.raw();
        lua_pushstring(sL, "__osc_game_state_mgr");
        lua_pushlightuserdata(sL, &game_state_mgr);
        lua_rawset(sL, LUA_REGISTRYINDEX);
    }

    // 11. Boot sim (registers moho/sim bindings, runs simInit.lua)
    osc::lua::SimLoader new_sim_loader;
    auto new_sim_result = new_sim_loader.boot_sim(sim_lua, vfs, sim);
    if (!new_sim_result) {
        spdlog::error("Reload sim boot failed: {}", new_sim_result.error().message);
        return;
    }

    // 12. Start the session from the setup
    {
        if (!load.replay && setup.slots.empty() && setup.ai_armies.empty() &&
            sim.army_count() >= 2) {
            // No sessionConfig: ARMY_2 is the AI (legacy behavior).
            setup.ai_armies = {1};
            spdlog::info("Session: fallback — ARMY_2 as AI (no sessionConfig)");
        } else if (!setup.ai_armies.empty()) {
            spdlog::info("Session: {} AI armies (personality={}), {} total slots",
                         setup.ai_armies.size(), setup.ai_personality, setup.army_count);
        }
        for (size_t i = 0; i < setup.slots.size(); ++i) {
            if (!setup.slots[i].configured) continue;
            if (auto* brain = sim.get_army(static_cast<osc::i32>(i))) {
                brain->set_faction(setup.slots[i].faction);
                if (!setup.slots[i].human) {
                    brain->set_ai_personality(setup.slots[i].ai_personality.empty()
                                                  ? setup.ai_personality
                                                  : setup.slots[i].ai_personality);
                }
            }
        }
        osc::lua::SessionManager new_session_mgr;
        new_session_mgr.configure(setup);
        auto sess_result = new_session_mgr.start_session(sim_lua, vfs, sim, load.scenario_meta);
        if (!sess_result) {
            spdlog::warn("Reload session start failed: {}", sess_result.error().message);
        }
        sim.set_game_setup(setup);
        // Every game records (for LastGame and --record); a replay plays.
        if (!load.replay) sim.set_recording(true);
    }
    load.ok = true;
}

void adopt_world_load(WorldLoad& load, std::unique_ptr<osc::lua::LuaState>& sim_lua_state,
                      std::unique_ptr<osc::sim::SimState>& sim_state,
                      osc::lua::ScenarioMetadata& scenario_meta, osc::lua::LuaState& ui_lua_state,
                      WorldInterp* world_interp) {
    sim_lua_state = std::move(load.sim_lua);
    sim_state = std::move(load.sim);
    scenario_meta = std::move(load.scenario_meta);
    if (!sim_state) return;
    // A game quit while paused left its sound categories paused (retail
    // resumes them only from gamemain's OnResume): each game starts unpaused.
    if (load.sound) load.sound->resume_all();
    if (world_interp) {
        world_interp->attach(*sim_state, &ui_lua_state);
    }
}

void finish_world_load(osc::sim::SimState& sim_state, osc::lua::LuaState& sim_lua_state,
                       osc::lua::LuaState& ui_lua_state, osc::vfs::VirtualFileSystem& vfs,
                       osc::GameStateManager& game_state_mgr, osc::renderer::Renderer* renderer,
                       osc::blueprints::BlueprintStore* ui_store,
                       osc::renderer::InputHandler* input_handler,
                       std::unordered_set<osc::sim::EntityId>* prev_selection,
                       double& sim_accumulator, const std::string& launch_scenario) {
    lua_State* uiL = ui_lua_state.raw();

    // 13. Update UI state's sim_state registry pointer to new SimState
    sim::set_sim(uiL, &sim_state);

    // 14. Rebuild renderer scene, reading blueprints through the UI state's
    // store: its tables are uiL's
    if (renderer)
        renderer->build_scene(sim_state.terrain(), ui_store, osc::sim::world_blueprints(sim_state),
                              &vfs, uiL);

    // 15. The camera's reset came with the scene (CameraReset: the whole map)

    // 16. Update UI state registry pointers
    lua_pushstring(uiL, "__osc_scenario_path");
    lua_pushstring(uiL, launch_scenario.c_str());
    lua_rawset(uiL, LUA_REGISTRYINDEX);

    lua_pushstring(uiL, "__osc_hover_entity_id");
    lua_pushnumber(uiL, 0);
    lua_rawset(uiL, LUA_REGISTRYINDEX);

    // The player plays the army the lobby set up as theirs: not always the
    // first, as a lobby gives the player the slot taken (its start spot)
    osc::lua::set_focus_army(sim_lua_state.raw(), uiL, human_army(sim_lua_state.raw(), sim_state));

    // 17. Clear selection
    if (input_handler) input_handler->set_selected({});
    if (prev_selection) prev_selection->clear();

    // 18. Reset game state
    game_state_mgr.set_game_over(false);
    game_state_mgr.set_paused(false);
    sim_accumulator = 0.0;

    // 19. Transition to GAME (skip if caller already handles transitions)
    if (game_state_mgr.current() != osc::GameState::LOADING) {
        // Headless/smoke-test path: do full transition (its SetupUI the
        // game's UI state's)
        osc::set_ui_state(osc::UIState::Game);
        game_state_mgr.transition_to(osc::GameState::LOADING, uiL);
    }
    game_state_mgr.transition_to(osc::GameState::GAME, nullptr); // nullptr = skip SetupUI

    spdlog::info("=== Map reload complete ===");
}

// Returns true on success, false on critical failure.
bool execute_reload_sequence(
    std::unique_ptr<osc::lua::LuaState>& sim_lua_state,
    std::unique_ptr<osc::sim::SimState>& sim_state, osc::lua::LuaState& ui_lua_state,
    osc::vfs::VirtualFileSystem& vfs, osc::blueprints::BlueprintStore& store,
    osc::lua::InitLoader& loader, const osc::lua::InitConfig& config,
    osc::lua::ScenarioMetadata& scenario_meta, osc::GameStateManager& game_state_mgr,
    osc::renderer::Renderer* renderer,                      // nullable for headless
    osc::blueprints::BlueprintStore* ui_store,              // nullable for headless
    osc::renderer::InputHandler* input_handler,             // nullable for headless
    std::unordered_set<osc::sim::EntityId>* prev_selection, // nullable for headless
    WorldInterp* world_interp,                              // nullable for headless
    osc::u64 seed,                                          // the new game's random seed
    double& sim_accumulator, const std::string& launch_scenario,
    const osc::sim::Replay* replay,       // a replay to play instead
    const osc::sim::GameSetup* restart) { // a game started again
    WorldLoad load =
        begin_world_load(sim_lua_state, sim_state, ui_lua_state, vfs, store, loader, config,
                         scenario_meta, renderer, seed, launch_scenario, replay, restart);
    load_world(load, vfs, store, loader, config, game_state_mgr);
    const bool ok = load.ok;
    adopt_world_load(load, sim_lua_state, sim_state, scenario_meta, ui_lua_state, world_interp);
    if (!ok) return false;
    finish_world_load(*sim_state, *sim_lua_state, ui_lua_state, vfs, game_state_mgr, renderer,
                      ui_store, input_handler, prev_selection, sim_accumulator, launch_scenario);
    return true;
}

} // namespace osc::app
