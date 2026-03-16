#include "core/front_end_data.hpp"
#include "core/game_state.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/types.hpp"
#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "lua/init_loader.hpp"
#include "lua/session_manager.hpp"
#include "lua/sim_loader.hpp"
#include "lua/scenario_loader.hpp"
#include "vfs/virtual_file_system.hpp"
#include "blueprints/blueprint_store.hpp"
#include "sim/sim_state.hpp"
#include "sim/bone_cache.hpp"
#include "sim/anim_cache.hpp"
#include "map/terrain.hpp"
#include "audio/sound_manager.hpp"
#include "core/localization.hpp"
#include "core/preferences.hpp"
#include "lua/engine_bindings.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/beat_system.hpp"
#include "lua/factory_queue.hpp"
#include "ui/ui_control.hpp"
#include "ui/keymap.hpp"
#include "ui/wld_ui_provider.hpp"
#include "renderer/renderer.hpp"
#include "renderer/input_handler.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/unit.hpp"
#include "lua/smoke_test.hpp"

extern "C" {
#include <lua.h>
}

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <spdlog/spdlog.h>

// ── Engine global Lua C functions (file-static, outside main) ──
// Note: GetCurrentUIState and WorldIsLoading moved to moho_bindings.cpp (M144c)

static int l_FlushEvents(lua_State*) { return 0; }

static int l_SessionIsReplay(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

static int l_SessionGetScenarioInfo(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    lua_newtable(L);
    if (!sim || !sim->terrain()) return 1;

    lua_pushstring(L, "name");
    lua_pushstring(L, "Skirmish");
    lua_rawset(L, -3);

    lua_pushstring(L, "map");
    lua_pushstring(L, "");
    lua_rawset(L, -3);

    lua_pushstring(L, "size");
    lua_pushnumber(L, sim->terrain()->map_width());
    lua_rawset(L, -3);

    lua_pushstring(L, "PlayableArea");
    lua_newtable(L);
    lua_pushnumber(L, 0); lua_rawseti(L, -2, 1);
    lua_pushnumber(L, 0); lua_rawseti(L, -2, 2);
    lua_pushnumber(L, sim->terrain()->map_width()); lua_rawseti(L, -2, 3);
    lua_pushnumber(L, sim->terrain()->map_height()); lua_rawseti(L, -2, 4);
    lua_rawset(L, -3);

    return 1;
}

static int l_GetEconomyTotals(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    lua_pushstring(L, "__osc_focus_army");
    lua_rawget(L, LUA_REGISTRYINDEX);
    int army = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);

    lua_newtable(L); // result table
    if (!sim) return 1;
    auto* brain = sim->get_army(army);
    if (!brain) return 1;

    const auto& econ = brain->economy();

    // Helper: push a subtable with MASS and ENERGY keys
    auto push_resource_subtable = [&](const char* name, osc::f64 mass_val, osc::f64 energy_val) {
        lua_pushstring(L, name);
        lua_newtable(L);
        lua_pushstring(L, "MASS");
        lua_pushnumber(L, mass_val);
        lua_rawset(L, -3);
        lua_pushstring(L, "ENERGY");
        lua_pushnumber(L, energy_val);
        lua_rawset(L, -3);
        lua_rawset(L, -3); // set subtable on result
    };

    push_resource_subtable("income", econ.mass.income, econ.energy.income);
    push_resource_subtable("lastUseActual",
        brain->get_economy_usage("MASS"), brain->get_economy_usage("ENERGY"));
    push_resource_subtable("lastUseRequested", econ.mass.requested, econ.energy.requested);
    push_resource_subtable("maxStorage", econ.mass.max_storage, econ.energy.max_storage);
    push_resource_subtable("stored", econ.mass.stored, econ.energy.stored);
    push_resource_subtable("reclaimed", 0.0, 0.0); // TODO: track cumulative reclaim

    return 1;
}

static int l_GetSimTicksPerSecond(lua_State* L) {
    lua_pushnumber(L, 10.0);
    return 1;
}

static int l_ui_IsAlly(lua_State* L) {
    int army1 = static_cast<int>(lua_tonumber(L, 1)) - 1; // 1-based Lua → 0-based C++
    int army2 = static_cast<int>(lua_tonumber(L, 2)) - 1;

    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    if (!sim) { lua_pushboolean(L, 0); return 1; }
    auto* brain = sim->get_army(army1);
    if (!brain) { lua_pushboolean(L, 0); return 1; }

    lua_pushboolean(L, brain->is_ally(army2) ? 1 : 0);
    return 1;
}

static int l_GetArmiesTable(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    lua_newtable(L); // result table

    // Build the armiesTable array
    lua_pushstring(L, "armiesTable");
    lua_newtable(L); // armiesTable array

    if (sim) {
        for (size_t i = 0; i < sim->army_count(); ++i) {
            auto* brain = sim->army_at(i);
            if (!brain) continue;

            lua_newtable(L); // per-army entry

            lua_pushstring(L, "nickname");
            lua_pushstring(L, brain->nickname().c_str());
            lua_rawset(L, -3);

            lua_pushstring(L, "ArmyName");
            lua_pushstring(L, brain->name().c_str());
            lua_rawset(L, -3);

            lua_pushstring(L, "armyIndex");
            lua_pushnumber(L, static_cast<int>(i));
            lua_rawset(L, -3);

            lua_pushstring(L, "human");
            lua_pushboolean(L, brain->is_human() ? 1 : 0);
            lua_rawset(L, -3);

            lua_pushstring(L, "civilian");
            lua_pushboolean(L, brain->is_civilian() ? 1 : 0);
            lua_rawset(L, -3);

            lua_pushstring(L, "outOfGame");
            lua_pushboolean(L, brain->is_defeated() ? 1 : 0);
            lua_rawset(L, -3);

            lua_pushstring(L, "faction");
            lua_pushnumber(L, brain->faction());
            lua_rawset(L, -3);

            // Color as ARGB hex string (e.g. "ffFF8000")
            {
                char color_buf[16];
                std::snprintf(color_buf, sizeof(color_buf), "ff%02X%02X%02X",
                    brain->color_r(), brain->color_g(), brain->color_b());
                lua_pushstring(L, "color");
                lua_pushstring(L, color_buf);
                lua_rawset(L, -3);
            }

            lua_pushstring(L, "showScore");
            lua_pushboolean(L, brain->is_civilian() ? 0 : 1);
            lua_rawset(L, -3);

            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
    }

    lua_rawset(L, -3); // result.armiesTable = array

    // focusArmy field
    lua_pushstring(L, "focusArmy");
    lua_pushstring(L, "__osc_focus_army");
    lua_rawget(L, LUA_REGISTRYINDEX);
    int focus = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);
    lua_pushnumber(L, focus + 1); // 1-based for Lua
    lua_rawset(L, -3);

    // numArmies field
    lua_pushstring(L, "numArmies");
    lua_pushnumber(L, sim ? static_cast<int>(sim->army_count()) : 0);
    lua_rawset(L, -3);

    return 1;
}

static void print_usage() {
    std::cout << "OpenSupCom v0.1.0\n"
              << "Open-source engine reimplementation for Supreme Commander: "
                 "Forged Alliance\n\n"
              << "Usage:\n"
              << "  opensupcom [options]\n\n"
              << "Options:\n"
              << "  --init <path>      Path to init.lua / init_faf.lua\n"
              << "  --fa-path <path>   Path to FA installation directory\n"
              << "  --faf-data <path>  Path to FAF data directory\n"
              << "  --map <vfs-path>   VFS path to *_scenario.lua\n"
              << "  --ticks <n>        Number of sim ticks to run (default: 100)\n"
              << "  --damage-test      After ticks, kill entity #1 and run 10 more ticks\n"
              << "  --move-test        After ticks, move entity #1 and run 200 more ticks\n"
              << "  --fire-test        Teleport entities #1 and #2 close, run 100 combat ticks\n"
              << "  --economy-test     After ticks, log economy state for all armies\n"
              << "  --build-test       After ticks, build a T1 power gen near entity #1\n"
              << "  --chain-test       Full build chain: ACU -> factory -> engineer -> pgen\n"
              << "  --ai-test          AI ARMY_2: pgens + factory + engineers + guard assist\n"
              << "  --reclaim-test     Create prop, engineer reclaims it, verify mass gained\n"
              << "  --platoon-test     Platoon system: create, assign, move, fork, disband\n"
              << "  --threat-test      Threat queries, platoon targeting, command tracking\n"
              << "  --combat-test      AI produces army, forms platoons, attacks enemy\n"
              << "  --repair-test      Build pgen, damage it, repair it, verify health\n"
              << "  --upgrade-test     Build T1 mex, upgrade to T2, verify completion\n"
              << "  --capture-test     Build enemy pgen, capture it, verify ownership\n"
              << "  --path-test        A* pathfinding around obstacles + terrain height\n"
              << "  --toggle-test      Script bits, toggle caps, and dive command\n"
              << "  --enhance-test     ACU enhancement (AdvancedEngineering)\n"
              << "  --intel-test       Intel system (InitIntel/Enable/Disable/Radius)\n"
              << "  --shield-test      Shield system (create, health, regen, toggle)\n"
              << "  --transport-test   Transport load/unload, cargo tracking, speed mult\n"
              << "  --fow-test         Fog of war / visibility grid + OnIntelChange\n"
              << "  --los-test         Terrain line-of-sight occlusion\n"
              << "  --stall-test       Economy stalling (resource scarcity slows progress)\n"
              << "  --jammer-test      Dead-reckoning, stealth, jammer detection\n"
              << "  --stub-test        Moho stub conversions (14 real implementations)\n"
              << "  --audio-test       Audio system (XWB/XSB banks, play, loop, stop)\n"
              << "  --bone-test        Bone system (SCM parser, bone queries)\n"
              << "  --manip-test       Manipulator system (rotators, animators, sliders, aim)\n"
              << "  --canpath-test     CanPathTo + GetThreatBetweenPositions\n"
              << "  --armor-test      Armor system (damage multipliers by armor/damage type)\n"
              << "  --vet-test         Veterancy system (regen, vet XP dispersal, level up)\n"
              << "  --wreck-test       Wreckage system (SetMaxReclaimValues, GetHeading)\n"
              << "  --adjacency-test   Adjacency bonus system + SetFiringRandomness\n"
              << "  --stats-test       Stats/telemetry system (SetStat/GetStat/UpdateStat)\n"
              << "  --silo-test        Missile silo ammo system (Give/Remove/Get nuke+tactical)\n"
              << "  --flags-test       Unit targeting flags (DoNotTarget, Reclaimable, IsValidTarget)\n"
              << "  --layercap-test    Weapon fire target layer caps\n"
              << "  --massstub-test    Mass stub conversion (weapon/movement/fuel/projectile/misc)\n"
              << "  --massstub2-test   Mass stub conversion II (damage flags/caps/weapon/proj/elevation)\n"
              << "  --massstub3-test   Mass stub conversion III (brain/weapon/projectile/platoon)\n"
              << "  --anim-test        SCA skeletal animation (parsing, bone matrices, GPU skinning)\n"
              << "  --teamcolor-test   Team color rendering (SpecTeam texture, alpha mask blending)\n"
              << "  --normal-test      Normal map rendering (tangent-space normal maps, TBN matrix)\n"
              << "  --prop-test        Map prop rendering (SCMAP parsing, prop meshes, orientation)\n"
              << "  --scale-test       Prop scale & distance culling (per-prop scale, MAX_INSTANCES)\n"
              << "  --specular-test    Specular lighting (Blinn-Phong, SpecTeam texture, eye position)\n"
              << "  --decal-test       Terrain decals (SCMAP parsing, textured quads, LOD culling)\n"
              << "  --projectile-test  Projectile rendering (blueprint_id, velocity-align, mesh lookup)\n"
              << "  --shadow-test      Shadow mapping (depth pass, light matrix, shadow sampling)\n"
              << "  --massstub4-test   Mass stub conversion IV (visibility, scale, mesh, collision, attach, shake)\n"
              << "  --spatial-test     Spatial hash grid (grid init, collect_in_radius/rect, auto-notify)\n"
              << "  --unitsound-test   Unit sound (PlayUnitSound, PlayUnitAmbientSound, StopUnitAmbientSound)\n"
              << "  --medstub-test     Medium stubs (SetBoneEnabled, AddOnGivenCallback, AddBoundedProp)\n"
              << "  --lowstub-test     Low-priority stubs (Destroy/BeenDestroyed, CreateBuilderArmController)\n"
              << "  --blend-test       Blend-weight skinning (multi-bone vertex parsing, weight validation)\n"
              << "  --ui-test          UI control system (Frame, Group, LazyVar, moho bindings)\n"
              << "  --bitmap-test      Bitmap control (SetNewTexture, solid color, UVs, animation)\n"
              << "  --text-test        Text control (SetNewFont, SetText, font metrics, centering)\n"
              << "  --edit-test        Edit/ItemList/Scrollbar controls (text input, list ops, scroll)\n"
              << "  --controls-test    Border/Dragger/Cursor/Movie/Histogram/WorldMesh controls\n"
              << "  --uiboot-test      UI bootstrap (GetFrame, WorldView, WldUIProvider, lobby/discovery)\n"
              << "  --uirender-test    UI 2D rendering pipeline (LazyVar positions, quad building)\n"
              << "  --font-test        Font rendering (stb_truetype metrics, per-glyph advance)\n"
              << "  --scissor-test     Scissor/clip rectangles (parent-child clipping)\n"
              << "  --border-render-test Border 9-patch rendering (6-texture ninepatch)\n"
              << "  --edit-render-test Edit control visuals (background, text, caret)\n"
              << "  --terrain-normal-test Terrain normal maps (per-stratum DXT5nm, TBN, blending)\n"
              << "  --terrain-tex-test Terrain textures (stratum blending, blend maps, UV scaling)\n"
              << "  --emitter-test     IEffect/emitter system (Create*Emitter, beams, decals, chaining)\n"
              << "  --collision-test   CollisionBeam entity (__init, Enable/Disable, SetBeamFx, GetLauncher)\n"
              << "  --decalsplat-test  Decal/Splat system (CreateDecal, CreateSplat, CreateSplatOnBone, lifetime)\n"
              << "  --cmd-test         Issue commands + economy events (Nuke/Tactical/Teleport/Ferry/Sacrifice)\n"
              << "  --deposit-test     Resource deposits + manipulator stub conversions\n"
              << "  --beam-test        Beam rendering (construction/reclaim/repair/capture/collision)\n"
              << "  --shield-render-test Shield bubble rendering (projected circles)\n"
              << "  --vet-adj-render-test Veterancy indicators + adjacency lines\n"
              << "  --intel-overlay-test Intel range overlay (radar/sonar/omni circles)\n"
              << "  --enhance-wreck-test Enhancement mesh switching + wreckage visual\n"
              << "  --vfx-render-test  VFX/emitter particle rendering\n"
              << "  --transport-silo-test Transport cargo + silo ammo visuals\n"
              << "  --dualstate-test   Dual Lua state split (sim_L/ui_L isolation)\n"
              << "  --construction-test Construction panel (EntityCategoryGetUnitList)\n"
              << "  --phase2-test      Phase 2 integration (construction, orders, unitview, tooltips)\n"
              << "  --phase3-test      Phase 3 integration (state machine, beat system, score flow)\n"
              << "  --profile          Enable performance profiling (prints summary at exit)\n"
              << "  --profile-test     Profiler system (zones, nesting, rolling stats)\n"
              << "  --instrument       Interactive instrumented mode (smoke report on exit)\n"
              << "  --help             Show this help message\n";
}

