// The headless run: an AI game or --ticks, the test modes, and the run's
// end (M192 step 2b, moved from run()).

#include "app/app_internal.hpp"
#include "core/log.hpp"
#include "core/profiler.hpp"
#include "core/test_status.hpp"
#include "sim/army_brain.hpp"
#include "sim/entity.hpp"
#include "sim/sim_random.hpp"
#include "sim/unit.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <string>

namespace osc::app {

bool App::after_headless_tick() {
    if (!check_catch_up()) {
        osc::test_status::fail("[FAIL] saved game: it did not play as it was saved");
        return false;
    }
    if (!opt.save_path.empty() && sim_state->tick_count() == opt.save_at) {
        if (opt.scripted_orders) issue_order_before_save(*sim_state);
        const auto start = std::chrono::steady_clock::now();
        auto save = osc::sim::save_game(*sim_state, "headless");
        if (special_files) (void)special_files->sign_snapshot(save);
        if (osc::lua::write_saved_game(save, opt.save_path)) {
            save_written = true;
            spdlog::info(
                "Saved game: {} written in {:.0f} ms", opt.save_path,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count());
        } else {
            osc::test_status::fail("[FAIL] saved game: cannot write {}", opt.save_path);
        }
    }
    // After the save, as a load of it would follow it.
    if (opt.post_load_at != 0 && sim_state->tick_count() == opt.post_load_at) post_load();
    return true;
}

namespace {

/// The armies the ended game declared victorious, "ARMY_2 and ARMY_4 win",
/// or "a draw" when none is.
std::string victors(const osc::sim::SimState& sim) {
    std::string names;
    for (size_t a = 0; a < sim.army_count(); ++a) {
        const auto* brain = sim.army_at(a);
        if (!brain || brain->state() != osc::sim::BrainState::Victory) continue;
        if (!names.empty()) names += " and ";
        names += brain->name();
    }
    if (names.empty()) return "a draw";
    return names + (names.find(" and ") == std::string::npos ? " wins" : " win");
}

} // namespace

void App::tick_headless() {
    const auto start = BenchRecorder::Clock::now();
    sim_state->tick();
    if (bench) bench->record(BenchRecorder::Clock::now() - start);
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

        // Up to tick max_ticks: a game restored from a save (M208c) starts
        // at its saved tick.
        osc::u32 ticks_run = sim_state->tick_count();
        osc::i32 result = 0;
        osc::u32 log_interval = 100; // log stats every 10 game seconds

        for (osc::u32 i = ticks_run; i < max_ticks; i++) {
            if (opt.scripted_orders) issue_scripted_orders(*sim_state, i);
            tick_headless();
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

            // Every army is an AI: the game ends when the game does (the
            // victory script's EndGame, or one team left), not when army 1
            // falls (player_result is the focus army's own result).
            if (sim_state->game_ended()) {
                result = 1;
                spdlog::info("=== Game Over at tick {} ({:.1f}s): {} ===", ticks_run,
                             ticks_run * osc::sim::SimState::SECONDS_PER_TICK, victors(*sim_state));
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
        spdlog::info("  Result: {}", result == 0 ? "No winner (timeout)" : victors(*sim_state));
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
        for (osc::u32 i = sim_state->tick_count(); i < opt.tick_count; i++) {
            osc::Profiler::instance().begin_frame();
            tick_headless();
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

    // --bench's report (M223)
    if (bench && sim_state) {
        if (bench->tick_ms().empty())
            spdlog::warn("--bench: no headless ticks ran (it times --ticks and --ai-skirmish)");
        else if (bench->write(*sim_state))
            spdlog::info("Bench report: {} ({} ticks)", opt.bench_report, bench->tick_ms().size());
        else osc::test_status::fail("[FAIL] --bench: cannot write {}", opt.bench_report);
    }

    // Print profiling summary if enabled
    if (opt.profile_enabled) {
        osc::Profiler::instance().log_summary();
    }

    if (!opt.save_path.empty() && !save_written) {
        osc::test_status::fail("[FAIL] saved game: the run ended at tick {}, before --save-at {}",
                               sim_state ? sim_state->tick_count() : 0, opt.save_at);
    }
    const bool checked = opt.any_test || opt.save_to_load || !opt.save_path.empty();
    const int exit_code = checked ? finish_test_run("integration tests") : 0;
    recording_writer.write();
    osc::log::shutdown();
    return exit_code;
}

} // namespace osc::app
