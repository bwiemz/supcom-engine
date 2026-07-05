// Loopback lockstep tests: two in-process sims driven over an INetTransport
// stay bit-for-bit in sync, stall when a peer's frame is missing, and detect a
// desync via exchanged checksums. This is the multiplayer sync engine, verified
// headlessly (no sockets, no game data).

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

#include <algorithm>
#include <memory>
#include <vector>

using osc::sim::CommandType;
using osc::sim::LockstepSession;
using osc::sim::LoopbackHub;
using osc::sim::LoopbackTransport;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

osc::u32 spawn_mover(SimState& sim, osc::f32 speed) {
    auto u = std::make_unique<Unit>();
    u->set_army(0);
    u->set_max_speed(speed);
    u->set_position({0.0f, 0.0f, 0.0f});
    return sim.entity_registry().register_entity(std::move(u));
}

UnitCommand move_to(osc::f32 x, osc::f32 z) {
    UnitCommand c;
    c.type = CommandType::Move;
    c.target_pos = {x, 0.0f, z};
    return c;
}

} // namespace

TEST_CASE("Two lockstep peers stay in sync over a transport", "[lockstep]") {
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr);
    SimState b(gb.L, nullptr);
    osc::u32 ida = spawn_mover(a, 5.0f);
    osc::u32 idb = spawn_mover(b, 5.0f);
    REQUIRE(ida == idb);

    LoopbackHub hub;
    LoopbackTransport ta(hub, hub.add_endpoint());
    LoopbackTransport tb(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1});
    LockstepSession sb(b, tb, 1, {0, 1});

    for (int round = 0; round < 30; ++round) {
        if (round == 0) sa.submit_local({ida}, move_to(500.0f, 0.0f), true);
        sa.send_frame();
        sb.send_frame();
        sa.receive_and_advance();
        sb.receive_and_advance();

        CHECK(a.tick_count() == b.tick_count());
        CHECK(a.compute_sync_checksum() == b.compute_sync_checksum());
    }

    CHECK(a.tick_count() == 30);
    CHECK_FALSE(sa.desynced());
    CHECK_FALSE(sb.desynced());
    // The order issued on peer A took effect on peer B too.
    auto* ub = static_cast<Unit*>(b.entity_registry().find(idb));
    CHECK(ub->position().x > 0.0f);
}

TEST_CASE("A peer stalls until every participant confirms the frame", "[lockstep]") {
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr);
    SimState b(gb.L, nullptr);
    spawn_mover(a, 5.0f);
    spawn_mover(b, 5.0f);

    LoopbackHub hub;
    LoopbackTransport ta(hub, hub.add_endpoint());
    LoopbackTransport tb(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1});
    LockstepSession sb(b, tb, 1, {0, 1});

    // A sends and tries to advance, but B has not sent its frame yet.
    sa.send_frame();
    sa.receive_and_advance();
    CHECK(a.tick_count() == 0); // stalled — waiting for peer 1

    // B sends; now A can advance.
    sb.send_frame();
    sa.receive_and_advance();
    CHECK(a.tick_count() == 1);
}

TEST_CASE("Exchanged checksums flag a desync", "[lockstep]") {
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr);
    SimState b(gb.L, nullptr);
    spawn_mover(a, 5.0f);
    spawn_mover(b, 5.0f);
    // Divergent initial state: A has an extra unit B never gets.
    spawn_mover(a, 5.0f);

    LoopbackHub hub;
    LoopbackTransport ta(hub, hub.add_endpoint());
    LoopbackTransport tb(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1});
    LockstepSession sb(b, tb, 1, {0, 1});

    for (int round = 0; round < 4; ++round) {
        sa.send_frame();
        sb.send_frame();
        sa.receive_and_advance();
        sb.receive_and_advance();
    }

    CHECK(a.compute_sync_checksum() != b.compute_sync_checksum());
    CHECK((sa.desynced() || sb.desynced())); // divergence detected in-protocol
}

TEST_CASE("LockstepSession times out a silent peer", "[lockstep][drop]") {
    LoopbackHub hub;
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr), b(gb.L, nullptr);
    LoopbackTransport ta(hub, hub.add_endpoint());
    LoopbackTransport tb(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1});
    LockstepSession sb(b, tb, 1, {0, 1});
    sa.set_drop_timeout(5);

    // A few healthy rounds so each side's peer_confirmed_ is armed.
    for (int r = 0; r < 3; ++r) {
        sa.send_frame();
        sb.send_frame();
        sa.receive_and_advance();
        sb.receive_and_advance();
    }
    REQUIRE(sa.take_dropped().empty());

    // B goes silent; only A keeps sending. A must drop source 1 within ~5 rounds.
    bool dropped = false;
    for (int r = 0; r < 30 && !dropped; ++r) {
        sa.send_frame();
        sa.receive_and_advance();
        auto d = sa.take_dropped();
        if (std::find(d.begin(), d.end(), 1u) != d.end()) dropped = true;
    }
    REQUIRE(dropped);
    REQUIRE(sa.has_dropped(1));

    // After the drop, A is no longer gated on the silent peer — it advances.
    osc::u32 before = a.tick_count();
    sa.send_frame();
    sa.receive_and_advance();
    REQUIRE(a.tick_count() > before);
}