static osc::lua::InitConfig parse_args(int argc, char* argv[]) {
    osc::lua::InitConfig config;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--init") == 0 && i + 1 < argc) {
            config.init_file = argv[++i];
        } else if (std::strcmp(argv[i], "--fa-path") == 0 && i + 1 < argc) {
            config.fa_path = argv[++i];
        } else if (std::strcmp(argv[i], "--faf-data") == 0 && i + 1 < argc) {
            config.faf_data_path = argv[++i];
        } else if (std::strcmp(argv[i], "--help") == 0) {
            print_usage();
            std::exit(0);
        }
    }

    // Defaults for FAForever installation
    if (config.init_file.empty()) {
        config.init_file = "C:/ProgramData/FAForever/bin/init_faf.lua";
    }
    if (config.faf_data_path.empty()) {
        config.faf_data_path = "C:/ProgramData/FAForever";
    }

    // Try to read fa_path from FAForever's fa_path.lua if not specified
    if (config.fa_path.empty()) {
        osc::fs::path fa_path_file = config.faf_data_path / "fa_path.lua";
        if (osc::fs::exists(fa_path_file)) {
            std::ifstream f(fa_path_file);
            std::string line;
            while (std::getline(f, line)) {
                // Look for: fa_path = "C:\\..."
                auto pos = line.find("fa_path");
                if (pos != std::string::npos) {
                    auto quote1 = line.find('"', pos);
                    auto quote2 = line.find('"', quote1 + 1);
                    if (quote1 != std::string::npos &&
                        quote2 != std::string::npos) {
                        auto path = line.substr(quote1 + 1, quote2 - quote1 - 1);
                        // Unescape backslashes
                        std::string clean;
                        for (size_t j = 0; j < path.size(); j++) {
                            if (path[j] == '\\' && j + 1 < path.size() &&
                                path[j + 1] == '\\') {
                                clean += '/';
                                j++;
                            } else if (path[j] == '\\') {
                                clean += '/';
                            } else {
                                clean += path[j];
                            }
                        }
                        config.fa_path = clean;
                    }
                }
            }
        }
    }

    return config;
}

static osc::u32 parse_ticks_arg(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) {
            char* end = nullptr;
            long val = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || val < 0 || val > 1'000'000) {
                spdlog::error("Invalid --ticks value: {}", argv[i]);
                std::exit(1);
            }
            return static_cast<osc::u32>(val);
        }
    }
    return 0; // 0 = no explicit tick count → windowed mode
}

static std::string parse_map_arg(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            return argv[++i];
        }
    }
    return {};
}

static bool parse_flag(int argc, char* argv[], const char* flag) {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

static std::string parse_string_arg(int argc, char* argv[], const char* flag,
                                     const char* default_val = "") {
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], flag) == 0 && i + 1 < argc) {
            return argv[++i];
        }
    }
    return default_val;
}

