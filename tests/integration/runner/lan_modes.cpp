// The two-process multiplayer harness, for the mp CTest pairs (run_pair.py):
// what main() runs before any engine init.

#include "test_modes.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/net_lobby.hpp"
#include "sim/lobby_net.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <spdlog/spdlog.h>

namespace osc::test {

namespace {

/// The host's hosted time, which seeds the game: the joiner must have it
/// from its welcome.
constexpr u64 kHostedTime = 0xA5A5F00DCAFEBABEull;
/// The lobby's word to launch (the scripts' own launch message, here).
constexpr std::string_view kLaunch = "launch";

/// Poll `net` for up to `wait_ms`, handing each event to `seen`, until it
/// returns true. False on the deadline.
template <typename Seen> bool poll_until(sim::LobbyNet& net, i64 wait_ms, Seen seen) {
    const i64 deadline = lua::net_lobby_clock_ms() + wait_ms;
    while (lua::net_lobby_clock_ms() < deadline) {
        for (const sim::LobbyEvent& e : net.poll(lua::net_lobby_clock_ms()))
            if (seen(e)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

/// The lobby, to its launch: the host waits for the joiner and tells it to
/// launch; the joiner joins (again, while the host isn't listening yet) and
/// waits for the word. Null if it never came. `joiner_uid`: the joiner's.
std::unique_ptr<sim::LobbyNet> lobby_to_launch(bool is_host, const std::string& address, u16 port,
                                               u32& joiner_uid) {
    if (is_host) {
        auto net = std::make_unique<sim::LobbyNet>("Host", 1, sim::LobbyTransport::Udp);
        if (!net->host(port, kHostedTime)) {
            spdlog::error("[mp] host: can't listen on port {}", port);
            return nullptr;
        }
        spdlog::info("[mp] host: lobby on port {}, waiting for a player...", net->port());
        const bool joined = poll_until(*net, 60000, [&](const sim::LobbyEvent& e) {
            if (e.kind != sim::LobbyEvent::Kind::PeerJoined) return false;
            joiner_uid = e.uid;
            return true;
        });
        if (!joined) {
            spdlog::error("[mp] host: no player joined");
            return nullptr;
        }
        net->send(joiner_uid, std::vector<u8>(kLaunch.begin(), kLaunch.end()));
        net->stop_joining();
        return net;
    }

    const i64 deadline = lua::net_lobby_clock_ms() + 30000;
    while (lua::net_lobby_clock_ms() < deadline) {
        auto net = std::make_unique<sim::LobbyNet>("Joiner", 1, sim::LobbyTransport::Udp);
        if (!net->join(address, port)) {
            spdlog::error("[mp] joiner: {} is no address", address);
            return nullptr;
        }
        bool failed = false;
        const bool launched = poll_until(*net, 15000, [&](const sim::LobbyEvent& e) {
            using Kind = sim::LobbyEvent::Kind;
            if (e.kind == Kind::ConnectionFailed || e.kind == Kind::Ejected) failed = true;
            return failed || (e.kind == Kind::Data &&
                              std::string(e.payload.begin(), e.payload.end()) == kLaunch);
        });
        if (launched && !failed) {
            joiner_uid = net->local_uid();
            return net;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    spdlog::error("[mp] joiner: never launched from {}:{}", address, port);
    return nullptr;
}

} // namespace

// ── Two processes in lockstep over a lobby's connections (M218g) ──
// One process runs `--mp-host`, another `--mp-join <addr>`: a lobby hosted and
// joined over real UDP (the reliable streams games play over, M220c), launched as a lobby's
// LaunchGame launches (its connections become the game's: mp_begin_lobby_game, mp_attach_session),
// then identical minimal sims in lockstep. The host issues scripted player orders through the same
// route_command path the game uses. Each side reports its final tick and checksum; desynced=0 on
// both processes is a synced match.
// --mp-slow <ms>: the joiner takes that long a round, and must set the
// game's pace rather than be dropped (M218h).
int run_mp_lobby_test(bool is_host, const std::string& address, osc::u16 port, osc::u32 frames,
                      bool inject_desync, osc::u32 drop_at, osc::u32 slow_ms) {
    using namespace osc;
    namespace chrono = std::chrono;

    lua::mp_teardown();
    u32 joiner_uid = 0;
    std::unique_ptr<sim::LobbyNet> net = lobby_to_launch(is_host, address, port, joiner_uid);
    if (!net) return 1;
    const u64 seed = net->hosted_time();
    if (seed != kHostedTime) {
        spdlog::error("[mp] the seed is {:#018x}, not the host's {:#018x}", seed, kHostedTime);
        return 1;
    }
    // Source 0 the host's (army 0), 1 the joiner's (army 1)
    lua::mp_begin_lobby_game(
        std::make_unique<sim::LobbyGameTransport>(std::move(net), lua::net_lobby_clock_ms),
        is_host ? 0 : 1, {0, 1}, {{0, "Host"}, {joiner_uid, "Joiner"}}, seed);
    spdlog::info("[mp] {} launched", is_host ? "host" : "joiner");

    // Minimal deterministic sim, constructed identically on both sides.
    lua_State* L = lua_open();
    auto sim = std::make_unique<sim::SimState>(L, nullptr);
    std::vector<u32> unit_ids;
    for (int i = 0; i < 3; ++i) {
        auto u = std::make_unique<sim::Unit>();
        u->set_army(0);
        u->set_max_speed(4.0f);
        u->set_position({static_cast<f32>(i * 10), 0.0f, 0.0f});
        unit_ids.push_back(sim->entity_registry().register_entity(std::move(u)));
    }

    // Build the LockstepSession over the lobby's connections and install the
    // sim's command sink, as the game does at launch.
    lua::mp_attach_session(*sim);
    auto* session = lua::mp_net_state().session.get();
    const u32 rng_probe = sim->sim_rand(); // equal on both sides iff the seed is

    auto issue = [&](u32 unit_id, const sim::UnitCommand& cmd) {
        sim->set_human_input_active(true); // player order → sink → session
        sim->route_command({unit_id}, cmd, true);
        sim->set_human_input_active(false);
    };
    auto issue_move = [&](u32 unit_id, f32 x, f32 z) {
        sim::UnitCommand cmd;
        cmd.type = sim::CommandType::Move;
        cmd.target_pos = {x, 0.0f, z};
        issue(unit_id, cmd);
    };
    auto issue_stop = [&](u32 unit_id) {
        sim::UnitCommand cmd;
        cmd.type = sim::CommandType::Stop;
        issue(unit_id, cmd);
    };

    bool stalled = false;
    u32 dropped_count = 0;
    // Run the full frame count (don't early-exit on desync): both peers must keep
    // exchanging frames so a divergence is detected symmetrically on both sides —
    // if one side bailed the moment it noticed, it would starve the other of the
    // frame carrying the mismatching checksum.
    for (u32 round = 0; round < frames; ++round) {
        // --mp-slow: the joiner is a slow machine, a round taking this long
        if (!is_host && slow_ms > 0) std::this_thread::sleep_for(chrono::milliseconds(slow_ms));
        // --mp-drop-at: the joiner leaves mid-match, without a goodbye.
        if (!is_host && drop_at > 0 && round == drop_at) {
            spdlog::warn("[mp] joiner leaving at round {} (--mp-drop-at)", round);
            std::fflush(stdout);
            std::_Exit(0);
        }
        // Host scripts a few player orders at known frames (joiner stays silent).
        if (is_host) {
            if (round == 1) issue_move(unit_ids[0], 500.0f, 0.0f);
            if (round == 10) issue_move(unit_ids[1], -300.0f, 200.0f);
            if (round == 20) issue_move(unit_ids[2], 100.0f, -400.0f);
            if (round == 30) issue_stop(unit_ids[0]); // mid-move Stop → must sync
            if (inject_desync && round == 15) {
                // Negative test: apply a LOCAL-only order (human input inactive
                // → route_command's direct branch, NOT broadcast) so this sim
                // deliberately diverges. The checksum exchange must catch it.
                sim::UnitCommand cmd;
                cmd.type = sim::CommandType::Move;
                cmd.target_pos = {999.0f, 0.0f, 999.0f};
                sim->route_command({unit_ids[1]}, cmd, true);
            }
        }
        session->send_frame();
        auto last_resend = chrono::steady_clock::now();
        // Pump until this side advances one tick (both peers confirmed frame).
        for (int i = 0; i < 20000 && sim->tick_count() <= round; ++i) {
            session->receive_and_advance();
            for (u32 s : session->take_dropped()) {
                ++dropped_count;
                spdlog::warn("[mp] peer {} dropped — playing on", s);
            }
            if (sim->tick_count() <= round) {
                // Resend while stalled so a silent peer's drop timer accrues
                // (it counts the frames sent since the peer's last), but only at
                // the sim's ~10 Hz: resending every poll would race ahead and
                // drop a peer that is merely slow.
                auto now = chrono::steady_clock::now();
                if (now - last_resend >= chrono::milliseconds(100)) {
                    session->send_frame();
                    last_resend = now;
                }
                std::this_thread::sleep_for(chrono::milliseconds(1));
            }
        }
        if (sim->tick_count() <= round) {
            spdlog::error("[mp] stalled at round {} (tick {})", round, sim->tick_count());
            stalled = true;
            break;
        }
    }

    const u32 final_tick = sim->tick_count();
    const u32 checksum = sim->compute_sync_checksum();
    // Done, but the other side may not be: it may still want this side's
    // last frame, which closing now could lose (a socket closed with data
    // unread resets the connection, and the peer loses what it had yet to
    // read). So go on answering for a second -- well inside the drop
    // timeout -- before closing.
    if (!stalled) {
        const auto until = chrono::steady_clock::now() + chrono::seconds(1);
        auto last_send = chrono::steady_clock::now();
        while (chrono::steady_clock::now() < until) {
            session->receive_and_advance();
            if (chrono::steady_clock::now() - last_send >= chrono::milliseconds(100)) {
                session->send_frame();
                last_send = chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(chrono::milliseconds(1));
        }
        dropped_count += static_cast<u32>(session->take_dropped().size());
    }
    const bool desynced = session->desynced();
    spdlog::info("[mp] {} RESULT: tick={} checksum={:#010x} rng={:#010x} desynced={} dropped={}",
                 is_host ? "HOST" : "JOINER", final_tick, checksum, rng_probe, desynced,
                 dropped_count);
    // Machine-greppable summary line (stdout).
    std::printf("MP_RESULT role=%s tick=%u checksum=%08x rng=%08x desynced=%d stalled=%d "
                "dropped=%u\n",
                is_host ? "host" : "joiner", final_tick, checksum, rng_probe, desynced ? 1 : 0,
                stalled ? 1 : 0, dropped_count);
    std::fflush(stdout);

    // Tear down the session and connections before destroying the sim + Lua
    // state (the session references the sim; the sim references L).
    sim->clear_local_command_sink();
    lua::mp_teardown();
    sim.reset();
    lua_close(L);

    if (inject_desync) {
        // Negative test: success == the injected divergence WAS detected.
        return (desynced && !stalled) ? 0 : 3;
    }
    if (desynced || stalled) return 2;
    // --mp-drop-at: the host must have dropped the joiner, and played on;
    // else no one may be dropped (a slow joiner sets the pace)
    const u32 want_dropped = is_host && drop_at > 0 ? 1 : 0;
    if (dropped_count != want_dropped) {
        spdlog::error("[mp] {} dropped {} peers, not {}", is_host ? "host" : "joiner",
                      dropped_count, want_dropped);
        return 4;
    }
    return 0;
}

} // namespace osc::test
