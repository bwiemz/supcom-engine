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

#include <memory>
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
