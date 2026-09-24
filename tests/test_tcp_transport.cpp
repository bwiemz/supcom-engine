// Real TCP transport tests, run over the localhost loopback interface: message
// relay through a host, plus a full lockstep session driven over actual sockets.

#include <catch2/catch_test_macros.hpp>

#include "sim/army_brain.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/manipulator.hpp"
#include "sim/net_transport.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using osc::sim::CommandType;
using osc::sim::LockstepSession;
using osc::sim::SimState;
using osc::sim::TcpTransport;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

// Poll receive() a bounded number of times until it yields something (localhost
// delivery is effectively immediate, so this rarely spins).
std::vector<std::vector<osc::u8>> recv_soon(TcpTransport& t, int tries = 200) {
    for (int i = 0; i < tries; ++i) {
        auto msgs = t.receive();
        if (!msgs.empty()) return msgs;
    }
    return {};
}

osc::u32 spawn_mover(SimState& sim, osc::f32 speed) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(speed);
    return sim.entity_registry().register_entity(std::move(u));
}

UnitCommand move_to(osc::f32 x, osc::f32 z) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = {x, 0.0f, z};
    return c;
}

} // namespace

TEST_CASE("Wire framing hands out whole messages, however they arrive", "[tcp]") {
    using osc::sim::extract_wire_frames;
    const auto framed = [](std::vector<osc::u8> msg) {
        const auto len = static_cast<osc::u32>(msg.size());
        std::vector<osc::u8> out{static_cast<osc::u8>(len), static_cast<osc::u8>(len >> 8),
                                 static_cast<osc::u8>(len >> 16), static_cast<osc::u8>(len >> 24)};
        out.insert(out.end(), msg.begin(), msg.end());
        return out;
    };
    std::vector<osc::u8> wire = framed({1, 2, 3});
    const auto second = framed({4, 5});
    wire.insert(wire.end(), second.begin(), second.end());

    // Byte by byte: a message is handed out only once it is whole.
    std::vector<osc::u8> buf;
    std::vector<std::vector<osc::u8>> out;
    for (osc::u8 b : wire) {
        buf.push_back(b);
        REQUIRE(extract_wire_frames(buf, out));
    }
    REQUIRE(out.size() == 2);
    CHECK(out[0] == std::vector<osc::u8>{1, 2, 3});
    CHECK(out[1] == std::vector<osc::u8>{4, 5});
    CHECK(buf.empty());
}

TEST_CASE("A peer announcing an oversized message is refused before its body", "[tcp]") {
    using osc::sim::extract_wire_frames;
    using osc::sim::kMaxWireMessage;
    // A forged header claims 0xF0000000 bytes: refused at once, rather than
    // buffering while it trickles in.
    std::vector<osc::u8> buf{0x00, 0x00, 0x00, 0xF0, 1, 2, 3};
    std::vector<std::vector<osc::u8>> out;
    CHECK_FALSE(extract_wire_frames(buf, out));
    CHECK(out.empty());

    // The largest allowed message waits for its body.
    const osc::u32 max = kMaxWireMessage;
    buf = {static_cast<osc::u8>(max), static_cast<osc::u8>(max >> 8),
           static_cast<osc::u8>(max >> 16), static_cast<osc::u8>(max >> 24), 9};
    CHECK(extract_wire_frames(buf, out));
    CHECK(out.empty());
    CHECK(buf.size() == 5);
}

TEST_CASE("TCP host relays a message between clients", "[tcp]") {
    auto host = TcpTransport::host(0);
    REQUIRE(host->ok());
    osc::u16 port = host->port();
    REQUIRE(port != 0);

    auto a = TcpTransport::join("127.0.0.1", port);
    auto b = TcpTransport::join("127.0.0.1", port);
    REQUIRE(a->ok());
    REQUIRE(b->ok());
    REQUIRE(host->poll_connections() == 2);

    std::vector<osc::u8> payload{1, 2, 3, 4, 5};
    a->broadcast(payload);

    auto at_host = recv_soon(*host);
    REQUIRE(at_host.size() == 1);
    CHECK(at_host[0] == payload);

    auto at_b = recv_soon(*b); // relayed by the host
    REQUIRE(at_b.size() == 1);
    CHECK(at_b[0] == payload);
}

TEST_CASE("TCP host broadcast reaches every client", "[tcp]") {
    auto host = TcpTransport::host(0);
    REQUIRE(host->ok());
    auto a = TcpTransport::join("127.0.0.1", host->port());
    auto b = TcpTransport::join("127.0.0.1", host->port());
    REQUIRE(host->poll_connections() == 2);

    std::vector<osc::u8> payload{9, 8, 7};
    host->broadcast(payload);
    CHECK(recv_soon(*a).size() == 1);
    CHECK(recv_soon(*b).size() == 1);
}

TEST_CASE("Lockstep runs over real TCP sockets", "[tcp][lockstep]") {
    auto host_t = TcpTransport::host(0);
    REQUIRE(host_t->ok());
    auto client_t = TcpTransport::join("127.0.0.1", host_t->port());
    REQUIRE(client_t->ok());
    REQUIRE(host_t->poll_connections() == 1);

    LuaGuard gh, gc;
    SimState h(gh.L, nullptr);
    SimState c(gc.L, nullptr);
    osc::u32 idh = spawn_mover(h, 5.0f);
    osc::u32 idc = spawn_mover(c, 5.0f);
    REQUIRE(idh == idc);

    LockstepSession sh(h, *host_t, 0, {0, 1});
    LockstepSession sc(c, *client_t, 1, {0, 1});

    for (int round = 0; round < 40; ++round) {
        if (round == 0) sh.submit_local({idh}, move_to(500.0f, 0.0f), true);
        sh.send_frame();
        sc.send_frame();
        // Pump until each side has consumed the other's frame for this round.
        for (int i = 0; i < 200 && (h.tick_count() <= static_cast<osc::u32>(round) ||
                                    c.tick_count() <= static_cast<osc::u32>(round));
             ++i) {
            sh.receive_and_advance();
            sc.receive_and_advance();
        }
        REQUIRE(h.tick_count() == c.tick_count());
    }

    CHECK(h.tick_count() == 40);
    CHECK(h.compute_sync_checksum() == c.compute_sync_checksum());
    CHECK_FALSE(sh.desynced());
    CHECK_FALSE(sc.desynced());
    // The order issued on the host took effect on the client too.
    auto* uc = static_cast<Unit*>(c.entity_registry().find(idc));
    CHECK(uc->position().x > 0.0f);
}

TEST_CASE("TCP host survives sending to a vanished peer and drops it", "[tcp]") {
    auto host = TcpTransport::host(0);
    REQUIRE(host->ok());
    auto client = TcpTransport::join("127.0.0.1", host->port());
    REQUIRE(client->ok());
    REQUIRE(host->poll_connections() == 1);

    client.reset(); // peer disappears without a goodbye (crash, cable pull)

    // On POSIX the first send after the peer's RST fails with EPIPE, which by
    // default raises SIGPIPE and kills the whole process. The send path must
    // neither die nor keep the dead peer around.
    std::vector<osc::u8> payload(1024, 7);
    for (int i = 0; i < 500 && host->peer_count() > 0; ++i) {
        host->broadcast(payload);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(host->peer_count() == 0);
}
