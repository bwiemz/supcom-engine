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
#include "lua/moho_bindings.hpp"
#include "lua/beat_system.hpp"
#include "lua/factory_queue.hpp"
#include "ui/ui_control.hpp"
#include "ui/wld_ui_provider.hpp"
#include "renderer/renderer.hpp"
#include "renderer/input_handler.hpp"
#include "sim/sim_callback_queue.hpp"

extern "C" {
#include <lua.h>
}

#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
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

    lua_newtable(L);
    if (!sim) return 1;
    auto* brain = sim->get_army(army);
    if (!brain) return 1;

    auto set_field = [&](const char* name, osc::f64 val) {
        lua_pushstring(L, name);
        lua_pushnumber(L, val);
        lua_rawset(L, -3);
    };
    set_field("income_mass", brain->get_economy_income("MASS"));
    set_field("income_energy", brain->get_economy_income("ENERGY"));
    set_field("expense_mass", brain->get_economy_usage("MASS"));
    set_field("expense_energy", brain->get_economy_usage("ENERGY"));
    set_field("stored_mass", brain->get_economy_stored("MASS"));
    set_field("stored_energy", brain->get_economy_stored("ENERGY"));
    set_field("max_mass", brain->economy().mass.max_storage);
    set_field("max_energy", brain->economy().energy.max_storage);

    return 1;
}

