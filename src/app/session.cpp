// A game's session: test-run results, traces, recording and replays, and
// the sound engine each sim gets (M192 step 2, moved from app.cpp).

#include "app/app_internal.hpp"
#include "core/game_state.hpp"
#include "lua/lua_state.hpp"
#include "lua/special_files.hpp"
#include "renderer/renderer.hpp"
#include "sim/unit.hpp"
#include "sim/build_info.hpp"
#include "lua/mp_net_state.hpp"

#include <random>

namespace osc::app {

int finish_test_run(const char* mode, osc::u32 smoke_issues) {
    if (smoke_issues > 0) {
        osc::test_status::record_failure(
            fmt::format("{} smoke issue(s) reported (see smoke report)", smoke_issues));
    }
    const int failures = osc::test_status::failure_count();
    if (failures == 0) {
        spdlog::info("=== {}: PASS ===", mode);
        return 0;
    }
    spdlog::error("=== {}: FAIL ({} failed check(s)) ===", mode, failures);
    for (const auto& message : osc::test_status::failure_messages()) {
        spdlog::error("  - {}", message);
    }
    return 1;
}

void detach_ui_from_sim(lua_State* uiL) {
    lua_pushstring(uiL, "osc_sim_state");
    lua_pushnil(uiL);
    lua_rawset(uiL, LUA_REGISTRYINDEX);
}

/// Give a sim (and its Lua state) the application's sound engine.
void attach_sound(osc::lua::LuaState& sim_lua, osc::sim::SimState& sim,
                  osc::audio::SoundManager* sound) {
    lua_State* L = sim_lua.raw();
    lua_pushstring(L, "osc_sound_manager");
    if (sound) lua_pushlightuserdata(L, sound);
    else lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    sim.set_sound_manager(sound);
}

/// --checksum-trace <file>: every game's sims write their per-tick checksum
/// here (see SimState::set_checksum_trace); null when not asked for.
std::ofstream* g_checksum_trace = nullptr;

/// --entity-trace <file> [--entity-trace-ticks <from>-<to>]: every entity's
/// synced state at those ticks (see SimState::set_entity_trace).
std::ofstream* g_entity_trace = nullptr;
osc::u32 g_entity_trace_from = 0;
osc::u32 g_entity_trace_to = 0xFFFFFFFFu;

/// --rng-trace <file> [--rng-trace-ticks <from>-<to>]: every random draw and
/// the script that made it (see SimState::set_rng_trace).
std::ofstream* g_rng_trace = nullptr;
osc::u32 g_rng_trace_from = 0;
osc::u32 g_rng_trace_to = 0xFFFFFFFFu;

/// --record <file>: each game records (SimState::set_recording), and the
/// run writes the last one's replay here as it ends. Empty: no recording.
std::string g_record_path;

/// Write `sim`'s recording to `path`.
bool write_recording(const osc::sim::SimState& sim, const std::string& path) {
    return osc::lua::write_replay_file(sim.recorded_replay(), path);
}

/// A replay file that can start its game, or nothing (the reason logged).
std::optional<osc::sim::Replay> load_replay(const std::string& path) {
    auto replay = osc::lua::read_replay_file(path);
    if (replay && replay->build != osc::sim::build_id()) {
        spdlog::warn("Replay: recorded by build {}, playing on {}; a different build may "
                     "play it differently",
                     replay->build, osc::sim::build_id());
    }
    return replay;
}

/// --scripted-orders: army 1 also takes a player's orders, routed as a
/// player's so a recording holds them: moves, now and then a fire state or
/// a pause (SimCallbacks) and a Stop. Picked from their own random stream,
/// never the sim's.
void issue_scripted_orders(osc::sim::SimState& sim, osc::sim::SimRandom& rng, osc::u32 tick) {
    if (tick % 40 != 20 || !sim.terrain()) return;
    std::vector<osc::u32> movers, all; // army 1's live units, in id order
    sim.entity_registry().for_each([&](const osc::sim::Entity& e) {
        if (!e.is_unit() || e.destroyed() || e.army() != 0) return;
        all.push_back(e.entity_id());
        if (static_cast<const osc::sim::Unit&>(e).has_command_cap("RULEUCC_Move"))
            movers.push_back(e.entity_id());
    });
    if (all.empty()) return;
    auto any = [&](const std::vector<osc::u32>& ids) {
        return ids[static_cast<size_t>(rng.next_int(0, static_cast<osc::i64>(ids.size()) - 1))];
    };
    osc::sim::UnitCommand cmd;
    std::vector<osc::u32> ids;
    switch ((tick / 40) % 6) {
    case 3: { // a fire state
        osc::sim::SimCallbackEntry cb;
        cb.func_name = osc::sim::kUnitSettingCallback;
        cb.args["Setting"] = std::string("FireState");
        cb.args["Value"] = static_cast<osc::f64>(rng.next_int(0, 2));
        cb.unit_ids = {any(all)};
        sim.submit_callback(std::move(cb));
        return;
    }
    case 4: { // pause or resume
        const osc::u32 id = any(all);
        const auto* u = static_cast<const osc::sim::Unit*>(sim.entity_registry().find(id));
        osc::sim::SimCallbackEntry cb;
        cb.func_name = osc::sim::kUnitSettingCallback;
        cb.args["Setting"] = std::string("Paused");
        cb.args["Value"] = !u->is_paused();
        cb.unit_ids = {id};
        sim.submit_callback(std::move(cb));
        return;
    }
    case 5: // stop
        cmd.type = osc::sim::CommandType::Stop;
        ids = {any(all)};
        break;
    default: { // move a few units somewhere
        if (movers.empty()) return;
        cmd.type = osc::sim::CommandType::Move;
        const auto w = static_cast<double>(sim.terrain()->map_width());
        const auto h = static_cast<double>(sim.terrain()->map_height());
        cmd.target_pos = {static_cast<osc::f32>(rng.next_double() * w), 0.0f,
                          static_cast<osc::f32>(rng.next_double() * h)};
        for (int k = 0; k < 3; ++k) ids.push_back(any(movers));
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        break;
    }
    }
    sim.set_human_input_active(true);
    sim.route_command(ids, cmd, true);
    sim.set_human_input_active(false);
}

/// --replay: play the recorded game to its end, checking the checksum after
/// every tick against the recording's. 0 when they match throughout.
int play_replay(osc::sim::SimState& sim, const osc::sim::Replay& replay) {
    osc::sim::ReplayPlayback playback(replay);
    playback.start(sim);
    spdlog::info("Replay: {} commands over {} ticks", replay.commands.size(), replay.final_tick);
    while (!playback.finished(sim)) {
        sim.tick();
        if (!playback.check(sim)) {
            const osc::u32 tick = playback.diverged_at();
            spdlog::error("Replay diverged at tick {}: checksum {:08x}, recorded {:08x}", tick,
                          sim.compute_sync_checksum(),
                          replay.checksums[tick - replay.checksum_from]);
            std::printf("REPLAY diverged tick=%u\n", tick);
            return 1;
        }
    }
    spdlog::info("Replay: matched the recording at every tick");
    std::printf("REPLAY ok ticks=%u checksum=%08x\n", sim.tick_count(),
                sim.compute_sync_checksum());
    return 0;
}

/// The random seed of a new game: `--seed` when given; else a fixed one when
/// the run must repeat (tests, headless runs, captures); else a fresh one.
/// A multiplayer session then replaces it with the seed its peers share.
osc::u64 new_game_seed(const std::string& seed_arg, bool reproducible) {
    if (!seed_arg.empty()) return std::strtoull(seed_arg.c_str(), nullptr, 0);
    if (reproducible) return osc::sim::SimRandom::kDefaultSeed;
    std::random_device rd;
    return (static_cast<osc::u64>(rd()) << 32) ^ rd();
}

/// The seed a launch boots its sim with. A multiplayer launch uses the seed
/// the lobby shared: every peer must roll the same numbers from the first
/// line of boot (scenario scripts, BeginSession's AI setup), not only once
/// the session attaches afterwards.
osc::u64 launch_seed(const std::string& seed_arg, bool reproducible) {
    const auto& mp = osc::lua::mp_net_state();
    if (mp.transport_ready) return mp.seed;
    return new_game_seed(seed_arg, reproducible);
}

} // namespace osc::app
