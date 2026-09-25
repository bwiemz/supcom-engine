// The two-process LAN harnesses (multiplayer steps 4 and 5): what main()
// ran before any engine init, for the mp CTest pairs (run_pair.py).

#include "test_modes.hpp"
#include "lua/lan_lobby.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/manipulator.hpp"
#include "sim/net_transport.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <chrono>
#include <cstdio>
#include <thread>
#include <spdlog/spdlog.h>

namespace osc::test {

// ── Headless two-process LAN lockstep verification (multiplayer step 4) ──
// One process runs `--mp-host`, another `--mp-join <addr>`. They connect over
// real TCP, build identical minimal sims, and drive a LockstepSession: the host
// issues scripted player orders through the SAME route_command path the game
// uses; both sims advance in lockstep, exchanging command frames + checksums.
// Each side self-reports its final tick / checksum and whether it desynced —
// matching checksums with desynced()==false on both processes is a synced match.
int run_mp_lan_test(bool is_host, const std::string& address, osc::u16 port, osc::u32 frames,
                    bool inject_desync) {
    using namespace osc;
    namespace chrono = std::chrono;

    // Stand up the transport through the SAME entry points the lobby
    // HostGame / JoinGame bindings use, so this verifies that plumbing too.
    auto& mp = lua::mp_net_state();
    mp.reset();
    const bool ok = is_host ? lua::mp_begin_host(port) : lua::mp_begin_join(address, port);
    if (!ok) {
        spdlog::error("[mp] transport setup failed ({})", is_host ? "host" : "join");
        return 1;
    }

    if (is_host) {
        spdlog::info("[mp] host listening on port {} — waiting for a peer...", mp.port);
        bool connected = false;
        for (int i = 0; i < 6000 && !connected; ++i) { // up to ~60s
            if (lua::mp_poll_connections() >= 1) connected = true;
            else std::this_thread::sleep_for(chrono::milliseconds(10));
        }
        if (!connected) {
            spdlog::error("[mp] host: no peer connected, aborting");
            lua::mp_teardown();
            return 1;
        }
        spdlog::info("[mp] host: peer connected");
    } else {
        spdlog::info("[mp] client connected to {}:{}", address, port);
    }

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

    // Build the LockstepSession over the transport and install the sim's
    // command sink — exactly what the game does at launch.
    lua::mp_attach_session(*sim);
    auto* session = mp.session.get();

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
    // Run the full frame count (don't early-exit on desync): both peers must keep
    // exchanging frames so a divergence is detected symmetrically on both sides —
    // if one side bailed the moment it noticed, it would starve the other of the
    // frame carrying the mismatching checksum.
    for (u32 round = 0; round < frames; ++round) {
        // Host scripts a few player orders at known frames (client stays silent).
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
        // Pump until this side advances one tick (both peers confirmed frame).
        for (int i = 0; i < 20000 && sim->tick_count() <= round; ++i) {
            lua::mp_pump(); // drain the mux (game channel) + accept peers
            session->receive_and_advance();
            if (sim->tick_count() <= round) std::this_thread::sleep_for(chrono::milliseconds(1));
        }
        if (sim->tick_count() <= round) {
            spdlog::error("[mp] stalled at round {} (tick {})", round, sim->tick_count());
            stalled = true;
            break;
        }
    }

    const u32 final_tick = sim->tick_count();
    const u32 checksum = sim->compute_sync_checksum();
    const bool desynced = session->desynced();
    spdlog::info("[mp] {} RESULT: tick={} checksum={:#010x} desynced={}",
                 is_host ? "HOST" : "CLIENT", final_tick, checksum, desynced);
    // Machine-greppable summary line (stdout).
    std::printf("MP_RESULT role=%s tick=%u checksum=%08x desynced=%d stalled=%d\n",
                is_host ? "host" : "client", final_tick, checksum, desynced ? 1 : 0,
                stalled ? 1 : 0);
    std::fflush(stdout);

    // Tear down the session/transport before destroying the sim + Lua state
    // (the session references the sim; the sim references L).
    sim->clear_local_command_sink();
    lua::mp_teardown();
    sim.reset();
    lua_close(L);

    if (inject_desync) {
        // Negative test: success == the injected divergence WAS detected.
        return (desynced && !stalled) ? 0 : 3;
    }
    return (desynced || stalled) ? 2 : 0;
}

// ── Headless two-process LAN lobby lifecycle verification ──
// `--lan-host` / `--lan-join <ip>` drive the REAL lobby handshake over TCP: the
// host advertises a config (scenario + seed), the client applies it and readies,
// the host fires the launch barrier. Both then seed a sim from the shared seed
// (printed as an rng probe to prove it propagated), attach a LockstepSession over
// the mux *game* channel of the same connection, and run a scripted lockstep
// match. Matching scenario/seed/rng/checksum with desynced=0 on both processes ==
// a synced LAN lobby→game lifecycle. (Uses a minimal sim rather than loading the
// full scenario, so the network path — not the FA scenario boot — is what's
// exercised; scenario boot is covered by --full-smoke-test.)
int run_lan_lobby_test(bool is_host, const std::string& address, osc::u16 port, osc::u32 frames,
                       osc::u32 drop_at) {
    using namespace osc;
    namespace chrono = std::chrono;

    auto& mp = lua::mp_net_state();
    mp.reset();
    const u64 host_seed = 0xA5A5F00DCAFEBABEull;
    const std::string scenario = "/maps/SCMP_009/SCMP_009_scenario.lua";

    const bool ok = is_host ? lua::mp_begin_host(port) : lua::mp_begin_join(address, port);
    if (!ok) {
        spdlog::error("[lan] transport setup failed ({})", is_host ? "host" : "join");
        return 1;
    }

    if (is_host) {
        spdlog::info("[lan] host listening on port {} — waiting for a peer...", mp.port);
        bool connected = false;
        for (int i = 0; i < 6000 && !connected; ++i) {
            if (lua::mp_poll_connections() >= 1) connected = true;
            else std::this_thread::sleep_for(chrono::milliseconds(10));
        }
        if (!connected) {
            spdlog::error("[lan] host: no peer connected, aborting");
            lua::mp_teardown();
            return 1;
        }
        mp.lobby->set_host_config(lua::LanSessionConfig{scenario, host_seed});
    }

    // Drive the lobby handshake to the launch barrier on both sides.
    auto* lob = mp.lobby.get();
    for (int i = 0; i < 10000 && !lob->launch_ready(); ++i) {
        lua::mp_pump();
        lob->poll();
        if (is_host && lob->state() == lua::LanLobby::State::Ready) lob->request_launch();
        if (!lob->launch_ready()) std::this_thread::sleep_for(chrono::milliseconds(1));
    }
    if (!lob->launch_ready()) {
        spdlog::error("[lan] lobby handshake did not reach launch");
        lua::mp_teardown();
        return 2;
    }
    spdlog::info("[lan] {} launch barrier reached: scenario='{}' seed={:#018x}",
                 is_host ? "HOST" : "CLIENT", lob->config().scenario, lob->config().seed);

    // Apply the synced seed and build the lockstep session over the game channel.
    mp.seed = lob->config().seed;
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
    lua::mp_attach_session(*sim); // seeds the sim from mp.seed
    auto* session = mp.session.get();
    const u32 rng_probe = sim->sim_rand(); // identical iff the seed propagated

    auto issue_move = [&](u32 id, f32 x, f32 z) {
        sim::UnitCommand cmd;
        cmd.type = sim::CommandType::Move;
        cmd.target_pos = {x, 0.0f, z};
        sim->set_human_input_active(true);
        sim->route_command({id}, cmd, true);
        sim->set_human_input_active(false);
    };

    bool stalled = false;
    u32 dropped_count = 0;
    for (u32 round = 0; round < frames; ++round) {
        // Optional: the client leaves mid-match to exercise drop handling.
        if (!is_host && drop_at > 0 && round == drop_at) {
            spdlog::warn("[lan] client leaving at round {} (--mp-drop-at)", round);
            std::fflush(stdout);
            std::exit(0);
        }
        if (is_host) {
            if (round == 1) issue_move(unit_ids[0], 500.0f, 0.0f);
            if (round == 10) issue_move(unit_ids[1], -300.0f, 200.0f);
        }
        session->send_frame();
        auto last_resend = chrono::steady_clock::now();
        for (int i = 0; i < 20000 && sim->tick_count() <= round; ++i) {
            lua::mp_pump();
            session->receive_and_advance();
            for (u32 s : session->take_dropped()) {
                ++dropped_count;
                spdlog::warn("[lan] peer {} dropped — continuing solo", s);
            }
            if (sim->tick_count() <= round) {
                // Resend while stalled so a silent peer's drop timer accrues
                // (behind = next_frame_ - peer_confirmed grows only on send_frame),
                // but only at the ~10 Hz sim cadence — resending every poll would
                // race next_frame_ ahead and false-drop a merely-slow peer.
                auto now = chrono::steady_clock::now();
                if (now - last_resend >= chrono::milliseconds(100)) {
                    session->send_frame();
                    last_resend = now;
                }
                std::this_thread::sleep_for(chrono::milliseconds(1));
            }
        }
        if (sim->tick_count() <= round) {
            stalled = true;
            break;
        }
    }

    const u32 checksum = sim->compute_sync_checksum();
    const bool desynced = session->desynced();
    spdlog::info("[lan] {} RESULT tick={} checksum={:#010x} rng={:#010x} desynced={} dropped={}",
                 is_host ? "HOST" : "CLIENT", sim->tick_count(), checksum, rng_probe, desynced,
                 dropped_count);
    std::printf("LAN_RESULT role=%s scenario=%s seed=%08x%08x rng=%08x tick=%u "
                "checksum=%08x desynced=%d stalled=%d dropped=%d\n",
                is_host ? "host" : "client", lob->config().scenario.c_str(),
                static_cast<u32>(lob->config().seed >> 32),
                static_cast<u32>(lob->config().seed & 0xFFFFFFFFull), rng_probe, sim->tick_count(),
                checksum, desynced ? 1 : 0, stalled ? 1 : 0, dropped_count > 0 ? 1 : 0);
    std::fflush(stdout);

    sim->clear_local_command_sink();
    lua::mp_teardown();
    sim.reset();
    lua_close(L);
    return (desynced || stalled) ? 2 : 0;
}

} // namespace osc::test