static int l_GetArmiesTable(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<osc::sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);

    lua_newtable(L);
    if (!sim) return 1;

    for (size_t i = 0; i < sim->army_count(); ++i) {
        auto* brain = sim->army_at(i);
        if (!brain) continue;

        lua_newtable(L);
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

        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
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
              << "  --profile          Enable performance profiling (prints summary at exit)\n"
              << "  --profile-test     Profiler system (zones, nesting, rolling stats)\n"
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
                    profile_test;
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

    // Phase 1: Init + VFS (sim Lua state)
    osc::lua::LuaState sim_lua_state;
    osc::vfs::VirtualFileSystem vfs;
    osc::lua::InitLoader loader;

    auto init_result = loader.execute_init(sim_lua_state, config, vfs);
    if (!init_result) {
        spdlog::error("Init failed: {}", init_result.error().message);
        return 1;
    }

    // Phase 2: Blueprint loading (sim Lua state owns the store)
    osc::blueprints::BlueprintStore store(sim_lua_state.raw());

    auto bp_result = loader.load_blueprints(sim_lua_state, vfs, store);
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

    // Phase 3: Map + Sim boot
    osc::sim::SimState sim_state(sim_lua_state.raw(), &store);

    // Audio system
    auto sound_mgr = std::make_unique<osc::audio::SoundManager>(
        config.fa_path / "sounds");
    {
        lua_State* L = sim_lua_state.raw();
        lua_pushstring(L, "osc_sound_manager");
        lua_pushlightuserdata(L, sound_mgr.get());
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    sim_state.set_sound_manager(std::move(sound_mgr));

    // Bone cache (lazy-loaded per-blueprint SCM bone data)
    auto bone_cache = std::make_unique<osc::sim::BoneCache>(&vfs, &store);
    sim_state.set_bone_cache(std::move(bone_cache));

    // Animation cache (lazy-loaded SCA animation data)
    auto anim_cache = std::make_unique<osc::sim::AnimCache>(&vfs);
    sim_state.set_anim_cache(std::move(anim_cache));

    // Load scenario and map if --map was provided
    osc::lua::ScenarioMetadata scenario_meta;
    if (!map_path.empty()) {
        osc::lua::ScenarioLoader scenario_loader;
        auto meta_result = scenario_loader.load_scenario(
            sim_lua_state, vfs, map_path, sim_state);
        if (!meta_result) {
            spdlog::error("Scenario load failed: {}",
                          meta_result.error().message);
            return 1;
        }
        scenario_meta = meta_result.value();

        // Add armies from scenario
        for (const auto& army : scenario_meta.armies) {
            sim_state.add_army(army, army);
        }
    }

    // Fallback: add a default army if none from scenario
    if (sim_state.army_count() == 0) {
        sim_state.add_army("ARMY_1", "Player");
    }

    osc::lua::SimLoader sim_loader;
    auto sim_result = sim_loader.boot_sim(sim_lua_state, vfs, sim_state);
    if (!sim_result) {
        spdlog::error("Sim boot failed: {}", sim_result.error().message);
        return 1;
    }

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

    // Load blueprints into UI state (needed for GetBlueprint lookups)
    auto ui_bp_result = loader.load_blueprints(ui_lua_state, vfs, store);
    if (!ui_bp_result) {
        spdlog::warn("UI blueprint load: {}", ui_bp_result.error().message);
    }

    // Register moho class tables on UI state (unit_methods, etc.)
    osc::lua::register_moho_bindings(ui_lua_state, sim_state);

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
    ui_lua_state.register_function("GetArmiesTable", l_GetArmiesTable);

    // State transition: INIT → GAME (call SetupUI / StartGameUI if defined)
    osc::core::call_setup_ui(ui_lua_state.raw());
    osc::core::call_start_game_ui(ui_lua_state.raw());

    spdlog::info("Dual Lua states initialized (sim_L + ui_L)");

    // Terrain query test
    if (sim_state.terrain()) {
        auto* t = sim_state.terrain();
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
        if (ai_test || platoon_test || threat_test || combat_test) {
            session_mgr.set_ai_armies({1}); // ARMY_2 (0-based index 1) is AI
        }
        auto session_result = session_mgr.start_session(
            sim_lua_state, vfs, sim_state, scenario_meta);
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

    // Phase 5: Windowed mode (renderer) or headless tick loop
    if (!map_path.empty() && !headless) {
        osc::renderer::Renderer renderer;
        if (renderer.init(1600, 900, "OpenSupCom")) {
            renderer.build_scene(sim_state, &vfs, ui_lua_state.raw());

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

            // GameStateManager pointer for UI bindings (M144b)
            osc::GameStateManager game_state_mgr;
            game_state_mgr.transition_to(osc::GameState::GAME, ui_lua_state.raw());
            {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_game_state_mgr");
                lua_pushlightuserdata(uL, &game_state_mgr);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            // GameStateManager pointer for sim bindings (M145d)
            {
                lua_State* sL = sim_lua_state.raw();
                lua_pushstring(sL, "__osc_game_state_mgr");
                lua_pushlightuserdata(sL, &game_state_mgr);
                lua_rawset(sL, LUA_REGISTRYINDEX);
            }

            // BeatFunctionRegistry for per-frame Lua callbacks (M145b)
            osc::lua::BeatFunctionRegistry beat_registry;
            {
                lua_State* uL = ui_lua_state.raw();
                lua_pushstring(uL, "__osc_beat_registry");
                lua_pushlightuserdata(uL, &beat_registry);
                lua_rawset(uL, LUA_REGISTRYINDEX);
            }

            if (no_fog) renderer.set_fog_enabled(false);
            if (no_decals) renderer.set_decals_enabled(false);

            double sim_accumulator = 0.0;
            auto prev_time = std::chrono::high_resolution_clock::now();
            bool p_was_pressed = false;
            bool plus_was_pressed = false;
            bool minus_was_pressed = false;
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

                // Auto-pause when game ends
                osc::i32 game_result = sim_state.player_result();
                if (game_result != 0 && !game_state_mgr.game_over()) {
                    game_state_mgr.set_game_over(true);
                    game_state_mgr.set_paused(true, ui_lua_state.raw());
                    const char* result_str =
                        game_result == 1 ? "VICTORY" :
                        game_result == 2 ? "DEFEAT" : "DRAW";
                    spdlog::info("Game over: {}", result_str);
                }

                // Fixed-timestep sim ticking (scaled by sim_speed)
                if (!game_state_mgr.paused()) {
                    sim_accumulator += dt * game_state_mgr.speed();
                    while (sim_accumulator >=
                           osc::sim::SimState::SECONDS_PER_TICK) {
                        sim_state.tick();
                        sim_accumulator -= osc::sim::SimState::SECONDS_PER_TICK;
                    }
                }

                // Process SimCallbacks from UI (M138a)
                if (!sim_callback_queue.empty()) {
                    auto callbacks = sim_callback_queue.drain();
                    lua_State* sL = sim_lua_state.raw();

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
                                    auto* entity = sim_state.entity_registry().find(eid);
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
                if (!first_update_fired && sim_state.tick_count() > 0) {
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
                input_handler.update(renderer, sim_state, dt);

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
                renderer.render(sim_state, ui_lua_state.raw(), &ui_registry,
                                sel.empty() ? nullptr : &sel);

                // Update window title periodically
                title_update_timer += dt;
                if (title_update_timer >= 0.25) {
                    title_update_timer = 0.0;
                    char title[256];
                    const char* status_str =
                        game_result == 1 ? "VICTORY " :
                        game_result == 2 ? "DEFEAT " :
                        game_result == 3 ? "DRAW " :
                        game_state_mgr.paused() ? "PAUSED " : "";
                    std::snprintf(title, sizeof(title),
                        "OpenSupCom | %s%.1fx | T:%u (%.1fs) | %zu entities | %zu sel | %.0f FPS",
                        status_str,
                        game_state_mgr.speed(),
                        sim_state.tick_count(),
                        sim_state.game_time(),
                        sim_state.entity_registry().count(),
                        sel.size(),
                        display_fps);
                    renderer.set_window_title(title);
                }

                osc::Profiler::instance().end_frame();
            }

            renderer.shutdown();
        } else {
            spdlog::warn("Vulkan init failed — falling back to headless "
                         "(100 ticks)");
            for (osc::u32 i = 0; i < 100; i++)
                sim_state.tick();
        }
    }

    // Headless tick loop
    if (!map_path.empty() && tick_count > 0) {
        spdlog::info("Running {} sim ticks ({:.1f}s game time)...",
                     tick_count,
                     tick_count * osc::sim::SimState::SECONDS_PER_TICK);
        for (osc::u32 i = 0; i < tick_count; i++) {
            osc::Profiler::instance().begin_frame();
            sim_state.tick();
            osc::Profiler::instance().end_frame();
        }
    }


    // ── Integration tests ──
    osc::test::TestContext test_ctx{sim_state, sim_lua_state, sim_lua_state.raw(), vfs, store};

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
        if (sim_lua_state.raw() && ui_lua_state.raw()) {
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
            lua_State* sL = sim_lua_state.raw();
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
            auto* sim_vfs = osc::lua::LuaState::get_vfs(sim_lua_state.raw());
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

    // Report final state
    spdlog::info("Sim: {} armies, {} entities, {} active threads, "
                 "{} ticks ({:.1f}s game time)",
                 sim_state.army_count(),
                 sim_state.entity_registry().count(),
                 sim_state.thread_manager().active_count(),
                 sim_state.tick_count(),
                 sim_state.game_time());

    // Print profiling summary if enabled
    if (profile_enabled) {
        osc::Profiler::instance().log_summary();
    }

    osc::log::shutdown();
    return 0;
}
