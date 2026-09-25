// The reload: a new game's sim from a launch (M192 step 2, moved from
// app.cpp).

#include "app/app_internal.hpp"
#include "core/front_end_data.hpp"
#include "core/game_state.hpp"
#include "lua/lua_state.hpp"
#include "lua/session_manager.hpp"
#include "lua/sim_loader.hpp"
#include "blueprints/blueprint_store.hpp"
#include "sim/bone_cache.hpp"
#include "sim/anim_cache.hpp"
#include "renderer/renderer.hpp"
#include "sim/lockstep_session.hpp"
#include "lua/mp_net_state.hpp"

namespace osc::app {

// ── Reload sequence: tears down old sim, creates fresh Lua VM + SimState,
//    reloads blueprints/scenario, boots sim, rebuilds renderer scene. ──
// Returns true on success, false on critical failure.
bool execute_reload_sequence(std::unique_ptr<osc::lua::LuaState>& sim_lua_state,
                             std::unique_ptr<osc::sim::SimState>& sim_state,
                             osc::lua::LuaState& ui_lua_state, osc::vfs::VirtualFileSystem& vfs,
                             osc::blueprints::BlueprintStore& store, osc::lua::InitLoader& loader,
                             const osc::lua::InitConfig& config,
                             osc::lua::ScenarioMetadata& scenario_meta,
                             osc::GameStateManager& game_state_mgr,
                             osc::renderer::Renderer* renderer,            // nullable for headless
                             osc::renderer::InputHandler* input_handler,   // nullable for headless
                             std::unordered_set<osc::u32>* prev_selection, // nullable for headless
                             WorldInterp* world_interp,                    // nullable for headless
                             osc::u64 seed, // the new game's random seed
                             double& sim_accumulator, const std::string& launch_scenario,
                             const osc::sim::Replay* replay) { // a replay to play instead

    lua_State* uiL = ui_lua_state.raw();

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

    // 3. Create fresh sim Lua state
    sim_lua_state = std::make_unique<osc::lua::LuaState>();
    sim_lua_state->set_vfs(&vfs);
    sim_lua_state->set_blueprint_store(&store);

    // 4. Run init sequence on new sim state (polyfills, config, class, import)
    auto reinit_result = loader.execute_init(*sim_lua_state, config, vfs);
    if (!reinit_result) {
        spdlog::error("Reload init failed: {}", reinit_result.error().message);
        return false;
    }

    // 5. Rebind BlueprintStore to new Lua state and reload blueprints
    store.rebind(sim_lua_state->raw());
    auto rebp_result = loader.load_blueprints(*sim_lua_state, vfs, store);
    if (!rebp_result) {
        spdlog::error("Reload blueprint load failed: {}", rebp_result.error().message);
        return false;
    }

    // 6. Create fresh SimState
    sim_state = std::make_unique<osc::sim::SimState>(sim_lua_state->raw(), &store);
    if (replay) seed = replay->setup.seed;
    sim_state->set_seed(seed);
    sim_state->set_checksum_trace(g_checksum_trace);
    sim_state->set_entity_trace(g_entity_trace, g_entity_trace_from, g_entity_trace_to);
    sim_state->set_rng_trace(g_rng_trace, g_rng_trace_from, g_rng_trace_to);
    spdlog::info("Game seed {:#018x}", seed);

    // 7. Audio (the application's engine, kept in the UI state), bone
    // cache, anim cache
    {
        lua_pushstring(uiL, "osc_sound_manager");
        lua_rawget(uiL, LUA_REGISTRYINDEX);
        auto* sound = static_cast<osc::audio::SoundManager*>(lua_touserdata(uiL, -1));
        lua_pop(uiL, 1);
        attach_sound(*sim_lua_state, *sim_state, sound);
    }
    if (world_interp) world_interp->attach(*sim_state);
    sim_state->set_bone_cache(
        std::make_unique<osc::sim::BoneCache>(&vfs, &store));
    sim_state->set_anim_cache(
        std::make_unique<osc::sim::AnimCache>(&vfs));

    // The game's setup: the lobby's sessionConfig (which of the scenario's
    // armies play, who plays each, the options), the scenario and the seed.
    // Moho creates only the filled slots' armies, and retail InitializeArmies
    // spawns an ACU for every army ListArmies() returns -- an army without a
    // brain then runs its commander's scripts against no brain at all.
    osc::sim::GameSetup setup;
    if (replay) {
        setup = replay->setup; // the recorded game's own
    } else {
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
    }
    if (!replay) {
        setup.scenario = launch_scenario;
        setup.seed = seed;
    }

    // 8. Load scenario from selected map
    osc::lua::ScenarioLoader new_scenario_loader;
    auto new_meta_result = new_scenario_loader.load_scenario(
        *sim_lua_state, vfs, launch_scenario, *sim_state);
    if (!new_meta_result) {
        spdlog::error("Reload scenario failed: {}",
                      new_meta_result.error().message);
    } else {
        scenario_meta = new_meta_result.value();
        const size_t army_limit =
            setup.army_count > 0
                ? std::min(static_cast<size_t>(setup.army_count), scenario_meta.armies.size())
                : scenario_meta.armies.size();
        for (size_t i = 0; i < army_limit; ++i) {
            sim_state->add_army(scenario_meta.armies[i], scenario_meta.armies[i]);
        }
    }
    if (sim_state->army_count() == 0) {
        sim_state->add_army("ARMY_1", "Player");
    }

    // 9. Register GiveResources SimCallback on new state
    {
        lua_State* sL = sim_lua_state->raw();
        lua_pushstring(sL, "SimCallbacks");
        lua_rawget(sL, LUA_GLOBALSINDEX);
        if (!lua_istable(sL, -1)) {
            lua_pop(sL, 1);
            lua_newtable(sL);
            lua_pushstring(sL, "SimCallbacks");
            lua_pushvalue(sL, -2);
            lua_rawset(sL, LUA_GLOBALSINDEX);
        }
        sim_lua_state->do_string(R"(
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
        lua_State* sL = sim_lua_state->raw();
        lua_pushstring(sL, "__osc_game_state_mgr");
        lua_pushlightuserdata(sL, &game_state_mgr);
        lua_rawset(sL, LUA_REGISTRYINDEX);
    }

    // 11. Boot sim (registers moho/sim bindings, runs simInit.lua)
    osc::lua::SimLoader new_sim_loader;
    auto new_sim_result = new_sim_loader.boot_sim(
        *sim_lua_state, vfs, *sim_state);
    if (!new_sim_result) {
        spdlog::error("Reload sim boot failed: {}",
                      new_sim_result.error().message);
        return false;
    }

    // 12. Start the session from the setup
    {
        if (!replay && setup.slots.empty() && setup.ai_armies.empty() &&
            sim_state->army_count() >= 2) {
            // No sessionConfig: ARMY_2 is the AI (legacy behavior).
            setup.ai_armies = {1};
            spdlog::info("Session: fallback — ARMY_2 as AI (no sessionConfig)");
        } else if (!setup.ai_armies.empty()) {
            spdlog::info("Session: {} AI armies (personality={}), {} total slots",
                         setup.ai_armies.size(), setup.ai_personality, setup.army_count);
        }
        for (size_t i = 0; i < setup.slots.size(); ++i) {
            if (!setup.slots[i].configured) continue;
            if (auto* brain = sim_state->get_army(static_cast<osc::i32>(i)))
                brain->set_faction(setup.slots[i].faction);
        }
        osc::lua::SessionManager new_session_mgr;
        new_session_mgr.configure(setup);
        auto sess_result = new_session_mgr.start_session(
            *sim_lua_state, vfs, *sim_state, scenario_meta);
        if (!sess_result) {
            spdlog::warn("Reload session start failed: {}",
                         sess_result.error().message);
        }
        sim_state->set_game_setup(setup);
        // Every game records (for LastGame and --record); a replay plays.
        if (!replay) sim_state->set_recording(true);
    }

    // 13. Update UI state's sim_state registry pointer to new SimState
    {
        lua_pushstring(uiL, "osc_sim_state");
        lua_pushlightuserdata(uiL, sim_state.get());
        lua_rawset(uiL, LUA_REGISTRYINDEX);
    }

    // 14. Rebuild renderer scene
    if (renderer)
        renderer->build_scene(sim_state->terrain(), sim_state->blueprint_store(),
                              osc::sim::world_blueprints(*sim_state), &vfs, uiL);

    // 15. Reset camera to map center (spherical coords: target + distance)
    if (renderer && sim_state->terrain()) {
        osc::f32 cx = sim_state->terrain()->map_width() * 0.5f;
        osc::f32 cz = sim_state->terrain()->map_height() * 0.5f;
        renderer->camera().set_target(cx, cz);
        renderer->camera().set_distance(300.0f);
    }

    // 16. Update UI state registry pointers
    lua_pushstring(uiL, "__osc_scenario_path");
    lua_pushstring(uiL, launch_scenario.c_str());
    lua_rawset(uiL, LUA_REGISTRYINDEX);

    lua_pushstring(uiL, "__osc_hover_entity_id");
    lua_pushnumber(uiL, 0);
    lua_rawset(uiL, LUA_REGISTRYINDEX);

    lua_pushstring(uiL, "__osc_focus_army");
    lua_pushnumber(uiL, 0);
    lua_rawset(uiL, LUA_REGISTRYINDEX);

    // 17. Clear selection
    if (input_handler) input_handler->set_selected({});
    if (prev_selection) prev_selection->clear();

    // 18. Reset game state
    game_state_mgr.set_game_over(false);
    game_state_mgr.set_paused(false, uiL);
    sim_accumulator = 0.0;

    // 19. Transition to GAME (skip if caller already handles transitions)
    if (game_state_mgr.current() != osc::GameState::LOADING) {
        // Headless/smoke-test path: do full transition
        game_state_mgr.transition_to(osc::GameState::LOADING, uiL);
    }
    game_state_mgr.transition_to(osc::GameState::GAME, nullptr); // nullptr = skip SetupUI

    spdlog::info("=== Map reload complete ===");
    return true;
}

} // namespace osc::app
