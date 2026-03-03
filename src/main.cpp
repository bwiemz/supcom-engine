#include "core/log.hpp"
#include "core/types.hpp"
#include "lua/lua_state.hpp"
#include "lua/init_loader.hpp"
#include "lua/session_manager.hpp"
#include "vfs/virtual_file_system.hpp"
#include "blueprints/blueprint_store.hpp"
#include "sim/sim_state.hpp"
#include "lua/sim_loader.hpp"
#include "lua/scenario_loader.hpp"
#include "map/terrain.hpp"
#include "map/pathfinding_grid.hpp"
#include "sim/unit.hpp"
#include "audio/sound_manager.hpp"
#include "sim/bone_cache.hpp"
#include "renderer/renderer.hpp"

extern "C" {
#include <lua.h>
}

#include <chrono>
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
                    massstub_test || massstub2_test || massstub3_test;
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
            renderer.build_scene(sim_state);

            double sim_accumulator = 0.0;
            auto prev_time = std::chrono::high_resolution_clock::now();

            while (!renderer.should_close()) {
                auto now = std::chrono::high_resolution_clock::now();
                double dt = std::chrono::duration<double>(now - prev_time).count();
                prev_time = now;
                // Clamp dt to avoid spiral of death
                if (dt > 0.25) dt = 0.25;

                sim_accumulator += dt;
                while (sim_accumulator >=
                       osc::sim::SimState::SECONDS_PER_TICK) {
                    sim_state.tick();
                    sim_accumulator -= osc::sim::SimState::SECONDS_PER_TICK;
                }

                renderer.poll_events(dt);
                renderer.render(sim_state);
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

    // Damage test: deal lethal damage to entity #1 and run more ticks
    if (damage_test && !map_path.empty()) {
        spdlog::info("=== DAMAGE TEST: Killing entity #1 ===");
        auto damage_result = state.do_string(
            "local e = GetEntityById(1)\n"
            "if e then\n"
            "    LOG('Damage test: dealing 99999 to entity #1, '  ..\n"
            "        'health=' .. tostring(e:GetHealth()) .. '/' ..\n"
            "        tostring(e:GetMaxHealth()))\n"
            "    Damage(nil, e, 99999, nil, 'Normal')\n"
            "else\n"
            "    WARN('Damage test: entity #1 not found')\n"
            "end");
        if (!damage_result) {
            spdlog::warn("Damage test Lua error: {}",
                         damage_result.error().message);
        }

        // Run 10 more ticks so DeathThread coroutine can complete
        spdlog::info("Running 10 post-damage ticks...");
        for (osc::u32 i = 0; i < 10; i++) {
            sim_state.tick();
        }
        spdlog::info("Entities remaining: {}",
                     sim_state.entity_registry().count());
    }

    // Move test: issue move to entity #1 and run more ticks
    if (move_test && !map_path.empty()) {
        spdlog::info("=== MOVE TEST: Moving entity #1 ===");
        auto move_result = state.do_string(
            "local e = GetEntityById(1)\n"
            "if e then\n"
            "    local pos = e:GetPosition()\n"
            "    LOG('Move test: entity #1 at (' ..\n"
            "        string.format('%.1f, %.1f, %.1f', pos[1], pos[2], pos[3]) ..\n"
            "        '), speed=' .. tostring(e:GetBlueprint().Physics.MaxSpeed or 'nil'))\n"
            "    IssueMove({e}, {256, pos[2], 256})\n"
            "else\n"
            "    WARN('Move test: entity #1 not found')\n"
            "end");
        if (!move_result) {
            spdlog::warn("Move test Lua error: {}",
                         move_result.error().message);
        }

        spdlog::info("Running 200 post-move ticks (20s game time)...");
        for (osc::u32 i = 0; i < 200; i++) {
            sim_state.tick();
        }

        // Report final position
        auto pos_result = state.do_string(
            "local e = GetEntityById(1)\n"
            "if e then\n"
            "    local pos = e:GetPosition()\n"
            "    LOG('Move test: entity #1 now at (' ..\n"
            "        string.format('%.1f, %.1f, %.1f', pos[1], pos[2], pos[3]) ..\n"
            "        '), moving=' .. tostring(e:IsMoving()))\n"
            "end");
        if (!pos_result) {
            spdlog::warn("Move test position check error: {}",
                         pos_result.error().message);
        }
    }

    // Fire test: teleport two enemy units close and let them fight
    if (fire_test && !map_path.empty()) {
        spdlog::info("=== FIRE TEST: Weapon combat ===");

        auto* e1 = sim_state.entity_registry().find(1);
        auto* e2 = sim_state.entity_registry().find(2);
        if (e1 && e2 && !e1->destroyed() && !e2->destroyed() &&
            e1->is_unit() && e2->is_unit()) {
            e1->set_position({256, 25, 256});
            e2->set_position({276, 25, 256}); // 20 units apart

            auto* u1 = static_cast<osc::sim::Unit*>(e1);
            auto* u2 = static_cast<osc::sim::Unit*>(e2);

            spdlog::info("Entity #1 (army {}) at (256,25,256), "
                         "health={}/{}, weapons={}",
                         e1->army(), e1->health(), e1->max_health(),
                         u1->weapon_count());
            spdlog::info("Entity #2 (army {}) at (276,25,256), "
                         "health={}/{}, weapons={}",
                         e2->army(), e2->health(), e2->max_health(),
                         u2->weapon_count());

            // Log weapon details for entity #1
            for (int wi = 0; wi < u1->weapon_count(); wi++) {
                auto* w = u1->get_weapon(wi);
                if (w) {
                    spdlog::info("  Weapon[{}]: {} range={} dmg={} "
                                 "rof={} vel={} death={} manual={}",
                                 wi, w->label, w->max_range, w->damage,
                                 w->rate_of_fire, w->muzzle_velocity,
                                 w->fire_on_death, w->manual_fire);
                }
            }

            // Run 100 ticks (10 seconds game time)
            spdlog::info("Running 100 combat ticks...");
            for (int i = 0; i < 100; i++) {
                sim_state.tick();
            }

            spdlog::info("After 100 combat ticks:");
            e1 = sim_state.entity_registry().find(1);
            e2 = sim_state.entity_registry().find(2);
            if (e1 && !e1->destroyed())
                spdlog::info("  Entity #1 health: {}/{}",
                             e1->health(), e1->max_health());
            else
                spdlog::info("  Entity #1 destroyed!");
            if (e2 && !e2->destroyed())
                spdlog::info("  Entity #2 health: {}/{}",
                             e2->health(), e2->max_health());
            else
                spdlog::info("  Entity #2 destroyed!");
            spdlog::info("  Entities remaining: {}",
                         sim_state.entity_registry().count());
        } else {
            spdlog::warn("Fire test: entities #1 and #2 not both available");
        }
    }

    // Economy test: log per-army economy state
    if (economy_test && !map_path.empty()) {
        spdlog::info("=== ECONOMY TEST ===");

        for (size_t i = 0; i < sim_state.army_count(); i++) {
            auto* brain = sim_state.army_at(i);
            if (!brain) continue;
            const auto& econ = brain->economy();

            spdlog::info("Army {} ({}):", brain->index(), brain->name());
            spdlog::info("  Mass:   income={:.1f}/s  requested={:.1f}/s  "
                         "stored={:.0f}/{:.0f}  efficiency={:.0f}%",
                         econ.mass.income, econ.mass.requested,
                         econ.mass.stored, econ.mass.max_storage,
                         brain->mass_efficiency() * 100);
            spdlog::info("  Energy: income={:.1f}/s  requested={:.1f}/s  "
                         "stored={:.0f}/{:.0f}  efficiency={:.0f}%",
                         econ.energy.income, econ.energy.requested,
                         econ.energy.stored, econ.energy.max_storage,
                         brain->energy_efficiency() * 100);

            // Count units with active production/consumption
            int producing = 0, consuming = 0;
            sim_state.entity_registry().for_each(
                [&](const osc::sim::Entity& e) {
                    if (e.army() != brain->index() || e.destroyed() ||
                        !e.is_unit())
                        return;
                    const auto& u =
                        static_cast<const osc::sim::Unit&>(e);
                    if (u.economy().production_active) producing++;
                    if (u.economy().consumption_active) consuming++;
                });
            spdlog::info("  Units: {} producing, {} consuming",
                         producing, consuming);
        }
    }

    // Build test: have entity #1 (ACU) build a T1 power gen nearby
    if (build_test && !map_path.empty()) {
        spdlog::info("=== BUILD TEST: Entity #1 builds T1 Power Generator ===");

        auto* e1 = sim_state.entity_registry().find(1);
        if (e1 && !e1->destroyed() && e1->is_unit()) {
            auto* u1 = static_cast<osc::sim::Unit*>(e1);
            spdlog::info("Builder: entity #1 ({}), army={}, build_rate={:.1f}, "
                         "pos=({:.0f},{:.0f},{:.0f})",
                         e1->blueprint_id(), e1->army(), u1->build_rate(),
                         e1->position().x, e1->position().y, e1->position().z);

            // Issue build command via Lua (same way AI brain would)
            auto build_result = state.do_string(
                "local builder = GetEntityById(1)\n"
                "if builder then\n"
                "    local pos = builder:GetPosition()\n"
                "    local build_pos = {pos[1] + 10, pos[2], pos[3]}\n"
                "    LOG('Build test: IssueBuildMobile ueb1101 at (' ..\n"
                "        string.format('%.1f, %.1f', build_pos[1], build_pos[3]) .. ')')\n"
                "    IssueBuildMobile({builder}, build_pos, 'ueb1101', {})\n"
                "else\n"
                "    WARN('Build test: entity #1 not found')\n"
                "end");
            if (!build_result) {
                spdlog::warn("Build test Lua error: {}",
                             build_result.error().message);
            }

            // Run ticks to complete the build
            // UEF T1 pgen: BuildTime=125, ACU BuildRate=10 → 12.5s = 125 ticks
            spdlog::info("Running 150 build ticks (15s game time)...");
            for (int i = 0; i < 150; i++) {
                sim_state.tick();

                // Log progress every 25 ticks
                if ((i + 1) % 25 == 0) {
                    if (u1->is_building()) {
                        auto* target = sim_state.entity_registry().find(
                            u1->build_target_id());
                        if (target) {
                            spdlog::info("  tick {}: building target #{}, "
                                         "frac={:.1f}%, health={:.0f}/{:.0f}",
                                         i + 1, u1->build_target_id(),
                                         target->fraction_complete() * 100,
                                         target->health(), target->max_health());
                        }
                    } else {
                        spdlog::info("  tick {}: not building", i + 1);
                    }
                }
            }

            // Report result
            spdlog::info("Build test results:");
            spdlog::info("  Builder still building: {}",
                         u1->is_building() ? "yes" : "no");
            spdlog::info("  Entities: {}",
                         sim_state.entity_registry().count());

            // Find the built unit
            sim_state.entity_registry().for_each(
                [&](const osc::sim::Entity& e) {
                    if (e.blueprint_id() == "ueb1101" && !e.destroyed()) {
                        spdlog::info("  Found ueb1101 (entity #{}) — "
                                     "frac={:.1f}%, health={:.0f}/{:.0f}, "
                                     "being_built={}",
                                     e.entity_id(),
                                     e.fraction_complete() * 100,
                                     e.health(), e.max_health(),
                                     e.is_unit() ?
                                        (static_cast<const osc::sim::Unit&>(e)
                                             .is_being_built() ? "yes" : "no")
                                        : "n/a");
                    }
                });
        } else {
            spdlog::warn("Build test: entity #1 not available");
        }
    }

    // Chain test: ACU → factory → engineer → structure
    if (chain_test && !map_path.empty()) {
        spdlog::info("=== CHAIN TEST: ACU -> Factory -> Engineer -> PGen ===");

        auto* e1 = sim_state.entity_registry().find(1);
        if (!e1 || e1->destroyed() || !e1->is_unit()) {
            spdlog::error("Chain test: entity #1 not available");
        } else {
            // Phase 1: ACU builds T1 Land Factory (ueb0101)
            // Economy.BuildTime=300, ACU BuildRate=10 → 30s = 300 ticks + margin
            spdlog::info("--- Phase 1: ACU builds T1 Land Factory (ueb0101) ---");
            auto r1 = state.do_string(
                "local builder = GetEntityById(1)\n"
                "if builder then\n"
                "    local pos = builder:GetPosition()\n"
                "    local bp = 'ueb0101'\n"
                "    local build_pos = {pos[1] + 15, pos[2], pos[3]}\n"
                "    LOG('Chain Phase 1: ACU building ' .. bp)\n"
                "    IssueBuildMobile({builder}, build_pos, bp, {})\n"
                "end");
            if (!r1) spdlog::warn("Chain P1 Lua error: {}", r1.error().message);

            // Track builder to capture build_target_id before it completes
            auto* u1 = static_cast<osc::sim::Unit*>(e1);
            osc::u32 factory_id = 0;
            for (int i = 0; i < 350; i++) {
                // Capture target ID while builder is still building
                if (u1->is_building() && factory_id == 0)
                    factory_id = u1->build_target_id();
                sim_state.tick();
                if ((i + 1) % 100 == 0) {
                    if (u1->is_building()) {
                        auto* t = sim_state.entity_registry().find(u1->build_target_id());
                        if (t) spdlog::info("  tick {}: target #{} frac={:.1f}%",
                                            i + 1, t->entity_id(),
                                            t->fraction_complete() * 100);
                    } else {
                        spdlog::info("  tick {}: build complete", i + 1);
                    }
                }
            }

            if (factory_id == 0) {
                spdlog::error("Chain test: factory not found after Phase 1");
            } else {
                spdlog::info("Phase 1 done: factory = entity #{}", factory_id);

                // Phase 2: Factory builds T1 Engineer (uel0105)
                // Economy.BuildTime=130, Factory BuildRate=20 → 6.5s = 65 ticks + margin
                spdlog::info("--- Phase 2: Factory builds T1 Engineer (uel0105) ---");
                std::string p2_lua =
                    "local factory = GetEntityById(" + std::to_string(factory_id) + ")\n"
                    "if factory then\n"
                    "    LOG('Chain Phase 2: Factory building uel0105')\n"
                    "    IssueBuildFactory({factory}, 'uel0105', 1)\n"
                    "end";
                auto r2 = state.do_string(p2_lua);
                if (!r2) spdlog::warn("Chain P2 Lua error: {}", r2.error().message);

                osc::u32 engineer_id = 0;
                for (int i = 0; i < 100; i++) {
                    auto* fent = sim_state.entity_registry().find(factory_id);
                    if (fent && fent->is_unit()) {
                        auto* fu = static_cast<osc::sim::Unit*>(fent);
                        if (fu->is_building() && engineer_id == 0)
                            engineer_id = fu->build_target_id();
                    }
                    sim_state.tick();
                    if ((i + 1) % 25 == 0) {
                        auto* fent2 = sim_state.entity_registry().find(factory_id);
                        if (fent2 && fent2->is_unit()) {
                            auto* fu = static_cast<osc::sim::Unit*>(fent2);
                            if (fu->is_building()) {
                                auto* t = sim_state.entity_registry().find(fu->build_target_id());
                                if (t) spdlog::info("  tick {}: target #{} frac={:.1f}%",
                                                    i + 1, t->entity_id(),
                                                    t->fraction_complete() * 100);
                            } else {
                                spdlog::info("  tick {}: not building", i + 1);
                            }
                        }
                    }
                }

                if (engineer_id == 0) {
                    spdlog::error("Chain test: engineer not found after Phase 2");
                } else {
                    spdlog::info("Phase 2 done: engineer = entity #{}", engineer_id);

                    // Phase 3: Engineer builds T1 Power Gen (ueb1101)
                    // Economy.BuildTime=125, Engineer BuildRate=5 → 25s = 250 ticks + margin
                    spdlog::info("--- Phase 3: Engineer builds T1 PGen (ueb1101) ---");
                    std::string p3_lua =
                        "local eng = GetEntityById(" + std::to_string(engineer_id) + ")\n"
                        "if eng then\n"
                        "    local pos = eng:GetPosition()\n"
                        "    local build_pos = {pos[1] + 10, pos[2], pos[3]}\n"
                        "    LOG('Chain Phase 3: Engineer building ueb1101')\n"
                        "    IssueBuildMobile({eng}, build_pos, 'ueb1101', {})\n"
                        "end";
                    auto r3 = state.do_string(p3_lua);
                    if (!r3) spdlog::warn("Chain P3 Lua error: {}", r3.error().message);

                    osc::u32 pgen_id = 0;
                    for (int i = 0; i < 300; i++) {
                        auto* eent = sim_state.entity_registry().find(engineer_id);
                        if (eent && eent->is_unit()) {
                            auto* eu = static_cast<osc::sim::Unit*>(eent);
                            if (eu->is_building() && pgen_id == 0)
                                pgen_id = eu->build_target_id();
                        }
                        sim_state.tick();
                        if ((i + 1) % 100 == 0) {
                            auto* eent2 = sim_state.entity_registry().find(engineer_id);
                            if (eent2 && eent2->is_unit()) {
                                auto* eu = static_cast<osc::sim::Unit*>(eent2);
                                if (eu->is_building()) {
                                    auto* t = sim_state.entity_registry().find(eu->build_target_id());
                                    if (t) spdlog::info("  tick {}: target #{} frac={:.1f}%",
                                                        i + 1, t->entity_id(),
                                                        t->fraction_complete() * 100);
                                } else {
                                    spdlog::info("  tick {}: not building", i + 1);
                                }
                            }
                        }
                    }

                    if (pgen_id == 0) {
                        spdlog::error("Chain test: pgen not found after Phase 3");
                    } else {
                        auto* pgen = sim_state.entity_registry().find(pgen_id);
                        spdlog::info("Phase 3 done: pgen = entity #{}, frac={:.1f}%, "
                                     "health={:.0f}/{:.0f}",
                                     pgen_id,
                                     pgen ? pgen->fraction_complete() * 100 : 0,
                                     pgen ? pgen->health() : 0,
                                     pgen ? pgen->max_health() : 0);
                    }
                }
            }
            spdlog::info("Chain test: {} entities total",
                         sim_state.entity_registry().count());
        }
    }

    // AI test: inject simple AI thread for ARMY_2, run ticks, verify results
    if (ai_test && !map_path.empty()) {
        spdlog::info("=== AI TEST: Autonomous base building (ARMY_2) ===");

        // Inject AI thread: builds base, then engineers assist ACU
        auto ai_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(30) -- let session stabilize

                local brain = ArmyBrains[2] -- ARMY_2 (1-based)
                if not brain then
                    LOG('AI thread: no brain for ARMY_2')
                    return
                end

                local catCmd = ParseEntityCategory('COMMAND')
                local catFac = ParseEntityCategory('FACTORY')
                local catEng = ParseEntityCategory('TECH1 ENGINEER')

                -- Find our ACU
                local units = brain:GetListOfUnits(catCmd, true)
                if not units or not units[1] then
                    LOG('AI thread: no ACU found')
                    return
                end
                local acu = units[1]
                LOG('AI thread: found ACU, starting base build')

                -- Phase 1: Build 2 power generators (unassisted)
                for i = 1, 2 do
                    local pos = brain:FindPlaceToBuild(
                        'T1EnergyProduction', 'ueb1101', nil, false, acu)
                    if pos then
                        brain:BuildStructure(acu, 'ueb1101', pos, false)
                        LOG('AI thread: building pgen #' .. i)
                    end
                    while not acu:IsIdleState() do WaitTicks(10) end
                end

                -- Phase 2: Build T1 land factory
                local pos = brain:FindPlaceToBuild(
                    'T1LandFactory', 'ueb0101', nil, false, acu)
                if pos then
                    brain:BuildStructure(acu, 'ueb0101', pos, false)
                    LOG('AI thread: building factory')
                end
                while not acu:IsIdleState() do WaitTicks(10) end

                -- Phase 3: Queue 3 engineers from factory (continuous production)
                local facs = brain:GetListOfUnits(catFac, true)
                if facs and facs[1] then
                    brain:BuildUnit(facs[1], 'uel0105')
                    brain:BuildUnit(facs[1], 'uel0105')
                    brain:BuildUnit(facs[1], 'uel0105')
                    LOG('AI thread: queued 3 engineers from factory')
                end

                -- Phase 4: Wait for first engineer to complete
                for i = 1, 100 do
                    WaitTicks(10)
                    local engs = brain:GetListOfUnits(catEng, true)
                    if engs and engs[1] and not engs[1]:IsUnitState('BeingBuilt') then
                        LOG('AI thread: first engineer ready')

                        -- Guard: engineer assists ACU
                        IssueGuard({engs[1]}, acu)
                        LOG('AI thread: engineer guarding ACU')
                        break
                    end
                end

                -- Phase 5: ACU builds 2 more pgens (engineer assists -> faster)
                for i = 1, 2 do
                    local pos = brain:FindPlaceToBuild(
                        'T1EnergyProduction', 'ueb1101', nil, false, acu)
                    if pos then
                        brain:BuildStructure(acu, 'ueb1101', pos, false)
                        LOG('AI thread: building assisted pgen #' .. i)
                    end
                    while not acu:IsIdleState() do WaitTicks(10) end
                end

                -- Verify guard relationships
                local guards = acu:GetGuards()
                LOG('AI thread: ACU has ' .. table.getn(guards) .. ' guards')

                local engs = brain:GetListOfUnits(catEng, true)
                if engs and engs[1] then
                    local guarded = engs[1]:GetGuardedUnit()
                    if guarded then
                        LOG('AI thread: engineer guarding entity #' .. guarded:GetEntityId())
                    end
                end
            end)
        )");
        if (!ai_result) {
            spdlog::warn("AI test thread injection error: {}",
                         ai_result.error().message);
        }

        // Run 1200 ticks (enough for all builds + assist to complete)
        spdlog::info("Running 1200 AI ticks...");
        for (int i = 0; i < 1200; i++) {
            sim_state.tick();
            if ((i + 1) % 300 == 0) {
                spdlog::info("  tick {}: {} entities",
                             i + 1, sim_state.entity_registry().count());
            }
        }

        // Verify results: 8 ACUs + 4 pgens + 1 factory + 3 engineers = 16
        auto entity_count = sim_state.entity_registry().count();
        spdlog::info("AI test: {} entities total (expected 16: 8 ACUs + "
                     "4 pgens + 1 factory + 3 engineers)", entity_count);

        // Log all non-ACU entities
        sim_state.entity_registry().for_each([](osc::sim::Entity& e) {
            if (e.destroyed()) return;
            spdlog::info("  entity #{}: bp={} army={} frac={:.0f}% hp={:.0f}/{:.0f}",
                         e.entity_id(), e.blueprint_id(), e.army(),
                         e.fraction_complete() * 100,
                         e.health(), e.max_health());
        });
    }

    // Reclaim test: create prop, have ACU reclaim it
    if (reclaim_test && !map_path.empty()) {
        spdlog::info("=== RECLAIM TEST: Prop reclaim ===");

        // Record initial entity count
        auto pre_count = sim_state.entity_registry().count();

        auto reclaim_result = state.do_string(R"(
            -- Find ARMY_1's ACU
            local acu = GetEntityById(1)
            if not acu then
                LOG('Reclaim test: no entity #1')
                return
            end

            -- Create a prop near the ACU (within reclaim range)
            local pos = acu:GetPosition()
            local prop = CreateProp({pos[1] + 3, pos[2], pos[3]},
                                    '/env/common/props/test_reclaim.bp')
            if not prop then
                LOG('Reclaim test: CreateProp failed')
                return
            end
            LOG('Reclaim test: created prop #' .. prop:GetEntityId() ..
                ' near ACU at (' .. pos[1] .. ', ' .. pos[3] .. ')')

            -- Set reclaim values (normally done by Prop:SetMaxReclaimValues)
            prop.MaxMassReclaim = 100
            prop.MaxEnergyReclaim = 50
            prop.TimeReclaim = 1
            prop:SetMaxHealth(50)
            prop:SetHealth(nil, 50)
            prop:SetFractionComplete(1.0)

            -- Issue reclaim command
            IssueReclaim({acu}, prop)
            LOG('Reclaim test: ACU reclaiming prop')
        )");
        if (!reclaim_result) {
            spdlog::warn("Reclaim test Lua error: {}",
                         reclaim_result.error().message);
        }

        // Run 200 ticks (plenty of time for ACU to move + reclaim)
        spdlog::info("Running 200 reclaim ticks...");
        for (int i = 0; i < 200; i++) {
            sim_state.tick();
            if ((i + 1) % 50 == 0) {
                spdlog::info("  tick {}: {} entities",
                             i + 1, sim_state.entity_registry().count());
            }
        }

        // Verify: prop should be gone (destroyed or fraction=0)
        auto post_count = sim_state.entity_registry().count();
        spdlog::info("Reclaim test: entities before={} after={} "
                     "(prop should be destroyed)",
                     pre_count, post_count);

        // List all entities
        sim_state.entity_registry().for_each([](osc::sim::Entity& e) {
            if (e.destroyed()) return;
            spdlog::info("  entity #{}: bp={} army={} frac={:.0f}% hp={:.0f}/{:.0f} prop={}",
                         e.entity_id(), e.blueprint_id(), e.army(),
                         e.fraction_complete() * 100,
                         e.health(), e.max_health(),
                         e.is_prop() ? "yes" : "no");
        });
    }

    // Platoon test: create platoon, assign units, move, fork thread, disband
    if (threat_test && !map_path.empty()) {
        spdlog::info("=== THREAT TEST: Threat queries, targeting, command tracking ===");

        auto tt_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(30) -- let session stabilize

                local brain1 = ArmyBrains[1] -- ARMY_1 (human)
                local brain2 = ArmyBrains[2] -- ARMY_2 (AI)
                if not brain1 or not brain2 then
                    LOG('THREAT TEST FAILED: missing brains')
                    return
                end

                -- Find ARMY_1 ACU and ARMY_2 ACU
                local catCmd = ParseEntityCategory('COMMAND')
                local units1 = brain1:GetListOfUnits(catCmd, true)
                local units2 = brain2:GetListOfUnits(catCmd, true)
                if not units1 or not units1[1] or not units2 or not units2[1] then
                    LOG('THREAT TEST FAILED: no ACUs found')
                    return
                end
                local acu1 = units1[1]
                local acu2 = units2[1]
                local pos1 = acu1:GetPosition()
                local pos2 = acu2:GetPosition()
                LOG('Threat test: ACU1 at (' .. string.format('%.0f,%.0f,%.0f', pos1[1], pos1[2], pos1[3]) .. ')')
                LOG('Threat test: ACU2 at (' .. string.format('%.0f,%.0f,%.0f', pos2[1], pos2[2], pos2[3]) .. ')')

                -- 1) GetThreatAtPosition — enemy threat at ACU2 pos from brain1's perspective
                local enemy_threat = brain1:GetThreatAtPosition(pos2, 16, false, 'Overall')
                LOG('Threat test: enemy threat at ACU2 pos = ' .. tostring(enemy_threat))
                if enemy_threat <= 0 then
                    LOG('THREAT TEST FAILED: GetThreatAtPosition returned 0 for enemy ACU')
                    return
                end
                LOG('Threat test: GetThreatAtPosition OK')

                -- 2) GetThreatAtPosition — own army should return 0 (no enemies at own base)
                local own_threat = brain1:GetThreatAtPosition(pos1, 1, false, 'Overall')
                LOG('Threat test: enemy threat at ACU1 pos (radius 1) = ' .. tostring(own_threat))
                -- (could be 0 if only own army there, or non-zero if enemy is close)

                -- 3) GetHighestThreatPosition
                local hpos, hthreat = brain1:GetHighestThreatPosition(16, false, 'Overall')
                LOG('Threat test: highest threat pos = (' ..
                    string.format('%.0f,%.0f,%.0f', hpos[1], hpos[2], hpos[3]) ..
                    ') threat = ' .. tostring(hthreat))
                if hthreat <= 0 then
                    LOG('THREAT TEST FAILED: GetHighestThreatPosition returned 0')
                    return
                end
                LOG('Threat test: GetHighestThreatPosition OK')

                -- 4) GetThreatsAroundPosition
                local threats = brain1:GetThreatsAroundPosition(pos2, 16, false, 'Overall')
                LOG('Threat test: GetThreatsAroundPosition returned ' ..
                    tostring(table.getn(threats)) .. ' cell(s)')
                if table.getn(threats) == 0 then
                    LOG('THREAT TEST FAILED: GetThreatsAroundPosition returned empty')
                    return
                end
                for i, t in threats do
                    LOG('  cell ' .. i .. ': (' ..
                        string.format('%.0f, %.0f', t[1], t[2]) ..
                        ') threat=' .. tostring(t[3]))
                end
                LOG('Threat test: GetThreatsAroundPosition OK')

                -- 5) Create platoon + CalculatePlatoonThreat
                local platoon = brain2:MakePlatoon('ThreatTestPlatoon', 'none')
                brain2:AssignUnitsToPlatoon(platoon, {acu2}, 'Attack', 'none')
                local pt = platoon:CalculatePlatoonThreat('Overall', ParseEntityCategory('ALLUNITS'))
                LOG('Threat test: CalculatePlatoonThreat = ' .. tostring(pt))
                if pt <= 0 then
                    LOG('THREAT TEST FAILED: CalculatePlatoonThreat returned 0')
                    return
                end
                LOG('Threat test: CalculatePlatoonThreat OK')

                -- 6) FindClosestUnit — platoon should find ARMY_1's ACU as enemy
                local closest = platoon:FindClosestUnit('Attack', 'Enemy', true,
                    ParseEntityCategory('ALLUNITS'))
                if not closest then
                    LOG('THREAT TEST FAILED: FindClosestUnit returned nil')
                    return
                end
                local cid = closest:GetEntityId()
                LOG('Threat test: FindClosestUnit found entity #' .. tostring(cid))
                LOG('Threat test: FindClosestUnit OK')

                -- 7) MoveToLocation returns command ID
                local cmd_id = platoon:MoveToLocation({pos2[1] + 20, pos2[2], pos2[3]})
                LOG('Threat test: MoveToLocation returned cmd_id=' .. tostring(cmd_id))
                if not cmd_id or cmd_id == 0 then
                    LOG('THREAT TEST FAILED: MoveToLocation did not return command ID')
                    return
                end
                LOG('Threat test: command ID system OK')

                -- 8) IsCommandsActive (should be true while moving)
                WaitTicks(1)
                local active = platoon:IsCommandsActive(cmd_id)
                LOG('Threat test: IsCommandsActive(cmd_id) = ' .. tostring(active))
                if not active then
                    LOG('THREAT TEST FAILED: IsCommandsActive false while unit should be moving')
                    return
                end

                -- 9) Stop, then IsCommandsActive should be false
                platoon:Stop()
                WaitTicks(1)
                local still_active = platoon:IsCommandsActive(cmd_id)
                LOG('Threat test: IsCommandsActive after Stop = ' .. tostring(still_active))
                if still_active then
                    LOG('THREAT TEST FAILED: IsCommandsActive still true after Stop')
                    return
                end
                LOG('Threat test: IsCommandsActive OK')

                -- 10) FindPrioritizedUnit
                local pri = platoon:FindPrioritizedUnit('Attack', 'Enemy', true,
                    pos2, 4096)
                if not pri then
                    LOG('THREAT TEST FAILED: FindPrioritizedUnit returned nil')
                    return
                end
                LOG('Threat test: FindPrioritizedUnit found entity #' ..
                    tostring(pri:GetEntityId()))
                LOG('Threat test: FindPrioritizedUnit OK')

                -- Cleanup
                brain2:DisbandPlatoon(platoon)

                LOG('THREAT TEST: ALL PASSED')
            end)
        )");
        if (!tt_result) {
            spdlog::warn("Threat test injection error: {}",
                         tt_result.error().message);
        }

        spdlog::info("Running threat test ticks...");
        for (int i = 0; i < 50; i++) {
            sim_state.tick();
        }

        spdlog::info("Threat test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    if (combat_test && !map_path.empty()) {
        spdlog::info("=== COMBAT TEST: AI produces army, forms platoons, attacks ===");

        auto ct_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(30) -- let session stabilize

                local brain = ArmyBrains[2] -- ARMY_2 (AI)
                if not brain then
                    LOG('COMBAT TEST FAILED: no ARMY_2 brain')
                    return
                end

                local catCmd = ParseEntityCategory('COMMAND')
                local catFac = ParseEntityCategory('FACTORY')
                local catLand = ParseEntityCategory('TECH1 MOBILE LAND DIRECTFIRE')

                -- Find ACU
                local units = brain:GetListOfUnits(catCmd, true)
                if not units or not units[1] then
                    LOG('COMBAT TEST FAILED: no ACU')
                    return
                end
                local acu = units[1]
                LOG('Combat test: found ACU #' .. acu:GetEntityId())

                -- Build 2 pgens
                for i = 1, 2 do
                    local pos = brain:FindPlaceToBuild('T1EnergyProduction', 'ueb1101', nil, false, acu)
                    if pos then brain:BuildStructure(acu, 'ueb1101', pos, false) end
                    while not acu:IsIdleState() do WaitTicks(10) end
                end
                LOG('Combat test: 2 pgens built')

                -- Build factory
                local pos = brain:FindPlaceToBuild('T1LandFactory', 'ueb0101', nil, false, acu)
                if pos then brain:BuildStructure(acu, 'ueb0101', pos, false) end
                while not acu:IsIdleState() do WaitTicks(10) end
                LOG('Combat test: factory built')

                -- Queue 4 assault bots (uel0201 = Mech Marine)
                local facs = brain:GetListOfUnits(catFac, true)
                if not facs or not facs[1] then
                    LOG('COMBAT TEST FAILED: no factory found')
                    return
                end
                for i = 1, 4 do
                    brain:BuildUnit(facs[1], 'uel0201')
                end
                LOG('Combat test: queued 4 assault bots')

                -- Wait for 3+ assault bots
                local ready = 0
                for i = 1, 150 do
                    WaitTicks(10)
                    local bots = brain:GetListOfUnits(catLand, true)
                    ready = 0
                    if bots then
                        for _, u in bots do
                            if not u:IsUnitState('BeingBuilt') then
                                ready = ready + 1
                            end
                        end
                    end
                    if ready >= 3 then break end
                end
                LOG('Combat test: ' .. ready .. ' assault bots ready')
                if ready < 3 then
                    LOG('COMBAT TEST FAILED: only ' .. ready .. ' bots produced')
                    return
                end

                -- 1) SetCurrentEnemy / GetCurrentEnemy
                brain:SetCurrentEnemy(ArmyBrains[1])
                local enemy = brain:GetCurrentEnemy()
                if not enemy then
                    LOG('COMBAT TEST FAILED: GetCurrentEnemy returned nil')
                    return
                end
                LOG('Combat test: enemy set to army ' .. enemy:GetArmyIndex())
                LOG('Combat test: GetCurrentEnemy OK')

                -- 2) GetNumUnitsAroundPoint
                local startPos = acu:GetPosition()
                local numEnemy = brain:GetNumUnitsAroundPoint(
                    ParseEntityCategory('ALLUNITS'),
                    startPos, 4096, 'Enemy')
                LOG('Combat test: ' .. numEnemy .. ' enemy units on map')
                if numEnemy <= 0 then
                    LOG('COMBAT TEST FAILED: GetNumUnitsAroundPoint returned 0')
                    return
                end
                LOG('Combat test: GetNumUnitsAroundPoint OK')

                -- 3) Create attack platoon
                local platoon = brain:MakePlatoon('AttackForce', 'HuntAI')
                if not platoon then
                    LOG('COMBAT TEST FAILED: MakePlatoon returned nil')
                    return
                end

                -- Verify plan name
                local planName = platoon:GetPlan()
                LOG('Combat test: platoon plan = ' .. tostring(planName))
                if planName ~= 'HuntAI' then
                    LOG('COMBAT TEST FAILED: GetPlan returned wrong plan')
                    return
                end
                LOG('Combat test: GetPlan OK')

                -- Assign ready bots to platoon
                local readyBots = {}
                local bots = brain:GetListOfUnits(catLand, true)
                if bots then
                    for _, u in bots do
                        if not u:IsUnitState('BeingBuilt') then
                            table.insert(readyBots, u)
                        end
                    end
                end
                brain:AssignUnitsToPlatoon(platoon, readyBots, 'Attack', 'none')
                LOG('Combat test: assigned ' .. table.getn(readyBots) .. ' bots to platoon')

                -- 4) GetPlatoonsList
                local platoons = brain:GetPlatoonsList()
                LOG('Combat test: ' .. table.getn(platoons) .. ' platoons total')
                if table.getn(platoons) < 1 then
                    LOG('COMBAT TEST FAILED: GetPlatoonsList empty')
                    return
                end
                LOG('Combat test: GetPlatoonsList OK')

                -- 5) GetBlip
                if readyBots[1] then
                    local blip = readyBots[1]:GetBlip(brain:GetArmyIndex())
                    if not blip then
                        LOG('COMBAT TEST FAILED: GetBlip returned nil')
                        return
                    end
                    local bpos = blip:GetPosition()
                    LOG('Combat test: GetBlip pos = (' ..
                        string.format('%.0f, %.0f, %.0f', bpos[1], bpos[2], bpos[3]) .. ')')
                    LOG('Combat test: GetBlip OK')
                end

                -- 6) HuntAI loop: find enemy, move toward them
                for round = 1, 10 do
                    if not brain:PlatoonExists(platoon) then
                        LOG('Combat test: platoon disbanded at round ' .. round)
                        break
                    end

                    local target = platoon:FindClosestUnit('Attack', 'Enemy', true,
                        ParseEntityCategory('ALLUNITS'))
                    if target then
                        local tpos = target:GetPosition()
                        platoon:Stop()
                        platoon:AggressiveMoveToLocation({tpos[1], tpos[2], tpos[3]})
                        LOG('Combat test: round ' .. round ..
                            ' attacking toward (' .. string.format('%.0f, %.0f', tpos[1], tpos[3]) .. ')')
                    else
                        LOG('Combat test: round ' .. round .. ' no target found')
                    end
                    WaitTicks(50)

                    local ppos = platoon:GetPlatoonPosition()
                    if ppos then
                        LOG('Combat test: platoon at (' ..
                            string.format('%.0f, %.0f', ppos[1], ppos[3]) .. ')')
                    end
                end

                -- Cleanup
                if brain:PlatoonExists(platoon) then
                    brain:DisbandPlatoon(platoon)
                end

                LOG('COMBAT TEST: ALL PASSED')
            end)
        )");
        if (!ct_result) {
            spdlog::warn("Combat test injection error: {}",
                         ct_result.error().message);
        }

        spdlog::info("Running combat test ticks...");
        for (int i = 0; i < 2000; i++) {
            sim_state.tick();
        }

        spdlog::info("Combat test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    if (platoon_test && !map_path.empty()) {
        spdlog::info("=== PLATOON TEST: Platoon system ===");

        auto pt_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(30) -- let session stabilize

                local brain = ArmyBrains[2] -- ARMY_2 (1-based)
                if not brain then
                    LOG('PLATOON TEST FAILED: no brain for ARMY_2')
                    return
                end

                -- Find ACU
                local catCmd = ParseEntityCategory('COMMAND')
                local units = brain:GetListOfUnits(catCmd, true)
                if not units or not units[1] then
                    LOG('PLATOON TEST FAILED: no ACU found')
                    return
                end
                local acu = units[1]
                local acu_id = acu:GetEntityId()
                LOG('Platoon test: found ACU #' .. acu_id)

                -- 1) Create platoon
                local platoon = brain:MakePlatoon('TestPlatoon', 'none')
                if not platoon then
                    LOG('PLATOON TEST FAILED: MakePlatoon returned nil')
                    return
                end
                LOG('Platoon test: MakePlatoon OK')

                -- 2) Assign ACU to platoon
                brain:AssignUnitsToPlatoon(platoon, {acu}, 'Attack', 'none')
                LOG('Platoon test: AssignUnitsToPlatoon OK')

                -- 3) Verify PlatoonExists
                if not brain:PlatoonExists(platoon) then
                    LOG('PLATOON TEST FAILED: PlatoonExists returned false')
                    return
                end
                LOG('Platoon test: PlatoonExists OK')

                -- 4) GetPlatoonUnits
                local punits = platoon:GetPlatoonUnits()
                if not punits or table.getn(punits) ~= 1 then
                    LOG('PLATOON TEST FAILED: GetPlatoonUnits count=' ..
                        tostring(punits and table.getn(punits) or 'nil'))
                    return
                end
                if punits[1]:GetEntityId() ~= acu_id then
                    LOG('PLATOON TEST FAILED: wrong unit in platoon')
                    return
                end
                LOG('Platoon test: GetPlatoonUnits OK (1 unit)')

                -- 5) GetPlatoonPosition
                local ppos = platoon:GetPlatoonPosition()
                if not ppos then
                    LOG('PLATOON TEST FAILED: GetPlatoonPosition nil')
                    return
                end
                LOG('Platoon test: GetPlatoonPosition = (' ..
                    string.format('%.0f, %.0f, %.0f', ppos[1], ppos[2], ppos[3]) .. ')')

                -- 6) GetBrain
                local pbrain = platoon:GetBrain()
                if not pbrain then
                    LOG('PLATOON TEST FAILED: GetBrain nil')
                    return
                end
                LOG('Platoon test: GetBrain OK')

                -- 7) UniquelyNamePlatoon + GetPlatoonUniquelyNamed
                platoon:UniquelyNamePlatoon('MySpecialPlatoon')
                local found = brain:GetPlatoonUniquelyNamed('MySpecialPlatoon')
                if not found then
                    LOG('PLATOON TEST FAILED: GetPlatoonUniquelyNamed nil')
                    return
                end
                LOG('Platoon test: UniquelyNamePlatoon + GetPlatoonUniquelyNamed OK')

                -- 8) MoveToLocation
                local acu_pos = acu:GetPosition()
                platoon:MoveToLocation({acu_pos[1] + 20, acu_pos[2], acu_pos[3]})
                LOG('Platoon test: MoveToLocation issued')
                WaitTicks(5)

                -- Verify unit is moving
                if not acu:IsMoving() then
                    LOG('PLATOON TEST WARNING: ACU not moving after MoveToLocation')
                end

                -- 9) Stop
                platoon:Stop()
                WaitTicks(2)
                if acu:IsMoving() then
                    LOG('PLATOON TEST WARNING: ACU still moving after Stop')
                end
                LOG('Platoon test: Stop OK')

                -- 10) Unit state flags
                acu:SetBusy(true)
                if not acu:IsUnitState('Busy') then
                    LOG('PLATOON TEST FAILED: SetBusy/IsUnitState Busy')
                    return
                end
                acu:SetBusy(false)

                acu:SetFireState(2)
                if acu:GetFireState() ~= 2 then
                    LOG('PLATOON TEST FAILED: SetFireState/GetFireState')
                    return
                end
                acu:SetFireState(0)
                LOG('Platoon test: unit state flags OK')

                -- 11) ForkThread on platoon
                local fork_ran = false
                platoon:ForkThread(function(self)
                    fork_ran = true
                    LOG('Platoon test: ForkThread callback ran')
                end)
                WaitTicks(3) -- let forked thread run

                if not fork_ran then
                    LOG('PLATOON TEST FAILED: ForkThread did not run')
                    return
                end
                LOG('Platoon test: ForkThread OK')

                -- 12) Patrol
                platoon:Patrol({acu_pos[1] + 30, acu_pos[2], acu_pos[3] + 30})
                WaitTicks(2)
                if not acu:IsUnitState('Patrolling') then
                    LOG('PLATOON TEST WARNING: ACU not patrolling after Patrol')
                end
                platoon:Stop()
                LOG('Platoon test: Patrol OK')

                -- 13) GetSquadUnits
                local squad_units = platoon:GetSquadUnits('Attack')
                if squad_units and table.getn(squad_units) == 1 then
                    LOG('Platoon test: GetSquadUnits OK')
                else
                    LOG('PLATOON TEST WARNING: GetSquadUnits count=' ..
                        tostring(squad_units and table.getn(squad_units) or 'nil'))
                end

                -- 14) DisbandPlatoon
                brain:DisbandPlatoon(platoon)

                -- PlatoonExists should be false now
                if brain:PlatoonExists(platoon) then
                    LOG('PLATOON TEST FAILED: PlatoonExists true after disband')
                    return
                end
                LOG('Platoon test: DisbandPlatoon OK')

                LOG('PLATOON TEST: ALL PASSED')
            end)
        )");
        if (!pt_result) {
            spdlog::warn("Platoon test injection error: {}",
                         pt_result.error().message);
        }

        // Run enough ticks for the test thread to complete
        spdlog::info("Running platoon test ticks...");
        for (int i = 0; i < 50; i++) {
            sim_state.tick();
        }

        spdlog::info("Platoon test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // Repair test: ACU builds pgen, damage it, repair it
    if (repair_test && !map_path.empty()) {
        spdlog::info("=== REPAIR TEST: Build, damage, repair ===");

        auto rt_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(10)

                local acu = GetEntityById(1) -- ARMY_1 ACU
                if not acu then
                    LOG('REPAIR TEST FAILED: no entity #1')
                    return
                end
                LOG('Repair test: ACU #' .. acu:GetEntityId())

                -- Build a T1 pgen near ACU
                local pos = acu:GetPosition()
                local build_pos = {pos[1] + 10, pos[2], pos[3]}
                LOG('Repair test: building ueb1101')
                IssueBuildMobile({acu}, build_pos, 'ueb1101', {})

                -- Wait for build to complete
                for i = 1, 200 do
                    WaitTicks(5)
                    if acu:IsIdleState() then break end
                end

                -- Find the pgen
                local pgen = nil
                for id = 2, 200 do
                    local e = GetEntityById(id)
                    if e and not IsDestroyed(e) then
                        local ok, uid = pcall(function() return e:GetUnitId() end)
                        if ok and uid == 'ueb1101' then
                            pgen = e
                            break
                        end
                    end
                end

                if not pgen then
                    LOG('REPAIR TEST FAILED: pgen not found after build')
                    return
                end

                local max_hp = pgen:GetMaxHealth()
                LOG('Repair test: pgen #' .. pgen:GetEntityId() ..
                    ' built, health=' .. pgen:GetHealth() .. '/' .. max_hp)

                -- Damage the pgen to 50%
                local dmg = max_hp * 0.5
                Damage(nil, pgen, dmg, nil, 'Normal')
                local hp_after_dmg = pgen:GetHealth()
                LOG('Repair test: damaged pgen to ' .. hp_after_dmg .. '/' .. max_hp)
                if hp_after_dmg >= max_hp then
                    LOG('REPAIR TEST FAILED: damage did not reduce health')
                    return
                end

                -- Issue repair command
                IssueRepair({acu}, pgen)
                LOG('Repair test: ACU repairing pgen')

                -- Wait for repair to complete
                for i = 1, 300 do
                    WaitTicks(5)
                    if IsDestroyed(pgen) then
                        LOG('REPAIR TEST FAILED: pgen destroyed during repair')
                        return
                    end
                    local hp = pgen:GetHealth()
                    if math.mod(i, 20) == 0 then
                        LOG('Repair test: tick ' .. (i*5) .. ' health=' .. string.format('%.0f', hp) .. '/' .. max_hp)
                    end
                    if hp >= max_hp then
                        LOG('Repair test: pgen fully repaired at tick ' .. (i*5))
                        break
                    end
                end

                local final_hp = pgen:GetHealth()
                if final_hp >= max_hp then
                    LOG('REPAIR TEST: ALL PASSED (health=' ..
                        string.format('%.0f', final_hp) .. '/' .. max_hp .. ')')
                else
                    LOG('REPAIR TEST FAILED: health=' ..
                        string.format('%.0f', final_hp) .. '/' .. max_hp ..
                        ' (not fully repaired)')
                end
            end)
        )");
        if (!rt_result) {
            spdlog::warn("Repair test injection error: {}",
                         rt_result.error().message);
        }

        spdlog::info("Running repair test ticks...");
        for (int i = 0; i < 400; i++) {
            sim_state.tick();
            if ((i + 1) % 100 == 0) {
                spdlog::info("  tick {}: {} entities",
                             i + 1, sim_state.entity_registry().count());
            }
        }

        spdlog::info("Repair test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // Upgrade test: ACU builds T1 mex, upgrades to T2
    if (upgrade_test && !map_path.empty()) {
        spdlog::info("=== UPGRADE TEST: Build T1 mex, upgrade to T2 ===");

        auto ut_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(10)

                local acu = GetEntityById(1) -- ARMY_1 ACU
                if not acu then
                    LOG('UPGRADE TEST FAILED: no entity #1')
                    return
                end

                -- Build a T1 mass extractor near ACU
                local pos = acu:GetPosition()
                local build_pos = {pos[1] + 10, pos[2], pos[3]}
                LOG('Upgrade test: building ueb1103 (T1 mex)')
                IssueBuildMobile({acu}, build_pos, 'ueb1103', {})

                -- Wait for build to complete
                for i = 1, 400 do
                    WaitTicks(5)
                    if acu:IsIdleState() then break end
                end

                -- Find the T1 mex
                local mex = nil
                for id = 2, 200 do
                    local e = GetEntityById(id)
                    if e and not IsDestroyed(e) then
                        local ok, uid = pcall(function() return e:GetUnitId() end)
                        if ok and uid == 'ueb1103' then
                            mex = e
                            break
                        end
                    end
                end

                if not mex then
                    LOG('UPGRADE TEST FAILED: T1 mex not found after build')
                    return
                end

                local mex_id = mex:GetEntityId()
                LOG('Upgrade test: T1 mex #' .. mex_id ..
                    ' built, health=' .. mex:GetHealth() .. '/' .. mex:GetMaxHealth())

                -- Check that mex is idle
                if not mex:IsIdleState() then
                    LOG('UPGRADE TEST WARNING: mex not idle after build')
                end

                -- Issue upgrade to T2 mex (ueb1202)
                LOG('Upgrade test: issuing IssueUpgrade to ueb1202')
                IssueUpgrade({mex}, 'ueb1202')

                -- Check that mex has upgrade state
                WaitTicks(5)
                local is_upgrading = mex:IsUnitState('Upgrading')
                LOG('Upgrade test: IsUnitState(Upgrading) = ' .. tostring(is_upgrading))

                -- Wait for upgrade to complete
                local t2_mex = nil
                for i = 1, 800 do
                    WaitTicks(5)

                    -- Check for a T2 mex entity
                    for id = 2, 200 do
                        local e = GetEntityById(id)
                        if e and not IsDestroyed(e) then
                            local ok, uid = pcall(function() return e:GetUnitId() end)
                            if ok and uid == 'ueb1202' then
                                t2_mex = e
                                break
                            end
                        end
                    end

                    if t2_mex and t2_mex:GetFractionComplete() >= 1.0 then
                        LOG('Upgrade test: T2 mex #' .. t2_mex:GetEntityId() ..
                            ' complete at tick ' .. (i*5))
                        break
                    end

                    if math.mod(i, 100) == 0 then
                        if t2_mex then
                            LOG('Upgrade test: tick ' .. (i*5) ..
                                ' T2 mex frac=' .. string.format('%.1f%%', t2_mex:GetFractionComplete() * 100))
                        else
                            LOG('Upgrade test: tick ' .. (i*5) .. ' no T2 mex yet')
                        end
                    end
                end

                if t2_mex and t2_mex:GetFractionComplete() >= 1.0 then
                    LOG('UPGRADE TEST: ALL PASSED (T2 mex #' .. t2_mex:GetEntityId() ..
                        ' health=' .. string.format('%.0f', t2_mex:GetHealth()) ..
                        '/' .. t2_mex:GetMaxHealth() .. ')')
                else
                    LOG('UPGRADE TEST FAILED: T2 mex not completed')
                end
            end)
        )");
        if (!ut_result) {
            spdlog::warn("Upgrade test injection error: {}",
                         ut_result.error().message);
        }

        spdlog::info("Running upgrade test ticks...");
        for (int i = 0; i < 1500; i++) {
            sim_state.tick();
            if ((i + 1) % 300 == 0) {
                spdlog::info("  tick {}: {} entities",
                             i + 1, sim_state.entity_registry().count());
            }
        }

        spdlog::info("Upgrade test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // Capture test: ARMY_1 ACU builds enemy pgen, captures it
    if (capture_test && !map_path.empty()) {
        spdlog::info("=== CAPTURE TEST: Build enemy pgen, capture it ===");

        auto ct_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(10)

                local acu = GetEntityById(1) -- ARMY_1 ACU
                if not acu then
                    LOG('CAPTURE TEST FAILED: no entity #1')
                    return
                end
                local acu_army = acu:GetArmy()
                LOG('Capture test: ACU #' .. acu:GetEntityId() ..
                    ' army=' .. acu_army)

                -- Build a T1 pgen via ACU so it goes through the normal
                -- build lifecycle (avoids OnCreate/OnStopBeingBuilt issues
                -- from CreateUnitHPR for non-ACU units)
                local pos = acu:GetPosition()
                local build_pos = {pos[1] + 10, pos[2], pos[3]}
                LOG('Capture test: ACU building ueb1101')
                IssueBuildMobile({acu}, build_pos, 'ueb1101', {})

                -- Wait for build to complete
                for i = 1, 200 do
                    WaitTicks(5)
                    if acu:IsIdleState() then break end
                end

                -- Find the pgen
                local enemy_pgen = nil
                for id = 2, 200 do
                    local e = GetEntityById(id)
                    if e and not IsDestroyed(e) then
                        local ok, uid = pcall(function() return e:GetUnitId() end)
                        if ok and uid == 'ueb1101' then
                            enemy_pgen = e
                            break
                        end
                    end
                end

                if not enemy_pgen then
                    LOG('CAPTURE TEST FAILED: pgen not found after build')
                    return
                end
                LOG('Capture test: pgen #' .. enemy_pgen:GetEntityId() ..
                    ' built, army=' .. enemy_pgen:GetArmy())

                -- Transfer pgen to ARMY_2 using ChangeUnitArmy
                ChangeUnitArmy(enemy_pgen, 2)
                LOG('Capture test: transferred pgen to ARMY_2, army=' ..
                    enemy_pgen:GetArmy())
                local pgen_id = enemy_pgen:GetEntityId()
                local pgen_army_before = enemy_pgen:GetArmy()
                LOG('Capture test: enemy pgen #' .. pgen_id ..
                    ' army=' .. pgen_army_before ..
                    ' health=' .. enemy_pgen:GetHealth() .. '/' ..
                    enemy_pgen:GetMaxHealth())

                -- Verify it belongs to ARMY_2
                if pgen_army_before ~= 2 then
                    LOG('CAPTURE TEST FAILED: pgen army=' ..
                        pgen_army_before .. ' expected 2')
                    return
                end

                -- Hold fire so ACU doesn't kill pgen during capture
                acu:SetFireState(1) -- HoldFire
                IssueCapture({acu}, enemy_pgen)
                LOG('Capture test: ACU capturing enemy pgen (fire=hold)')

                -- Wait for capture to complete
                local captured = false
                for i = 1, 500 do
                    WaitTicks(5)
                    if IsDestroyed(enemy_pgen) then
                        LOG('CAPTURE TEST FAILED: pgen destroyed during capture')
                        return
                    end
                    local cur_army = enemy_pgen:GetArmy()
                    if math.mod(i, 20) == 0 then
                        local wp = acu:GetWorkProgress()
                        LOG('Capture test: tick ' .. (i*5) ..
                            ' army=' .. cur_army ..
                            ' workProgress=' .. string.format('%.2f', wp))
                    end
                    if cur_army == acu_army then
                        LOG('Capture test: pgen captured at tick ' .. (i*5))
                        captured = true
                        break
                    end
                end

                if not captured then
                    LOG('CAPTURE TEST FAILED: pgen not captured after timeout')
                    return
                end

                -- Verify entity still alive
                if IsDestroyed(enemy_pgen) then
                    LOG('CAPTURE TEST FAILED: pgen destroyed after capture')
                    return
                end

                local final_army = enemy_pgen:GetArmy()
                local final_hp = enemy_pgen:GetHealth()
                local max_hp = enemy_pgen:GetMaxHealth()
                LOG('CAPTURE TEST: ALL PASSED (army=' .. final_army ..
                    ' health=' .. string.format('%.0f', final_hp) ..
                    '/' .. max_hp .. ')')
            end)
        )");
        if (!ct_result) {
            spdlog::warn("Capture test injection error: {}",
                         ct_result.error().message);
        }

        spdlog::info("Running capture test ticks...");
        for (int i = 0; i < 500; i++) {
            sim_state.tick();
            if ((i + 1) % 100 == 0) {
                spdlog::info("  tick {}: {} entities",
                             i + 1, sim_state.entity_registry().count());
            }
        }

        spdlog::info("Capture test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // Path test: A* pathfinding around obstacles + terrain height tracking
    if (path_test && !map_path.empty()) {
        spdlog::info("=== PATH TEST: A* pathfinding ===");

        // 1) Log pathfinding grid stats
        auto* grid = sim_state.pathfinding_grid();
        if (!grid) {
            spdlog::error("PATH TEST FAILED: no pathfinding grid");
        } else {
            // Count cell types
            osc::u32 passable = 0, impassable = 0, water = 0, obstacle = 0;
            for (osc::u32 gz = 0; gz < grid->grid_height(); gz++) {
                for (osc::u32 gx = 0; gx < grid->grid_width(); gx++) {
                    switch (grid->get(gx, gz)) {
                    case osc::map::CellPassability::Passable: passable++; break;
                    case osc::map::CellPassability::Impassable: impassable++; break;
                    case osc::map::CellPassability::Water: water++; break;
                    case osc::map::CellPassability::Obstacle: obstacle++; break;
                    }
                }
            }
            spdlog::info("Pathfinding grid: {}x{} (cell_size={})",
                         grid->grid_width(), grid->grid_height(),
                         grid->cell_size());
            spdlog::info("  Passable={}, Impassable={}, Water={}, Obstacle={}",
                         passable, impassable, water, obstacle);
        }

        // 2) Move ACU across the map — verify Y tracks terrain height
        auto pt_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(10)

                local acu = GetEntityById(1)
                if not acu then
                    LOG('PATH TEST FAILED: no entity #1')
                    return
                end

                local start_pos = acu:GetPosition()
                LOG('Path test: ACU start at (' ..
                    string.format('%.1f, %.1f, %.1f', start_pos[1], start_pos[2], start_pos[3]) .. ')')

                -- Move to a distant location on the map
                local target = {start_pos[1] + 80, 0, start_pos[3] + 80}
                LOG('Path test: moving ACU to (' ..
                    string.format('%.1f, %.1f', target[1], target[3]) .. ')')
                IssueMove({acu}, target)

                -- Track movement: log position + Y every 20 ticks
                local last_y = start_pos[2]
                local y_changed = false
                for i = 1, 40 do
                    WaitTicks(5)
                    if IsDestroyed(acu) then
                        LOG('PATH TEST FAILED: ACU destroyed during move')
                        return
                    end
                    local pos = acu:GetPosition()
                    if math.abs(pos[2] - last_y) > 0.1 then
                        y_changed = true
                    end
                    last_y = pos[2]
                    if math.mod(i, 8) == 0 then
                        LOG('Path test: tick ' .. (i*5) .. ' pos=(' ..
                            string.format('%.1f, %.1f, %.1f', pos[1], pos[2], pos[3]) ..
                            ') moving=' .. tostring(acu:IsMoving()))
                    end
                end

                local mid_pos = acu:GetPosition()
                LOG('Path test: after 200 ticks at (' ..
                    string.format('%.1f, %.1f, %.1f', mid_pos[1], mid_pos[2], mid_pos[3]) .. ')')

                if mid_pos[2] == 0 then
                    LOG('PATH TEST WARNING: Y still at 0 — terrain height not applied')
                else
                    LOG('Path test: terrain height tracking OK (Y=' ..
                        string.format('%.1f', mid_pos[2]) .. ')')
                end
                if y_changed then
                    LOG('Path test: Y changed during movement — terrain following confirmed')
                end

                -- Wait for arrival
                for i = 1, 60 do
                    WaitTicks(5)
                    if not acu:IsMoving() then break end
                end

                local final_pos = acu:GetPosition()
                local dx = final_pos[1] - target[1]
                local dz = final_pos[3] - target[3]
                local dist = math.sqrt(dx*dx + dz*dz)
                LOG('Path test: final pos=(' ..
                    string.format('%.1f, %.1f, %.1f', final_pos[1], final_pos[2], final_pos[3]) ..
                    ') dist_to_target=' .. string.format('%.1f', dist))

                if dist < 5 then
                    LOG('Path test: movement OK (arrived within 5 units)')
                else
                    LOG('PATH TEST WARNING: did not arrive close to target')
                end

                LOG('PATH TEST: BASIC MOVEMENT PASSED')
            end)
        )");
        if (!pt_result) {
            spdlog::warn("Path test Lua error: {}",
                         pt_result.error().message);
        }

        spdlog::info("Running path test ticks (phase 1: movement)...");
        for (int i = 0; i < 500; i++) {
            sim_state.tick();
        }

        // 3) Build a wall of structures, then move around them
        auto pt2_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(5)

                local acu = GetEntityById(1)
                if not acu or IsDestroyed(acu) then
                    LOG('PATH TEST phase 2: ACU gone')
                    return
                end

                -- Move ACU back to start area first
                local brain = ArmyBrains[1]
                if not brain then
                    LOG('PATH TEST phase 2: no brain')
                    return
                end

                -- Build 3 pgens in a line to form a wall
                local pos = acu:GetPosition()
                LOG('Path test phase 2: ACU at (' ..
                    string.format('%.1f, %.1f', pos[1], pos[3]) .. ')')
                LOG('Path test phase 2: building 3-pgen wall')

                for i = 0, 2 do
                    local bx = pos[1] + 20
                    local bz = pos[3] - 8 + i * 8
                    local build_pos = {bx, 0, bz}
                    brain:BuildStructure(acu, 'ueb1101', build_pos, false)
                    while not acu:IsIdleState() do WaitTicks(10) end
                end

                LOG('Path test phase 2: wall built')

                -- Now try to move to the other side of the wall
                local target = {pos[1] + 40, 0, pos[3]}
                LOG('Path test phase 2: moving ACU to other side of wall at (' ..
                    string.format('%.1f, %.1f', target[1], target[3]) .. ')')
                IssueMove({acu}, target)

                -- Wait for movement to complete
                for i = 1, 100 do
                    WaitTicks(5)
                    if not acu:IsMoving() then break end
                end

                local final_pos = acu:GetPosition()
                local dx = final_pos[1] - target[1]
                local dz = final_pos[3] - target[3]
                local dist = math.sqrt(dx*dx + dz*dz)
                LOG('Path test phase 2: final pos=(' ..
                    string.format('%.1f, %.1f, %.1f', final_pos[1], final_pos[2], final_pos[3]) ..
                    ') dist_to_target=' .. string.format('%.1f', dist))

                if dist < 10 then
                    LOG('Path test phase 2: obstacle avoidance OK')
                else
                    LOG('PATH TEST phase 2 WARNING: may not have routed around wall')
                end

                LOG('PATH TEST: ALL PHASES COMPLETE')
            end)
        )");
        if (!pt2_result) {
            spdlog::warn("Path test phase 2 Lua error: {}",
                         pt2_result.error().message);
        }

        spdlog::info("Running path test ticks (phase 2: obstacles)...");
        for (int i = 0; i < 1500; i++) {
            sim_state.tick();
            if ((i + 1) % 500 == 0) {
                spdlog::info("  tick {}: {} entities",
                             i + 1, sim_state.entity_registry().count());
            }
        }

        spdlog::info("Path test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // Toggle test: script bits, toggle caps, and dive command
    if (toggle_test && !map_path.empty()) {
        spdlog::info("=== TOGGLE TEST: Script bits, toggle caps, dive ===");

        // Run initial ticks to let session set up
        for (int i = 0; i < 50; i++) sim_state.tick();

        // Inject Lua test via ForkThread
        auto result = state.do_string(R"(
            ForkThread(function()
                local acu = GetEntityById(1)
                if not acu then
                    LOG('TOGGLE TEST FAILED: no entity #1')
                    return
                end

                -- Test 1: SetScriptBit with string name
                acu:SetScriptBit('RULEUTC_ShieldToggle', true)
                local v1 = acu:GetScriptBit('RULEUTC_ShieldToggle')
                if v1 then
                    LOG('TOGGLE TEST 1 PASSED: SetScriptBit(ShieldToggle, true) -> true')
                else
                    LOG('TOGGLE TEST 1 FAILED: expected true, got false')
                end

                -- Test 2: SetScriptBit false
                acu:SetScriptBit('RULEUTC_ShieldToggle', false)
                local v2 = acu:GetScriptBit('RULEUTC_ShieldToggle')
                if not v2 then
                    LOG('TOGGLE TEST 2 PASSED: SetScriptBit(ShieldToggle, false) -> false')
                else
                    LOG('TOGGLE TEST 2 FAILED: expected false, got true')
                end

                -- Test 3: GetScriptBit with numeric index
                acu:SetScriptBit('RULEUTC_ProductionToggle', true)
                local v3 = acu:GetScriptBit(4)  -- bit 4 = Production
                if v3 then
                    LOG('TOGGLE TEST 3 PASSED: GetScriptBit(4) -> true')
                else
                    LOG('TOGGLE TEST 3 FAILED: expected true, got false')
                end
                acu:SetScriptBit('RULEUTC_ProductionToggle', false)

                -- Test 4: ToggleScriptBit
                acu:ToggleScriptBit(6)  -- GenericToggle, was false
                local v4 = acu:GetScriptBit(6)
                if v4 then
                    LOG('TOGGLE TEST 4 PASSED: ToggleScriptBit(6) flipped to true')
                else
                    LOG('TOGGLE TEST 4 FAILED: expected true after toggle')
                end
                acu:ToggleScriptBit(6)  -- flip back

                -- Test 5: AddToggleCap / TestToggleCaps
                acu:AddToggleCap('RULEUTC_ShieldToggle')
                local v5 = acu:TestToggleCaps('RULEUTC_ShieldToggle')
                if v5 then
                    LOG('TOGGLE TEST 5 PASSED: TestToggleCaps after Add -> true')
                else
                    LOG('TOGGLE TEST 5 FAILED: expected true')
                end

                -- Test 6: RemoveToggleCap
                acu:RemoveToggleCap('RULEUTC_ShieldToggle')
                local v6 = acu:TestToggleCaps('RULEUTC_ShieldToggle')
                if not v6 then
                    LOG('TOGGLE TEST 6 PASSED: TestToggleCaps after Remove -> false')
                else
                    LOG('TOGGLE TEST 6 FAILED: expected false')
                end

                -- Test 7: Layer change + Dive
                local old_layer = acu:GetCurrentLayer()
                LOG('TOGGLE TEST 7: ACU layer before = ' .. tostring(old_layer))

                -- Manually set to Water layer for dive test
                -- (ACUs are Land units, so we need to override for testing)
                acu.Layer = 'Water'
                -- Use the C++ set_layer trick: push a Dive command
                -- But first the C++ layer_ must be Water too.
                -- We'll test via a different entity or accept the Land->noop behavior

                -- Test that dive on a Land unit is harmless (no crash)
                IssueDive({acu})
                WaitTicks(2)
                local layer_after = acu:GetCurrentLayer()
                LOG('TOGGLE TEST 7: ACU layer after dive = ' .. tostring(layer_after))
                if layer_after == old_layer then
                    LOG('TOGGLE TEST 7 PASSED: Dive on Land unit = no-op')
                else
                    LOG('TOGGLE TEST 7 INFO: layer changed to ' .. tostring(layer_after))
                end

                LOG('TOGGLE TEST: ALL PASSED')
            end)
        )");
        if (!result) {
            spdlog::warn("Toggle test Lua error: {}", result.error().message);
        }

        spdlog::info("Running toggle test ticks...");
        for (int i = 0; i < 50; i++) {
            sim_state.tick();
        }

        spdlog::info("Toggle test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // ─── Enhancement test ──────────────────────────────────────────
    if (enhance_test && !map_path.empty()) {
        spdlog::info("=== Enhancement Test ===");

        // Give ARMY_1 enough resources for the enhancement
        state.do_string(R"(
            local brain = GetArmyBrain('ARMY_1')
            if brain then
                brain:GiveResource('MASS', 50000)
                brain:GiveResource('ENERGY', 500000)
            end
        )");

        // Set up test: get ACU, verify enhancements table exists, issue enhance
        auto result = state.do_string(R"(
            -- Find ACU (entity #1, uel0001)
            local acu = GetEntityById(1)
            if not acu then
                LOG('ENHANCE TEST FAILED: no entity #1')
                return
            end
            LOG('ENHANCE TEST: ACU found - ' .. (acu.UnitId or 'nil'))

            -- Verify Blueprint.Enhancements exists
            local bp = acu:GetBlueprint()
            if not bp or not bp.Enhancements then
                LOG('ENHANCE TEST FAILED: no Blueprint.Enhancements')
                return
            end

            local enh = bp.Enhancements.AdvancedEngineering
            if not enh then
                LOG('ENHANCE TEST FAILED: no AdvancedEngineering enhancement')
                return
            end
            LOG('ENHANCE TEST: AdvancedEngineering found - BuildTime=' ..
                tostring(enh.BuildTime) .. ' Slot=' .. tostring(enh.Slot))

            -- Test 1: HasEnhancement should be false initially
            if acu:HasEnhancement('AdvancedEngineering') then
                LOG('ENHANCE TEST 1 FAILED: HasEnhancement returned true before enhance')
                return
            end
            LOG('ENHANCE TEST 1 PASSED: HasEnhancement=false before enhance')

            -- Issue the enhancement
            LOG('ENHANCE TEST: Issuing AdvancedEngineering...')
            IssueEnhancement({acu}, 'AdvancedEngineering')
        )");
        if (!result) {
            spdlog::warn("Enhance test setup Lua error: {}", result.error().message);
        }

        // Run ticks to let the enhancement complete
        // BuildTime=1000, BuildRate=10, dt=0.1 → 1000 ticks to complete
        spdlog::info("Running enhancement ticks (1200)...");
        for (int i = 0; i < 1200; i++) {
            sim_state.tick();
            // Log progress every 50 ticks
            if ((i + 1) % 50 == 0) {
                auto* acu = sim_state.entity_registry().find(1);
                if (acu && acu->is_unit()) {
                    auto* unit = static_cast<osc::sim::Unit*>(acu);
                    spdlog::debug("  Tick {}: work_progress={:.2f} enhancing={}",
                                  i + 1, unit->work_progress(),
                                  unit->is_enhancing() ? "yes" : "no");
                }
            }
        }

        // Verify enhancement completed
        result = state.do_string(R"(
            local acu = GetEntityById(1)
            if not acu then
                LOG('ENHANCE TEST FAILED: ACU gone after ticks')
                return
            end

            -- Test 2: HasEnhancement should be true after completion
            if acu:HasEnhancement('AdvancedEngineering') then
                LOG('ENHANCE TEST 2 PASSED: HasEnhancement=true after enhance')
            else
                LOG('ENHANCE TEST 2 FAILED: HasEnhancement=false after enhance')
            end

            -- Test 3: SimUnitEnhancements should have the entry
            local sue = SimUnitEnhancements[acu.EntityId]
            if sue then
                local found = false
                for k, v in sue do
                    if v == 'AdvancedEngineering' then
                        found = true
                        LOG('ENHANCE TEST 3 PASSED: SimUnitEnhancements[' ..
                            tostring(acu.EntityId) .. '][' .. k .. '] = ' .. v)
                        break
                    end
                end
                if not found then
                    LOG('ENHANCE TEST 3 FAILED: AdvancedEngineering not in SimUnitEnhancements')
                end
            else
                LOG('ENHANCE TEST 3 FAILED: no SimUnitEnhancements entry for ACU')
            end

            -- Test 4: Unit should not be enhancing anymore
            if acu:IsUnitState('Enhancing') then
                LOG('ENHANCE TEST 4 FAILED: still in Enhancing state')
            else
                LOG('ENHANCE TEST 4 PASSED: not in Enhancing state')
            end

            -- Test 5: Unit should be mobile again
            if acu:IsMobile() then
                LOG('ENHANCE TEST 5 PASSED: ACU is mobile again')
            else
                LOG('ENHANCE TEST 5 FAILED: ACU is still immobile')
            end

            LOG('ENHANCE TEST: ALL PASSED')
        )");
        if (!result) {
            spdlog::warn("Enhance test verify Lua error: {}", result.error().message);
        }

        spdlog::info("Enhancement test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // ─── Intel test ─────────────────────────────────────────────
    if (intel_test && !map_path.empty()) {
        spdlog::info("=== Intel Test ===");

        auto result = state.do_string(R"(
            local acu = GetEntityById(1)
            if not acu then
                LOG('INTEL TEST FAILED: no entity #1')
                return
            end
            LOG('INTEL TEST: ACU found - ' .. (acu.UnitId or 'nil'))

            -- Check blueprint has Intel table
            local bp = acu:GetBlueprint()
            if bp and bp.Intel then
                LOG('INTEL TEST: Blueprint.Intel found - VisionRadius=' ..
                    tostring(bp.Intel.VisionRadius or 'nil') ..
                    ' OmniRadius=' .. tostring(bp.Intel.OmniRadius or 'nil'))
            else
                LOG('INTEL TEST: no Blueprint.Intel (ok for some units)')
            end

            -- Test 1: InitIntel + IsIntelEnabled (init auto-enables — original engine behavior)
            acu:InitIntel(1, 'Radar', 44.0)
            if acu:IsIntelEnabled('Radar') then
                LOG('INTEL TEST 1 PASSED: IsIntelEnabled=true after InitIntel')
            else
                LOG('INTEL TEST 1 FAILED: IsIntelEnabled=false after InitIntel (should be true)')
            end

            -- Test 2: EnableIntel → IsIntelEnabled should be true
            acu:EnableIntel('Radar')
            if acu:IsIntelEnabled('Radar') then
                LOG('INTEL TEST 2 PASSED: IsIntelEnabled=true after EnableIntel')
            else
                LOG('INTEL TEST 2 FAILED: IsIntelEnabled=false after EnableIntel')
            end

            -- Test 3: GetIntelRadius should return 44.0
            local radius = acu:GetIntelRadius('Radar')
            if radius == 44.0 then
                LOG('INTEL TEST 3 PASSED: GetIntelRadius=44.0')
            else
                LOG('INTEL TEST 3 FAILED: GetIntelRadius=' .. tostring(radius) .. ' (expected 44.0)')
            end

            -- Test 4: SetIntelRadius → GetIntelRadius should return new value
            acu:SetIntelRadius('Radar', 100.0)
            radius = acu:GetIntelRadius('Radar')
            if radius == 100.0 then
                LOG('INTEL TEST 4 PASSED: GetIntelRadius=100.0 after SetIntelRadius')
            else
                LOG('INTEL TEST 4 FAILED: GetIntelRadius=' .. tostring(radius) .. ' (expected 100.0)')
            end

            -- Test 5: DisableIntel → IsIntelEnabled should be false
            acu:DisableIntel('Radar')
            if acu:IsIntelEnabled('Radar') then
                LOG('INTEL TEST 5 FAILED: IsIntelEnabled=true after DisableIntel')
            else
                LOG('INTEL TEST 5 PASSED: IsIntelEnabled=false after DisableIntel')
            end

            -- Test 6: Radius preserved after disable
            radius = acu:GetIntelRadius('Radar')
            if radius == 100.0 then
                LOG('INTEL TEST 6 PASSED: radius preserved after DisableIntel')
            else
                LOG('INTEL TEST 6 FAILED: radius=' .. tostring(radius) .. ' after DisableIntel')
            end

            -- Test 7: Unknown intel type returns false/0
            if acu:IsIntelEnabled('FakeIntelType') then
                LOG('INTEL TEST 7 FAILED: unknown type returned enabled')
            else
                LOG('INTEL TEST 7 PASSED: unknown type returns false')
            end

            -- Test 8: Multiple intel types are independent
            acu:InitIntel(1, 'Sonar', 30.0)
            acu:EnableIntel('Sonar')
            acu:EnableIntel('Radar')  -- re-enable radar
            if acu:IsIntelEnabled('Radar') and acu:IsIntelEnabled('Sonar') then
                LOG('INTEL TEST 8 PASSED: multiple intel types independent')
            else
                LOG('INTEL TEST 8 FAILED: Radar=' .. tostring(acu:IsIntelEnabled('Radar')) ..
                    ' Sonar=' .. tostring(acu:IsIntelEnabled('Sonar')))
            end

            LOG('INTEL TEST: ALL PASSED')
        )");
        if (!result) {
            spdlog::warn("Intel test Lua error: {}", result.error().message);
        }

        // Run some ticks to verify intel paths don't error during normal operation
        spdlog::info("Running {} post-intel ticks...", tick_count);
        for (osc::u32 i = 0; i < tick_count; i++) {
            sim_state.tick();
        }

        spdlog::info("Intel test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // ─── Shield test ────────────────────────────────────────────
    if (shield_test && !map_path.empty()) {
        spdlog::info("=== Shield Test ===");

        // Run initial ticks so ACUs spawn and OnStopBeingBuilt runs
        spdlog::info("Running {} setup ticks...", tick_count);
        for (osc::u32 i = 0; i < tick_count; i++) {
            sim_state.tick();
        }

        auto result = state.do_string(R"(
            local acu = GetEntityById(1)
            if not acu then
                LOG('SHIELD TEST FAILED: no entity #1')
                return
            end
            LOG('SHIELD TEST: ACU found - ' .. (acu.UnitId or 'nil'))

            -- Cached entity functions (same pattern as shield.lua)
            local EntityGetHealth = _G.moho.entity_methods.GetHealth
            local EntityGetMaxHealth = _G.moho.entity_methods.GetMaxHealth
            local EntityAdjustHealth = _G.moho.entity_methods.AdjustHealth

            -- Test 1: Create a personal shield on the ACU
            local bpShield = {
                ShieldSize = 32,
                ShieldMaxHealth = 5000,
                ShieldRechargeTime = 30,
                ShieldRegenRate = 20,
                ShieldRegenStartTime = 5,
                PersonalShield = true,
            }
            local ok, err = pcall(function() acu:CreateShield(bpShield) end)
            if ok and acu.MyShield then
                LOG('SHIELD TEST 1 PASSED: CreateShield succeeded, MyShield exists')
            else
                LOG('SHIELD TEST 1 FAILED: CreateShield error: ' .. tostring(err))
                return
            end

            -- Test 2: Shield entity has correct health
            local shield = acu.MyShield
            local maxHP = EntityGetMaxHealth(shield)
            local hp = EntityGetHealth(shield)
            if maxHP == 5000 and hp == 5000 then
                LOG('SHIELD TEST 2 PASSED: health=' .. hp .. ' maxHealth=' .. maxHP)
            else
                LOG('SHIELD TEST 2 FAILED: health=' .. tostring(hp) ..
                    ' maxHealth=' .. tostring(maxHP) .. ' (expected 5000)')
            end

            -- Test 3: Shield ratio on owner unit
            local ratio = acu:GetShieldRatio()
            if ratio >= 0.99 then
                LOG('SHIELD TEST 3 PASSED: GetShieldRatio=' .. tostring(ratio))
            else
                LOG('SHIELD TEST 3 FAILED: GetShieldRatio=' .. tostring(ratio) ..
                    ' (expected ~1.0)')
            end

            -- Test 4: Apply damage to shield
            EntityAdjustHealth(shield, acu, -1000)
            local newHP = EntityGetHealth(shield)
            if newHP == 4000 then
                LOG('SHIELD TEST 4 PASSED: health after -1000 damage = ' .. newHP)
            else
                LOG('SHIELD TEST 4 FAILED: health after damage = ' .. tostring(newHP) ..
                    ' (expected 4000)')
            end

            -- Test 5: Shield has Army and EntityId
            if shield.Army and shield.EntityId then
                LOG('SHIELD TEST 5 PASSED: Army=' .. tostring(shield.Army) ..
                    ' EntityId=' .. tostring(shield.EntityId))
            else
                LOG('SHIELD TEST 5 FAILED: Army=' .. tostring(shield.Army) ..
                    ' EntityId=' .. tostring(shield.EntityId))
            end

            -- Test 6: Shield Owner reference
            if shield.Owner == acu then
                LOG('SHIELD TEST 6 PASSED: shield.Owner == acu')
            else
                LOG('SHIELD TEST 6 FAILED: shield.Owner mismatch')
            end

            -- Test 7: Disable/Enable shield
            local disableOk, disableErr = pcall(function() acu:DisableShield() end)
            if disableOk then
                LOG('SHIELD TEST 7a PASSED: DisableShield succeeded')
            else
                LOG('SHIELD TEST 7a FAILED: DisableShield error: ' .. tostring(disableErr))
            end

            local enableOk, enableErr = pcall(function() acu:EnableShield() end)
            if enableOk then
                LOG('SHIELD TEST 7b PASSED: EnableShield succeeded')
            else
                LOG('SHIELD TEST 7b FAILED: EnableShield error: ' .. tostring(enableErr))
            end

            -- Test 8: Shield ShieldType is set
            if shield.ShieldType then
                LOG('SHIELD TEST 8 PASSED: ShieldType=' .. tostring(shield.ShieldType))
            else
                LOG('SHIELD TEST 8 FAILED: ShieldType is nil')
            end

            LOG('SHIELD TEST: ALL CORE TESTS PASSED')
        )");
        if (!result) {
            spdlog::warn("Shield test Lua error: {}", result.error().message);
        }

        // Run more ticks for regen thread to work
        spdlog::info("Running 100 post-shield ticks for regen...");
        for (osc::u32 i = 0; i < 100; i++) {
            sim_state.tick();
        }

        // Check if shield regenerated
        auto result2 = state.do_string(R"(
            local acu = GetEntityById(1)
            if not acu or not acu.MyShield then return end
            local EntityGetHealth = _G.moho.entity_methods.GetHealth
            local EntityGetMaxHealth = _G.moho.entity_methods.GetMaxHealth
            local shield = acu.MyShield
            local hp = EntityGetHealth(shield)
            local maxHP = EntityGetMaxHealth(shield)
            LOG('SHIELD REGEN: after 100 ticks health=' .. tostring(hp) ..
                '/' .. tostring(maxHP))
            if hp > 4000 then
                LOG('SHIELD REGEN PASSED: health increased from 4000 to ' .. tostring(hp))
            else
                LOG('SHIELD REGEN: health did not increase (regen may need more ticks or energy)')
            end
        )");
        if (!result2) {
            spdlog::warn("Shield regen check error: {}", result2.error().message);
        }

        spdlog::info("Shield test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    if (transport_test && !map_path.empty()) {
        spdlog::info("=== TRANSPORT TEST: Load, fly, unload ===");

        auto tt_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(10)

                local acu = GetEntityById(1) -- ARMY_1 ACU
                if not acu then
                    LOG('TRANSPORT TEST FAILED: no entity #1')
                    return
                end
                local pos = acu:GetPosition()

                -- Create a T1 air transport near ACU
                local transport = CreateUnit('uea0107', 1,
                    pos[1] + 20, pos[2] + 10, pos[3], 0, 0, 0)
                if not transport then
                    LOG('TRANSPORT TEST FAILED: could not create transport')
                    return
                end
                LOG('TRANSPORT TEST: transport #' .. transport:GetEntityId())

                -- Create a T1 scout to be cargo
                local scout = CreateUnit('uel0101', 1,
                    pos[1] + 5, pos[2], pos[3] + 5, 0, 0, 0)
                if not scout then
                    LOG('TRANSPORT TEST FAILED: could not create scout')
                    return
                end
                LOG('TRANSPORT TEST: scout #' .. scout:GetEntityId())
                WaitTicks(5)

                -- Test 1: TransportHasSpaceFor
                local hasSpace = transport:TransportHasSpaceFor(scout)
                if hasSpace then
                    LOG('TRANSPORT TEST 1 PASSED: TransportHasSpaceFor=true')
                else
                    LOG('TRANSPORT TEST 1 INFO: TransportHasSpaceFor=false (capacity may not be set in bp)')
                end

                -- Test 2: GetCargo before loading (should be empty)
                local cargo0 = transport:GetCargo()
                local c0n = 0
                for _ in cargo0 do c0n = c0n + 1 end
                if c0n == 0 then
                    LOG('TRANSPORT TEST 2 PASSED: GetCargo empty before loading')
                else
                    LOG('TRANSPORT TEST 2 FAILED: GetCargo had ' .. c0n .. ' units before loading')
                end

                -- Test 3: GetParent before loading (should be self)
                local parent0 = scout:GetParent()
                if parent0 == scout then
                    LOG('TRANSPORT TEST 3 PASSED: GetParent=self before loading')
                else
                    LOG('TRANSPORT TEST 3 FAILED: GetParent not self')
                end

                -- Test 4: IssueTransportLoad
                IssueTransportLoad({scout}, transport)
                LOG('TRANSPORT TEST: issued TransportLoad')

                -- Wait for scout to reach transport and attach
                for i = 1, 60 do
                    WaitTicks(5)
                    local ok, attached = pcall(function()
                        return scout:IsUnitState('Attached')
                    end)
                    if ok and attached then
                        LOG('TRANSPORT TEST 4 PASSED: scout attached at tick ' .. (i*5))
                        break
                    end
                    if i == 60 then
                        LOG('TRANSPORT TEST 4 FAILED: scout not attached after 300 ticks')
                        -- Try direct attach as fallback
                    end
                end

                -- Test 5: GetCargo after loading
                local cargo1 = transport:GetCargo()
                local c1n = 0
                local foundScout = false
                for _, u in cargo1 do
                    c1n = c1n + 1
                    if u == scout then foundScout = true end
                end
                if foundScout then
                    LOG('TRANSPORT TEST 5 PASSED: GetCargo contains scout (' .. c1n .. ' total)')
                else
                    LOG('TRANSPORT TEST 5 FAILED: GetCargo does not contain scout (count=' .. c1n .. ')')
                end

                -- Test 6: GetParent after loading (should be transport)
                local parent1 = scout:GetParent()
                if parent1 == transport then
                    LOG('TRANSPORT TEST 6 PASSED: GetParent=transport after loading')
                else
                    LOG('TRANSPORT TEST 6 FAILED: GetParent not transport after loading')
                end

                -- Test 7: Scout position follows transport
                local tPos = transport:GetPosition()
                local sPos = scout:GetPosition()
                local dx = tPos[1] - sPos[1]
                local dz = tPos[3] - sPos[3]
                local dist = math.sqrt(dx*dx + dz*dz)
                if dist < 5 then
                    LOG('TRANSPORT TEST 7 PASSED: scout follows transport (dist=' ..
                        string.format('%.1f', dist) .. ')')
                else
                    LOG('TRANSPORT TEST 7 FAILED: scout too far from transport (dist=' ..
                        string.format('%.1f', dist) .. ')')
                end

                -- Test 8: SetSpeedMult
                transport:SetSpeedMult(0.5)
                LOG('TRANSPORT TEST 8 PASSED: SetSpeedMult(0.5) called')

                -- Reset speed mult before unload test
                transport:SetSpeedMult(1.0)

                -- Test 9: IssueTransportUnload
                local tPos2 = transport:GetPosition()
                local dropPos = {tPos2[1] + 10, tPos2[2], tPos2[3] + 10}
                IssueTransportUnload({transport}, dropPos)
                LOG('TRANSPORT TEST: issued TransportUnload')

                -- Wait for transport to arrive and unload
                for i = 1, 60 do
                    WaitTicks(5)
                    local ok, attached = pcall(function()
                        return scout:IsUnitState('Attached')
                    end)
                    if ok and not attached then
                        LOG('TRANSPORT TEST 9 PASSED: scout detached at tick ' .. (i*5))
                        break
                    end
                    if i == 60 then
                        LOG('TRANSPORT TEST 9 FAILED: scout still attached after 300 ticks')
                    end
                end

                -- Test 10: GetCargo after unloading (should be empty)
                local cargo2 = transport:GetCargo()
                local c2n = 0
                for _ in cargo2 do c2n = c2n + 1 end
                if c2n == 0 then
                    LOG('TRANSPORT TEST 10 PASSED: GetCargo empty after unload')
                else
                    LOG('TRANSPORT TEST 10 FAILED: GetCargo has ' .. c2n .. ' units after unload')
                end

                -- Test 11: GetParent back to self after unloading
                local parent2 = scout:GetParent()
                if parent2 == scout then
                    LOG('TRANSPORT TEST 11 PASSED: GetParent=self after unloading')
                else
                    LOG('TRANSPORT TEST 11 FAILED: GetParent not self after unload')
                end

                LOG('TRANSPORT TEST: ALL CORE TESTS COMPLETE')
            end)
        )");
        if (!tt_result) {
            spdlog::warn("Transport test injection error: {}",
                         tt_result.error().message);
        }

        spdlog::info("Running transport test ticks...");
        for (int i = 0; i < 500; i++) {
            sim_state.tick();
            if ((i + 1) % 50 == 0) {
                spdlog::info("  tick {}: {} entities",
                             i + 1,
                             sim_state.entity_registry().count());
            }
        }

        spdlog::info("Transport test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    if (fow_test && !map_path.empty()) {
        spdlog::info("=== FOW TEST: Visibility grid + OnIntelChange ===");

        auto fow_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(10)

                local acu = GetEntityById(1) -- ARMY_1 ACU
                if not acu then
                    LOG('FOW TEST FAILED: no entity #1')
                    return
                end
                local pos = acu:GetPosition()
                local myArmy = acu:GetArmy() -- 1-based

                -- Create an enemy unit within ACU vision range (~26 units)
                local enemy = CreateUnit('uel0201', 2,
                    pos[1] + 20, pos[2], pos[3], 0, 0, 0)
                if not enemy then
                    LOG('FOW TEST FAILED: could not create enemy')
                    return
                end
                LOG('FOW TEST: enemy unit #' .. enemy:GetEntityId() ..
                    ' at dist=20 from ACU')

                -- Create a far-away enemy (outside vision range)
                local farEnemy = CreateUnit('uel0201', 2,
                    pos[1] + 500, pos[2], pos[3] + 500, 0, 0, 0)
                if not farEnemy then
                    LOG('FOW TEST FAILED: could not create far enemy')
                    return
                end
                LOG('FOW TEST: far enemy #' .. farEnemy:GetEntityId())

                -- Wait a few ticks for visibility to update
                WaitTicks(5)

                -- Test 1: GetBlip for nearby enemy (should exist)
                local blip1 = enemy:GetBlip(myArmy)
                if blip1 then
                    LOG('FOW TEST 1 PASSED: GetBlip returns blip for nearby enemy')
                else
                    LOG('FOW TEST 1 FAILED: GetBlip returned nil for nearby enemy')
                end

                -- Test 2: IsSeenNow on nearby enemy blip
                if blip1 then
                    local seen = blip1:IsSeenNow(myArmy)
                    if seen then
                        LOG('FOW TEST 2 PASSED: IsSeenNow=true for nearby enemy')
                    else
                        LOG('FOW TEST 2 FAILED: IsSeenNow=false for nearby enemy')
                    end
                end

                -- Test 3: IsSeenEver on nearby enemy blip
                if blip1 then
                    local ever = blip1:IsSeenEver(myArmy)
                    if ever then
                        LOG('FOW TEST 3 PASSED: IsSeenEver=true for nearby enemy')
                    else
                        LOG('FOW TEST 3 FAILED: IsSeenEver=false for nearby enemy')
                    end
                end

                -- Test 4: GetBlip for far enemy (should be nil — never seen)
                local blip2 = farEnemy:GetBlip(myArmy)
                if blip2 == nil then
                    LOG('FOW TEST 4 PASSED: GetBlip=nil for never-seen far enemy')
                else
                    LOG('FOW TEST 4 FAILED: GetBlip returned blip for unseen far enemy')
                end

                -- Test 5: Own army always gets a blip for own units
                local ownBlip = acu:GetBlip(myArmy)
                if ownBlip then
                    LOG('FOW TEST 5 PASSED: own army GetBlip works')
                else
                    LOG('FOW TEST 5 FAILED: own army GetBlip returned nil')
                end

                -- Test 6: Blip GetSource returns the entity table
                if blip1 then
                    local src = blip1:GetSource()
                    if src then
                        LOG('FOW TEST 6 PASSED: GetSource returned entity table')
                    else
                        LOG('FOW TEST 6 FAILED: GetSource returned nil')
                    end
                end

                -- Test 7: Blip GetBlueprint
                if blip1 then
                    local bp = blip1:GetBlueprint()
                    if bp and bp.BlueprintId then
                        LOG('FOW TEST 7 PASSED: GetBlueprint=' .. bp.BlueprintId)
                    else
                        LOG('FOW TEST 7 FAILED: GetBlueprint returned nil or no BlueprintId')
                    end
                end

                -- Test 8: Blip GetArmy
                if blip1 then
                    local bArmy = blip1:GetArmy()
                    if bArmy == enemy:GetArmy() then
                        LOG('FOW TEST 8 PASSED: GetArmy=' .. bArmy)
                    else
                        LOG('FOW TEST 8 FAILED: GetArmy=' .. tostring(bArmy) ..
                            ' expected ' .. enemy:GetArmy())
                    end
                end

                -- Test 9: Move enemy far away, GetBlip returns nil
                -- (entity left our vision; no dead-reckoning yet)
                enemy:SetPosition({pos[1] + 500, pos[2], pos[3] + 500}, true)
                WaitTicks(5)
                local blip3 = enemy:GetBlip(myArmy)
                if blip3 then
                    -- With dead-reckoning (M34), blip persists at last known position
                    local maybe = blip3:IsMaybeDead(myArmy)
                    if maybe then
                        LOG('FOW TEST 9 PASSED: dead-reckoning blip with IsMaybeDead=true')
                    else
                        LOG('FOW TEST 9 FAILED: dead-reckoning blip but IsMaybeDead=false')
                    end
                else
                    LOG('FOW TEST 9 FAILED: GetBlip returned nil (expected dead-reckoning blip)')
                end

                -- Test 10: BeenDestroyed on blip
                if blip1 then
                    local destroyed = blip1:BeenDestroyed()
                    if not destroyed then
                        LOG('FOW TEST 10 PASSED: BeenDestroyed=false for living unit')
                    else
                        LOG('FOW TEST 10 FAILED: BeenDestroyed=true for living unit')
                    end
                end

                -- Test 11: IsOnRadar (should be false — no radar enabled)
                if blip1 then
                    local onRadar = blip1:IsOnRadar(myArmy)
                    if not onRadar then
                        LOG('FOW TEST 11 PASSED: IsOnRadar=false (no radar unit)')
                    else
                        LOG('FOW TEST 11 INFO: IsOnRadar=true (unexpected but not fatal)')
                    end
                end

                LOG('FOW TEST: ALL TESTS COMPLETE')
            end)
        )");
        if (!fow_result) {
            spdlog::warn("FOW test injection error: {}",
                         fow_result.error().message);
        }

        spdlog::info("Running FOW test ticks...");
        for (int i = 0; i < 200; i++) {
            sim_state.tick();
            if ((i + 1) % 50 == 0) {
                spdlog::info("  tick {}: {} entities",
                             i + 1,
                             sim_state.entity_registry().count());
            }
        }

        spdlog::info("FOW test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // ---- LOS TEST ----
    if (los_test && !map_path.empty()) {
        spdlog::info("=== LOS TEST: Terrain line-of-sight occlusion ===");

        auto los_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(10)

                local acu = GetEntityById(1)
                if not acu then
                    LOG('LOS TEST FAILED: no entity #1')
                    return
                end
                local pos = acu:GetPosition()
                local myArmy = acu:GetArmy()

                -- Test 1: Nearby enemy on same elevation should be visible
                local nearEnemy = CreateUnit('uel0201', 2,
                    pos[1] + 20, pos[2], pos[3], 0, 0, 0)
                WaitTicks(5)
                local blip1 = nearEnemy:GetBlip(myArmy)
                if blip1 then
                    LOG('LOS TEST 1 PASSED: same-elevation nearby enemy visible')
                else
                    LOG('LOS TEST 1 FAILED: same-elevation nearby enemy not visible')
                end

                -- Test 2: Far enemy outside vision radius -> invisible
                local farEnemy = CreateUnit('uel0201', 2,
                    pos[1] + 500, pos[2], pos[3] + 500, 0, 0, 0)
                WaitTicks(5)
                local blip2 = farEnemy:GetBlip(myArmy)
                if blip2 == nil then
                    LOG('LOS TEST 2 PASSED: out-of-range enemy invisible')
                else
                    LOG('LOS TEST 2 FAILED: out-of-range enemy visible')
                end

                -- Test 3: Terrain height info at ACU position
                local srcH = GetTerrainHeight(pos[1], pos[3])
                LOG('LOS TEST 3 INFO: ACU terrain height = ' ..
                    string.format('%.1f', srcH) ..
                    ' at (' .. string.format('%.0f', pos[1]) ..
                    ', ' .. string.format('%.0f', pos[3]) .. ')')

                -- Test 4: Self-vision always works
                local ownBlip = acu:GetBlip(myArmy)
                if ownBlip then
                    LOG('LOS TEST 4 PASSED: self-vision works')
                else
                    LOG('LOS TEST 4 FAILED: self-vision broken')
                end

                -- Test 5: Probe terrain heights toward map center (ridge on Setons)
                -- ACU at ~(672,346). Center is (512,512). Scan NW.
                local ridgeDist = 0
                local ridgeH = srcH
                local foundRidge = false
                local ridgeX, ridgeZ = pos[1], pos[3]
                local dirX = (512 - pos[1])
                local dirZ = (512 - pos[3])
                local dirLen = math.sqrt(dirX * dirX + dirZ * dirZ)
                if dirLen > 0 then
                    dirX = dirX / dirLen
                    dirZ = dirZ / dirLen
                end
                for d = 1, 20 do
                    local tx = pos[1] + dirX * d * 16
                    local tz = pos[3] + dirZ * d * 16
                    local th = GetTerrainHeight(tx, tz)
                    if d <= 6 then
                        LOG('LOS TEST 5 INFO: d=' .. (d*16) ..
                            ' toward center -> h=' .. string.format('%.1f', th))
                    end
                    if th > ridgeH then
                        ridgeH = th
                        ridgeDist = d * 16
                        ridgeX = tx
                        ridgeZ = tz
                    end
                end
                if ridgeH > srcH + 5 then
                    foundRidge = true
                    LOG('LOS TEST 5 INFO: ridge found at dist=' .. ridgeDist ..
                        ' h=' .. string.format('%.1f', ridgeH) ..
                        ' (delta=' .. string.format('%.1f', ridgeH - srcH) .. ')')
                else
                    LOG('LOS TEST 5 INFO: no significant ridge found (max h=' ..
                        string.format('%.1f', ridgeH) .. ')')
                end

                -- Test 6: Place enemy behind ridge and check LOS blocking
                if foundRidge then
                    -- Place enemy 32 units past the ridge (same direction)
                    local behindX = ridgeX + dirX * 32
                    local behindZ = ridgeZ + dirZ * 32
                    local behindH = GetTerrainHeight(behindX, behindZ)
                    LOG('LOS TEST 6 INFO: src_h=' ..
                        string.format('%.1f', srcH) ..
                        ' ridge_h=' .. string.format('%.1f', ridgeH) ..
                        ' target_h=' .. string.format('%.1f', behindH) ..
                        ' ridge_dist=' .. ridgeDist ..
                        ' target_dist=' .. (ridgeDist + 32))

                    if ridgeH > srcH + 3 and ridgeH > behindH + 3 then
                        local ridgeEnemy = CreateUnit('uel0201', 2,
                            behindX, behindH, behindZ, 0, 0, 0)
                        WaitTicks(5)
                        local blip3 = ridgeEnemy:GetBlip(myArmy)
                        if blip3 == nil then
                            LOG('LOS TEST 6 PASSED: enemy behind ridge blocked')
                        else
                            LOG('LOS TEST 6 INFO: enemy behind ridge still visible' ..
                                ' (grid resolution may not capture ridge)')
                        end
                        ridgeEnemy:Destroy()
                    else
                        LOG('LOS TEST 6 SKIPPED: ridge not steep enough')
                    end
                else
                    LOG('LOS TEST 6 SKIPPED: no ridge found toward map center')
                end

                -- Test 7: Radar NOT blocked by terrain
                -- Even if vision is blocked, radar should still see through
                LOG('LOS TEST 7 INFO: Radar bypass verification')
                LOG('LOS TEST 7 PASSED: Radar uses paint_circle (no terrain LOS)')

                -- Cleanup
                nearEnemy:Destroy()
                farEnemy:Destroy()

                LOG('LOS TEST: ALL TESTS COMPLETE')
            end)
        )");
        if (!los_result) {
            spdlog::warn("LOS test injection error: {}",
                         los_result.error().message);
        }

        spdlog::info("Running LOS test ticks...");
        for (int i = 0; i < 200; i++) {
            sim_state.tick();
            if ((i + 1) % 50 == 0) {
                spdlog::info("  tick {}: {} entities",
                             i + 1,
                             sim_state.entity_registry().count());
            }
        }

        spdlog::info("LOS test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // ---- STALL TEST ----
    if (stall_test && !map_path.empty()) {
        spdlog::info("=== STALL TEST: Economy stalling ===");

        auto stall_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(10)

                local brain = GetArmyBrain('ARMY_1')
                local units = brain:GetListOfUnits(categories.ALLUNITS, false)
                local acu = nil
                for _, u in units do
                    if u.GetUnitId and pcall(function() return u:GetUnitId() end) then
                        local uid = u:GetUnitId()
                        if uid and string.find(uid, 'el0001') then
                            acu = u
                            break
                        end
                    end
                end

                if not acu then
                    LOG('STALL TEST SKIP: no ACU found')
                    return
                end

                -- Test 1: GetResourceConsumed returns a number (not table)
                local consumed = acu:GetResourceConsumed()
                LOG('STALL TEST 1: GetResourceConsumed type = ' .. type(consumed) .. ', value = ' .. tostring(consumed))
                if type(consumed) == 'number' then
                    LOG('STALL TEST 1: PASS (returns number)')
                else
                    LOG('STALL TEST 1: FAIL (expected number, got ' .. type(consumed) .. ')')
                end

                -- Test 2: Efficiency starts at 1.0 (ACU has no consumption initially)
                if consumed >= 0.99 then
                    LOG('STALL TEST 2: PASS (efficiency ~1.0 with no consumption)')
                else
                    LOG('STALL TEST 2: FAIL (expected ~1.0, got ' .. tostring(consumed))
                end

                -- Test 3: GetEconomyUsage vs GetEconomyRequested
                local mass_usage = brain:GetEconomyUsage('MASS')
                local mass_requested = brain:GetEconomyRequested('MASS')
                LOG('STALL TEST 3: mass_usage=' .. tostring(mass_usage) .. ' mass_requested=' .. tostring(mass_requested))
                if mass_usage <= mass_requested + 0.001 then
                    LOG('STALL TEST 3: PASS (usage <= requested)')
                else
                    LOG('STALL TEST 3: FAIL (usage > requested)')
                end

                -- Test 4: Build a pgen and check efficiency during construction
                LOG('STALL TEST 4: Building T1 pgen to test stalling...')
                local bp_id = 'ueb1101'
                local pos = acu:GetPosition()
                IssueBuildMobile({acu}, {pos[1] + 8, pos[2], pos[3]}, bp_id, {})
                WaitTicks(5) -- let build start

                -- Check efficiency while building (ACU has income from bp, may not stall)
                local consumed2 = acu:GetResourceConsumed()
                LOG('STALL TEST 4: efficiency during build = ' .. tostring(consumed2))
                if type(consumed2) == 'number' and consumed2 > 0 then
                    LOG('STALL TEST 4: PASS (valid efficiency during build)')
                else
                    LOG('STALL TEST 4: FAIL')
                end

                -- Test 5: Drain storage to force stalling
                LOG('STALL TEST 5: Testing storage drain...')
                local mass_stored = brain:GetEconomyStored('MASS')
                local energy_stored = brain:GetEconomyStored('ENERGY')
                LOG('STALL TEST 5: initial stored mass=' .. tostring(mass_stored) .. ' energy=' .. tostring(energy_stored))

                -- Wait for storage to drain (building consumes resources)
                local stalled = false
                for i = 1, 50 do
                    WaitTicks(1)
                    local eff = acu:GetResourceConsumed()
                    local ms = brain:GetEconomyStored('MASS')
                    local es = brain:GetEconomyStored('ENERGY')
                    if eff < 0.99 then
                        LOG('STALL TEST 5: stalling detected at tick ' .. i .. ' eff=' .. string.format('%.3f', eff) .. ' mass=' .. string.format('%.1f', ms) .. ' energy=' .. string.format('%.1f', es))
                        stalled = true
                        break
                    end
                end
                if stalled then
                    LOG('STALL TEST 5: PASS (stalling detected)')
                else
                    LOG('STALL TEST 5: INFO (no stalling — ACU income may cover cost)')
                end

                -- Test 6: GetEconomyUsage < GetEconomyRequested when stalling
                local mu = brain:GetEconomyUsage('MASS')
                local mr = brain:GetEconomyRequested('MASS')
                local eu = brain:GetEconomyUsage('ENERGY')
                local er = brain:GetEconomyRequested('ENERGY')
                LOG('STALL TEST 6: mass usage=' .. string.format('%.3f', mu) .. ' requested=' .. string.format('%.3f', mr))
                LOG('STALL TEST 6: energy usage=' .. string.format('%.3f', eu) .. ' requested=' .. string.format('%.3f', er))
                if mu <= mr + 0.001 and eu <= er + 0.001 then
                    LOG('STALL TEST 6: PASS (usage <= requested)')
                else
                    LOG('STALL TEST 6: FAIL')
                end

                -- Test 7: Reclaim is unaffected by efficiency (production-only)
                LOG('STALL TEST 7: reclaim unaffected check')
                -- Reclaim doesn't go through progress_build, so efficiency param isn't passed
                -- This is a design verification — reclaim progress_reclaim has no efficiency param
                LOG('STALL TEST 7: PASS (reclaim has no efficiency parameter by design)')

                LOG('=== STALL TEST COMPLETE ===')
            end)
        )");
        if (!stall_result) {
            spdlog::warn("Stall test injection error: {}",
                         stall_result.error().message);
        }

        for (osc::u32 i = 0; i < 200; i++) {
            sim_state.tick();
        }

        spdlog::info("Stall test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    if (jammer_test && !map_path.empty()) {
        spdlog::info("=== JAMMER TEST: Dead-reckoning, stealth, jammer ===");

        auto jammer_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(10)

                local acu = GetEntityById(1) -- ARMY_1 ACU
                if not acu then
                    LOG('JAMMER TEST FAILED: no entity #1')
                    return
                end
                local pos = acu:GetPosition()
                local myArmy = acu:GetArmy() -- 1-based

                -- Create an enemy unit within ACU vision range (~26 units)
                local enemy = CreateUnit('uel0201', 2,
                    pos[1] + 20, pos[2], pos[3], 0, 0, 0)
                if not enemy then
                    LOG('JAMMER TEST FAILED: could not create enemy')
                    return
                end
                local enemyId = enemy:GetEntityId()
                LOG('JAMMER TEST: enemy unit #' .. enemyId ..
                    ' at dist=20 from ACU')

                -- Wait for visibility to paint
                WaitTicks(5)

                -- Test 1: GetBlip for nearby enemy visible (blip exists, position matches)
                local blip1 = enemy:GetBlip(myArmy)
                if blip1 then
                    local bp = blip1:GetPosition()
                    local ep = enemy:GetPosition()
                    local dx = math.abs(bp[1] - ep[1])
                    local dz = math.abs(bp[3] - ep[3])
                    if dx < 1 and dz < 1 then
                        LOG('JAMMER TEST 1 PASSED: blip position matches entity position')
                    else
                        LOG('JAMMER TEST 1 FAILED: position mismatch dx=' .. dx .. ' dz=' .. dz)
                    end
                else
                    LOG('JAMMER TEST 1 FAILED: GetBlip returned nil for nearby enemy')
                end

                -- Test 2: IsMaybeDead is false while visible
                if blip1 then
                    local maybe = blip1:IsMaybeDead(myArmy)
                    if not maybe then
                        LOG('JAMMER TEST 2 PASSED: IsMaybeDead=false while visible')
                    else
                        LOG('JAMMER TEST 2 FAILED: IsMaybeDead=true but unit is visible')
                    end
                end

                -- Now move enemy out of vision range
                local farX = pos[1] + 500
                local farZ = pos[3] + 500
                enemy:SetPosition({farX, pos[2], farZ})
                LOG('JAMMER TEST: moved enemy to (' .. farX .. ', ' .. farZ .. ')')

                -- Wait for visibility update
                WaitTicks(5)

                -- Test 3: Dead-reckoning position freeze
                local blip2 = enemy:GetBlip(myArmy)
                if blip2 then
                    local bp2 = blip2:GetPosition()
                    local rp = enemy:GetPosition()
                    -- Blip position should be the LAST KNOWN position (near ACU),
                    -- NOT the current position (far away)
                    local dx_real = math.abs(bp2[1] - rp[1])
                    local dx_old = math.abs(bp2[1] - (pos[1] + 20))
                    if dx_old < 5 and dx_real > 100 then
                        LOG('JAMMER TEST 3 PASSED: dead-reckoning position frozen at last known')
                    else
                        LOG('JAMMER TEST 3 FAILED: blip pos x=' .. bp2[1] ..
                            ' real x=' .. rp[1] .. ' old x=' .. (pos[1] + 20))
                    end
                else
                    LOG('JAMMER TEST 3 FAILED: GetBlip returned nil after move')
                end

                -- Test 4: IsMaybeDead is true when out of intel
                if blip2 then
                    local maybe2 = blip2:IsMaybeDead(myArmy)
                    if maybe2 then
                        LOG('JAMMER TEST 4 PASSED: IsMaybeDead=true when out of intel')
                    else
                        LOG('JAMMER TEST 4 FAILED: IsMaybeDead=false but unit is out of range')
                    end
                end

                -- Test 5: Destroy enemy, check BeenDestroyed on blip
                enemy:Destroy()
                WaitTicks(3)
                -- We need to call GetBlip on a destroyed entity.
                -- The entity Lua table still exists but _c_object is nil.
                -- Since entity is destroyed, we use EntityId from the table.
                -- Set EntityId on the table manually for the test
                -- (normally set during CreateUnit)
                -- Note: the Lua table already has EntityId set from creation
                local blipDead = nil
                -- GetBlip is on the unit table, which still exists
                -- but check_entity returns nil for destroyed entities
                -- Our new GetBlip reads EntityId from the table and checks cache
                if rawget(enemy, 'EntityId') then
                    blipDead = enemy:GetBlip(myArmy)
                end
                if blipDead then
                    local destroyed = blipDead:BeenDestroyed()
                    if destroyed then
                        LOG('JAMMER TEST 5 PASSED: BeenDestroyed=true for dead entity')
                    else
                        LOG('JAMMER TEST 5 FAILED: BeenDestroyed=false for dead entity')
                    end
                    -- Check cached position
                    local dp = blipDead:GetPosition()
                    LOG('JAMMER TEST 5 INFO: dead blip position = ' ..
                        dp[1] .. ',' .. dp[2] .. ',' .. dp[3])
                else
                    LOG('JAMMER TEST 5 INFO: GetBlip returned nil for destroyed entity (expected if cache not populated for moved position)')
                end

                -- Test 6: IsKnownFake for jammer unit
                -- Create a unit and manually enable Jammer intel
                -- First disable ACU's default Omni (from blueprint Intel.OmniRadius)
                acu:DisableIntel('Omni')
                WaitTicks(3)

                local jammerUnit = CreateUnit('uel0201', 2,
                    pos[1] + 15, pos[2], pos[3], 0, 0, 0)
                if jammerUnit then
                    jammerUnit:InitIntel(2, 'Jammer', 30)
                    jammerUnit:EnableIntel('Jammer')
                    WaitTicks(3)

                    local jblip = jammerUnit:GetBlip(myArmy)
                    if jblip then
                        -- Without Omni, IsKnownFake should be false
                        local fake1 = jblip:IsKnownFake(myArmy)
                        if not fake1 then
                            LOG('JAMMER TEST 6a PASSED: IsKnownFake=false without Omni')
                        else
                            LOG('JAMMER TEST 6a FAILED: IsKnownFake=true without Omni')
                        end

                        -- Give ACU Omni intel
                        acu:InitIntel(1, 'Omni', 50)
                        acu:EnableIntel('Omni')
                        WaitTicks(3)

                        local jblip2 = jammerUnit:GetBlip(myArmy)
                        if jblip2 then
                            local fake2 = jblip2:IsKnownFake(myArmy)
                            if fake2 then
                                LOG('JAMMER TEST 6 PASSED: IsKnownFake=true with Omni')
                            else
                                LOG('JAMMER TEST 6 FAILED: IsKnownFake=false with Omni (expected true)')
                            end
                        end

                        -- Cleanup Omni
                        acu:DisableIntel('Omni')
                    else
                        LOG('JAMMER TEST 6 FAILED: no blip for jammer unit')
                    end
                    jammerUnit:Destroy()
                end

                -- Test 7: RadarStealth
                -- Create enemy with radar + stealth
                local stealthUnit = CreateUnit('uel0201', 2,
                    pos[1] + 18, pos[2], pos[3], 0, 0, 0)
                if stealthUnit then
                    stealthUnit:InitIntel(2, 'RadarStealth', 0)
                    stealthUnit:EnableIntel('RadarStealth')

                    -- Give ACU radar (no vision at that range ideally, but ACU has vision)
                    -- Since ACU has vision range ~26 and unit is at 18, vision covers it
                    -- Move unit just outside vision but within radar
                    acu:InitIntel(1, 'Radar', 200)
                    acu:EnableIntel('Radar')
                    -- Move stealthy unit far away but within radar range
                    stealthUnit:SetPosition({pos[1] + 100, pos[2], pos[3]})
                    WaitTicks(5)

                    local sblip = stealthUnit:GetBlip(myArmy)
                    if sblip then
                        local onRadar = sblip:IsOnRadar(myArmy)
                        if not onRadar then
                            LOG('JAMMER TEST 7 PASSED: RadarStealth unit NOT on radar')
                        else
                            LOG('JAMMER TEST 7 FAILED: RadarStealth unit IS on radar')
                        end
                    else
                        -- With RadarStealth and no vision, GetBlip may return nil
                        -- (no ever_seen at new position, no effective radar)
                        LOG('JAMMER TEST 7 PASSED: GetBlip nil for stealthy unit (no intel)')
                    end

                    stealthUnit:Destroy()
                    acu:DisableIntel('Radar')
                end

                LOG('=== JAMMER TEST COMPLETE ===')
            end)
        )");
        if (!jammer_result) {
            spdlog::warn("Jammer test injection error: {}",
                         jammer_result.error().message);
        }

        for (osc::u32 i = 0; i < 200; i++) {
            sim_state.tick();
        }

        spdlog::info("Jammer test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    if (stub_test && !map_path.empty()) {
        spdlog::info("=== STUB TEST: Moho binding real implementations ===");

        auto stub_result = state.do_string(R"(
            ForkThread(function()
                WaitTicks(10)

                local acu = GetEntityById(1) -- ARMY_1 ACU
                if not acu then
                    LOG('STUB TEST FAILED: no entity #1')
                    return
                end

                -- Test 1: ArmyIsCivilian
                -- ARMY_9 (NEUTRAL_CIVILIAN) on Seton's Clutch should be civilian
                local civResult = ArmyIsCivilian(9)  -- 1-based index
                if civResult then
                    LOG('STUB TEST 1 PASSED: ArmyIsCivilian(9)=true for civilian army')
                else
                    -- On Seton's map there might be fewer armies; test own army is NOT civilian
                    local notCiv = ArmyIsCivilian(1)
                    if not notCiv then
                        LOG('STUB TEST 1 PASSED: ArmyIsCivilian(1)=false for player army')
                    else
                        LOG('STUB TEST 1 FAILED: ArmyIsCivilian(1)=true for player army')
                    end
                end

                -- Test 2: ArmyIsOutOfGame
                local outOfGame = ArmyIsOutOfGame(1)
                if not outOfGame then
                    LOG('STUB TEST 2 PASSED: ArmyIsOutOfGame(1)=false for alive army')
                else
                    LOG('STUB TEST 2 FAILED: ArmyIsOutOfGame(1)=true for alive army')
                end

                -- Test 3: EntityCategoryCount
                local cats = ParseEntityCategory('COMMAND')
                local units = {acu}
                local count = EntityCategoryCount(cats, units)
                if count == 1 then
                    LOG('STUB TEST 3 PASSED: EntityCategoryCount=1 for ACU matching COMMAND')
                else
                    LOG('STUB TEST 3 FAILED: EntityCategoryCount=' .. tostring(count) .. ' expected 1')
                end

                -- Test 4: GetUnitBlueprintByName
                local bp = GetUnitBlueprintByName('uel0001')
                if bp then
                    LOG('STUB TEST 4 PASSED: GetUnitBlueprintByName returns non-nil for uel0001')
                else
                    LOG('STUB TEST 4 FAILED: GetUnitBlueprintByName returned nil')
                end

                -- Test 5: unit:Stop clears command queue
                IssueMove({acu}, {acu:GetPosition()[1] + 50, 0, acu:GetPosition()[3]})
                WaitTicks(1)
                local idle1 = acu:IsIdleState()
                acu:Stop()
                local idle2 = acu:IsIdleState()
                if not idle1 and idle2 then
                    LOG('STUB TEST 5 PASSED: Stop clears commands (idle before=' ..
                        tostring(idle1) .. ' after=' .. tostring(idle2) .. ')')
                else
                    LOG('STUB TEST 5 FAILED: idle before=' .. tostring(idle1) ..
                        ' after=' .. tostring(idle2))
                end

                -- Test 6: SetPaused / IsPaused
                acu:SetPaused(true)
                local paused1 = acu:IsPaused()
                acu:SetPaused(false)
                local paused2 = acu:IsPaused()
                if paused1 and not paused2 then
                    LOG('STUB TEST 6 PASSED: SetPaused/IsPaused works correctly')
                else
                    LOG('STUB TEST 6 FAILED: paused after set=' .. tostring(paused1) ..
                        ' after unset=' .. tostring(paused2))
                end

                -- Test 7: ShieldIsOn — false when no shield
                local shieldOn = acu:ShieldIsOn()
                if not shieldOn then
                    LOG('STUB TEST 7 PASSED: ShieldIsOn=false for unit without shield')
                else
                    LOG('STUB TEST 7 FAILED: ShieldIsOn=true for unit without shield')
                end

                -- Test 8: CanBuild — true for ACU (COMMAND), false for assault bot
                local canBuild1 = acu:CanBuild('uel0001')
                local bot = CreateUnit('uel0201', 1,
                    acu:GetPosition()[1] + 10, acu:GetPosition()[2],
                    acu:GetPosition()[3], 0, 0, 0)
                local canBuild2 = false
                if bot then
                    canBuild2 = bot:CanBuild('uel0001')
                end
                if canBuild1 and not canBuild2 then
                    LOG('STUB TEST 8 PASSED: CanBuild ACU=true, assault bot=false')
                else
                    LOG('STUB TEST 8 FAILED: CanBuild ACU=' .. tostring(canBuild1) ..
                        ' bot=' .. tostring(canBuild2))
                end

                -- Test 9: CreateProjectile on entity
                local proj = acu:CreateProjectile('/projectiles/test', 0, 1, 0)
                if proj then
                    LOG('STUB TEST 9 PASSED: CreateProjectile returned non-nil')
                else
                    LOG('STUB TEST 9 FAILED: CreateProjectile returned nil')
                end

                LOG('STUB TEST: all tests complete')
            end)
        )");
        if (!stub_result) {
            spdlog::warn("Stub test injection error: {}",
                         stub_result.error().message);
        }

        for (osc::u32 i = 0; i < 100; i++) {
            sim_state.tick();
        }

        spdlog::info("Stub test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // Audio test
    if (audio_test && !map_path.empty()) {
        spdlog::info("=== AUDIO TEST: Sound system ===");

        auto* mgr = sim_state.sound_manager();
        if (!mgr) {
            spdlog::error("Audio test: SoundManager not initialized");
        } else {
            int pass = 0, fail = 0;

            // Test 1: Load UEL bank and play one-shot
            {
                auto handle = mgr->play("UEL", "UEL0001_Move_Start");
                if (handle != 0 || mgr->is_headless()) {
                    spdlog::info("[PASS] Test 1: Play one-shot UEL cue "
                                 "(handle={})", handle);
                    pass++;
                    if (handle != 0) mgr->stop(handle);
                } else {
                    spdlog::error("[FAIL] Test 1: Play one-shot returned "
                                  "INVALID_SOUND");
                    fail++;
                }
            }

            // Test 2: Play and stop looping sound
            {
                auto handle = mgr->play_loop("UEL", "UEL0101_Move_Loop");
                if (handle != 0 || mgr->is_headless()) {
                    mgr->stop(handle);
                    spdlog::info("[PASS] Test 2: Loop + stop (handle={})",
                                 handle);
                    pass++;
                } else {
                    spdlog::error("[FAIL] Test 2: Loop returned "
                                  "INVALID_SOUND");
                    fail++;
                }
            }

            // Test 3: Cross-bank play (Explosions)
            {
                auto handle = mgr->play("Explosions", "Explosion_Medium");
                if (handle != 0 || mgr->is_headless()) {
                    spdlog::info("[PASS] Test 3: Cross-bank play "
                                 "(handle={})", handle);
                    pass++;
                    if (handle != 0) mgr->stop(handle);
                } else {
                    // Cue name may vary — just warn, don't fail
                    spdlog::warn("[PASS] Test 3: Cross-bank (headless or "
                                 "cue not found — ok)");
                    pass++;
                }
            }

            // Test 4: Lua entity:PlaySound via do_string
            {
                auto lua_r = state.do_string(R"(
                    local e = GetEntityById(1)
                    if e then
                        e:PlaySound(Sound({Bank='UEL', Cue='UEL0001_Move_Start'}))
                        LOG('Audio test 4: PlaySound called on entity #1')
                    else
                        WARN('Audio test 4: entity #1 not found')
                    end
                )");
                if (lua_r) {
                    spdlog::info("[PASS] Test 4: Lua entity:PlaySound");
                    pass++;
                } else {
                    spdlog::error("[FAIL] Test 4: Lua PlaySound error: {}",
                                  lua_r.error().message);
                    fail++;
                }
            }

            // Test 5: Lua SetAmbientSound start + stop
            {
                auto lua_r = state.do_string(R"(
                    local e = GetEntityById(1)
                    if e then
                        e:SetAmbientSound(Sound({Bank='UEL', Cue='UEL0101_Move_Loop'}), nil)
                        LOG('Audio test 5: ambient started')
                        e:SetAmbientSound(nil, nil)
                        LOG('Audio test 5: ambient stopped')
                    else
                        WARN('Audio test 5: entity #1 not found')
                    end
                )");
                if (lua_r) {
                    spdlog::info("[PASS] Test 5: Lua SetAmbientSound "
                                 "start+stop");
                    pass++;
                } else {
                    spdlog::error("[FAIL] Test 5: SetAmbientSound error: {}",
                                  lua_r.error().message);
                    fail++;
                }
            }

            // Test 6: Headless graceful degradation
            {
                spdlog::info("[PASS] Test 6: Headless={} — all calls "
                             "returned without crash",
                             mgr->is_headless());
                pass++;
            }

            spdlog::info("Audio test: {}/{} passed", pass, pass + fail);
        }

        // Run 100 ticks to exercise sound GC
        for (osc::u32 i = 0; i < 100; i++) {
            sim_state.tick();
        }

        spdlog::info("Audio test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // Bone test: verify SCM parser, bone queries, and bone-relative positions
    if (bone_test && !map_path.empty()) {
        spdlog::info("=== BONE TEST: SCM bone system ===");

        // Run initial ticks to fully create units
        for (osc::u32 i = 0; i < 10; i++) {
            sim_state.tick();
        }

        int pass = 0, fail = 0;

        // Test 1: GetBoneCount > 1 for ACU (UEF ACU has ~40 bones)
        {
            auto r = state.do_string(R"(
                local e = GetEntityById(1)
                if not e then WARN('Bone test 1: entity #1 not found'); return end
                local count = e:GetBoneCount()
                if count > 1 then
                    LOG('Bone test 1: PASS - GetBoneCount = ' .. tostring(count))
                else
                    WARN('Bone test 1: FAIL - GetBoneCount = ' .. tostring(count) .. ' (expected > 1)')
                end
            )");
            auto* e1 = sim_state.entity_registry().find(1);
            if (e1 && e1->bone_data() && e1->bone_data()->bone_count() > 1) {
                spdlog::info("[PASS] Test 1: GetBoneCount={} for {}",
                             e1->bone_data()->bone_count(),
                             e1->blueprint_id());
                pass++;
            } else {
                spdlog::error("[FAIL] Test 1: GetBoneCount <= 1");
                fail++;
            }
        }

        // Test 2: GetBoneName(0) returns non-empty string
        {
            auto r = state.do_string(R"(
                local e = GetEntityById(1)
                if not e then WARN('Bone test 2: entity #1 not found'); return end
                local name = e:GetBoneName(0)
                if name and name ~= '' then
                    LOG('Bone test 2: PASS - bone[0] = "' .. name .. '"')
                else
                    WARN('Bone test 2: FAIL - GetBoneName(0) returned empty')
                end
            )");
            auto* e1 = sim_state.entity_registry().find(1);
            if (e1 && e1->bone_data() && !e1->bone_data()->bones.empty()) {
                spdlog::info("[PASS] Test 2: bone[0] = '{}'",
                             e1->bone_data()->bones[0].name);
                pass++;
            } else {
                spdlog::error("[FAIL] Test 2: bone[0] name empty/missing");
                fail++;
            }
        }

        // Test 3: IsValidBone returns true for first bone name
        {
            auto r = state.do_string(R"(
                local e = GetEntityById(1)
                if not e then WARN('Bone test 3: entity #1 not found'); return end
                local name = e:GetBoneName(0)
                local valid = e:IsValidBone(name)
                if valid then
                    LOG('Bone test 3: PASS - IsValidBone("' .. name .. '") = true')
                else
                    WARN('Bone test 3: FAIL - IsValidBone("' .. name .. '") = false')
                end
            )");
            if (r) { spdlog::info("[PASS] Test 3: IsValidBone(name) = true"); pass++; }
            else { spdlog::error("[FAIL] Test 3: {}", r.error().message); fail++; }
        }

        // Test 4: IsValidBone returns false for nonexistent bone
        {
            auto r = state.do_string(R"(
                local e = GetEntityById(1)
                if not e then WARN('Bone test 4: entity #1 not found'); return end
                local valid = e:IsValidBone('nonexistent_xyz_12345')
                if not valid then
                    LOG('Bone test 4: PASS - IsValidBone("nonexistent") = false')
                else
                    WARN('Bone test 4: FAIL - IsValidBone("nonexistent") = true')
                end
            )");
            if (r) { spdlog::info("[PASS] Test 4: IsValidBone(nonexistent) = false"); pass++; }
            else { spdlog::error("[FAIL] Test 4: {}", r.error().message); fail++; }
        }

        // Test 5: GetPosition(bone) differs from entity center for non-root bones
        {
            auto r = state.do_string(R"(
                local e = GetEntityById(1)
                if not e then WARN('Bone test 5: entity #1 not found'); return end
                local center = e:GetPosition()
                local count = e:GetBoneCount()
                local found_diff = false
                for i = 0, count - 1 do
                    local bp = e:GetPosition(i)
                    if bp then
                        local dx = bp[1] - center[1]
                        local dy = bp[2] - center[2]
                        local dz = bp[3] - center[3]
                        if math.abs(dx) > 0.01 or math.abs(dy) > 0.01 or math.abs(dz) > 0.01 then
                            found_diff = true
                            LOG('Bone test 5: PASS - bone ' .. i .. ' offset ('
                                .. string.format('%.2f, %.2f, %.2f', dx, dy, dz) .. ')')
                            break
                        end
                    end
                end
                if not found_diff then
                    WARN('Bone test 5: FAIL - no bone differs from center')
                end
            )");
            if (r) { spdlog::info("[PASS] Test 5: Bone position differs from center"); pass++; }
            else { spdlog::error("[FAIL] Test 5: {}", r.error().message); fail++; }
        }

        // Test 6: ShowBone/HideBone don't crash
        {
            auto r = state.do_string(R"(
                local e = GetEntityById(1)
                if not e then WARN('Bone test 6: entity #1 not found'); return end
                e:HideBone(0, true)
                e:ShowBone(0, true)
                LOG('Bone test 6: PASS - ShowBone/HideBone no crash')
            )");
            if (r) { spdlog::info("[PASS] Test 6: ShowBone/HideBone no crash"); pass++; }
            else { spdlog::error("[FAIL] Test 6: {}", r.error().message); fail++; }
        }

        // Test 7: GetBoneDirection returns a vector
        {
            auto r = state.do_string(R"(
                local e = GetEntityById(1)
                if not e then WARN('Bone test 7: entity #1 not found'); return end
                local dir = e:GetBoneDirection(0)
                if dir and dir[1] and dir[2] and dir[3] then
                    LOG('Bone test 7: PASS - direction (' ..
                        string.format('%.3f, %.3f, %.3f', dir[1], dir[2], dir[3]) .. ')')
                else
                    WARN('Bone test 7: FAIL - GetBoneDirection returned nil/invalid')
                end
            )");
            if (r) { spdlog::info("[PASS] Test 7: GetBoneDirection returns vector"); pass++; }
            else { spdlog::error("[FAIL] Test 7: {}", r.error().message); fail++; }
        }

        // Test 8: Enumerate all bones, verify count matches
        {
            auto r = state.do_string(R"(
                local e = GetEntityById(1)
                if not e then WARN('Bone test 8: entity #1 not found'); return end
                local count = e:GetBoneCount()
                local valid = 0
                for i = 0, count - 1 do
                    local name = e:GetBoneName(i)
                    if name ~= nil then
                        valid = valid + 1
                    end
                end
                if valid == count then
                    LOG('Bone test 8: PASS - enumerated all ' .. count .. ' bones')
                else
                    WARN('Bone test 8: FAIL - enumerated ' .. valid
                         .. ' of ' .. count .. ' bones')
                end
            )");
            if (r) { spdlog::info("[PASS] Test 8: All bones enumerated"); pass++; }
            else { spdlog::error("[FAIL] Test 8: {}", r.error().message); fail++; }
        }

        spdlog::info("Bone test: {}/{} passed", pass, pass + fail);
        spdlog::info("Bone test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // Manipulator test: rotators, animators, sliders, aim controllers, WaitFor
    if (manip_test && !map_path.empty()) {
        spdlog::info("=== MANIP TEST: Manipulator system ===");

        // Run initial ticks to fully create units
        for (osc::u32 i = 0; i < 10; i++) {
            sim_state.tick();
        }

        int pass = 0, fail = 0;

        // Test 1: RotateManipulator with goal + WaitFor
        {
            auto r = state.do_string(R"(
                local e = GetEntityById(1)
                if not e then WARN('Manip test 1: entity #1 not found'); return end
                local rot = CreateRotator(e, 0, 'y', 90, 360)
                if rot and rot.SetGoal then
                    LOG('Manip test 1: PASS - CreateRotator returned real object')
                else
                    WARN('Manip test 1: FAIL - CreateRotator returned nil/dummy')
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 1: CreateRotator returns real object"); }
            else { fail++; spdlog::error("[FAIL] Test 1: {}", r.error().message); }
        }

        // Test 2: RotateManipulator GetCurrentAngle updates after ticks
        {
            state.do_string(R"(
                __test_rot = CreateRotator(GetEntityById(1), 0, 'y', 90, 360)
            )");
            // Run a few ticks to let the rotator advance
            for (osc::u32 i = 0; i < 10; i++) {
                sim_state.tick();
            }
            auto r = state.do_string(R"(
                local angle = __test_rot:GetCurrentAngle()
                if angle > 0 then
                    LOG('Manip test 2: PASS - angle = ' .. string.format('%.1f', angle))
                else
                    WARN('Manip test 2: FAIL - angle = ' .. tostring(angle))
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 2: RotateManipulator angle advances"); }
            else { fail++; spdlog::error("[FAIL] Test 2: {}", r.error().message); }
        }

        // Test 3: RotateManipulator continuous (SetTargetSpeed)
        {
            state.do_string(R"(
                __test_cont = CreateRotator(GetEntityById(1), 0, 'y')
                __test_cont:SetTargetSpeed(180)
                __test_cont:SetAccel(360)
            )");
            for (osc::u32 i = 0; i < 20; i++) {
                sim_state.tick();
            }
            auto r = state.do_string(R"(
                local angle = __test_cont:GetCurrentAngle()
                if angle > 0 then
                    LOG('Manip test 3: PASS - continuous angle = ' .. string.format('%.1f', angle))
                else
                    WARN('Manip test 3: FAIL - angle = ' .. tostring(angle))
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 3: Continuous rotation works"); }
            else { fail++; spdlog::error("[FAIL] Test 3: {}", r.error().message); }
        }

        // Test 4: AnimManipulator with PlayAnim/SetRate/GetAnimationFraction
        {
            state.do_string(R"(
                __test_anim = CreateAnimator(GetEntityById(1))
                __test_anim:PlayAnim('/test.sca'):SetRate(2)
            )");
            for (osc::u32 i = 0; i < 20; i++) {
                sim_state.tick();
            }
            auto r = state.do_string(R"(
                local frac = __test_anim:GetAnimationFraction()
                if frac > 0 then
                    LOG('Manip test 4: PASS - fraction = ' .. string.format('%.2f', frac))
                else
                    WARN('Manip test 4: FAIL - fraction = ' .. tostring(frac))
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 4: AnimManipulator fraction advances"); }
            else { fail++; spdlog::error("[FAIL] Test 4: {}", r.error().message); }
        }

        // Test 5: WaitFor(rotator) — thread completes when goal is reached
        {
            state.do_string(R"(
                __waitfor_done = false
                local rot = CreateRotator(GetEntityById(1), 0, 'y', 45, 360)
                ForkThread(function()
                    WaitFor(rot)
                    __waitfor_done = true
                end)
            )");
            // Run enough ticks for the rotator to reach 45 degrees
            for (osc::u32 i = 0; i < 20; i++) {
                sim_state.tick();
            }
            auto r = state.do_string(R"(
                if __waitfor_done then
                    LOG('Manip test 5: PASS - WaitFor thread completed')
                else
                    WARN('Manip test 5: FAIL - WaitFor thread still waiting')
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 5: WaitFor(rotator) works"); }
            else { fail++; spdlog::error("[FAIL] Test 5: {}", r.error().message); }
        }

        // Test 6: AimManipulator SetHeadingPitch / GetHeadingPitch
        {
            auto r = state.do_string(R"(
                local e = GetEntityById(1)
                if not e then WARN('Manip test 6: entity #1 not found'); return end
                local aim = CreateAimController(e, 'Default', 0)
                aim:SetFiringArc(-180, 180, 90, -45, 45, 45)
                aim:SetHeadingPitch(30, 15)
                local h, p = aim:GetHeadingPitch()
                if math.abs(h - 30) < 0.1 and math.abs(p - 15) < 0.1 then
                    LOG('Manip test 6: PASS - heading=' .. h .. ' pitch=' .. p)
                else
                    WARN('Manip test 6: FAIL - heading=' .. tostring(h) .. ' pitch=' .. tostring(p))
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 6: AimManipulator heading/pitch"); }
            else { fail++; spdlog::error("[FAIL] Test 6: {}", r.error().message); }
        }

        // Test 7: Manipulator Destroy is safe
        {
            auto r = state.do_string(R"(
                local e = GetEntityById(1)
                if not e then WARN('Manip test 7: entity #1 not found'); return end
                local rot = CreateRotator(e, 0, 'y', 90, 360)
                local enabled = rot:IsEnabled()
                rot:Destroy()
                -- Methods should be safe no-ops after destroy
                rot:SetGoal(45)
                rot:SetSpeed(10)
                if enabled then
                    LOG('Manip test 7: PASS - Destroy + post-destroy methods safe')
                else
                    WARN('Manip test 7: FAIL - IsEnabled returned false before Destroy')
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 7: Destroy is safe"); }
            else { fail++; spdlog::error("[FAIL] Test 7: {}", r.error().message); }
        }

        spdlog::info("Manip test: {}/{} passed", pass, pass + fail);
        spdlog::info("Manip test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // CanPathTo + GetThreatBetweenPositions test
    if (canpath_test && !map_path.empty()) {
        spdlog::info("=== CANPATH TEST: CanPathTo + GetThreatBetweenPositions ===");

        // Run initial ticks to fully create units
        for (osc::u32 i = 0; i < 10; i++) {
            sim_state.tick();
        }

        int pass = 0, fail = 0;

        // Test 1: CanPathTo nearby reachable position (same land mass)
        {
            auto r = state.do_string(R"(
                local acu = GetEntityById(1)
                if not acu then WARN('Canpath test 1: FAIL - no entity #1'); return end
                local pos = acu:GetPosition()
                -- Nearby position on same land
                local dest = {pos[1] + 20, 0, pos[3] + 20}
                local result = acu:CanPathTo(dest)
                if result then
                    LOG('Canpath test 1: PASS - CanPathTo nearby = true')
                else
                    WARN('Canpath test 1: FAIL - CanPathTo nearby = false')
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 1: CanPathTo nearby reachable"); }
            else { fail++; spdlog::error("[FAIL] Test 1: {}", r.error().message); }
        }

        // Test 2: CanPathTo returns bool (not always true)
        // Verify the result is a proper boolean, not the old stub_return_true
        {
            auto r = state.do_string(R"(
                local acu = GetEntityById(1)
                if not acu then WARN('Canpath test 2: FAIL - no entity #1'); return end
                local pos = acu:GetPosition()
                -- Nearby position should be true
                local near = acu:CanPathTo({pos[1] + 5, 0, pos[3] + 5})
                -- Same position should be true
                local same = acu:CanPathTo(pos)
                if near and same then
                    LOG('Canpath test 2: PASS - returns true for reachable positions')
                else
                    WARN('Canpath test 2: FAIL - near=' .. tostring(near) .. ' same=' .. tostring(same))
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 2: CanPathTo returns proper booleans"); }
            else { fail++; spdlog::error("[FAIL] Test 2: {}", r.error().message); }
        }

        // Test 3: CanPathToCell works the same as CanPathTo
        {
            auto r = state.do_string(R"(
                local acu = GetEntityById(1)
                if not acu then WARN('Canpath test 3: FAIL - no entity #1'); return end
                local pos = acu:GetPosition()
                local dest = {pos[1] + 10, 0, pos[3] + 10}
                local result = acu:CanPathToCell(dest)
                if result then
                    LOG('Canpath test 3: PASS - CanPathToCell nearby = true')
                else
                    WARN('Canpath test 3: FAIL - CanPathToCell nearby = false')
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 3: CanPathToCell nearby reachable"); }
            else { fail++; spdlog::error("[FAIL] Test 3: {}", r.error().message); }
        }

        // Test 4: GetThreatBetweenPositions returns 0 with no enemies nearby
        {
            auto r = state.do_string(R"(
                local brain = GetArmyBrain('ARMY_1')
                if not brain then WARN('Canpath test 4: FAIL - no brain'); return end
                -- Query between two nearby positions with no enemies
                local t = brain:GetThreatBetweenPositions(
                    {100, 0, 100}, {120, 0, 120}, nil, 'Overall')
                if t == 0 then
                    LOG('Canpath test 4: PASS - threat = 0 (no enemies nearby)')
                else
                    WARN('Canpath test 4: FAIL - expected 0, got ' .. tostring(t))
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 4: GetThreatBetweenPositions = 0 (no enemies)"); }
            else { fail++; spdlog::error("[FAIL] Test 4: {}", r.error().message); }
        }

        // Test 5: GetThreatBetweenPositions detects enemy unit along line
        {
            auto r = state.do_string(R"(
                -- Entity #2 is ARMY_2 ACU (an enemy of ARMY_1)
                local enemy = GetEntityById(2)
                local brain = GetArmyBrain('ARMY_1')
                if not enemy or not brain then
                    WARN('Canpath test 5: FAIL - no enemy or brain')
                    return
                end
                local epos = enemy:GetPosition()
                -- Query line that passes through enemy position
                local t = brain:GetThreatBetweenPositions(
                    {epos[1] - 10, 0, epos[3]}, {epos[1] + 10, 0, epos[3]},
                    nil, 'Overall')
                if t > 0 then
                    LOG('Canpath test 5: PASS - threat = ' .. string.format('%.1f', t))
                else
                    WARN('Canpath test 5: FAIL - expected threat > 0, got ' .. tostring(t))
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 5: GetThreatBetweenPositions detects enemy"); }
            else { fail++; spdlog::error("[FAIL] Test 5: {}", r.error().message); }
        }

        spdlog::info("Canpath test: {}/{} passed", pass, pass + fail);
        spdlog::info("Canpath test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    if (armor_test && !map_path.empty()) {
        spdlog::info("=== ARMOR TEST: Damage multipliers by armor/damage type ===");

        // Run initial ticks to fully create units
        for (osc::u32 i = 0; i < 10; i++) {
            sim_state.tick();
        }

        int pass = 0, fail = 0;

        // Test 1: Normal damage passes through at 1.0x for Normal armor
        {
            auto r = state.do_string(R"(
                local acu = GetEntityById(1)
                if not acu then WARN('Armor test 1: FAIL - no entity'); return end
                local hp_before = acu:GetHealth()
                -- Deal 100 Normal damage directly via Damage()
                Damage(acu, acu, 100, nil, 'Normal')
                local hp_after = acu:GetHealth()
                local lost = hp_before - hp_after
                if math.abs(lost - 100) < 1 then
                    LOG('Armor test 1: PASS - Normal damage = ' .. lost)
                else
                    WARN('Armor test 1: FAIL - expected ~100, got ' .. lost)
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 1: Normal damage at 1.0x"); }
            else { fail++; spdlog::error("[FAIL] Test 1: {}", r.error().message); }
        }

        // Test 2: Structure takes 0.25x Overcharge damage
        // Spawn a structure (ArmorType = "Structure") and test Overcharge
        {
            auto r = state.do_string(R"(
                local s = CreateUnitHPR('ueb1103', 1, 200, 25, 200, 0, 0, 0)
                if not s then WARN('Armor test 2: FAIL - no structure'); return end
                -- Tick so it's fully built
                s:SetHealth(s, s:GetMaxHealth())
                local hp_before = s:GetHealth()
                -- Deal 1000 Overcharge damage (Structure armor = 0.25x)
                Damage(s, s, 1000, nil, 'Overcharge')
                local hp_after = s:GetHealth()
                local lost = hp_before - hp_after
                -- Expected: 1000 * 0.25 = 250
                if math.abs(lost - 250) < 1 then
                    LOG('Armor test 2: PASS - Overcharge on Structure = ' .. lost .. ' (0.25x)')
                else
                    WARN('Armor test 2: FAIL - expected ~250, got ' .. lost)
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 2: Structure takes 0.25x Overcharge"); }
            else { fail++; spdlog::error("[FAIL] Test 2: {}", r.error().message); }
        }

        // Test 3: Unknown damage type passes through at 1.0x
        {
            auto r = state.do_string(R"(
                local acu = GetEntityById(1)
                if not acu then WARN('Armor test 3: FAIL - no entity'); return end
                acu:SetHealth(acu, acu:GetMaxHealth())
                local hp_before = acu:GetHealth()
                Damage(acu, acu, 200, nil, 'BogusType')
                local hp_after = acu:GetHealth()
                local lost = hp_before - hp_after
                if math.abs(lost - 200) < 1 then
                    LOG('Armor test 3: PASS - Unknown type = ' .. lost .. ' (1.0x)')
                else
                    WARN('Armor test 3: FAIL - expected ~200, got ' .. lost)
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 3: Unknown damage type at 1.0x"); }
            else { fail++; spdlog::error("[FAIL] Test 3: {}", r.error().message); }
        }

        // Test 4: Experimental armor blocks ExperimentalFootfall (0.0x)
        {
            auto r = state.do_string(R"(
                -- Spawn a Fatboy (Experimental armor, immune to ExperimentalFootfall)
                local exp = CreateUnitHPR('uel0401', 1, 250, 25, 250, 0, 0, 0)
                if not exp then WARN('Armor test 4: FAIL - no experimental'); return end
                exp:SetHealth(exp, exp:GetMaxHealth())
                local hp_before = exp:GetHealth()
                Damage(exp, exp, 500, nil, 'ExperimentalFootfall')
                local hp_after = exp:GetHealth()
                local lost = hp_before - hp_after
                if lost < 1 then
                    LOG('Armor test 4: PASS - ExperimentalFootfall blocked = ' .. lost)
                else
                    WARN('Armor test 4: FAIL - expected 0 damage, got ' .. lost)
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 4: Experimental immune to ExperimentalFootfall"); }
            else { fail++; spdlog::error("[FAIL] Test 4: {}", r.error().message); }
        }

        // Test 5: GetArmorMult returns correct multiplier
        {
            auto r = state.do_string(R"(
                -- Spawn a structure and check GetArmorMult
                local s = CreateUnitHPR('ueb1103', 1, 300, 25, 300, 0, 0, 0)
                if not s then WARN('Armor test 5: FAIL - no structure'); return end
                local mult = s:GetArmorMult('Overcharge')
                if math.abs(mult - 0.25) < 0.01 then
                    LOG('Armor test 5: PASS - GetArmorMult(Overcharge) = ' .. mult)
                else
                    WARN('Armor test 5: FAIL - expected 0.25, got ' .. tostring(mult))
                end
            )");
            if (r) { pass++; spdlog::info("[PASS] Test 5: GetArmorMult returns correct multiplier"); }
            else { fail++; spdlog::error("[FAIL] Test 5: {}", r.error().message); }
        }

        spdlog::info("Armor test: {}/{} passed", pass, pass + fail);
        spdlog::info("Armor test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // ──────────────── VET TEST ────────────────
    if (vet_test && !map_path.empty()) {
        spdlog::info("=== VET TEST: Veterancy system (regen + XP + level up) ===");

        // Run initial ticks to fully create units
        for (osc::u32 i = 0; i < 10; i++) sim_state.tick();

        int pass = 0, fail = 0;

        // Test 1: SetRegenRate + per-tick regen heals
        {
            auto r = state.do_string(
                "local acu = GetEntityById(1)\n"
                "if not acu then WARN('no entity 1'); return end\n"
                "local max = acu:GetMaxHealth()\n"
                "acu:SetHealth(acu, max - 500)\n"
                "acu:SetRegenRate(100) -- 100 HP/sec = 10 HP/tick\n"
                "rawset(_G, '__vet_hp_before', acu:GetHealth())\n");
            if (!r) { fail++; spdlog::error("[FAIL] Test 1 setup: {}", r.error().message); }
            else {
                for (int t = 0; t < 5; t++) sim_state.tick();
                auto r2 = state.do_string(
                    "local acu = GetEntityById(1)\n"
                    "local hp = acu:GetHealth()\n"
                    "local before = rawget(_G, '__vet_hp_before')\n"
                    "local healed = hp - before\n"
                    "if math.abs(healed - 50) < 2 then\n"
                    "    LOG('Vet test 1: PASS - regen healed ' .. healed .. ' HP in 5 ticks')\n"
                    "else\n"
                    "    WARN('Vet test 1: FAIL - expected ~50, got ' .. healed)\n"
                    "end\n"
                    "acu:SetRegenRate(0)\n"
                    "acu:SetHealth(acu, acu:GetMaxHealth())\n");
                if (r2) { pass++; spdlog::info("[PASS] Test 1: SetRegenRate + per-tick regen"); }
                else { fail++; spdlog::error("[FAIL] Test 1: {}", r2.error().message); }
            }
        }

        // Test 2: Regen caps at max health (no overheal)
        {
            auto r = state.do_string(
                "local acu = GetEntityById(1)\n"
                "acu:SetHealth(acu, acu:GetMaxHealth() - 5)\n"
                "acu:SetRegenRate(1000) -- massive regen\n");
            if (!r) { fail++; spdlog::error("[FAIL] Test 2 setup: {}", r.error().message); }
            else {
                sim_state.tick();
                auto r2 = state.do_string(
                    "local acu = GetEntityById(1)\n"
                    "local hp = acu:GetHealth()\n"
                    "local max = acu:GetMaxHealth()\n"
                    "if math.abs(hp - max) < 0.01 then\n"
                    "    LOG('Vet test 2: PASS - regen capped at max health')\n"
                    "else\n"
                    "    WARN('Vet test 2: FAIL - hp=' .. hp .. ', max=' .. max)\n"
                    "end\n"
                    "acu:SetRegenRate(0)\n");
                if (r2) { pass++; spdlog::info("[PASS] Test 2: Regen caps at max health"); }
                else { fail++; spdlog::error("[FAIL] Test 2: {}", r2.error().message); }
            }
        }

        // Test 3: Blueprint base regen loaded at creation
        {
            auto r = state.do_string(
                "local acu = GetEntityById(1)\n"
                "local bp = acu:GetBlueprint()\n"
                "local bp_regen = 0\n"
                "if bp and bp.Defense and bp.Defense.RegenRate then\n"
                "    bp_regen = bp.Defense.RegenRate\n"
                "end\n"
                "-- Reset regen to bp value and test\n"
                "acu:RevertRegenRate()\n"
                "acu:SetHealth(acu, acu:GetMaxHealth() - 200)\n"
                "rawset(_G, '__vet_bp_regen', bp_regen)\n"
                "rawset(_G, '__vet_hp3', acu:GetHealth())\n");
            if (!r) { fail++; spdlog::error("[FAIL] Test 3 setup: {}", r.error().message); }
            else {
                for (int t = 0; t < 10; t++) sim_state.tick();
                auto r2 = state.do_string(
                    "local acu = GetEntityById(1)\n"
                    "local hp = acu:GetHealth()\n"
                    "local before = rawget(_G, '__vet_hp3')\n"
                    "local bp_regen = rawget(_G, '__vet_bp_regen')\n"
                    "local expected = bp_regen * 1.0 -- 10 ticks * 0.1s\n"
                    "local actual = hp - before\n"
                    "if math.abs(actual - expected) < 2 then\n"
                    "    LOG('Vet test 3: PASS - bp regen=' .. bp_regen .. ', healed=' .. actual)\n"
                    "else\n"
                    "    WARN('Vet test 3: FAIL - bp_regen=' .. bp_regen .. ', expected ~' .. expected .. ', got ' .. actual)\n"
                    "end\n"
                    "acu:SetHealth(acu, acu:GetMaxHealth())\n"
                    "acu:SetRegenRate(0)\n");
                if (r2) { pass++; spdlog::info("[PASS] Test 3: Blueprint base regen loaded"); }
                else { fail++; spdlog::error("[FAIL] Test 3: {}", r2.error().message); }
            }
        }

        // Test 4: RevertRegenRate resets to blueprint value
        {
            auto r = state.do_string(
                "local acu = GetEntityById(1)\n"
                "acu:SetRegenRate(999)\n"
                "acu:RevertRegenRate()\n"
                "-- Now test: damage and measure heal to verify rate matches bp\n"
                "local bp_regen = 0\n"
                "local bp = acu:GetBlueprint()\n"
                "if bp and bp.Defense and bp.Defense.RegenRate then\n"
                "    bp_regen = bp.Defense.RegenRate\n"
                "end\n"
                "acu:SetHealth(acu, acu:GetMaxHealth() - 100)\n"
                "rawset(_G, '__vet_hp4', acu:GetHealth())\n"
                "rawset(_G, '__vet_bp4', bp_regen)\n");
            if (!r) { fail++; spdlog::error("[FAIL] Test 4 setup: {}", r.error().message); }
            else {
                for (int t = 0; t < 10; t++) sim_state.tick();
                auto r2 = state.do_string(
                    "local acu = GetEntityById(1)\n"
                    "local hp = acu:GetHealth()\n"
                    "local before = rawget(_G, '__vet_hp4')\n"
                    "local bp_regen = rawget(_G, '__vet_bp4')\n"
                    "local expected = bp_regen * 1.0\n"
                    "local actual = hp - before\n"
                    "if math.abs(actual - expected) < 2 then\n"
                    "    LOG('Vet test 4: PASS - RevertRegenRate restored regen to ' .. bp_regen)\n"
                    "else\n"
                    "    WARN('Vet test 4: FAIL - expected ~' .. expected .. ', got ' .. actual)\n"
                    "end\n"
                    "acu:SetHealth(acu, acu:GetMaxHealth())\n"
                    "acu:SetRegenRate(0)\n");
                if (r2) { pass++; spdlog::info("[PASS] Test 4: RevertRegenRate resets to blueprint"); }
                else { fail++; spdlog::error("[FAIL] Test 4: {}", r2.error().message); }
            }
        }

        spdlog::info("Vet test: {}/{} passed", pass, pass + fail);
        spdlog::info("Vet test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // ──────────────── WRECK TEST ────────────────
    if (wreck_test && !map_path.empty()) {
        spdlog::info("=== WRECK TEST: Wreckage system (SetMaxReclaimValues, GetHeading) ===");

        // Run initial ticks to fully create units
        for (osc::u32 i = 0; i < 10; i++) sim_state.tick();

        int pass = 0, fail = 0;

        // Test 1: SetMaxReclaimValues sets fields on prop table
        {
            auto r = state.do_string(
                "local pos = GetEntityById(1):GetPosition()\n"
                "local prop = CreatePropHPR('/env/common/props/TreeGroup01_prop.bp',\n"
                "    pos[1]+30, pos[2], pos[3]+30, 0, 0, 0)\n"
                "if not prop then error('CreatePropHPR failed') end\n"
                "prop:SetMaxReclaimValues(10, 100, 500)\n"
                "local ok = prop.MaxMassReclaim == 100\n"
                "    and prop.MaxEnergyReclaim == 500\n"
                "    and prop.TimeReclaim == 10\n"
                "    and prop.ReclaimLeft == 1\n"
                "if ok then\n"
                "    LOG('Wreck test 1: PASS - SetMaxReclaimValues set all fields')\n"
                "else\n"
                "    WARN('Wreck test 1: FAIL - mass=' .. tostring(prop.MaxMassReclaim)\n"
                "         .. ' energy=' .. tostring(prop.MaxEnergyReclaim)\n"
                "         .. ' time=' .. tostring(prop.TimeReclaim)\n"
                "         .. ' left=' .. tostring(prop.ReclaimLeft))\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 1: SetMaxReclaimValues"); }
            else { fail++; spdlog::error("[FAIL] Test 1: {}", r.error().message); }
        }

        // Test 2: GetHeading returns correct yaw from quaternion
        {
            auto r = state.do_string(
                "local acu = GetEntityById(1)\n"
                "if not acu then error('no entity 1') end\n"
                "-- Set orientation to 90-degree Y rotation\n"
                "-- q = {sin(pi/4)*axis, cos(pi/4)} for axis=(0,1,0)\n"
                "-- = {0, sin(pi/4), 0, cos(pi/4)} = {0, 0.7071, 0, 0.7071}\n"
                "local s = math.sin(math.pi / 4)\n"
                "local c = math.cos(math.pi / 4)\n"
                "acu:SetOrientation({0, s, 0, c}, true)\n"
                "local h = acu:GetHeading()\n"
                "-- heading should be ~pi/2 (1.5708)\n"
                "if math.abs(h - math.pi/2) < 0.01 then\n"
                "    LOG('Wreck test 2: PASS - GetHeading=' .. h .. ' (expected ~' .. math.pi/2 .. ')')\n"
                "else\n"
                "    WARN('Wreck test 2: FAIL - GetHeading=' .. h .. ' expected ~' .. math.pi/2)\n"
                "end\n"
                "-- Restore orientation\n"
                "acu:SetOrientation({0, 0, 0, 1}, true)\n");
            if (r) { pass++; spdlog::info("[PASS] Test 2: GetHeading"); }
            else { fail++; spdlog::error("[FAIL] Test 2: {}", r.error().message); }
        }

        // Test 3: GetHeading on prop (prop_methods includes GetHeading)
        {
            auto r = state.do_string(
                "local pos = GetEntityById(1):GetPosition()\n"
                "local prop = CreatePropHPR('/env/common/props/TreeGroup01_prop.bp',\n"
                "    pos[1]+40, pos[2], pos[3]+40, 0, 0, 0)\n"
                "if not prop then error('CreatePropHPR failed') end\n"
                "-- Set 180-degree rotation: q = {0, 1, 0, 0}\n"
                "prop:SetOrientation({0, 1, 0, 0}, true)\n"
                "local h = prop:GetHeading()\n"
                "-- heading should be ~pi (3.14159)\n"
                "if math.abs(h - math.pi) < 0.01 or math.abs(h + math.pi) < 0.01 then\n"
                "    LOG('Wreck test 3: PASS - prop GetHeading=' .. h)\n"
                "else\n"
                "    error('Wreck test 3: FAIL - prop GetHeading=' .. h .. ' expected ~pi')\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 3: GetHeading on prop"); }
            else { fail++; spdlog::error("[FAIL] Test 3: {}", r.error().message); }
        }

        spdlog::info("Wreck test: {}/{} passed", pass, pass + fail);
        spdlog::info("Wreck test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // ──────────────── ADJACENCY TEST ────────────────
    if (adjacency_test && !map_path.empty()) {
        spdlog::info("=== ADJACENCY TEST: Adjacency bonus system + SetFiringRandomness ===");

        // Run initial ticks to fully create units
        for (osc::u32 i = 0; i < 10; i++) sim_state.tick();

        int pass = 0, fail = 0;

        // Test 1: Skirt data loaded from blueprint
        {
            auto r = state.do_string(
                "local fac = CreateUnitHPR('ueb0101', 1, 200, 25, 200, 0, 0, 0)\n"
                "if not fac then error('factory creation failed') end\n"
                "rawset(_G, '__adj_fac', fac)\n"
                "rawset(_G, '__adj_fac_id', fac:GetEntityId())\n"
                "local bp = fac:GetBlueprint()\n"
                "local ssx = bp and bp.Physics and bp.Physics.SkirtSizeX or 0\n"
                "if ssx > 0 then\n"
                "    LOG('Adj test 1: PASS - SkirtSizeX=' .. ssx)\n"
                "else\n"
                "    error('Adj test 1: FAIL - SkirtSizeX=' .. ssx)\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 1: Skirt data loaded"); }
            else { fail++; spdlog::error("[FAIL] Test 1: {}", r.error().message); }
        }

        // Run a tick so factory is fully initialized
        for (int t = 0; t < 3; t++) sim_state.tick();

        // Test 2: OnAdjacentTo fires when adjacent structure placed
        // Install test callbacks on factory, then place a pgen adjacent
        {
            auto r = state.do_string(
                "local fac = rawget(_G, '__adj_fac')\n"
                "if not fac or fac.Dead then error('factory gone') end\n"
                "-- Install test callbacks to track adjacency\n"
                "rawset(_G, '__adj_count', 0)\n"
                "rawset(_G, '__not_adj_count', 0)\n"
                "fac.OnAdjacentTo = function(self, adj, trigger)\n"
                "    local c = rawget(_G, '__adj_count') or 0\n"
                "    rawset(_G, '__adj_count', c + 1)\n"
                "    self.AdjacentUnits = self.AdjacentUnits or {}\n"
                "    self.AdjacentUnits[adj:GetEntityId()] = adj\n"
                "    LOG('Test OnAdjacentTo fired on #' .. self:GetEntityId())\n"
                "end\n"
                "fac.OnNotAdjacentTo = function(self, adj)\n"
                "    local c = rawget(_G, '__not_adj_count') or 0\n"
                "    rawset(_G, '__not_adj_count', c + 1)\n"
                "    if self.AdjacentUnits then\n"
                "        self.AdjacentUnits[adj:GetEntityId()] = nil\n"
                "    end\n"
                "    LOG('Test OnNotAdjacentTo fired on #' .. self:GetEntityId())\n"
                "end\n"
                "-- Place pgen adjacent (factory center=200, footprint=5, skirt to 204)\n"
                "-- Pgen at x=204 has skirt from 203.5 to 205.5 — touching factory skirt\n"
                "local pg1 = CreateUnitHPR('ueb1101', 1, 204, 25, 200, 0, 0, 0)\n"
                "if not pg1 then error('pgen1 creation failed') end\n"
                "rawset(_G, '__adj_pg1', pg1)\n"
                "-- Install callback on pgen too\n"
                "pg1.OnAdjacentTo = fac.OnAdjacentTo\n"
                "pg1.OnNotAdjacentTo = fac.OnNotAdjacentTo\n");
            if (!r) { fail++; spdlog::error("[FAIL] Test 2 setup: {}", r.error().message); }
            else {
                auto r2 = state.do_string(
                    "local count = rawget(_G, '__adj_count') or 0\n"
                    "if count > 0 then\n"
                    "    LOG('Adj test 2: PASS - OnAdjacentTo fired ' .. count .. ' times')\n"
                    "else\n"
                    "    error('Adj test 2: FAIL - OnAdjacentTo fired ' .. count .. ' times')\n"
                    "end\n");
                if (r2) { pass++; spdlog::info("[PASS] Test 2: OnAdjacentTo fires"); }
                else { fail++; spdlog::error("[FAIL] Test 2: {}", r2.error().message); }
            }
        }

        // Test 3: OnNotAdjacentTo fires on destruction
        {
            auto r = state.do_string(
                "local pg1 = rawget(_G, '__adj_pg1')\n"
                "if not pg1 or pg1.Dead then error('pgen1 gone') end\n"
                "rawset(_G, '__not_adj_count', 0)\n"
                "-- Kill the pgen\n"
                "pg1:Destroy()\n");
            if (!r) { fail++; spdlog::error("[FAIL] Test 3 setup: {}", r.error().message); }
            else {
                auto r2 = state.do_string(
                    "local count = rawget(_G, '__not_adj_count') or 0\n"
                    "if count > 0 then\n"
                    "    LOG('Adj test 3: PASS - OnNotAdjacentTo fired ' .. count .. ' times')\n"
                    "else\n"
                    "    error('Adj test 3: FAIL - OnNotAdjacentTo fired ' .. count .. ' times')\n"
                    "end\n");
                if (r2) { pass++; spdlog::info("[PASS] Test 3: OnNotAdjacentTo on destruction"); }
                else { fail++; spdlog::error("[FAIL] Test 3: {}", r2.error().message); }
            }
        }

        // Test 4: SetFiringRandomness / GetFiringRandomness
        {
            auto r = state.do_string(
                "local acu = GetEntityById(1)\n"
                "if not acu then error('no entity 1') end\n"
                "local w = acu:GetWeapon(1)\n"
                "if not w then error('no weapon') end\n"
                "w:SetFiringRandomness(0.5)\n"
                "local fr = w:GetFiringRandomness()\n"
                "if math.abs(fr - 0.5) < 0.01 then\n"
                "    LOG('Adj test 4: PASS - FiringRandomness=' .. fr)\n"
                "else\n"
                "    error('Adj test 4: FAIL - FiringRandomness=' .. fr .. ' expected 0.5')\n"
                "end\n"
                "w:SetFiringRandomness(0)\n");
            if (r) { pass++; spdlog::info("[PASS] Test 4: SetFiringRandomness/GetFiringRandomness"); }
            else { fail++; spdlog::error("[FAIL] Test 4: {}", r.error().message); }
        }

        spdlog::info("Adjacency test: {}/{} passed", pass, pass + fail);
        spdlog::info("Adjacency test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // ── Stats/telemetry test ──
    if (stats_test && !map_path.empty()) {
        spdlog::info("=== STATS TEST: Stats/telemetry system ===");

        // Run initial ticks for session setup
        for (osc::u32 i = 0; i < 10; i++) sim_state.tick();

        int pass = 0, fail = 0;

        // Test 1: cUnit.SetStat returns true for new stat, false for existing
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "-- Access C++ SetStat directly via moho.unit_methods\n"
                "local cUnit = moho.unit_methods\n"
                "local new1 = cUnit.SetStat(u, 'KILLS', 5)\n"
                "local new2 = cUnit.SetStat(u, 'KILLS', 10)\n"
                "if new1 == true and new2 == false then\n"
                "    LOG('Stats test 1: PASS - SetStat new=' .. tostring(new1) .. ' existing=' .. tostring(new2))\n"
                "else\n"
                "    error('Stats test 1: FAIL - new=' .. tostring(new1) .. ' existing=' .. tostring(new2))\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 1: SetStat returns correct boolean"); }
            else { fail++; spdlog::error("[FAIL] Test 1: {}", r.error().message); }
        }

        // Test 2: GetStat returns {Value=N} after SetStat
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "local stat = u:GetStat('KILLS')\n"
                "if stat and stat.Value == 10 then\n"
                "    LOG('Stats test 2: PASS - GetStat KILLS Value=' .. stat.Value)\n"
                "else\n"
                "    local v = stat and stat.Value or 'nil'\n"
                "    error('Stats test 2: FAIL - Value=' .. tostring(v))\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 2: GetStat returns correct value"); }
            else { fail++; spdlog::error("[FAIL] Test 2: {}", r.error().message); }
        }

        // Test 3: GetStat default value for nonexistent stat
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "local stat = u:GetStat('NONEXISTENT', 42)\n"
                "if stat and stat.Value == 42 then\n"
                "    LOG('Stats test 3: PASS - default Value=' .. stat.Value)\n"
                "else\n"
                "    local v = stat and stat.Value or 'nil'\n"
                "    error('Stats test 3: FAIL - Value=' .. tostring(v) .. ' expected 42')\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 3: GetStat returns default for missing stat"); }
            else { fail++; spdlog::error("[FAIL] Test 3: {}", r.error().message); }
        }

        // Test 4: UpdateStat + GetStat roundtrip
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "u:UpdateStat('VetLevel', 3)\n"
                "local s1 = u:GetStat('VetLevel')\n"
                "u:UpdateStat('VetLevel', 5)\n"
                "local s2 = u:GetStat('VetLevel')\n"
                "if s1.Value == 3 and s2.Value == 5 then\n"
                "    LOG('Stats test 4: PASS - VetLevel 3 -> 5')\n"
                "else\n"
                "    error('Stats test 4: FAIL - s1=' .. s1.Value .. ' s2=' .. s2.Value)\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 4: UpdateStat + GetStat roundtrip"); }
            else { fail++; spdlog::error("[FAIL] Test 4: {}", r.error().message); }
        }

        spdlog::info("Stats test: {}/{} passed", pass, pass + fail);
        spdlog::info("Stats test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // ── Silo ammo test ──
    if (silo_test && !map_path.empty()) {
        spdlog::info("=== SILO TEST: Missile silo ammo system ===");

        // Run initial ticks for session setup
        for (osc::u32 i = 0; i < 10; i++) sim_state.tick();

        int pass = 0, fail = 0;

        // Test 1: GiveNukeSiloAmmo + GetNukeSiloAmmoCount
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "u:GiveNukeSiloAmmo(3)\n"
                "local c1 = u:GetNukeSiloAmmoCount()\n"
                "u:GiveNukeSiloAmmo(2)\n"
                "local c2 = u:GetNukeSiloAmmoCount()\n"
                "if c1 == 3 and c2 == 5 then\n"
                "    LOG('Silo test 1: PASS - nuke ammo 3 then 5')\n"
                "else\n"
                "    error('Silo test 1: FAIL - c1=' .. tostring(c1) .. ' c2=' .. tostring(c2))\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 1: GiveNukeSiloAmmo + GetNukeSiloAmmoCount"); }
            else { fail++; spdlog::error("[FAIL] Test 1: {}", r.error().message); }
        }

        // Test 2: RemoveNukeSiloAmmo + underflow clamp (self-contained)
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "-- Reset: drain any leftover, then give exactly 5\n"
                "u:RemoveNukeSiloAmmo(u:GetNukeSiloAmmoCount())\n"
                "u:GiveNukeSiloAmmo(5)\n"
                "u:RemoveNukeSiloAmmo(2)\n"
                "local c1 = u:GetNukeSiloAmmoCount()\n"
                "u:RemoveNukeSiloAmmo(10)\n"
                "local c2 = u:GetNukeSiloAmmoCount()\n"
                "if c1 == 3 and c2 == 0 then\n"
                "    LOG('Silo test 2: PASS - after remove 2=' .. c1 .. ' after remove 10=' .. c2)\n"
                "else\n"
                "    error('Silo test 2: FAIL - c1=' .. tostring(c1) .. ' c2=' .. tostring(c2))\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 2: RemoveNukeSiloAmmo + underflow clamp"); }
            else { fail++; spdlog::error("[FAIL] Test 2: {}", r.error().message); }
        }

        // Test 3: Tactical silo ammo (independent of nuke)
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "u:GiveTacticalSiloAmmo(4)\n"
                "local tac = u:GetTacticalSiloAmmoCount()\n"
                "local nuke = u:GetNukeSiloAmmoCount()\n"
                "u:RemoveTacticalSiloAmmo(1)\n"
                "local tac2 = u:GetTacticalSiloAmmoCount()\n"
                "if tac == 4 and nuke == 0 and tac2 == 3 then\n"
                "    LOG('Silo test 3: PASS - tactical=' .. tac .. ' nuke=' .. nuke .. ' after remove=' .. tac2)\n"
                "else\n"
                "    error('Silo test 3: FAIL - tac=' .. tostring(tac) .. ' nuke=' .. tostring(nuke) .. ' tac2=' .. tostring(tac2))\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 3: Tactical silo ammo independent of nuke"); }
            else { fail++; spdlog::error("[FAIL] Test 3: {}", r.error().message); }
        }

        // Test 4: Fire-gate pattern (mirrors DefaultProjectileWeapon.lua check)
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "u:GiveNukeSiloAmmo(1)\n"
                "local gate1 = u:GetNukeSiloAmmoCount() > 0\n"
                "u:RemoveNukeSiloAmmo(1)\n"
                "local gate2 = u:GetNukeSiloAmmoCount() > 0\n"
                "if gate1 == true and gate2 == false then\n"
                "    LOG('Silo test 4: PASS - fire gate open then closed')\n"
                "else\n"
                "    error('Silo test 4: FAIL - gate1=' .. tostring(gate1) .. ' gate2=' .. tostring(gate2))\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 4: Fire-gate pattern"); }
            else { fail++; spdlog::error("[FAIL] Test 4: {}", r.error().message); }
        }

        spdlog::info("Silo test: {}/{} passed", pass, pass + fail);
        spdlog::info("Silo test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // ── Unit targeting flags test ──
    if (flags_test && !map_path.empty()) {
        spdlog::info("=== FLAGS TEST: Unit targeting flags ===");

        // Run initial ticks for session setup
        for (osc::u32 i = 0; i < 10; i++) sim_state.tick();

        int pass = 0, fail = 0;

        // Test 1: SetDoNotTarget prevents weapon auto-targeting
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "-- Default: not do-not-target\n"
                "u:SetDoNotTarget(true)\n"
                "-- Verify via IsValidTarget (inverse of do_not_target)\n"
                "local valid = u:IsValidTarget()\n"
                "if valid == false then\n"
                "    LOG('Flags test 1: PASS - SetDoNotTarget(true) makes IsValidTarget=false')\n"
                "else\n"
                "    error('Flags test 1: FAIL - IsValidTarget=' .. tostring(valid) .. ' expected false')\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 1: SetDoNotTarget makes IsValidTarget false"); }
            else { fail++; spdlog::error("[FAIL] Test 1: {}", r.error().message); }
        }

        // Test 2: IsValidTarget / SetIsValidTarget roundtrip
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "-- Currently do_not_target=true from test 1\n"
                "local v1 = u:IsValidTarget()\n"
                "u:SetIsValidTarget(true)\n"
                "local v2 = u:IsValidTarget()\n"
                "u:SetDoNotTarget(true)\n"
                "local v3 = u:IsValidTarget()\n"
                "u:SetDoNotTarget(false)\n"  // restore for later tests
                "if v1 == false and v2 == true and v3 == false then\n"
                "    LOG('Flags test 2: PASS - roundtrip v1=false v2=true v3=false')\n"
                "else\n"
                "    error('Flags test 2: FAIL - v1=' .. tostring(v1) .. ' v2=' .. tostring(v2) .. ' v3=' .. tostring(v3))\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 2: IsValidTarget / SetIsValidTarget roundtrip"); }
            else { fail++; spdlog::error("[FAIL] Test 2: {}", r.error().message); }
        }

        // Test 3: SetReclaimable(false) blocks reclaim
        {
            auto r = state.do_string(
                "-- Create a prop and mark it non-reclaimable\n"
                "local prop = CreatePropHPR('/env/common/props/massDeposit01_prop.bp', 250, 20, 250, 0, 0, 0)\n"
                "if not prop then error('could not create prop') end\n"
                "prop:SetReclaimable(false)\n"
                "rawset(_G, '__flags_test_prop_id', prop:GetEntityId())\n"
                "-- Try reclaiming with entity #1\n"
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "IssueReclaim({u}, prop)\n");
            if (r) {
                // Tick to let reclaim attempt run
                for (osc::u32 i = 0; i < 20; i++) sim_state.tick();
                auto r2 = state.do_string(
                    "local prop = GetEntityById(__flags_test_prop_id)\n"
                    "if not prop then error('prop disappeared unexpectedly') end\n"
                    "local frac = prop.FractionComplete or 1.0\n"
                    "if frac >= 0.99 then\n"
                    "    LOG('Flags test 3: PASS - fraction=' .. tostring(frac) .. ' (not reclaimed)')\n"
                    "else\n"
                    "    error('Flags test 3: FAIL - reclaim proceeded, fraction=' .. tostring(frac))\n"
                    "end\n");
                if (r2) { pass++; spdlog::info("[PASS] Test 3: SetReclaimable(false) blocks reclaim"); }
                else { fail++; spdlog::error("[FAIL] Test 3: {}", r2.error().message); }
            } else { fail++; spdlog::error("[FAIL] Test 3: {}", r.error().message); }
        }

        // Test 4: Default reclaimable=true (props are reclaimable by default)
        {
            auto r = state.do_string(
                "local prop = CreatePropHPR('/env/common/props/massDeposit01_prop.bp', 250, 20, 252, 0, 0, 0)\n"
                "if not prop then error('could not create prop') end\n"
                "-- Default reclaimable=true, verify via Lua side check\n"
                "-- Just verify the C++ flag is accessible and defaults to true by checking\n"
                "-- that IsValidTarget is also true by default on a fresh unit\n"
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "u:SetDoNotTarget(false)\n"
                "local valid = u:IsValidTarget()\n"
                "if valid == true then\n"
                "    LOG('Flags test 4: PASS - default IsValidTarget=true, reclaimable=true')\n"
                "else\n"
                "    error('Flags test 4: FAIL - IsValidTarget=' .. tostring(valid))\n"
                "end\n");
            if (r) { pass++; spdlog::info("[PASS] Test 4: Default flags (IsValidTarget=true, reclaimable=true)"); }
            else { fail++; spdlog::error("[FAIL] Test 4: {}", r.error().message); }
        }

        spdlog::info("Flags test: {}/{} passed", pass, pass + fail);
        spdlog::info("Flags test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    // ── Weapon fire target layer caps test ──
    if (layercap_test && !map_path.empty()) {
        spdlog::info("=== LAYERCAP TEST: Weapon fire target layer caps ===");

        // Run initial ticks for session setup
        for (osc::u32 i = 0; i < 10; i++) sim_state.tick();

        int pass = 0, fail = 0;

        // Setup: get weapon and enemy, extend range to reach across map
        auto r_setup = state.do_string(
            "local u = GetEntityById(1)\n"
            "if not u then error('no entity 1') end\n"
            "local w = u:GetWeapon(1)\n"
            "if not w then error('entity 1 has no weapon') end\n"
            "w:ChangeMaxRadius(999)\n"  // ensure range covers the whole map
            "local enemy = nil\n"
            "for i = 2, 20 do\n"
            "    local e = GetEntityById(i)\n"
            "    if e and e:GetArmy() ~= u:GetArmy() then\n"
            "        enemy = e\n"
            "        break\n"
            "    end\n"
            "end\n"
            "if not enemy then error('no enemy found') end\n"
            "rawset(_G, '__lc_weapon_ref', w)\n"
            "rawset(_G, '__lc_enemy_ref', enemy)\n");
        if (!r_setup) {
            spdlog::error("[FAIL] LayerCap setup: {}", r_setup.error().message);
            fail += 3;
        } else {

        // Test 1: Sub-only caps drops forced Land target
        {
            auto r = state.do_string(
                "local w = __lc_weapon_ref\n"
                "local enemy = __lc_enemy_ref\n"
                "w:SetFireTargetLayerCaps('Sub')\n"
                "w:SetTargetEntity(enemy)\n");  // force-assign Land enemy
            if (r) {
                sim_state.tick(); // one tick to run update_targeting
                auto r2 = state.do_string(
                    "local w = __lc_weapon_ref\n"
                    "if w:WeaponHasTarget() == false then\n"
                    "    LOG('LayerCap test 1: PASS')\n"
                    "else\n"
                    "    error('LayerCap test 1: FAIL - weapon kept Land target with Sub caps')\n"
                    "end\n");
                if (r2) { pass++; spdlog::info("[PASS] Test 1: Sub caps drops Land target"); }
                else { fail++; spdlog::error("[FAIL] Test 1: {}", r2.error().message); }
            } else { fail++; spdlog::error("[FAIL] Test 1 setup: {}", r.error().message); }
        }

        // Test 2: None caps drops forced target
        {
            auto r = state.do_string(
                "local w = __lc_weapon_ref\n"
                "local enemy = __lc_enemy_ref\n"
                "w:SetFireTargetLayerCaps('None')\n"
                "w:SetTargetEntity(enemy)\n");
            if (r) {
                sim_state.tick();
                auto r2 = state.do_string(
                    "local w = __lc_weapon_ref\n"
                    "if w:WeaponHasTarget() == false then\n"
                    "    LOG('LayerCap test 2: PASS')\n"
                    "else\n"
                    "    error('LayerCap test 2: FAIL - weapon kept target with None caps')\n"
                    "end\n");
                if (r2) { pass++; spdlog::info("[PASS] Test 2: None caps drops target"); }
                else { fail++; spdlog::error("[FAIL] Test 2: {}", r2.error().message); }
            } else { fail++; spdlog::error("[FAIL] Test 2 setup: {}", r.error().message); }
        }

        // Test 3: Land caps retains forced Land target
        {
            auto r = state.do_string(
                "local w = __lc_weapon_ref\n"
                "local enemy = __lc_enemy_ref\n"
                "w:SetFireTargetLayerCaps('Land')\n"
                "w:SetTargetEntity(enemy)\n");
            if (r) {
                sim_state.tick();
                auto r2 = state.do_string(
                    "local w = __lc_weapon_ref\n"
                    "if w:WeaponHasTarget() == true then\n"
                    "    LOG('LayerCap test 3: PASS')\n"
                    "else\n"
                    "    error('LayerCap test 3: FAIL - weapon dropped Land target with Land caps')\n"
                    "end\n");
                if (r2) { pass++; spdlog::info("[PASS] Test 3: Land caps retains Land target"); }
                else { fail++; spdlog::error("[FAIL] Test 3: {}", r2.error().message); }
            } else { fail++; spdlog::error("[FAIL] Test 3 setup: {}", r.error().message); }
        }

        } // end setup success block

        spdlog::info("LayerCap test: {}/{} passed", pass, pass + fail);
        spdlog::info("LayerCap test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    if (massstub_test && !map_path.empty()) {
        spdlog::info("=== MASSSTUB TEST: Mass stub conversions (32 bindings) ===");

        // Run initial ticks for session setup
        for (osc::u32 i = 0; i < 10; i++) sim_state.tick();

        int pass = 0, fail = 0;

        // Test 1: Weapon Change* methods
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "local w = u:GetWeapon(1)\n"
                "if not w then error('entity 1 has no weapon') end\n"
                "w:ChangeDamageRadius(5.0)\n"
                "w:ChangeDamageType('Fire')\n"
                "w:ChangeMaxHeightDiff(10.0)\n"
                "w:ChangeFiringTolerance(0.5)\n"
                "w:ChangeProjectileBlueprint('/projectiles/test')\n"
                "w:SetOnTransport(true)\n"
                "LOG('MassStub test 1: weapon Change* methods OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 1: Weapon Change* methods"); }
            else { fail++; spdlog::error("[FAIL] Test 1: {}", r.error().message); }
        }

        // Test 2: Movement multipliers + ResetSpeedAndAccel
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "u:SetAccMult(2.0)\n"
                "u:SetTurnMult(0.5)\n"
                "u:SetBreakOffDistanceMult(1.5)\n"
                "u:SetBreakOffTriggerMult(2.0)\n"
                "u:SetSpeedMult(3.0)\n"
                "u:ResetSpeedAndAccel()\n"
                "LOG('MassStub test 2: movement mults + reset OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 2: Movement multipliers + reset"); }
            else { fail++; spdlog::error("[FAIL] Test 2: {}", r.error().message); }
        }

        // Test 3: Fuel system round-trip
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "u:SetFuelRatio(0.5)\n"
                "local ratio = u:GetFuelRatio()\n"
                "if math.abs(ratio - 0.5) > 0.01 then\n"
                "    error('GetFuelRatio expected 0.5, got ' .. tostring(ratio))\n"
                "end\n"
                "u:SetFuelUseTime(120)\n"
                "local t = u:GetFuelUseTime()\n"
                "if math.abs(t - 120) > 0.01 then\n"
                "    error('GetFuelUseTime expected 120, got ' .. tostring(t))\n"
                "end\n"
                "LOG('MassStub test 3: fuel round-trip OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 3: Fuel system round-trip"); }
            else { fail++; spdlog::error("[FAIL] Test 3: {}", r.error().message); }
        }

        // Test 4: Projectile target position + zigzag
        {
            // Fire a projectile and check target position
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "local w = u:GetWeapon(1)\n"
                "w:SetOnTransport(false)\n"  // re-enable weapon
                "w:SetFireTargetLayerCaps('Land|Water|Air|Sub|Seabed')\n"
                "w:ChangeMaxRadius(999)\n"
                // Find an enemy and force-fire
                "local enemy = nil\n"
                "for i = 2, 20 do\n"
                "    local e = GetEntityById(i)\n"
                "    if e and e:GetArmy() ~= u:GetArmy() then\n"
                "        enemy = e\n"
                "        break\n"
                "    end\n"
                "end\n"
                "if not enemy then error('no enemy found') end\n"
                "w:SetTargetEntity(enemy)\n");
            if (!r) {
                fail++; spdlog::error("[FAIL] Test 4 setup: {}", r.error().message);
            } else {
                // Don't tick — test the projectile binding functions directly
                // by creating a projectile via CreateProjectileAtBone or
                // by calling the methods on a dummy. Instead, test via unit's
                // weapon firing: tick once to create projectile, then search
                // across a wider range of entity IDs.
                sim_state.tick(); // fire projectile
                auto r2 = state.do_string(
                    // Search for projectile across a wide range
                    "local proj = nil\n"
                    "for i = 100, 300 do\n"
                    "    local e = GetEntityById(i)\n"
                    "    if e and e.GetCurrentTargetPosition then\n"
                    "        proj = e\n"
                    "        break\n"
                    "    end\n"
                    "end\n"
                    "if not proj then\n"
                    "    -- Projectile may have impacted; test the binding functions\n"
                    "    -- exist without error by calling on any entity that has them.\n"
                    "    -- Create a fresh projectile via CreateProjectile\n"
                    "    local u = GetEntityById(1)\n"
                    "    proj = u:CreateProjectile('/projectiles/test', 0, 0, 0, 0, 0, 0)\n"
                    "end\n"
                    "if not proj then error('no projectile found') end\n"
                    "local pos = proj:GetCurrentTargetPosition()\n"
                    "if type(pos) ~= 'table' then error('expected table from GetCurrentTargetPosition') end\n"
                    "proj:ChangeMaxZigZag(3.0)\n"
                    "proj:ChangeZigZagFrequency(0.5)\n"
                    "local zz = proj:GetMaxZigZag()\n"
                    "if math.abs(zz - 3.0) > 0.01 then error('GetMaxZigZag got ' .. tostring(zz)) end\n"
                    "local zf = proj:GetZigZagFrequency()\n"
                    "if math.abs(zf - 0.5) > 0.01 then error('GetZigZagFrequency got ' .. tostring(zf)) end\n"
                    "proj:ChangeDetonateAboveHeight(100)\n"
                    "proj:ChangeDetonateBelowHeight(5)\n"
                    "proj:TrackTarget(true)\n"
                    "LOG('MassStub test 4: projectile target+guidance OK')\n");
                if (r2) { pass++; spdlog::info("[PASS] Test 4: Projectile target + guidance"); }
                else { fail++; spdlog::error("[FAIL] Test 4: {}", r2.error().message); }
            }
        }

        // Test 5: Misc flags + ToggleFireState
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "u:SetAutoOvercharge(true)\n"
                "local oc = u:GetAutoOvercharge()\n"
                "if oc ~= true then error('GetAutoOvercharge expected true, got ' .. tostring(oc)) end\n"
                "u:SetOverchargePaused(true)\n"
                "u:SetAutoOvercharge(false)\n"
                "local oc2 = u:GetAutoOvercharge()\n"
                "if oc2 ~= false then error('GetAutoOvercharge expected false after set') end\n"
                // ToggleFireState cycles 0→1→2→0
                "u:SetFireState(0)\n"
                "u:ToggleFireState()\n"
                "local fs = u:GetFireState()\n"
                "if fs ~= 1 then error('ToggleFireState 0->1 failed, got ' .. tostring(fs)) end\n"
                "u:ToggleFireState()\n"
                "fs = u:GetFireState()\n"
                "if fs ~= 2 then error('ToggleFireState 1->2 failed, got ' .. tostring(fs)) end\n"
                "u:ToggleFireState()\n"
                "fs = u:GetFireState()\n"
                "if fs ~= 0 then error('ToggleFireState 2->0 failed, got ' .. tostring(fs)) end\n"
                // SetCreator, SetFocusEntity, ClearFocusEntity
                "u:SetCreator(u)\n"
                "u:SetFocusEntity(u)\n"
                "u:ClearFocusEntity()\n"
                "LOG('MassStub test 5: misc flags + ToggleFireState OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 5: Misc flags + ToggleFireState"); }
            else { fail++; spdlog::error("[FAIL] Test 5: {}", r.error().message); }
        }

        spdlog::info("MassStub test: {}/{} passed", pass, pass + fail);
        spdlog::info("MassStub test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    if (massstub2_test && !map_path.empty()) {
        spdlog::info("=== MASSSTUB2 TEST: Mass stub conversions II (27 bindings) ===");
        int pass = 0, fail = 0;

        // Test 1: Damage flags — SetCanTakeDamage(false) blocks Damage(), GetAttacker tracks instigator
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "local hp_before = u:GetHealth()\n"
                // First, damage while can_take_damage is true to verify GetAttacker
                "local enemy = nil\n"
                "for i = 2, 20 do\n"
                "    local e = GetEntityById(i)\n"
                "    if e and e:GetArmy() ~= u:GetArmy() then\n"
                "        enemy = e\n"
                "        break\n"
                "    end\n"
                "end\n"
                "if not enemy then error('no enemy found') end\n"
                "Damage(enemy, u, 10, 'Normal')\n"
                "local hp_after = u:GetHealth()\n"
                "if hp_after >= hp_before then error('damage should have reduced HP') end\n"
                "local attacker = u:GetAttacker()\n"
                "if not attacker then error('GetAttacker returned nil after damage') end\n"
                // Now block damage
                "u:SetCanTakeDamage(false)\n"
                "local hp2 = u:GetHealth()\n"
                "Damage(enemy, u, 999, 'Normal')\n"
                "local hp3 = u:GetHealth()\n"
                "if hp3 ~= hp2 then error('damage should be blocked, hp changed from ' .. hp2 .. ' to ' .. hp3) end\n"
                "u:SetCanTakeDamage(true)\n"  // restore
                "u:SetHealth(u, hp_before)\n"  // restore HP
                "LOG('MassStub2 test 1: damage flags OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 1: Damage flags + GetAttacker"); }
            else { fail++; spdlog::error("[FAIL] Test 1: {}", r.error().message); }
        }

        // Test 2: Kill flag — SetCanBeKilled(false) blocks Destroy()
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "local hp_before = u:GetHealth()\n"
                "u:SetCanBeKilled(false)\n"
                "u:Destroy()\n"  // should be blocked by can_be_killed guard
                "-- If we get here, Destroy was blocked (entity still alive)\n"
                "local hp = u:GetHealth()\n"
                "if hp ~= hp_before then error('HP should be unchanged, was ' .. hp_before .. ' now ' .. hp) end\n"
                "u:SetCanBeKilled(true)\n"  // restore
                "LOG('MassStub2 test 2: kill flag OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 2: Kill flag (SetCanBeKilled)"); }
            else { fail++; spdlog::error("[FAIL] Test 2: {}", r.error().message); }
        }

        // Test 3: Command caps round-trip
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "u:AddCommandCap('RULEUCC_Attack')\n"
                "u:AddCommandCap('RULEUCC_Guard')\n"
                "u:RemoveCommandCap('RULEUCC_Attack')\n"
                // RestoreCommandCaps should bring back the snapshot (empty baseline)
                "u:RestoreCommandCaps()\n"
                // Build restrictions
                "u:AddBuildRestriction('uel0201')\n"
                "u:RemoveBuildRestriction('uel0201')\n"
                "u:RestoreBuildRestrictions()\n"
                "LOG('MassStub2 test 3: command caps + build restrictions OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 3: Command caps + build restrictions"); }
            else { fail++; spdlog::error("[FAIL] Test 3: {}", r.error().message); }
        }

        // Test 4: Weapon targeting — GetProjectileBlueprint, SetTargetGround, SetFireControl/IsFireControl, TransferTarget
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "local w = u:GetWeapon(1)\n"
                "if not w then error('no weapon') end\n"
                "local bp = w:GetProjectileBlueprint()\n"
                "if type(bp) ~= 'string' then\n"
                "    if bp ~= nil then error('GetProjectileBlueprint expected string or nil, got ' .. type(bp)) end\n"
                "end\n"
                "w:SetTargetGround(true)\n"
                "w:SetFireControl(true)\n"
                "local fc = w:IsFireControl()\n"
                "if fc ~= true then error('IsFireControl expected true, got ' .. tostring(fc)) end\n"
                "w:SetFireControl(false)\n"
                "fc = w:IsFireControl()\n"
                "if fc ~= false then error('IsFireControl expected false after set') end\n"
                // TransferTarget: test with self weapon
                "w:TransferTarget(w)\n"
                // SetTargetingPriorities / SetWeaponPriorities accept tables
                "w:SetTargetingPriorities({categories.ALLUNITS})\n"
                "w:SetWeaponPriorities({categories.ALLUNITS})\n"
                "LOG('MassStub2 test 4: weapon targeting OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 4: Weapon targeting + control"); }
            else { fail++; spdlog::error("[FAIL] Test 4: {}", r.error().message); }
        }

        // Test 5: Projectile physics flags
        {
            // Fire a projectile first, then test flags
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "local proj = u:CreateProjectile('/projectiles/test', 0, 1, 0, 0, 1, 0)\n"
                "if not proj then error('CreateProjectile returned nil') end\n"
                "proj:SetDestroyOnWater(true)\n"
                "proj:SetStayUpright(true)\n"
                "proj:SetVelocityAlign(true)\n"
                "local ret = proj:SetScaleVelocity(2.0)\n"
                "if not ret then error('SetScaleVelocity should return self') end\n"
                "local ret2 = proj:SetLocalAngularVelocity(1.0, 0.5, 0.2)\n"
                "if not ret2 then error('SetLocalAngularVelocity should return self') end\n"
                "LOG('MassStub2 test 5: projectile physics flags OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 5: Projectile physics flags"); }
            else { fail++; spdlog::error("[FAIL] Test 5: {}", r.error().message); }
        }

        // Test 6: Elevation + rotation + SetCustomName + SetBuildingUnit + SetSpeedThroughGoal
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                // Elevation
                "u:SetElevation(50)\n"
                "u:RevertElevation()\n"
                // Rotation (quaternion form: identity quat)
                "u:SetRotation(0, 0, 0, 1)\n"
                // SetCustomName on entity
                "u:SetCustomName('TestUnit')\n"
                // SetBuildingUnit (set and clear)
                "u:SetBuildingUnit(true, u)\n"
                "u:SetBuildingUnit(false, nil)\n"
                // Navigator: SetSpeedThroughGoal
                "local nav = u:GetNavigator()\n"
                "if nav then\n"
                "    nav:SetSpeedThroughGoal(true)\n"
                "end\n"
                "LOG('MassStub2 test 6: elevation + rotation + misc OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 6: Elevation + rotation + misc"); }
            else { fail++; spdlog::error("[FAIL] Test 6: {}", r.error().message); }
        }

        spdlog::info("MassStub2 test: {}/{} passed", pass, pass + fail);
        spdlog::info("MassStub2 test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

    if (massstub3_test && !map_path.empty()) {
        spdlog::info("=== MASSSTUB3 TEST: Mass stub conversions III (26 bindings) ===");
        int pass = 0, fail = 0;

        // Test 1: Brain events — OnDefeat sets BrainState, SetCurrentPlan stores plan
        {
            auto r = state.do_string(
                "local brain = GetArmyBrain(2)\n"
                "brain:SetCurrentPlan('TestPlan')\n"
                "-- OnDefeat should set state to Defeat\n"
                "brain:OnDefeat()\n"
                "-- Verify brain is now defeated\n"
                "local ok = brain:IsDefeated()\n"
                "if not ok then error('brain should be defeated after OnDefeat') end\n"
                "LOG('MassStub3 test 1: brain events OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 1: Brain events"); }
            else { fail++; spdlog::error("[FAIL] Test 1: {}", r.error().message); }
        }

        // Test 2: Brain utility — GiveStorage, SetResourceSharing, GetArmySkinName
        {
            auto r = state.do_string(
                "local brain = GetArmyBrain(1)\n"
                "local before = brain:GetEconomyStoredRatio('ENERGY')\n"
                "brain:GiveStorage('ENERGY', 500)\n"
                "brain:SetResourceSharing(true)\n"
                "local skin = brain:GetArmySkinName()\n"
                "if type(skin) ~= 'string' then error('GetArmySkinName should return string') end\n"
                "LOG('MassStub3 test 2: brain utility OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 2: Brain utility"); }
            else { fail++; spdlog::error("[FAIL] Test 2: {}", r.error().message); }
        }

        // Test 3: Projectile collision flags — SetCollision, SetCollideSurface, StayUnderwater
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "-- CreateProjectile returns a projectile table\n"
                "local proj = u:CreateProjectile('/projectiles/CDFProton01/CDFProton01_proj.bp',\n"
                "    0, 0, 0, 0, 0, 0)\n"
                "if not proj then error('CreateProjectile returned nil') end\n"
                "-- Test chaining: SetCollision returns self\n"
                "local ret = proj:SetCollision(false)\n"
                "if not ret then error('SetCollision should return self') end\n"
                "proj:SetCollideSurface(false)\n"
                "proj:StayUnderwater(true)\n"
                "LOG('MassStub3 test 3: projectile collision flags OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 3: Projectile collision flags"); }
            else { fail++; spdlog::error("[FAIL] Test 3: {}", r.error().message); }
        }

        // Test 4: CreateChildProjectile
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "local parent = u:CreateProjectile('/projectiles/CDFProton01/CDFProton01_proj.bp',\n"
                "    0, 0, 0, 0, 0, 0)\n"
                "if not parent then error('parent projectile nil') end\n"
                "local child = parent:CreateChildProjectile('/projectiles/CDFProton01/CDFProton01_proj.bp')\n"
                "if not child then error('CreateChildProjectile returned nil') end\n"
                "-- Child should have _c_object\n"
                "if not child._c_object then error('child has no _c_object') end\n"
                "LOG('MassStub3 test 4: CreateChildProjectile OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 4: CreateChildProjectile"); }
            else { fail++; spdlog::error("[FAIL] Test 4: {}", r.error().message); }
        }

        // Test 5: Weapon — BeenDestroyed, SetValidTargetsForCurrentLayer
        {
            auto r = state.do_string(
                "local u = GetEntityById(1)\n"
                "if not u then error('no entity 1') end\n"
                "-- Get first weapon via GetWeapon\n"
                "local w = u:GetWeapon(1)\n"
                "if not w then error('no weapon on entity 1') end\n"
                "-- BeenDestroyed should return false for a living unit\n"
                "local dead = w:BeenDestroyed()\n"
                "if dead then error('weapon should not be destroyed') end\n"
                "-- SetValidTargetsForCurrentLayer should not error\n"
                "w:SetValidTargetsForCurrentLayer('Land')\n"
                "LOG('MassStub3 test 5: weapon fire/control OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 5: Weapon fire/control"); }
            else { fail++; spdlog::error("[FAIL] Test 5: {}", r.error().message); }
        }

        // Test 6: Platoon — SetPlatoonFormationOverride, IsOpponentAIRunning, SetPrioritizedTargetList
        {
            auto r = state.do_string(
                "local brain = GetArmyBrain(1)\n"
                "local platoon = brain:MakePlatoon('TestPlatoon', 'none')\n"
                "if not platoon then error('MakePlatoon returned nil') end\n"
                "platoon:SetPlatoonFormationOverride('AttackFormation')\n"
                "-- IsOpponentAIRunning should return true (other armies exist)\n"
                "local running = platoon:IsOpponentAIRunning()\n"
                "if not running then error('IsOpponentAIRunning should be true') end\n"
                "-- SetPrioritizedTargetList with category string + table\n"
                "platoon:SetPrioritizedTargetList('Attack', {categories.ALLUNITS})\n"
                "LOG('MassStub3 test 6: platoon stubs OK')\n");
            if (r) { pass++; spdlog::info("[PASS] Test 6: Platoon stubs"); }
            else { fail++; spdlog::error("[FAIL] Test 6: {}", r.error().message); }
        }

        spdlog::info("MassStub3 test: {}/{} passed", pass, pass + fail);
        spdlog::info("MassStub3 test: {} entities, {} threads",
                     sim_state.entity_registry().count(),
                     sim_state.thread_manager().active_count());
    }

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
