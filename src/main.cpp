#include "core/log.hpp"
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
#include "lua/moho_bindings.hpp"
#include "ui/ui_control.hpp"
#include "renderer/renderer.hpp"
#include "renderer/input_handler.hpp"

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
                    transport_silo_test;
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

    // Phase 1: Init + VFS
    osc::lua::LuaState state;
    osc::vfs::VirtualFileSystem vfs;
    osc::lua::InitLoader loader;

    auto init_result = loader.execute_init(state, config, vfs);
    if (!init_result) {
        spdlog::error("Init failed: {}", init_result.error().message);
        return 1;
    }

    // Phase 2: Blueprint loading
    osc::blueprints::BlueprintStore store(state.raw());

    auto bp_result = loader.load_blueprints(state, vfs, store);
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
    osc::sim::SimState sim_state(state.raw(), &store);

    // Audio system
    auto sound_mgr = std::make_unique<osc::audio::SoundManager>(
        config.fa_path / "sounds");
    {
        lua_State* L = state.raw();
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
            state, vfs, map_path, sim_state);
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
    auto sim_result = sim_loader.boot_sim(state, vfs, sim_state);
    if (!sim_result) {
        spdlog::error("Sim boot failed: {}", sim_result.error().message);
        return 1;
    }

    // UI control registry (M71)
    osc::ui::UIControlRegistry ui_registry;
    osc::lua::register_ui_bindings(state, ui_registry);

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
            state, vfs, sim_state, scenario_meta);
        if (!session_result) {
            spdlog::error("Session start failed: {}",
                          session_result.error().message);
            return 1;
        }
    }

    // Phase 5: Windowed mode (renderer) or headless tick loop
    if (!map_path.empty() && !headless) {
        osc::renderer::Renderer renderer;
        if (renderer.init(1600, 900, "OpenSupCom")) {
            renderer.build_scene(sim_state, &vfs, state.raw());

            // Player input handler (ARMY_1 = index 0)
            osc::renderer::InputHandler input_handler;
            input_handler.set_player_army(0);
            renderer.set_player_army(0);

            double sim_accumulator = 0.0;
            auto prev_time = std::chrono::high_resolution_clock::now();
            bool sim_paused = false;
            double sim_speed = 1.0;  // 1.0 = normal, 0.5 = half, 2.0 = double
            bool p_was_pressed = false;
            bool plus_was_pressed = false;
            bool minus_was_pressed = false;
            double title_update_timer = 0.0;
            double fps_accum = 0.0;
            int fps_frames = 0;
            double display_fps = 0.0;

            while (!renderer.should_close()) {
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
                    sim_paused = !sim_paused;
                    spdlog::info("Sim {}", sim_paused ? "PAUSED" : "RESUMED");
                }
                p_was_pressed = p_pressed;

                // Speed control (+/- keys, edge-triggered)
                bool plus_pressed = renderer.is_key_pressed(GLFW_KEY_EQUAL) ||
                                    renderer.is_key_pressed(GLFW_KEY_KP_ADD);
                if (plus_pressed && !plus_was_pressed) {
                    if (sim_speed < 10.0) {
                        sim_speed = std::min(sim_speed * 2.0, 10.0);
                        spdlog::info("Sim speed: {:.1f}x", sim_speed);
                    }
                }
                plus_was_pressed = plus_pressed;

                bool minus_pressed = renderer.is_key_pressed(GLFW_KEY_MINUS) ||
                                     renderer.is_key_pressed(GLFW_KEY_KP_SUBTRACT);
                if (minus_pressed && !minus_was_pressed) {
                    if (sim_speed > 0.125) {
                        sim_speed = std::max(sim_speed * 0.5, 0.125);
                        spdlog::info("Sim speed: {:.1f}x", sim_speed);
                    }
                }
                minus_was_pressed = minus_pressed;

                // Auto-pause when game ends
                osc::i32 game_result = sim_state.player_result();
                if (game_result != 0 && !sim_paused) {
                    sim_paused = true;
                    const char* result_str =
                        game_result == 1 ? "VICTORY" :
                        game_result == 2 ? "DEFEAT" : "DRAW";
                    spdlog::info("Game over: {}", result_str);
                }

                // Fixed-timestep sim ticking (scaled by sim_speed)
                if (!sim_paused) {
                    sim_accumulator += dt * sim_speed;
                    while (sim_accumulator >=
                           osc::sim::SimState::SECONDS_PER_TICK) {
                        sim_state.tick();
                        sim_accumulator -= osc::sim::SimState::SECONDS_PER_TICK;
                    }
                }

                renderer.poll_events(dt);

                // Player input: selection + commands
                input_handler.update(renderer, sim_state, dt);

                const auto& sel = input_handler.selected();
                renderer.render(sim_state, state.raw(), &ui_registry,
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
                        sim_paused ? "PAUSED " : "";
                    std::snprintf(title, sizeof(title),
                        "OpenSupCom | %s%.1fx | T:%u (%.1fs) | %zu entities | %zu sel | %.0f FPS",
                        status_str,
                        sim_speed,
                        sim_state.tick_count(),
                        sim_state.game_time(),
                        sim_state.entity_registry().count(),
                        sel.size(),
                        display_fps);
                    renderer.set_window_title(title);
                }
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
            sim_state.tick();
        }
    }


    // ── Integration tests ──
    osc::test::TestContext test_ctx{sim_state, state, state.raw(), vfs, store};

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

    // Report final state
    spdlog::info("Sim: {} armies, {} entities, {} active threads, "
                 "{} ticks ({:.1f}s game time)",
                 sim_state.army_count(),
                 sim_state.entity_registry().count(),
                 sim_state.thread_manager().active_count(),
                 sim_state.tick_count(),
                 sim_state.game_time());

    osc::log::shutdown();
    return 0;
}