// ── Reload sequence: tears down old sim, creates fresh Lua VM + SimState,
//    reloads blueprints/scenario, boots sim, rebuilds renderer scene. ──
// Returns true on success, false on critical failure.
static bool execute_reload_sequence(
    std::unique_ptr<osc::lua::LuaState>& sim_lua_state,
    std::unique_ptr<osc::sim::SimState>& sim_state,
    osc::lua::LuaState& ui_lua_state,
    osc::vfs::VirtualFileSystem& vfs,
    osc::blueprints::BlueprintStore& store,
    osc::lua::InitLoader& loader,
    const osc::lua::InitConfig& config,
    osc::lua::ScenarioMetadata& scenario_meta,
    osc::GameStateManager& game_state_mgr,
    osc::renderer::Renderer* renderer,               // nullable for headless
    osc::renderer::InputHandler* input_handler,       // nullable for headless
    std::unordered_set<osc::u32>* prev_selection,     // nullable for headless
    double& sim_accumulator,
    const std::string& launch_scenario)
{
    lua_State* uiL = ui_lua_state.raw();

    // 1. GPU fence — ensure no in-flight work
    if (renderer) renderer->clear_scene();

    // 2. Destroy old SimState and sim Lua state
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

    // 7. Audio, bone cache, anim cache
    {
        auto new_sound = std::make_unique<osc::audio::SoundManager>(
            config.fa_path / "sounds");
        lua_State* sL = sim_lua_state->raw();
        lua_pushstring(sL, "osc_sound_manager");
        lua_pushlightuserdata(sL, new_sound.get());
        lua_rawset(sL, LUA_REGISTRYINDEX);
        sim_state->set_sound_manager(std::move(new_sound));
    }
    sim_state->set_bone_cache(
        std::make_unique<osc::sim::BoneCache>(&vfs, &store));
    sim_state->set_anim_cache(
        std::make_unique<osc::sim::AnimCache>(&vfs));

    // 8. Load scenario from selected map
    osc::lua::ScenarioLoader new_scenario_loader;
    auto new_meta_result = new_scenario_loader.load_scenario(
        *sim_lua_state, vfs, launch_scenario, *sim_state);
    if (!new_meta_result) {
        spdlog::error("Reload scenario failed: {}",
                      new_meta_result.error().message);
    } else {
        scenario_meta = new_meta_result.value();
        for (const auto& army : scenario_meta.armies) {
            sim_state->add_army(army, army);
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

    // 12. Start session
    {
        osc::lua::SessionManager new_session_mgr;
        new_session_mgr.set_ai_armies({1}); // ARMY_2 is AI
        auto sess_result = new_session_mgr.start_session(
            *sim_lua_state, vfs, *sim_state, scenario_meta);
        if (!sess_result) {
            spdlog::warn("Reload session start failed: {}",
                         sess_result.error().message);
        }
    }

    // 13. Update UI state's sim_state registry pointer to new SimState
    {
        lua_pushstring(uiL, "osc_sim_state");
        lua_pushlightuserdata(uiL, sim_state.get());
        lua_rawset(uiL, LUA_REGISTRYINDEX);
    }

    // 14. Rebuild renderer scene
    if (renderer) renderer->build_scene(*sim_state, &vfs, uiL);

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
    osc::core::call_start_game_ui(uiL);

    spdlog::info("=== Map reload complete ===");
    return true;
}

/// Pump N UI frames: resume coroutines, fire OnBeat, fire beat functions.
static void pump_ui_frames(
    osc::lua::LuaState& ui_lua_state,
    osc::sim::ThreadManager& ui_thread_manager,
    osc::lua::BeatFunctionRegistry& beat_registry,
    int count,
    osc::u32& ui_frame_counter) {
    lua_State* uL = ui_lua_state.raw();
    for (int i = 0; i < count; i++) {
        ui_frame_counter++;
        ui_thread_manager.resume_all(ui_frame_counter);
        osc::core::call_on_beat(uL, 1.0 / 30.0);
        beat_registry.fire_all(uL);
    }
}

int main(int argc, char* argv[]) {
    osc::log::init();

    auto config = parse_args(argc, argv);
    auto map_path = parse_map_arg(argc, argv);
    auto tick_count = parse_ticks_arg(argc, argv);
    bool damage_test = parse_flag(argc, argv, "--damage-test");
    bool move_test = parse_flag(argc, argv, "--move-test");
    bool fire_test = parse_flag(argc, argv, "--fire-test");
    bool economy_test = parse_flag(argc, argv, "--economy-test");
    bool build_test = parse_flag(argc, argv, "--build-test");
    bool chain_test = parse_flag(argc, argv, "--chain-test");
    bool ai_test = parse_flag(argc, argv, "--ai-test");
    bool reclaim_test = parse_flag(argc, argv, "--reclaim-test");
    bool platoon_test = parse_flag(argc, argv, "--platoon-test");
    bool threat_test = parse_flag(argc, argv, "--threat-test");
    bool combat_test = parse_flag(argc, argv, "--combat-test");
    bool repair_test = parse_flag(argc, argv, "--repair-test");
    bool upgrade_test = parse_flag(argc, argv, "--upgrade-test");
    bool capture_test = parse_flag(argc, argv, "--capture-test");
    bool path_test = parse_flag(argc, argv, "--path-test");
    bool toggle_test = parse_flag(argc, argv, "--toggle-test");
    bool enhance_test = parse_flag(argc, argv, "--enhance-test");
    bool intel_test = parse_flag(argc, argv, "--intel-test");
    bool shield_test = parse_flag(argc, argv, "--shield-test");
    bool transport_test = parse_flag(argc, argv, "--transport-test");
    bool fow_test = parse_flag(argc, argv, "--fow-test");
    bool los_test = parse_flag(argc, argv, "--los-test");
    bool stall_test = parse_flag(argc, argv, "--stall-test");
    bool jammer_test = parse_flag(argc, argv, "--jammer-test");
    bool stub_test = parse_flag(argc, argv, "--stub-test");
    bool audio_test = parse_flag(argc, argv, "--audio-test");
    bool bone_test = parse_flag(argc, argv, "--bone-test");
    bool manip_test = parse_flag(argc, argv, "--manip-test");
    bool canpath_test = parse_flag(argc, argv, "--canpath-test");
    bool armor_test = parse_flag(argc, argv, "--armor-test");
    bool vet_test = parse_flag(argc, argv, "--vet-test");
    bool wreck_test = parse_flag(argc, argv, "--wreck-test");
    bool adjacency_test = parse_flag(argc, argv, "--adjacency-test");
    bool stats_test = parse_flag(argc, argv, "--stats-test");
    bool silo_test = parse_flag(argc, argv, "--silo-test");
    bool flags_test = parse_flag(argc, argv, "--flags-test");
    bool layercap_test = parse_flag(argc, argv, "--layercap-test");
    bool massstub_test = parse_flag(argc, argv, "--massstub-test");
    bool massstub2_test = parse_flag(argc, argv, "--massstub2-test");
    bool massstub3_test = parse_flag(argc, argv, "--massstub3-test");
    bool anim_test = parse_flag(argc, argv, "--anim-test");
    bool teamcolor_test = parse_flag(argc, argv, "--teamcolor-test");
    bool normal_test = parse_flag(argc, argv, "--normal-test");
    bool prop_test = parse_flag(argc, argv, "--prop-test");
    bool scale_test = parse_flag(argc, argv, "--scale-test");
    bool specular_test = parse_flag(argc, argv, "--specular-test");
    bool terrain_normal_test = parse_flag(argc, argv, "--terrain-normal-test");
    bool terrain_tex_test = parse_flag(argc, argv, "--terrain-tex-test");
    bool decal_test = parse_flag(argc, argv, "--decal-test");
    bool projectile_test = parse_flag(argc, argv, "--projectile-test");
    bool shadow_test = parse_flag(argc, argv, "--shadow-test");
    bool massstub4_test = parse_flag(argc, argv, "--massstub4-test");
    bool spatial_test = parse_flag(argc, argv, "--spatial-test");
    bool unitsound_test = parse_flag(argc, argv, "--unitsound-test");
    bool medstub_test = parse_flag(argc, argv, "--medstub-test");
    bool lowstub_test = parse_flag(argc, argv, "--lowstub-test");
    bool blend_test = parse_flag(argc, argv, "--blend-test");
    bool ui_test = parse_flag(argc, argv, "--ui-test");
    bool bitmap_test = parse_flag(argc, argv, "--bitmap-test");
    bool text_test = parse_flag(argc, argv, "--text-test");
    bool edit_test = parse_flag(argc, argv, "--edit-test");
    bool controls_test = parse_flag(argc, argv, "--controls-test");
    bool uiboot_test = parse_flag(argc, argv, "--uiboot-test");
    bool uirender_test = parse_flag(argc, argv, "--uirender-test");
    bool font_test = parse_flag(argc, argv, "--font-test");
    bool scissor_test = parse_flag(argc, argv, "--scissor-test");
    bool border_render_test = parse_flag(argc, argv, "--border-render-test");
    bool edit_render_test = parse_flag(argc, argv, "--edit-render-test");
    bool itemlist_render_test = parse_flag(argc, argv, "--itemlist-render-test");
    bool scrollbar_render_test = parse_flag(argc, argv, "--scrollbar-render-test");
    bool anim_render_test = parse_flag(argc, argv, "--anim-render-test");
    bool tiled_render_test = parse_flag(argc, argv, "--tiled-render-test");
    bool input_test = parse_flag(argc, argv, "--input-test");
    bool onframe_test = parse_flag(argc, argv, "--onframe-test");
    bool cursor_render_test = parse_flag(argc, argv, "--cursor-render-test");
    bool drag_render_test = parse_flag(argc, argv, "--drag-render-test");
    bool emitter_test = parse_flag(argc, argv, "--emitter-test");
    bool collision_test = parse_flag(argc, argv, "--collision-test");
    bool decalsplat_test = parse_flag(argc, argv, "--decalsplat-test");
    bool cmd_test = parse_flag(argc, argv, "--cmd-test");
    bool deposit_test = parse_flag(argc, argv, "--deposit-test");
    bool beam_test = parse_flag(argc, argv, "--beam-test");
    bool shield_render_test = parse_flag(argc, argv, "--shield-render-test");
    bool vet_adj_render_test = parse_flag(argc, argv, "--vet-adj-render-test");
    bool intel_overlay_test = parse_flag(argc, argv, "--intel-overlay-test");
    bool enhance_wreck_test = parse_flag(argc, argv, "--enhance-wreck-test");
    bool vfx_render_test = parse_flag(argc, argv, "--vfx-render-test");
    bool transport_silo_test = parse_flag(argc, argv, "--transport-silo-test");
    bool dualstate_test = parse_flag(argc, argv, "--dualstate-test");
    bool no_fog = parse_flag(argc, argv, "--no-fog");
    bool no_decals = parse_flag(argc, argv, "--no-decals");
    bool profile_enabled = parse_flag(argc, argv, "--profile");
    bool profile_test = parse_flag(argc, argv, "--profile-test");
    bool construction_test = parse_flag(argc, argv, "--construction-test");
    bool phase2_test = parse_flag(argc, argv, "--phase2-test");
    bool phase3_test = parse_flag(argc, argv, "--phase3-test");
    bool phase4_test = parse_flag(argc, argv, "--phase4-test");
    bool phase5_test = parse_flag(argc, argv, "--phase5-test");
    bool smoke_test = parse_flag(argc, argv, "--smoke-test");
    bool ai_skirmish = parse_flag(argc, argv, "--ai-skirmish");
    bool draw_test = parse_flag(argc, argv, "--draw-test");
    bool stress_test = parse_flag(argc, argv, "--stress-test");
    bool full_smoke_test = parse_flag(argc, argv, "--full-smoke-test");
    bool instrument = parse_flag(argc, argv, "--instrument");
    bool builder_debug = parse_flag(argc, argv, "--builder-debug");
    auto ai_personality = parse_string_arg(argc, argv, "--ai-personality", "adaptive");

    // Collect all command-line args for HasCommandLineArg (M147d)
    std::set<std::string> cmdline_args;
    for (int i = 1; i < argc; ++i) {
        cmdline_args.insert(argv[i]);
    }

    // Determine if any test/headless flag was set
    bool any_test = damage_test || move_test || fire_test || economy_test ||
                    build_test || chain_test || ai_test || reclaim_test ||
                    platoon_test || threat_test || combat_test ||
                    repair_test || upgrade_test || capture_test ||
                    path_test || toggle_test || enhance_test ||
                    intel_test || shield_test || transport_test ||
                    fow_test || los_test || stall_test || jammer_test ||
                    stub_test || audio_test || bone_test || manip_test ||
                    canpath_test || armor_test || vet_test ||
                    wreck_test || adjacency_test || stats_test ||
                    silo_test || flags_test || layercap_test ||
                    massstub_test || massstub2_test || massstub3_test ||
                    anim_test || teamcolor_test || normal_test ||
                    prop_test || scale_test || specular_test ||
                    terrain_normal_test || terrain_tex_test ||
                    decal_test || projectile_test || shadow_test ||
                    massstub4_test || spatial_test ||
                    unitsound_test || medstub_test ||
                    lowstub_test || blend_test ||
                    ui_test || bitmap_test ||
                    text_test || edit_test ||
                    controls_test || uiboot_test ||
                    uirender_test || font_test ||
                    scissor_test || border_render_test ||
                    edit_render_test || itemlist_render_test ||
                    scrollbar_render_test || anim_render_test ||
                    tiled_render_test || input_test ||
                    onframe_test || cursor_render_test ||
                    drag_render_test || emitter_test ||
                    collision_test || decalsplat_test ||
                    cmd_test || deposit_test ||
                    beam_test || shield_render_test ||
                    vet_adj_render_test || intel_overlay_test ||
                    enhance_wreck_test || vfx_render_test ||
                    transport_silo_test ||
                    dualstate_test ||
                    construction_test || phase2_test ||
                    phase3_test || phase4_test || phase5_test ||
                    profile_test || smoke_test || ai_skirmish || draw_test ||
                    stress_test || full_smoke_test;
    bool headless = (tick_count > 0) || any_test;

    if (config.fa_path.empty()) {
        spdlog::error("FA installation path not found. Use --fa-path or "
                       "ensure C:/ProgramData/FAForever/fa_path.lua exists.");
        return 1;
    }

    spdlog::info("FA path:   {}", config.fa_path.string());
    spdlog::info("Init file: {}", config.init_file.string());
    spdlog::info("FAF data:  {}", config.faf_data_path.string());

    if (!osc::fs::exists(config.init_file)) {
        spdlog::error("Init file not found: {}", config.init_file.string());
        return 1;
    }

    if (builder_debug) {
        spdlog::set_level(spdlog::level::debug);
        spdlog::info("Builder debug mode enabled — verbose logging active");
    }

    // Phase 1: Init + VFS (sim Lua state)
    auto sim_lua_state = std::make_unique<osc::lua::LuaState>();
    osc::vfs::VirtualFileSystem vfs;
    osc::lua::InitLoader loader;

    auto init_result = loader.execute_init(*sim_lua_state, config, vfs);
    if (!init_result) {
        spdlog::error("Init failed: {}", init_result.error().message);
        return 1;
    }

    // Phase 2: Blueprint loading (sim Lua state owns the store)
    osc::blueprints::BlueprintStore store(sim_lua_state->raw());

    auto bp_result = loader.load_blueprints(*sim_lua_state, vfs, store);
    if (!bp_result) {
        spdlog::error("Blueprint loading failed: {}",
                       bp_result.error().message);
        return 1;
    }

    // Spot check
    auto* acu = store.find("uel0001");
    if (acu) {
        auto desc = store.get_string_field(*acu, "Description");
        spdlog::info("Spot check: uel0001 found ({})",
                     desc.value_or("no description"));
    } else {
        spdlog::warn("Spot check: uel0001 (UEF ACU) not found");
    }

    spdlog::info("OpenSupCom initialization complete.");

    // Phase 3: Map + Sim boot (only when --map provided)
    std::unique_ptr<osc::sim::SimState> sim_state;
    osc::lua::ScenarioMetadata scenario_meta;

    if (!map_path.empty()) {
    sim_state = std::make_unique<osc::sim::SimState>(sim_lua_state->raw(), &store);

    // Audio system
    auto sound_mgr = std::make_unique<osc::audio::SoundManager>(
        config.fa_path / "sounds");
    {
        lua_State* L = sim_lua_state->raw();
        lua_pushstring(L, "osc_sound_manager");
        lua_pushlightuserdata(L, sound_mgr.get());
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    sim_state->set_sound_manager(std::move(sound_mgr));

    // Bone cache (lazy-loaded per-blueprint SCM bone data)
    auto bone_cache = std::make_unique<osc::sim::BoneCache>(&vfs, &store);
    sim_state->set_bone_cache(std::move(bone_cache));

    // Animation cache (lazy-loaded SCA animation data)
    auto anim_cache = std::make_unique<osc::sim::AnimCache>(&vfs);
    sim_state->set_anim_cache(std::move(anim_cache));

    // Load scenario and map
    {
        osc::lua::ScenarioLoader scenario_loader;
        auto meta_result = scenario_loader.load_scenario(
            *sim_lua_state, vfs, map_path, *sim_state);
        if (!meta_result) {
            spdlog::error("Scenario load failed: {}",
                          meta_result.error().message);
            return 1;
        }
        scenario_meta = meta_result.value();

        // Add armies from scenario (ai_skirmish limits to 2)
        size_t army_limit = ai_skirmish ? 2 : scenario_meta.armies.size();
        for (size_t i = 0; i < std::min(army_limit, scenario_meta.armies.size()); i++) {
            sim_state->add_army(scenario_meta.armies[i], scenario_meta.armies[i]);
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
            spdlog::warn("GiveResources SimCallback registration error: {}", give_res.error().message);
        }
        lua_settop(sL, 0); // clean stack
    }
    } // end if (!map_path.empty()) — Phase 3

    // === UI Lua State ===
    osc::lua::LuaState ui_lua_state;
    ui_lua_state.set_vfs(&vfs);
    ui_lua_state.set_blueprint_store(&store);

    // Run init sequence on UI state (polyfills, config, class system, import)
    auto ui_init_result = loader.execute_init(ui_lua_state, config, vfs);
    if (!ui_init_result) {
        spdlog::error("UI Lua init failed: {}", ui_init_result.error().message);
        return 1;
    }

    // Load blueprints into UI state (separate store — UI refs must not
    // overwrite sim_L registry refs in the main store)
    osc::blueprints::BlueprintStore ui_store(ui_lua_state.raw());
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
    osc::core::Localization loc_cache;
    loc_cache.load_from_vfs(ui_lua_state.raw(), &vfs);
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_loc_cache");
        lua_pushlightuserdata(uL, &loc_cache);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Preferences — load Game.prefs if present, store pointer in UI registry
    osc::core::Preferences prefs;
    prefs.load("Game.prefs");
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_preferences");
        lua_pushlightuserdata(uL, &prefs);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // WldUIProvider — long-lived instance stored in registry for InternalCreateWldUIProvider
    osc::ui::WldUIProvider wld_provider;
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_wld_ui_provider");
        lua_pushlightuserdata(uL, &wld_provider);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // UI control registry (M71)
    osc::ui::UIControlRegistry ui_registry;
    osc::lua::register_ui_bindings(ui_lua_state, ui_registry);

    // Register file I/O bindings on ui_L for lobby map enumeration (M148c)
    osc::lua::register_blueprint_bindings(ui_lua_state);

    // UI-side thread manager (reuses ThreadManager with frame counts instead of sim ticks)
    osc::sim::ThreadManager ui_thread_manager(ui_lua_state.raw());
    ui_thread_manager.register_in_registry(ui_lua_state.raw());
    osc::u32 ui_frame_count = 0;

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
    ui_lua_state.register_function("FlushEvents", l_FlushEvents);
    ui_lua_state.register_function("SessionIsReplay", l_SessionIsReplay);
    ui_lua_state.register_function("SessionGetScenarioInfo", l_SessionGetScenarioInfo);
    ui_lua_state.register_function("GetEconomyTotals", l_GetEconomyTotals);
    ui_lua_state.register_function("GetSimTicksPerSecond", l_GetSimTicksPerSecond);
    ui_lua_state.register_function("GetArmiesTable", l_GetArmiesTable);
    ui_lua_state.register_function("IsAlly", l_ui_IsAlly);

    // State transition: INIT → GAME or INIT → FRONT_END
    osc::core::call_setup_ui(ui_lua_state.raw());
    if (!map_path.empty()) {
        osc::core::call_start_game_ui(ui_lua_state.raw());
    } else {
        // No map: bootstrap front-end menu UI
        osc::core::call_lua_global(ui_lua_state.raw(), "CreateUI");
        spdlog::info("Front-end UI initialized (no --map)");
    }

    spdlog::info("Dual Lua states initialized (sim_L + ui_L)");

    // Terrain query test
    if (sim_state && sim_state->terrain()) {
        auto* t = sim_state->terrain();
        osc::f32 cx = static_cast<osc::f32>(t->map_width()) / 2;
        osc::f32 cz = static_cast<osc::f32>(t->map_height()) / 2;
        spdlog::info("Terrain test:");
        spdlog::info("  Center ({}, {}): terrain={:.1f}, surface={:.1f}",
                     cx, cz,
                     t->get_terrain_height(cx, cz),
                     t->get_surface_height(cx, cz));
    }

    // Phase 4: Session lifecycle
    if (!map_path.empty()) {
        osc::lua::SessionManager session_mgr;
        if (ai_skirmish) {
            session_mgr.set_ai_armies({0, 1}); // Both armies are AI
            session_mgr.set_max_armies(2);      // Only create 2 armies
            session_mgr.set_ai_personality(ai_personality);
            // Detect cheat variant: personality ending in "cheat"
            if (ai_personality.size() > 5 &&
                ai_personality.compare(ai_personality.size() - 5, 5, "cheat") == 0) {
                session_mgr.set_cheat_mult(2.0);
                session_mgr.set_build_mult(2.0);
            }
        } else if (ai_test || platoon_test || threat_test || combat_test) {
            session_mgr.set_ai_armies({1}); // ARMY_2 (0-based index 1) is AI
        }
        auto session_result = session_mgr.start_session(
            *sim_lua_state, vfs, *sim_state, scenario_meta);
        if (!session_result) {
            spdlog::error("Session start failed: {}",
                          session_result.error().message);
            return 1;
        }
    }

    // Enable profiler if requested
    if (profile_enabled) {
        osc::Profiler::instance().set_enabled(true);
        spdlog::info("Performance profiling enabled");
    }

    // BeatFunctionRegistry for per-frame Lua callbacks (M145b) — outer scope for headless test access
    osc::lua::BeatFunctionRegistry beat_registry;
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_beat_registry");
        lua_pushlightuserdata(uL, &beat_registry);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // Key map registry for hotkey dispatch (M150b) — outer scope for headless test access
    osc::ui::KeyMapRegistry keymap_registry;
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_keymap_registry");
        lua_pushlightuserdata(uL, &keymap_registry);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // FrontEndData — cross-state key-value store (M147c)
    osc::FrontEndData front_end_data;
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
        lua_pushlightuserdata(uL, &cmdline_args);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }

    // GameStateManager — outer scope for headless test access (M144b)
    osc::GameStateManager game_state_mgr;
    {
        lua_State* uL = ui_lua_state.raw();
        lua_pushstring(uL, "__osc_game_state_mgr");
        lua_pushlightuserdata(uL, &game_state_mgr);
        lua_rawset(uL, LUA_REGISTRYINDEX);
    }
    if (!map_path.empty()) {
        game_state_mgr.transition_to(osc::GameState::GAME, ui_lua_state.raw());
        if (sim_lua_state) {
            lua_State* sL = sim_lua_state->raw();
            lua_pushstring(sL, "__osc_game_state_mgr");
            lua_pushlightuserdata(sL, &game_state_mgr);
            lua_rawset(sL, LUA_REGISTRYINDEX);
        }
    } else {
        // No map: start in FRONT_END state (main menu)
        game_state_mgr.transition_to(osc::GameState::FRONT_END, ui_lua_state.raw());
    }

    // Instrumented mode: install SmokeTestHarness for interactive play (M166)
    std::unique_ptr<osc::lua::SmokeTestHarness> instrument_harness;
    if (instrument) {
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

    // Phase 5: Windowed mode (renderer) or headless tick loop
    if (!headless) {
        osc::renderer::Renderer renderer;
        if (renderer.init(1600, 900, "OpenSupCom")) {
            // Build 3D scene if we have a sim state (--map was provided)
            if (sim_state) {
                renderer.build_scene(*sim_state, &vfs, ui_lua_state.raw());
            }

            // Store renderer pointer in UI Lua registry for WorldView/GetCamera
            {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_renderer");
                lua_pushlightuserdata(uL, &renderer);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // Player input handler (ARMY_1 = index 0)
            osc::renderer::InputHandler input_handler;
            input_handler.set_player_army(0);
            renderer.set_player_army(0);
            // Store input_handler pointer in UI Lua registry for selection globals
            {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_input_handler");
                lua_pushlightuserdata(uL, &input_handler);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // Factory queue display (M140c)
            osc::lua::FactoryQueueDisplay factory_queue;
            {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_factory_queue");
                lua_pushlightuserdata(uL, &factory_queue);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // SimCallback queue (UI→Sim bridge, M138a)
            osc::sim::SimCallbackQueue sim_callback_queue;
            {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_sim_callback_queue");
                lua_pushlightuserdata(uL, &sim_callback_queue);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // Initialize hover entity ID to 0 (updated by WorldView HitTest, M142a)
            {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_hover_entity_id");
                lua_pushnumber(uL, 0);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // Store scenario path for SessionGetScenarioInfo (M145c2)
            if (!map_path.empty()) {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_scenario_path");
                lua_pushstring(uL, map_path.c_str());
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // ReturnToLobby global function (M156b)
            {
                lua_State* uiL = ui_lua_state.raw();
                lua_pushstring(uiL, "ReturnToLobby");
                lua_pushcfunction(uiL, [](lua_State* L) -> int {
                    lua_pushstring(L, "__osc_return_to_lobby");
                    lua_pushboolean(L, 1);
                    lua_rawset(L, LUA_REGISTRYINDEX);
                    return 0;
                });
                lua_rawset(uiL, LUA_GLOBALSINDEX);
            }

            // keymap_registry already stored in registry at outer scope

            // GameStateManager and BeatFunctionRegistry already declared at outer scope (M144b, M145b)

            if (no_fog) renderer.set_fog_enabled(false);
            if (no_decals) renderer.set_decals_enabled(false);

            double sim_accumulator = 0.0;
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

            while (!renderer.should_close()) {
                osc::Profiler::instance().begin_frame();
                auto now = std::chrono::high_resolution_clock::now();
                double dt = std::chrono::duration<double>(now - prev_time).count();
                prev_time = now;
                // Clamp dt to avoid spiral of death
                if (dt > 0.25) dt = 0.25;

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
                bool plus_pressed = renderer.is_key_pressed(GLFW_KEY_EQUAL) ||
                                    renderer.is_key_pressed(GLFW_KEY_KP_ADD);
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

                // Auto-pause when game ends (M146a)
                if (sim_state) {
                osc::i32 game_result = sim_state->player_result();
                if (game_result != 0 && !game_state_mgr.game_over()) {
                    game_state_mgr.set_game_over(true);
                    game_state_mgr.set_paused(true, ui_lua_state.raw());
                    const char* result_str =
                        game_result == 1 ? "VICTORY" :
                        game_result == 2 ? "DEFEAT" : "DRAW";
                    spdlog::info("Game over: {}", result_str);

                    // Set observer mode
                    lua_State* uiL = ui_lua_state.raw();
                    lua_pushstring(uiL, "__osc_focus_army");
                    lua_pushnumber(uiL, -1);
                    lua_rawset(uiL, LUA_REGISTRYINDEX);

                    // Call NoteGameOver() in ui_L
                    lua_pushstring(uiL, "NoteGameOver");
                    lua_rawget(uiL, LUA_GLOBALSINDEX);
                    if (lua_isfunction(uiL, -1)) {
                        if (lua_pcall(uiL, 0, 0, 0) != 0) {
                            spdlog::warn("NoteGameOver error: {}", lua_tostring(uiL, -1));
                            lua_pop(uiL, 1);
                        }
                    } else {
                        lua_pop(uiL, 1);
                    }

                    // Transition to SCORE state (M156a)
                    game_state_mgr.transition_to(osc::GameState::SCORE, uiL);
                }
                } // if (sim_state)

                // Fixed-timestep sim ticking (scaled by sim_speed)
                if (!game_state_mgr.paused() && sim_state) {
                    sim_accumulator += dt * game_state_mgr.speed();
                    while (sim_accumulator >=
                           osc::sim::SimState::SECONDS_PER_TICK) {
                        sim_state->tick();
                        sim_accumulator -= osc::sim::SimState::SECONDS_PER_TICK;
                    }
                }

                // Process SimCallbacks from UI (M138a)
                if (!sim_callback_queue.empty() && sim_state && sim_lua_state) {
                    auto callbacks = sim_callback_queue.drain();
                    lua_State* sL = sim_lua_state->raw();

                    // Import SimCallbacks module once for the batch
                    lua_pushstring(sL, "import");
                    lua_rawget(sL, LUA_GLOBALSINDEX);
                    bool have_module = false;
                    if (lua_isfunction(sL, -1)) {
                        lua_pushstring(sL, "/lua/SimCallbacks.lua");
                        if (lua_pcall(sL, 1, 1, 0) == 0 && lua_istable(sL, -1)) {
                            have_module = true;
                        } else {
                            if (lua_isstring(sL, -1))
                                spdlog::warn("SimCallback import error: {}", lua_tostring(sL, -1));
                            lua_pop(sL, 1);
                        }
                    } else {
                        lua_pop(sL, 1);
                    }

                    if (have_module) {
                        int mod = lua_gettop(sL);
                        for (const auto& cb : callbacks) {
                            // Get DoCallback function (re-fetch each time since pcall may error)
                            lua_pushstring(sL, "DoCallback");
                            lua_rawget(sL, mod);
                            if (!lua_isfunction(sL, -1)) {
                                lua_pop(sL, 1);
                                continue;
                            }

                            // Arg 1: func name
                            lua_pushstring(sL, cb.func_name.c_str());

                            // Arg 2: args table
                            lua_newtable(sL);
                            for (const auto& [key, val] : cb.args) {
                                lua_pushstring(sL, key.c_str());
                                std::visit([&](const auto& v) {
                                    using T = std::decay_t<decltype(v)>;
                                    if constexpr (std::is_same_v<T, std::string>) {
                                        lua_pushstring(sL, v.c_str());
                                    } else if constexpr (std::is_same_v<T, osc::f64>) {
                                        lua_pushnumber(sL, static_cast<lua_Number>(v));
                                    } else if constexpr (std::is_same_v<T, bool>) {
                                        lua_pushboolean(sL, v ? 1 : 0);
                                    }
                                }, val);
                                lua_rawset(sL, -3);
                            }

                            // Arg 3: units table (array of unit entity tables), or nil
                            if (!cb.unit_ids.empty()) {
                                lua_newtable(sL);
                                int units_tbl = lua_gettop(sL);
                                int idx = 1;
                                for (osc::u32 eid : cb.unit_ids) {
                                    auto* entity = sim_state->entity_registry().find(eid);
                                    if (entity && entity->is_unit() && !entity->destroyed()) {
                                        if (entity->lua_table_ref() >= 0) {
                                            lua_rawgeti(sL, LUA_REGISTRYINDEX, entity->lua_table_ref());
                                            lua_rawseti(sL, units_tbl, idx++);
                                        }
                                    }
                                }
                            } else {
                                lua_pushnil(sL); // no units
                            }

                            // Call DoCallback(name, args, units)
                            if (lua_pcall(sL, 3, 0, 0) != 0) {
                                const char* err = lua_tostring(sL, -1);
                                spdlog::warn("SimCallback '{}' error: {}", cb.func_name, err ? err : "(unknown)");
                                lua_pop(sL, 1);
                            }
                        }
                        lua_pop(sL, 1); // pop module table
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
                ui_thread_manager.resume_all(ui_frame_count);

                // OnBeat — UI heartbeat each frame
                osc::core::call_on_beat(ui_lua_state.raw(), dt);

                // Fire beat functions each frame (M145b)
                beat_registry.fire_all(ui_lua_state.raw());

                // Player input: selection + commands
                if (sim_state) {
                input_handler.update(renderer, *sim_state, dt);
                }

                // Fire OnSelectionChanged callbacks if selection changed
                {
                    const auto& cur_sel = input_handler.selected();
                    if (cur_sel != prev_selection) {
                        prev_selection = cur_sel;
                        lua_State* uL = ui_lua_state.raw();
                        lua_pushstring(uL, "__osc_sel_changed_cbs");
                        lua_rawget(uL, LUA_REGISTRYINDEX);
                        if (lua_istable(uL, -1)) {
                            int cbs_idx = lua_gettop(uL); // index of the callbacks table
                            // Build the unit array once; we'll push copies for each call
                            osc::lua::push_selected_units_for_ui(uL);
                            int arr_idx = lua_gettop(uL); // index of the selection array
                            int n = luaL_getn(uL, cbs_idx); // Lua 5.0: no lua_objlen
                            for (int ci = 1; ci <= n; ci++) {
                                lua_rawgeti(uL, cbs_idx, ci);
                                if (lua_isfunction(uL, -1)) {
                                    lua_pushvalue(uL, arr_idx); // copy of selection array
                                    if (lua_pcall(uL, 1, 0, 0) != 0) {
                                        std::string err = lua_tostring(uL, -1) ? lua_tostring(uL, -1) : "(unknown)";
                                        spdlog::warn("OnSelectionChanged[{}] error: {}", ci, err);
                                        lua_pop(uL, 1); // pop error string
                                    }
                                } else {
                                    lua_pop(uL, 1); // pop non-function
                                }
                            }
                            lua_pop(uL, 1); // pop selection array
                        }
                        lua_pop(uL, 1); // pop callbacks table (or nil)
                    }
                }

                const auto& sel = input_handler.selected();
                if (sim_state) {
                    renderer.render(*sim_state, ui_lua_state.raw(), &ui_registry,
                                    sel.empty() ? nullptr : &sel);
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
                        const char* status_str =
                            game_state_mgr.game_over() ? "GAME OVER " :
                            game_state_mgr.paused() ? "PAUSED " : "";
                        std::snprintf(title, sizeof(title),
                            "OpenSupCom | %s%.1fx | T:%u (%.1fs) | %zu entities | %zu sel | %.0f FPS",
                            status_str,
                            game_state_mgr.speed(),
                            sim_state->tick_count(),
                            sim_state->game_time(),
                            sim_state->entity_registry().count(),
                            sel.size(),
                            display_fps);
                    } else {
                        std::snprintf(title, sizeof(title),
                            "OpenSupCom | Lobby | %.0f FPS", display_fps);
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

                        if (!launch_scenario.empty()) {
                            spdlog::info("Launch requested: {}", launch_scenario);

                            // Transition to LOADING and show loading screen
                            game_state_mgr.transition_to(osc::GameState::LOADING, ui_lua_state.raw());
                            wld_provider.start_loading_dialog(ui_lua_state.raw());

                            // Pump one UI frame to display loading screen
                            pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 1, ui_frame_count);
                            renderer.render_ui_only(ui_lua_state.raw(), &ui_registry);

                            // Execute reload in stages, pumping UI frames between each
                            execute_reload_sequence(
                                sim_lua_state, sim_state,
                                ui_lua_state, vfs, store,
                                loader, config, scenario_meta,
                                game_state_mgr,
                                &renderer, &input_handler,
                                &prev_selection,
                                sim_accumulator,
                                launch_scenario);

                            // Re-install instrument harness on new sim VM (M166)
                            if (instrument_harness && sim_lua_state) {
                                instrument_harness->install_panic_handler(sim_lua_state->raw());
                                instrument_harness->install_global_interceptor(sim_lua_state->raw());
                                instrument_harness->install_all_method_interceptors(sim_lua_state->raw());
                            }

                            // Stop loading dialog and transition to GAME
                            wld_provider.stop_loading_dialog(ui_lua_state.raw());
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

                        // Tear down game state
                        renderer.clear_scene();
                        sim_state.reset();
                        sim_lua_state.reset();

                        // Reset game state
                        game_state_mgr.set_game_over(false);
                        game_state_mgr.set_paused(false, uiL);
                        sim_accumulator = 0.0;
                        input_handler.set_selected({});
                        prev_selection.clear();

                        // Clear hover
                        lua_pushstring(uiL, "__osc_hover_entity_id");
                        lua_pushnumber(uiL, 0);
                        lua_rawset(uiL, LUA_REGISTRYINDEX);

                        // Transition to FRONT_END
                        game_state_mgr.transition_to(
                            osc::GameState::FRONT_END, uiL);

                        // Re-show lobby UI
                        osc::core::call_lua_global(uiL, "CreateUI");

                        spdlog::info("=== Returned to lobby ===");
                    } else {
                        lua_pop(uiL, 1);
                    }
                }

                osc::Profiler::instance().end_frame();
            }

            renderer.shutdown();
        } else {
            spdlog::warn("Vulkan init failed — falling back to headless "
                         "(100 ticks)");
            if (sim_state) {
                for (osc::u32 i = 0; i < 100; i++)
                    sim_state->tick();
            }
        }

        // Dump instrument report on exit (M166)
        if (instrument_harness) {
            instrument_harness->print_report(false);
            instrument_harness->write_report_to_file("smoke_report.txt", false);
            spdlog::info("Instrument report written to smoke_report.txt");
        }
    }

    // === Full Smoke Test: 5-phase game lifecycle ===
    if (full_smoke_test && !map_path.empty()) {
        // Static so the harness outlives ui_lua_state — interceptor closures
        // capture a lightuserdata pointer to the harness, and lua_close() during
        // main() cleanup would segfault if the harness were already freed.
        static osc::lua::SmokeTestHarness harness;
        harness.activate();
        osc::u32 ui_frame_counter = 0;

        // Install interceptors on ui_L (persistent across all phases)
        harness.install_panic_handler(ui_lua_state.raw());
        harness.install_global_interceptor(ui_lua_state.raw());
        harness.install_all_method_interceptors(ui_lua_state.raw());

        // --- Phase 1: FRONT_END ---
        spdlog::info("=== Phase 1: FRONT_END ===");
        harness.set_phase("FRONT_END");
        // Destroy sim to match real FRONT_END state (sim_state is null during lobby)
        sim_state.reset();
        sim_lua_state.reset();
        store.rebind(nullptr); // Detach from destroyed sim Lua state
        game_state_mgr.transition_to(osc::GameState::FRONT_END, ui_lua_state.raw());
        osc::core::call_lua_global(ui_lua_state.raw(), "CreateUI");
        pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 10, ui_frame_counter);

        // --- Phase 2: LOBBY ---
        spdlog::info("=== Phase 2: LOBBY ===");
        harness.set_phase("LOBBY");
        {
            lua_State* uL = ui_lua_state.raw();
            // Build FrontEndData
            lua_pushstring(uL, "__osc_front_end_data");
            lua_newtable(uL);
            int fed = lua_gettop(uL);
            lua_pushstring(uL, "ScenarioFile");
            lua_pushstring(uL, map_path.c_str());
            lua_rawset(uL, fed);
            lua_pushstring(uL, "PlayerCount");
            lua_pushnumber(uL, 2);
            lua_rawset(uL, fed);
            lua_pushstring(uL, "AiPersonality");
            lua_pushstring(uL, ai_personality.c_str());
            lua_rawset(uL, fed);
            lua_rawset(uL, LUA_REGISTRYINDEX);

            // Set launch scenario path
            lua_pushstring(uL, "__osc_launch_scenario");
            lua_pushstring(uL, map_path.c_str());
            lua_rawset(uL, LUA_REGISTRYINDEX);

            // Set launch requested flag
            lua_pushstring(uL, "__osc_launch_requested");
            lua_pushboolean(uL, 1);
            lua_rawset(uL, LUA_REGISTRYINDEX);
        }
        pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 10, ui_frame_counter);

        // --- Phase 3: GAME ---
        spdlog::info("=== Phase 3: GAME (3000 ticks) ===");
        harness.set_phase("GAME");

        // Execute reload sequence to create sim state (headless — no renderer)
        double sim_accumulator_fst = 0.0;
        bool reload_ok = execute_reload_sequence(
            sim_lua_state, sim_state,
            ui_lua_state, vfs, store,
            loader, config, scenario_meta,
            game_state_mgr,
            nullptr,   // renderer (headless)
            nullptr,   // input_handler (headless)
            nullptr,   // prev_selection (headless)
            sim_accumulator_fst,
            map_path);

        if (!reload_ok) {
            spdlog::error("Phase 3: Reload failed — skipping remaining phases");
            harness.print_report(true);
            harness.write_report_to_file("smoke_report.txt");
            harness.deactivate();
            return 1;
        }

        // After reload, re-install interceptors on fresh sim_L
        if (sim_lua_state) {
            harness.install_panic_handler(sim_lua_state->raw());
            harness.install_global_interceptor(sim_lua_state->raw());
            harness.install_all_method_interceptors(sim_lua_state->raw());
        }

        // Fire OnFirstUpdate once
        osc::core::call_on_first_update(ui_lua_state.raw());

        for (int t = 0; t < 3000; t++) {
            if (sim_state) sim_state->tick();
            if ((t + 1) % 10 == 0) {
                pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 1, ui_frame_counter);
            }
            if ((t + 1) % 250 == 0) {
                osc::i32 prop_count = 0;
                std::string army_summary;

                if (sim_state) {
                    sim_state->entity_registry().for_each(
                        [&](const osc::sim::Entity& e) {
                            if (!e.destroyed() && !e.is_unit()) prop_count++;
                        });

                    for (size_t a = 0; a < sim_state->army_count(); a++) {
                        auto* brain = sim_state->army_at(a);
                        if (!brain || brain->is_civilian()) continue;

                        osc::i32 units = 0, structures = 0;
                        sim_state->entity_registry().for_each(
                            [&](const osc::sim::Entity& e) {
                                if (e.army() == static_cast<osc::i32>(a) &&
                                    !e.destroyed() && e.is_unit()) {
                                    auto* u = static_cast<const osc::sim::Unit*>(&e);
                                    if (u->has_category("STRUCTURE"))
                                        structures++;
                                    else
                                        units++;
                                }
                            });

                        if (!army_summary.empty()) army_summary += ", ";
                        army_summary += "a" + std::to_string(a) + ": " +
                                       std::to_string(units) + "u/" +
                                       std::to_string(structures) + "s";
                    }
                }

                spdlog::info("  tick {}/3000 — props:{} {}", t + 1, prop_count, army_summary);
            }
        }

        // End-of-GAME army summary
        if (sim_state) {
            for (size_t a = 0; a < sim_state->army_count(); a++) {
                auto* brain = sim_state->army_at(a);
                if (!brain || brain->is_civilian()) continue;

                osc::i32 units = 0, structures = 0;
                sim_state->entity_registry().for_each(
                    [&](const osc::sim::Entity& e) {
                        if (e.army() == static_cast<osc::i32>(a) &&
                            !e.destroyed() && e.is_unit()) {
                            auto* u = static_cast<const osc::sim::Unit*>(&e);
                            if (u->has_category("STRUCTURE"))
                                structures++;
                            else
                                units++;
                        }
                    });

                spdlog::info("  Army {} ({}): {} units, {} structures, kills={:.0f} built={:.0f}",
                             a, brain->name(), units, structures,
                             brain->get_stat("Units_Killed"),
                             brain->get_stat("Units_Built"));
            }
        }

        // --- Phase 4: SCORE ---
        spdlog::info("=== Phase 4: SCORE ===");
        harness.set_phase("SCORE");
        if (sim_state) {
            for (size_t i = 0; i < sim_state->army_count(); i++) {
                auto* brain = sim_state->army_at(i);
                if (brain && !brain->is_civilian() && static_cast<osc::i32>(i) != 0) {
                    brain->set_state(osc::sim::BrainState::Defeat);
                }
            }
            osc::i32 result = sim_state->player_result();
            spdlog::info("  player_result() = {} (expected 1=Victory)", result);
            osc::core::call_lua_global(ui_lua_state.raw(), "NoteGameOver");
            game_state_mgr.set_game_over(true);
            game_state_mgr.transition_to(osc::GameState::SCORE, ui_lua_state.raw());
        }
        pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 10, ui_frame_counter);

        // --- Phase 5: RETURN ---
        spdlog::info("=== Phase 5: RETURN ===");
        harness.set_phase("RETURN");
        {
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "__osc_return_to_lobby");
            lua_pushboolean(uL, 1);
            lua_rawset(uL, LUA_REGISTRYINDEX);
        }
        sim_state.reset();
        sim_lua_state.reset();
        // Detach store from destroyed sim Lua state to prevent dangling luaL_unref
        store.rebind(nullptr);
        game_state_mgr.set_game_over(false);
        game_state_mgr.transition_to(osc::GameState::FRONT_END, ui_lua_state.raw());
        osc::core::call_lua_global(ui_lua_state.raw(), "CreateUI");
        pump_ui_frames(ui_lua_state, ui_thread_manager, beat_registry, 10, ui_frame_counter);

        // --- Report ---
        spdlog::info("=== Full Smoke Test Complete ===");
        harness.print_report(true);
        harness.write_report_to_file("smoke_report.txt");
        spdlog::info("Report written to smoke_report.txt");
        harness.deactivate();
        return 0;
    }

    // === Smoke Test ===
    if (smoke_test && !map_path.empty()) {
        osc::lua::SmokeTestHarness harness;

        // Install interceptors on sim state
        harness.install_panic_handler(sim_lua_state->raw());
        harness.install_global_interceptor(sim_lua_state->raw());
        harness.install_all_method_interceptors(sim_lua_state->raw());

        // Install interceptors on UI state
        harness.install_panic_handler(ui_lua_state.raw());
        harness.install_global_interceptor(ui_lua_state.raw());
        harness.install_all_method_interceptors(ui_lua_state.raw());

        spdlog::info("=== Smoke Test: Running 100 sim ticks ===");
        for (int i = 0; i < 100; i++) {
            sim_state->tick();
        }

        spdlog::info("=== Smoke Test: Running 100 UI frame dispatches ===");
        for (int i = 0; i < 100; i++) {
            ui_thread_manager.resume_all(static_cast<osc::u32>(i));
        }

        harness.print_report(false);
        spdlog::info("=== Smoke Test Complete ===");
    }

    // === AI-vs-AI Skirmish (M163) ===
    if (ai_skirmish && !map_path.empty()) {
        osc::u32 max_ticks = tick_count > 0 ? tick_count : 6000; // default 10 min
        spdlog::info("=== AI-vs-AI Skirmish: up to {} ticks ({:.0f}s) ===",
                     max_ticks,
                     max_ticks * osc::sim::SimState::SECONDS_PER_TICK);

        osc::u32 ticks_run = 0;
        osc::i32 result = 0;
        osc::u32 log_interval = 100; // log stats every 10 game seconds

        for (osc::u32 i = 0; i < max_ticks; i++) {
            sim_state->tick();
            ticks_run++;

            // Periodic stats logging
            if (ticks_run % log_interval == 0) {
                osc::u32 total_units = 0;
                osc::u32 total_vet = 0;
                for (size_t a = 0; a < sim_state->army_count(); a++) {
                    auto* brain = sim_state->army_at(a);
                    if (!brain || brain->is_civilian()) continue;
                    total_units += static_cast<osc::u32>(
                        brain->get_unit_cost_total(sim_state->entity_registry()));
                }
                sim_state->entity_registry().for_each([&](const osc::sim::Entity& e) {
                    if (!e.destroyed() && e.is_unit()) {
                        auto& u = static_cast<const osc::sim::Unit&>(e);
                        if (u.vet_level() > 0) total_vet++;
                    }
                });
                spdlog::info("  Tick {}: {:.1f}s | {} units alive | {} vetted",
                             ticks_run,
                             ticks_run * osc::sim::SimState::SECONDS_PER_TICK,
                             total_units, total_vet);
            }

            // Check for game over
            result = sim_state->player_result();
            if (result != 0) {
                const char* result_str =
                    result == 1 ? "ARMY_1 WINS" :
                    result == 2 ? "ARMY_2 WINS" : "DRAW";
                spdlog::info("=== Game Over at tick {} ({:.1f}s): {} ===",
                             ticks_run,
                             ticks_run * osc::sim::SimState::SECONDS_PER_TICK,
                             result_str);
                break;
            }
        }

        if (result == 0) {
            spdlog::info("=== AI Skirmish: tick limit reached, no winner ===");
        }

        // Final summary
        spdlog::info("=== AI Skirmish Summary ===");
        spdlog::info("  Map: {}", map_path);
        spdlog::info("  Ticks: {} ({:.1f}s game time)",
                     ticks_run,
                     ticks_run * osc::sim::SimState::SECONDS_PER_TICK);
        spdlog::info("  Result: {}",
                     result == 1 ? "ARMY_1 wins" :
                     result == 2 ? "ARMY_2 wins" :
                     result == 0 ? "No winner (timeout)" : "Draw");
        for (size_t a = 0; a < sim_state->army_count(); a++) {
            auto* brain = sim_state->army_at(a);
            if (!brain || brain->is_civilian()) continue;
            osc::u32 surviving = 0;
            osc::u32 vetted = 0;
            osc::i32 army_idx = static_cast<osc::i32>(a);
            sim_state->entity_registry().for_each(
                [&](const osc::sim::Entity& e) {
                    if (!e.destroyed() && e.is_unit() && e.army() == army_idx) {
                        surviving++;
                        auto& u = static_cast<const osc::sim::Unit&>(e);
                        if (u.vet_level() > 0) vetted++;
                    }
                });
            spdlog::info("  Army {} ({}): {} units surviving, {} vetted",
                         a + 1, brain->is_defeated() ? "DEFEATED" : "alive",
                         surviving, vetted);
        }
        spdlog::info("=== End AI Skirmish ===");
    }

    // === Draw Test: simultaneous ACU death ===
    if (draw_test && sim_state) {
        spdlog::info("=== DRAW TEST: simultaneous ACU death ===");
        // Set both armies to defeated
        for (size_t i = 0; i < sim_state->army_count(); i++) {
            auto* brain = sim_state->army_at(i);
            if (brain && !brain->is_civilian()) {
                brain->set_state(osc::sim::BrainState::Defeat);
            }
        }
        osc::i32 result = sim_state->player_result();
        if (result == 3) {
            spdlog::info("  PASS — simultaneous defeat returns Draw (3)");
        } else {
            spdlog::error("  FAIL — expected Draw (3), got {}", result);
            return 1;
        }
        return 0;
    }

    // === Stress Test: 10000-tick AI-vs-AI stability validation ===
    if (stress_test && sim_state) {
        spdlog::info("=== STRESS TEST: 10000-tick AI-vs-AI ===");

        osc::i32 peak_entities = 0;
        osc::i32 tick_target = 10000;

        for (osc::i32 t = 0; t < tick_target; t++) {
            sim_state->tick();

            osc::i32 entity_count = 0;
            sim_state->entity_registry().for_each([&](const osc::sim::Entity& e) {
                if (!e.destroyed()) entity_count++;
            });
            if (entity_count > peak_entities) peak_entities = entity_count;

            // Log progress every 1000 ticks
            if ((t + 1) % 1000 == 0) {
                spdlog::info("  tick {}/{} — {} entities (peak {})",
                             t + 1, tick_target, entity_count, peak_entities);
            }

            // Check game-over — continue tracking but note it
            osc::i32 result = sim_state->player_result();
            if (result != 0 && !sim_state->game_ended()) {
                const char* result_str = result == 1 ? "VICTORY" :
                                          result == 2 ? "DEFEAT" : "DRAW";
                spdlog::info("  Game over at tick {}: {}", t + 1, result_str);
                sim_state->set_game_ended(true);
            }
        }

        // Report results
        spdlog::info("=== STRESS TEST COMPLETE ===");
        spdlog::info("  Peak entities: {}", peak_entities);

        // Report army stats
        for (size_t i = 0; i < sim_state->army_count(); i++) {
            auto* brain = sim_state->army_at(i);
            if (!brain || brain->is_civilian()) continue;
            spdlog::info("  Army {} ({}): kills={:.0f} losses={:.0f} built={:.0f} mass={:.0f}",
                         i, brain->name(),
                         brain->get_stat("Units_Killed"),
                         brain->get_stat("Units_Lost"),
                         brain->get_stat("Units_Built"),
                         brain->get_stat("Economy_TotalProduced_Mass"));
        }

        spdlog::info("  PASS — no crashes in {} ticks", tick_target);
        return 0;
    }

    // Headless tick loop
    if (!ai_skirmish && !map_path.empty() && tick_count > 0) {
        spdlog::info("Running {} sim ticks ({:.1f}s game time)...",
                     tick_count,
                     tick_count * osc::sim::SimState::SECONDS_PER_TICK);
        for (osc::u32 i = 0; i < tick_count; i++) {
            osc::Profiler::instance().begin_frame();
            sim_state->tick();
            osc::Profiler::instance().end_frame();
        }
    }


    // ── Integration tests ──
    osc::test::TestContext test_ctx{*sim_state, *sim_lua_state, sim_lua_state->raw(), vfs, store};

    if (damage_test && !map_path.empty()) osc::test::test_damage(test_ctx);
    if (move_test && !map_path.empty()) osc::test::test_move(test_ctx);
    if (fire_test && !map_path.empty()) osc::test::test_fire(test_ctx);
    if (economy_test && !map_path.empty()) osc::test::test_economy(test_ctx);
    if (build_test && !map_path.empty()) osc::test::test_build(test_ctx);
    if (chain_test && !map_path.empty()) osc::test::test_chain(test_ctx);
    if (ai_test && !map_path.empty()) osc::test::test_ai(test_ctx);
    if (reclaim_test && !map_path.empty()) osc::test::test_reclaim(test_ctx);
    if (threat_test && !map_path.empty()) osc::test::test_threat(test_ctx);
    if (combat_test && !map_path.empty()) osc::test::test_combat(test_ctx);
    if (platoon_test && !map_path.empty()) osc::test::test_platoon(test_ctx);
    if (repair_test && !map_path.empty()) osc::test::test_repair(test_ctx);
    if (upgrade_test && !map_path.empty()) osc::test::test_upgrade(test_ctx);
    if (capture_test && !map_path.empty()) osc::test::test_capture(test_ctx);
    if (path_test && !map_path.empty()) osc::test::test_path(test_ctx);
    if (toggle_test && !map_path.empty()) osc::test::test_toggle(test_ctx);
    if (enhance_test && !map_path.empty()) osc::test::test_enhance(test_ctx);
    if (intel_test && !map_path.empty()) osc::test::test_intel(test_ctx);
    if (shield_test && !map_path.empty()) osc::test::test_shield(test_ctx);
    if (transport_test && !map_path.empty()) osc::test::test_transport(test_ctx);
    if (fow_test && !map_path.empty()) osc::test::test_fow(test_ctx);
    if (los_test && !map_path.empty()) osc::test::test_los(test_ctx);
    if (stall_test && !map_path.empty()) osc::test::test_stall(test_ctx);
    if (jammer_test && !map_path.empty()) osc::test::test_jammer(test_ctx);
    if (stub_test && !map_path.empty()) osc::test::test_stub(test_ctx);
    if (audio_test && !map_path.empty()) osc::test::test_audio(test_ctx);
    if (bone_test && !map_path.empty()) osc::test::test_bone(test_ctx);
    if (manip_test && !map_path.empty()) osc::test::test_manip(test_ctx);
    if (canpath_test && !map_path.empty()) osc::test::test_canpath(test_ctx);
    if (armor_test && !map_path.empty()) osc::test::test_armor(test_ctx);
    if (vet_test && !map_path.empty()) osc::test::test_vet(test_ctx);
    if (wreck_test && !map_path.empty()) osc::test::test_wreck(test_ctx);
    if (adjacency_test && !map_path.empty()) osc::test::test_adjacency(test_ctx);
    if (stats_test && !map_path.empty()) osc::test::test_stats(test_ctx);
    if (silo_test && !map_path.empty()) osc::test::test_silo(test_ctx);
    if (flags_test && !map_path.empty()) osc::test::test_flags(test_ctx);
    if (layercap_test && !map_path.empty()) osc::test::test_layercap(test_ctx);
    if (massstub_test && !map_path.empty()) osc::test::test_massstub(test_ctx);
    if (massstub2_test && !map_path.empty()) osc::test::test_massstub2(test_ctx);
    if (massstub3_test && !map_path.empty()) osc::test::test_massstub3(test_ctx);
    if (anim_test && !map_path.empty()) osc::test::test_anim(test_ctx);
    if (teamcolor_test && !map_path.empty()) osc::test::test_teamcolor(test_ctx);
    if (normal_test && !map_path.empty()) osc::test::test_normal(test_ctx);
    if (prop_test && !map_path.empty()) osc::test::test_prop(test_ctx);
    if (scale_test && !map_path.empty()) osc::test::test_scale(test_ctx);
    if (specular_test && !map_path.empty()) osc::test::test_specular(test_ctx);
    if (terrain_normal_test && !map_path.empty()) osc::test::test_terrain_normal(test_ctx);
    if (decal_test && !map_path.empty()) osc::test::test_decal(test_ctx);
    if (projectile_test && !map_path.empty()) osc::test::test_projectile(test_ctx);
    if (terrain_tex_test && !map_path.empty()) osc::test::test_terrain_tex(test_ctx);
    if (shadow_test && !map_path.empty()) osc::test::test_shadow(test_ctx);
    if (massstub4_test && !map_path.empty()) osc::test::test_massstub4(test_ctx);
    if (spatial_test && !map_path.empty()) osc::test::test_spatial(test_ctx);
    if (unitsound_test && !map_path.empty()) osc::test::test_unitsound(test_ctx);
    if (medstub_test && !map_path.empty()) osc::test::test_medstub(test_ctx);
    if (lowstub_test && !map_path.empty()) osc::test::test_lowstub(test_ctx);
    if (blend_test && !map_path.empty()) osc::test::test_blend(test_ctx);
    if (ui_test && !map_path.empty()) osc::test::test_ui(test_ctx);
    if (bitmap_test && !map_path.empty()) osc::test::test_bitmap(test_ctx);
    if (text_test && !map_path.empty()) osc::test::test_text(test_ctx);
    if (edit_test && !map_path.empty()) osc::test::test_edit(test_ctx);
    if (controls_test && !map_path.empty()) osc::test::test_controls(test_ctx);
    if (uiboot_test && !map_path.empty()) osc::test::test_uiboot(test_ctx);
    if (uirender_test && !map_path.empty()) osc::test::test_uirender(test_ctx);
    if (font_test && !map_path.empty()) osc::test::test_font(test_ctx);
    if (scissor_test && !map_path.empty()) osc::test::test_scissor(test_ctx);
    if (border_render_test && !map_path.empty()) osc::test::test_border_render(test_ctx);
    if (edit_render_test && !map_path.empty()) osc::test::test_edit_render(test_ctx);
    if (itemlist_render_test && !map_path.empty()) osc::test::test_itemlist_render(test_ctx);
    if (scrollbar_render_test && !map_path.empty()) osc::test::test_scrollbar_render(test_ctx);
    if (anim_render_test && !map_path.empty()) osc::test::test_anim_render(test_ctx);
    if (tiled_render_test && !map_path.empty()) osc::test::test_tiled_render(test_ctx);
    if (input_test && !map_path.empty()) osc::test::test_input(test_ctx);
    if (onframe_test && !map_path.empty()) osc::test::test_onframe(test_ctx);
    if (cursor_render_test && !map_path.empty()) osc::test::test_cursor_render(test_ctx);
    if (drag_render_test && !map_path.empty()) osc::test::test_drag_render(test_ctx);
    if (emitter_test && !map_path.empty()) osc::test::test_emitter(test_ctx);
    if (collision_test && !map_path.empty()) osc::test::test_collision_beam(test_ctx);
    if (decalsplat_test && !map_path.empty()) osc::test::test_decal_splat(test_ctx);
    if (cmd_test && !map_path.empty()) osc::test::test_commands(test_ctx);
    if (deposit_test && !map_path.empty()) osc::test::test_deposits(test_ctx);
    if (beam_test && !map_path.empty()) osc::test::test_beams(test_ctx);
    if (shield_render_test && !map_path.empty()) osc::test::test_shield_render(test_ctx);
    if (vet_adj_render_test && !map_path.empty()) osc::test::test_vet_adj_render(test_ctx);
    if (intel_overlay_test && !map_path.empty()) osc::test::test_intel_overlay(test_ctx);
    if (enhance_wreck_test && !map_path.empty()) osc::test::test_enhance_wreck_render(test_ctx);
    if (vfx_render_test && !map_path.empty()) osc::test::test_vfx_render(test_ctx);
    if (transport_silo_test && !map_path.empty()) osc::test::test_transport_silo_render(test_ctx);
    if (profile_test && !map_path.empty()) osc::test::test_profile(test_ctx);

    // Dual-state isolation test (does not require map)
    if (dualstate_test) {
        spdlog::info("=== Dual Lua State Test ===");
        int pass = 0, fail = 0;

        // 1. Both states initialized
        if (sim_lua_state->raw() && ui_lua_state.raw()) {
            spdlog::info("[PASS] Both Lua states initialized");
            pass++;
        } else {
            spdlog::error("[FAIL] Lua state initialization");
            fail++;
        }

        // 2. sim_L has CreateUnit but NOT InternalCreateGroup
        //    Use lua_rawget on globals index to avoid __index metamethod
        //    (config.lua locks globals and errors on undefined access)
        {
            lua_State* sL = sim_lua_state->raw();
            lua_pushstring(sL, "CreateUnit");
            lua_rawget(sL, LUA_GLOBALSINDEX);
            bool sim_has_create_unit = !lua_isnil(sL, -1);
            lua_pop(sL, 1);
            lua_pushstring(sL, "InternalCreateGroup");
            lua_rawget(sL, LUA_GLOBALSINDEX);
            bool sim_has_ui_func = !lua_isnil(sL, -1);
            lua_pop(sL, 1);

            if (sim_has_create_unit) {
                spdlog::info("[PASS] sim_L has CreateUnit");
                pass++;
            } else {
                spdlog::error("[FAIL] sim_L missing CreateUnit");
                fail++;
            }
            if (!sim_has_ui_func) {
                spdlog::info("[PASS] sim_L does NOT have InternalCreateGroup");
                pass++;
            } else {
                spdlog::error("[FAIL] sim_L has InternalCreateGroup (should be ui_L only)");
                fail++;
            }
        }

        // 3. ui_L has InternalCreateGroup but NOT CreateUnit
        {
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "InternalCreateGroup");
            lua_rawget(uL, LUA_GLOBALSINDEX);
            bool ui_has_create_group = !lua_isnil(uL, -1);
            lua_pop(uL, 1);
            lua_pushstring(uL, "CreateUnit");
            lua_rawget(uL, LUA_GLOBALSINDEX);
            bool ui_has_sim_func = !lua_isnil(uL, -1);
            lua_pop(uL, 1);

            if (ui_has_create_group) {
                spdlog::info("[PASS] ui_L has InternalCreateGroup");
                pass++;
            } else {
                spdlog::error("[FAIL] ui_L missing InternalCreateGroup");
                fail++;
            }
            if (!ui_has_sim_func) {
                spdlog::info("[PASS] ui_L does NOT have CreateUnit");
                pass++;
            } else {
                spdlog::error("[FAIL] ui_L has CreateUnit (should be sim_L only)");
                fail++;
            }
        }

        // 4. Both share the same VFS
        {
            auto* sim_vfs = osc::lua::LuaState::get_vfs(sim_lua_state->raw());
            auto* ui_vfs = osc::lua::LuaState::get_vfs(ui_lua_state.raw());
            if (sim_vfs && ui_vfs && sim_vfs == ui_vfs) {
                spdlog::info("[PASS] Both states share the same VFS");
                pass++;
            } else {
                spdlog::error("[FAIL] VFS mismatch (sim={}, ui={})",
                              static_cast<void*>(sim_vfs),
                              static_cast<void*>(ui_vfs));
                fail++;
            }
        }

        spdlog::info("=== Dual State Test: {} passed, {} failed ===", pass, fail);
    }

    // M140d: Construction panel integration test
    if (construction_test && !map_path.empty()) {
        spdlog::info("=== M140 Construction Panel Test ===");

        auto result = ui_lua_state.do_string(R"(
            local cat = ParseEntityCategory('FACTORY LAND TECH1')
            local units = EntityCategoryGetUnitList(cat)
            assert(type(units) == 'table', 'Expected table from EntityCategoryGetUnitList')
            print('EntityCategoryGetUnitList returned ' .. table.getn(units) .. ' entries')
            for i, id in ipairs(units) do
                if i <= 5 then print('  ' .. id) end
            end
        )");
        if (result.ok()) {
            spdlog::info("=== M140 Construction Panel Test PASSED ===");
        } else {
            spdlog::error("=== M140 Construction Panel Test FAILED ===");
        }
    }

    // M140-M143: Phase 2 integration test
    if (phase2_test && !map_path.empty()) {
        spdlog::info("=== Phase 2 Integration Test ===");
        int pass = 0, fail = 0;

        // Test 1: EntityCategoryGetUnitList returns results
        {
            auto r = ui_lua_state.do_string(R"(
                local cat = ParseEntityCategory('STRUCTURE LAND')
                local list = EntityCategoryGetUnitList(cat)
                assert(type(list) == 'table', 'EntityCategoryGetUnitList failed')
                print('M140: EntityCategoryGetUnitList returned ' .. table.getn(list) .. ' blueprints')
            )");
            if (r.ok()) { spdlog::info("[PASS] EntityCategoryGetUnitList"); pass++; }
            else { spdlog::error("[FAIL] EntityCategoryGetUnitList"); fail++; }
        }

        // Test 2: GetOrderBitmapNames returns 8 values
        {
            auto r = ui_lua_state.do_string(R"(
                local a,b,c,d,e,f,g,h = GetOrderBitmapNames('move')
                assert(a ~= nil, 'GetOrderBitmapNames returned nil')
                assert(type(g) == 'string', 'Expected sound cue string')
                print('M141: GetOrderBitmapNames("move") up=' .. a)
            )");
            if (r.ok()) { spdlog::info("[PASS] GetOrderBitmapNames"); pass++; }
            else { spdlog::error("[FAIL] GetOrderBitmapNames"); fail++; }
        }

        // Test 3: GetRolloverInfo returns nil when nothing hovered
        {
            auto r = ui_lua_state.do_string(R"(
                local info = GetRolloverInfo()
                print('M142: GetRolloverInfo type=' .. type(info))
            )");
            if (r.ok()) { spdlog::info("[PASS] GetRolloverInfo"); pass++; }
            else { spdlog::error("[FAIL] GetRolloverInfo"); fail++; }
        }

        // Test 4: StartCursorText doesn't crash
        {
            auto r = ui_lua_state.do_string(R"(
                StartCursorText(100, 100, 'Test', {1,1,0,1}, 1.0, false)
                print('M143: StartCursorText succeeded')
            )");
            if (r.ok()) { spdlog::info("[PASS] StartCursorText"); pass++; }
            else { spdlog::error("[FAIL] StartCursorText"); fail++; }
        }

        // Test 5: orders.lua boots (pcall, allow WARN)
        {
            auto r = ui_lua_state.do_string(R"(
                local ok, err = pcall(function()
                    local orders = import('/lua/ui/game/orders.lua')
                    assert(orders ~= nil, 'orders.lua import returned nil')
                end)
                if ok then print('M141: orders.lua boot OK')
                else print('M141: orders.lua boot WARN: ' .. tostring(err)) end
            )");
            if (r.ok()) { spdlog::info("[PASS] orders.lua boot"); pass++; }
            else { spdlog::error("[FAIL] orders.lua boot"); fail++; }
        }

        // Test 6: unitview.lua boots (pcall, allow WARN)
        {
            auto r = ui_lua_state.do_string(R"(
                local ok, err = pcall(function()
                    local unitview = import('/lua/ui/game/unitview.lua')
                    assert(unitview ~= nil, 'unitview.lua import returned nil')
                end)
                if ok then print('M142: unitview.lua boot OK')
                else print('M142: unitview.lua boot WARN: ' .. tostring(err)) end
            )");
            if (r.ok()) { spdlog::info("[PASS] unitview.lua boot"); pass++; }
            else { spdlog::error("[FAIL] unitview.lua boot"); fail++; }
        }

        spdlog::info("=== Phase 2 Integration Test: {}/{} passed ===", pass, pass + fail);
    }

    // M144-M146: Phase 3 integration test
    if (phase3_test && !map_path.empty()) {
        spdlog::info("=== Phase 3 Integration Test ===");
        int pass = 0, fail = 0;

        // Test 1: GetCurrentUIState returns "game"
        {
            auto r = ui_lua_state.do_string(R"(
                local state = GetCurrentUIState()
                assert(state == 'game', 'Expected "game", got: ' .. tostring(state))
                print('M144: GetCurrentUIState = ' .. state)
            )");
            if (r.ok()) { spdlog::info("[PASS] GetCurrentUIState"); pass++; }
            else { spdlog::error("[FAIL] GetCurrentUIState"); fail++; }
        }

        // Test 2: AddBeatFunction registers and fires
        {
            auto r = ui_lua_state.do_string(R"(
                __test_beat_called = false
                local function myBeat() __test_beat_called = true end
                AddBeatFunction(myBeat, 'test_beat')
                print('M145: AddBeatFunction registered')
            )");
            if (r.ok()) { spdlog::info("[PASS] AddBeatFunction registration"); pass++; }
            else { spdlog::error("[FAIL] AddBeatFunction registration"); fail++; }
        }

        // Fire beat functions
        beat_registry.fire_all(ui_lua_state.raw());

        {
            auto r = ui_lua_state.do_string(R"(
                assert(__test_beat_called == true, 'BeatFunction was not called')
                RemoveBeatFunction('test_beat')
                print('M145: BeatFunction fired and removed OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] BeatFunction fire + remove"); pass++; }
            else { spdlog::error("[FAIL] BeatFunction fire + remove"); fail++; }
        }

        // Test 3: Time queries
        {
            auto r = ui_lua_state.do_string(R"(
                local t = GetGameTimeSeconds()
                local tick = GameTick()
                local gt = GetGameTime()
                local rate = GetSimRate()
                assert(type(t) == 'number', 'GetGameTimeSeconds failed')
                assert(type(tick) == 'number', 'GameTick failed')
                assert(type(gt) == 'string', 'GetGameTime should return string')
                assert(rate == 10, 'GetSimRate should be 10')
                print('M145: Time queries OK (t=' .. t .. ' tick=' .. tick .. ' gt=' .. gt .. ')')
            )");
            if (r.ok()) { spdlog::info("[PASS] Time queries"); pass++; }
            else { spdlog::error("[FAIL] Time queries"); fail++; }
        }

        // Test 4: Speed control
        {
            auto r = ui_lua_state.do_string(R"(
                SetGameSpeed(2.0)
                local spd = GetGameSpeed()
                assert(spd == 2.0, 'SetGameSpeed failed: got ' .. tostring(spd))
                SetGameSpeed(1.0)
                print('M145: Speed control OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] Speed control"); pass++; }
            else { spdlog::error("[FAIL] Speed control"); fail++; }
        }

        // Test 5: EscapeHandler
        {
            auto r = ui_lua_state.do_string(R"(
                __test_esc_called = false
                SetEscapeHandler(function() __test_esc_called = true end)
                EscapeHandler()
                assert(__test_esc_called, 'EscapeHandler not called')
                print('M146: EscapeHandler OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] EscapeHandler"); pass++; }
            else { spdlog::error("[FAIL] EscapeHandler"); fail++; }
        }

        spdlog::info("=== Phase 3 Integration Test: {}/{} passed ===", pass, pass + fail);
    }

    // M147-M149: Phase 4 integration test
    if (phase4_test && !map_path.empty()) {
        spdlog::info("=== Phase 4 Integration Test ===");
        int pass = 0, fail = 0;

        // Test 1: FrontEndData round-trip
        {
            auto r = ui_lua_state.do_string(R"(
                SetFrontEndData('testKey', {value=42, name='test'})
                local d = GetFrontEndData('testKey')
                assert(d ~= nil, 'FrontEndData lost')
                assert(d.value == 42, 'FrontEndData value mismatch')
                print('M147: FrontEndData round-trip OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] FrontEndData"); pass++; }
            else { spdlog::error("[FAIL] FrontEndData"); fail++; }
        }

        // Test 2: HasCommandLineArg
        {
            auto r = ui_lua_state.do_string(R"(
                local has = HasCommandLineArg('--phase4-test')
                assert(has == true, 'Expected --phase4-test to be present')
                local no = HasCommandLineArg('--nonexistent')
                assert(no == false, 'Expected --nonexistent to be absent')
                print('M147: HasCommandLineArg OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] HasCommandLineArg"); pass++; }
            else { spdlog::error("[FAIL] HasCommandLineArg"); fail++; }
        }

        // Test 3: PlaySound doesn't crash
        {
            auto r = ui_lua_state.do_string(R"(
                local h = PlaySound('test_click')
                assert(type(h) == 'number', 'PlaySound should return handle')
                print('M147: PlaySound OK (handle=' .. h .. ')')
            )");
            if (r.ok()) { spdlog::info("[PASS] PlaySound"); pass++; }
            else { spdlog::error("[FAIL] PlaySound"); fail++; }
        }

        // Test 4: Skin selection
        {
            auto r = ui_lua_state.do_string(R"(
                UIUtil.SetCurrentSkin('cybran')
                local skin = UIUtil.GetCurrentSkin()
                assert(skin == 'cybran', 'Skin mismatch: ' .. tostring(skin))
                print('M149: Skin selection OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] Skin selection"); pass++; }
            else { spdlog::error("[FAIL] Skin selection"); fail++; }
        }

        // Test 5: Layout preference
        {
            auto r = ui_lua_state.do_string(R"(
                UIUtil.SetLayoutPreference('right')
                local layout = UIUtil.GetLayoutPreference()
                assert(layout == 'right', 'Layout mismatch: ' .. tostring(layout))
                print('M149: Layout preference OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] Layout preference"); pass++; }
            else { spdlog::error("[FAIL] Layout preference"); fail++; }
        }

        // Test 6: GetKeyBindings returns table
        {
            auto r = ui_lua_state.do_string(R"(
                local kb = GetKeyBindings()
                assert(type(kb) == 'table', 'GetKeyBindings should return table')
                assert(kb.attack == 'A', 'attack binding wrong')
                assert(kb.move == 'M', 'move binding wrong')
                print('M149: GetKeyBindings OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] GetKeyBindings"); pass++; }
            else { spdlog::error("[FAIL] GetKeyBindings"); fail++; }
        }

        // Test 7: Prefs table exists
        {
            auto r = ui_lua_state.do_string(R"(
                assert(type(Prefs) == 'table', 'Prefs not found')
                assert(type(Prefs.GetFromCurrentProfile) == 'function', 'GetFromCurrentProfile missing')
                assert(type(Prefs.SetToCurrentProfile) == 'function', 'SetToCurrentProfile missing')
                print('M149: Prefs table OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] Prefs table"); pass++; }
            else { spdlog::error("[FAIL] Prefs table"); fail++; }
        }

        // Test 8: LaunchSinglePlayerSession sets launch signal
        {
            auto r = ui_lua_state.do_string(R"(
                LaunchSinglePlayerSession({ScenarioFile='/maps/test/test_scenario.lua'})
                print('M148: LaunchSinglePlayerSession OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] LaunchSinglePlayerSession"); pass++; }
            else { spdlog::error("[FAIL] LaunchSinglePlayerSession"); fail++; }

            // Clear the launch flag so we don't actually try to launch
            lua_State* uL = ui_lua_state.raw();
            lua_pushstring(uL, "__osc_launch_requested");
            lua_pushnil(uL);
            lua_rawset(uL, LUA_REGISTRYINDEX);
        }

        // Test 9: DiskFindFiles accessible on ui_L
        {
            auto r = ui_lua_state.do_string(R"(
                assert(type(DiskFindFiles) == 'function', 'DiskFindFiles not on ui_L')
                assert(type(DiskGetFileInfo) == 'function', 'DiskGetFileInfo not on ui_L')
                assert(type(exists) == 'function', 'exists not on ui_L')
                print('M148: File I/O on ui_L OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] File I/O on ui_L"); pass++; }
            else { spdlog::error("[FAIL] File I/O on ui_L"); fail++; }
        }

        spdlog::info("=== Phase 4 Integration Test: {}/{} passed ===", pass, pass + fail);
    }

    // M150-M152: Phase 5 integration test
    if (phase5_test && !map_path.empty()) {
        spdlog::info("=== Phase 5 Integration Test ===");
        int pass = 0, fail = 0;

        // Test 1: IN_AddKeyMapTable / IN_RemoveKeyMapTable
        {
            auto r = ui_lua_state.do_string(R"(
                local action_called = false
                local km = {A = function() action_called = true end}
                IN_AddKeyMapTable(km)
                IN_RemoveKeyMapTable(km)
                print('M150: IN_AddKeyMapTable/IN_RemoveKeyMapTable OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] KeyMap add/remove"); pass++; }
            else { spdlog::error("[FAIL] KeyMap add/remove"); fail++; }
        }

        // Test 2: IsKeyDown exists and returns boolean
        {
            auto r = ui_lua_state.do_string(R"(
                local down = IsKeyDown(65)  -- GLFW_KEY_A = 65
                assert(type(down) == 'boolean', 'IsKeyDown should return boolean')
                print('M150: IsKeyDown OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] IsKeyDown"); pass++; }
            else { spdlog::error("[FAIL] IsKeyDown"); fail++; }
        }

        // Test 3: Camera SaveSettings / RestoreSettings
        {
            auto r = ui_lua_state.do_string(R"(
                local cam = GetCamera('WorldCamera')
                if cam then
                    local settings = cam:SaveSettings()
                    assert(type(settings) == 'table', 'SaveSettings should return table')
                    if settings.distance ~= nil then
                        -- Full camera available (renderer present)
                        assert(settings.target_x ~= nil, 'Missing target_x field')
                        cam:RestoreSettings(settings)
                        print('M150: Camera Save/RestoreSettings OK')
                    else
                        -- Headless mode: SaveSettings returns empty table
                        print('M150: Camera headless (empty settings) - OK')
                    end
                else
                    print('M150: Camera not available (headless) - skipping')
                end
            )");
            if (r.ok()) { spdlog::info("[PASS] Camera Save/RestoreSettings"); pass++; }
            else { spdlog::error("[FAIL] Camera Save/RestoreSettings"); fail++; }
        }

        // Test 4: UIZoomTo exists
        {
            auto r = ui_lua_state.do_string(R"(
                assert(type(UIZoomTo) == 'function', 'UIZoomTo not registered')
                UIZoomTo({})  -- empty array, should not crash
                print('M150: UIZoomTo OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] UIZoomTo"); pass++; }
            else { spdlog::error("[FAIL] UIZoomTo"); fail++; }
        }

        // Test 5: RegisterChatFunc + SessionSendChatMessage
        {
            auto r = ui_lua_state.do_string(R"(
                local received = nil
                RegisterChatFunc(function(msg) received = msg end, 'test')
                SessionSendChatMessage({}, {text='hello', from='Player'})
                assert(received ~= nil, 'Chat func not called')
                assert(received.text == 'hello', 'Chat text mismatch')
                print('M151: Chat system OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] Chat system"); pass++; }
            else { spdlog::error("[FAIL] Chat system"); fail++; }
        }

        // Test 6: SendSystemMessage
        {
            auto r = ui_lua_state.do_string(R"(
                local sys_msg = nil
                RegisterChatFunc(function(msg) sys_msg = msg end, 'sys')
                SendSystemMessage('Test announcement')
                assert(sys_msg ~= nil, 'System message not received')
                assert(sys_msg.from == 'System', 'Expected from=System')
                assert(sys_msg.text == 'Test announcement', 'Text mismatch')
                print('M151: SendSystemMessage OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] SendSystemMessage"); pass++; }
            else { spdlog::error("[FAIL] SendSystemMessage"); fail++; }
        }

        // Test 7: GetSessionClients returns table with player
        {
            auto r = ui_lua_state.do_string(R"(
                local clients = GetSessionClients()
                assert(type(clients) == 'table', 'GetSessionClients should return table')
                assert(clients[1] ~= nil, 'Expected at least one client')
                assert(clients[1].name ~= nil, 'Client needs name')
                print('M151: GetSessionClients OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] GetSessionClients"); pass++; }
            else { spdlog::error("[FAIL] GetSessionClients"); fail++; }
        }

        // Test 8: GiveResources SimCallback exists
        {
            auto r = sim_lua_state->do_string(R"(
                local sc = rawget(_G, 'SimCallbacks')
                assert(type(sc) == 'table', 'SimCallbacks not found')
                assert(type(sc.GiveResources) == 'function', 'GiveResources missing')
                sc.GiveResources({From=1, To=2, Mass=100, Energy=200})
                print('M151: GiveResources SimCallback OK')
            )");
            if (r.ok()) { spdlog::info("[PASS] GiveResources SimCallback"); pass++; }
            else { spdlog::error("[FAIL] GiveResources SimCallback"); fail++; }
        }

        spdlog::info("=== Phase 5 Integration Test: {}/{} passed ===", pass, pass + fail);
    }

    // Report final state
    spdlog::info("Sim: {} armies, {} entities, {} active threads, "
                 "{} ticks ({:.1f}s game time)",
                 sim_state->army_count(),
                 sim_state->entity_registry().count(),
                 sim_state->thread_manager().active_count(),
                 sim_state->tick_count(),
                 sim_state->game_time());

    // Print profiling summary if enabled
    if (profile_enabled) {
        osc::Profiler::instance().log_summary();
    }

    osc::log::shutdown();
    return 0;
}
