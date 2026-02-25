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
#include "sim/unit.hpp"

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
    return 100; // default: 10 seconds game time at 10Hz
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

    // Phase 5: Run tick loop
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
