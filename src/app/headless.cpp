// The headless run: an AI game or --ticks, the test modes, and the run's
// end (M192 step 2b, moved from run()).

#include "app/app_internal.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/test_status.hpp"
#include "sim/entity.hpp"
#include "sim/sim_random.hpp"
#include "sim/unit.hpp"

#include <spdlog/spdlog.h>

#include <cstdio>

namespace osc::app {

bool App::after_headless_tick() {
    if (catch_up) {
        if (!catch_up->check(*sim_state)) {
            const u32 tick = catch_up->diverged_at();
            osc::test_status::fail("[FAIL] saved game: diverged at tick {} as it caught up", tick);
            std::printf("LOAD diverged tick=%u\n", tick);
            catch_up.reset();
            return false;
        }
        if (!sim_state->resuming()) {
            spdlog::info("Saved game: caught up at tick {}; the game plays on",
                         sim_state->tick_count());
            std::printf("LOAD resumed tick=%u\n", sim_state->tick_count());
            catch_up.reset();
        }
    }
    if (!opt.save_path.empty() && sim_state->tick_count() == opt.save_at) {
        if (opt.scripted_orders) issue_order_before_save(*sim_state);
        const auto save = osc::sim::save_game(*sim_state, "headless");
        if (!osc::lua::write_saved_game(save, opt.save_path))
            osc::test_status::fail("[FAIL] saved game: cannot write {}", opt.save_path);
    }
    return true;
}

int App::run_headless() {
    if (tests) {
        if (auto code = tests->headless_first(engine)) return *code;
    }

    // === AI-vs-AI Skirmish (M163) ===
    if (opt.ai_skirmish && !opt.map_path.empty()) {
        osc::u32 max_ticks = opt.tick_count > 0 ? opt.tick_count : 6000; // default 10 min
        spdlog::info("=== AI-vs-AI Skirmish: up to {} ticks ({:.0f}s) ===", max_ticks,
                     max_ticks * osc::sim::SimState::SECONDS_PER_TICK);

        osc::u32 ticks_run = 0;
        osc::i32 result = 0;
        osc::u32 log_interval = 100; // log stats every 10 game seconds

        osc::sim::SimRandom script_rng(0x5C817ED0);
        for (osc::u32 i = 0; i < max_ticks; i++) {
            if (opt.scripted_orders) issue_scripted_orders(*sim_state, script_rng, i);
            sim_state->tick();
            ticks_run++;
            if (!after_headless_tick()) break;

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
                spdlog::info("  Tick {}: {:.1f}s | {} units alive | {} vetted | {} sounds",
                             ticks_run, ticks_run * osc::sim::SimState::SECONDS_PER_TICK,
                             total_units, total_vet, sound.active_count());
            }

            // Check for game over
            result = sim_state->player_result();
            if (result != 0) {
                const char* result_str = result == 1   ? "ARMY_1 WINS"
                                         : result == 2 ? "ARMY_2 WINS"
                                                       : "DRAW";
                spdlog::info("=== Game Over at tick {} ({:.1f}s): {} ===", ticks_run,
                             ticks_run * osc::sim::SimState::SECONDS_PER_TICK, result_str);
                break;
            }
        }

        if (result == 0) {
            spdlog::info("=== AI Skirmish: tick limit reached, no winner ===");
        }

        // Final summary
        spdlog::info("=== AI Skirmish Summary ===");
        spdlog::info("  Map: {}", opt.map_path);
        spdlog::info("  Ticks: {} ({:.1f}s game time)", ticks_run,
                     ticks_run * osc::sim::SimState::SECONDS_PER_TICK);
        spdlog::info("  Result: {}", result == 1   ? "ARMY_1 wins"
                                     : result == 2 ? "ARMY_2 wins"
                                     : result == 0 ? "No winner (timeout)"
                                                   : "Draw");
        for (size_t a = 0; a < sim_state->army_count(); a++) {
            auto* brain = sim_state->army_at(a);
            if (!brain || brain->is_civilian()) continue;
            osc::u32 surviving = 0;
            osc::u32 vetted = 0;
            osc::i32 army_idx = static_cast<osc::i32>(a);
            sim_state->entity_registry().for_each([&](const osc::sim::Entity& e) {
                if (!e.destroyed() && e.is_unit() && e.army() == army_idx) {
                    surviving++;
                    auto& u = static_cast<const osc::sim::Unit&>(e);
                    if (u.vet_level() > 0) vetted++;
                }
            });
            spdlog::info("  Army {} ({}): {} units surviving, {} vetted", a + 1,
                         brain->is_defeated() ? "DEFEATED" : "alive", surviving, vetted);
        }
        spdlog::info("=== End AI Skirmish ===");
    }

    // Headless tick loop
    if (!opt.ai_skirmish && !opt.map_path.empty() && opt.tick_count > 0) {
        spdlog::info("Running {} sim ticks ({:.1f}s game time)...", opt.tick_count,
                     opt.tick_count * osc::sim::SimState::SECONDS_PER_TICK);
        for (osc::u32 i = 0; i < opt.tick_count; i++) {
            osc::Profiler::instance().begin_frame();
            sim_state->tick();
            osc::Profiler::instance().end_frame();
            if (!after_headless_tick()) break;
        }
    }


    if (tests) tests->headless(engine);

    // --dump-threads: where every live sim script thread is suspended.
    if (sim_state && parse_flag(argc, argv, "--dump-threads")) {
        for (const auto& line : sim_state->thread_manager().describe_threads()) {
            spdlog::info("[thread] {}", line);
        }
    }

    // Report final state
    if (sim_state) {
        spdlog::info("Sim: {} armies, {} entities, {} active threads, "
                     "{} ticks ({:.1f}s game time)",
                     sim_state->army_count(), sim_state->entity_registry().count(),
                     sim_state->thread_manager().active_count(), sim_state->tick_count(),
                     sim_state->game_time());
    }

    // Print profiling summary if enabled
    if (opt.profile_enabled) {
        osc::Profiler::instance().log_summary();
    }

    const int exit_code =
        opt.any_test || opt.save_to_load ? finish_test_run("integration tests") : 0;
    recording_writer.write();
    osc::log::shutdown();
    return exit_code;
}

} // namespace osc::app
