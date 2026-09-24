// Loopback lockstep tests: two in-process sims driven over an INetTransport
// stay bit-for-bit in sync, stall when a peer's frame is missing, and detect a
// desync via exchanged checksums. This is the multiplayer sync engine, verified
// headlessly (no sockets, no game data).

#include <catch2/catch_test_macros.hpp>

#include "sim/army_brain.hpp"
#include "sim/command_codec.hpp"
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
#include <string>
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
    // ...and named: an extra entity.
    const auto& domains = sa.desynced() ? sa.desync_domains() : sb.desync_domains();
    CHECK(std::find(domains.begin(), domains.end(), "entities") != domains.end());
}

TEST_CASE("A desync names the domain that diverged", "[lockstep]") {
    // Only a unit's fire state differs: a player setting, before it moves
    // or fires anything. The report says `units`, and nothing else.
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr);
    SimState b(gb.L, nullptr);
    const auto ida = spawn_mover(a, 5.0f);
    spawn_mover(b, 5.0f);
    static_cast<Unit*>(a.entity_registry().find(ida))->set_fire_state(1);

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
    REQUIRE(sa.desynced());
    CHECK(sa.desync_domains() == std::vector<std::string>{"units"});
    CHECK(sa.desync_tick() >= 1);
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

    // Late/buffered data from the dropped peer must not re-register it and
    // re-stall the survivor.
    sb.send_frame(); // B (already dropped by A) emits one more frame
    osc::u32 t2 = a.tick_count();
    sa.send_frame();
    sa.receive_and_advance();
    REQUIRE(a.tick_count() > t2); // A ignored B's late frame and kept advancing
    REQUIRE(sa.has_dropped(1));
}

TEST_CASE("A peer's frame carries only its own commands", "[lockstep]") {
    LuaGuard g;
    SimState a(g.L, nullptr);
    const osc::u32 id = spawn_mover(a, 6.0f);
    LoopbackHub hub;
    LoopbackTransport ta(hub, hub.add_endpoint());
    LoopbackTransport raw(hub, hub.add_endpoint()); // writes frames by hand
    LockstepSession sa(a, ta, 0, {0, 1});

    // Frame 1 as `from` would send it, holding one move in `claimed`'s name.
    auto send = [&](osc::u32 from, osc::u32 claimed) {
        std::vector<osc::u8> msg;
        osc::sim::ByteWriter w(msg);
        w.u8v(0); // a frame message
        w.u32v(from);
        w.u32v(1); // frame
        w.u8v(0);  // no checksum: its tick, and each domain's
        w.u32v(0);
        for (size_t i = 0; i < SimState::ChecksumParts::kCount; ++i) w.u64v(0);
        w.u32v(1); // one command
        osc::sim::ScheduledCommand c;
        c.exec_tick = 1;
        c.source = claimed;
        c.command = move_to(100.0f, 0.0f);
        c.unit_ids = {id};
        osc::sim::write_command(w, c);
        raw.broadcast(msg);
    };
    auto* unit = static_cast<Unit*>(a.entity_registry().find(id));

    sa.send_frame();
    send(1, 0); // peer 1 ordering in peer 0's name
    sa.receive_and_advance();
    send(0, 0); // a frame claiming to be this peer's own
    sa.receive_and_advance();
    CHECK(a.tick_count() == 0); // neither confirmed peer 1's frame
    CHECK(unit->command_queue().empty());

    send(1, 1); // peer 1, honestly
    sa.receive_and_advance();
    CHECK(a.tick_count() == 1);
    CHECK(unit->command_queue().size() == 1);
}

TEST_CASE("Survivors agree on a dropped peer's last frame", "[lockstep][drop]") {
    // Three peers. C's last frame -- carrying an order -- reaches A but not B
    // before C dies. Deciding the drop alone, A would play C's order and B
    // would not: a desync. By agreement, B gets the frame relayed, and both
    // defeat C's army on the same tick.
    LuaGuard ga, gb, gc;
    SimState a(ga.L, nullptr), b(gb.L, nullptr), c(gc.L, nullptr);
    std::vector<osc::u32> movers;
    for (SimState* s : {&a, &b, &c}) {
        s->set_victory_condition("sandbox");
        for (const char* army : {"ARMY_1", "ARMY_2", "ARMY_3"}) s->add_army(army, army);
        movers.clear();
        for (int army = 0; army < 3; ++army) {
            auto u = std::make_unique<Unit>();
            u->set_army(army);
            u->set_max_speed(6.0f);
            u->set_position({static_cast<osc::f32>(army) * 50.0f, 0.0f, 0.0f});
            movers.push_back(s->entity_registry().register_entity(std::move(u)));
        }
    }
    b.set_recording(true);
    LoopbackHub hub;
    LoopbackTransport ta(hub, hub.add_endpoint()), tb(hub, hub.add_endpoint()),
        tc(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1, 2});
    LockstepSession sb(b, tb, 1, {0, 1, 2});
    LockstepSession sc(c, tc, 2, {0, 1, 2});
    sa.set_drop_timeout(5);
    sb.set_drop_timeout(5);

    for (int round = 0; round < 5; ++round) {
        sa.send_frame();
        sb.send_frame();
        sc.send_frame();
        sa.receive_and_advance();
        sb.receive_and_advance();
        sc.receive_and_advance();
    }
    // C orders its unit, and dies mid-broadcast: the frame reaches A only.
    sc.submit_local({movers[2]}, move_to(300.0f, 0.0f), true);
    hub.set_link(2, 1, false);
    sa.send_frame();
    sb.send_frame();
    sc.send_frame();
    for (int round = 0; round < 30; ++round) {
        sa.receive_and_advance();
        sb.receive_and_advance();
        sa.send_frame();
        sb.send_frame();
    }
    sa.receive_and_advance();
    sb.receive_and_advance();

    REQUIRE(sa.has_dropped(2));
    REQUIRE(sb.has_dropped(2));
    CHECK(a.tick_count() > 10); // both play on past the drop
    CHECK(a.tick_count() == b.tick_count());
    CHECK(a.compute_sync_checksum() == b.compute_sync_checksum());
    CHECK_FALSE(sa.desynced());
    CHECK_FALSE(sb.desynced());
    CHECK(a.army_at(2)->is_defeated());
    CHECK(b.army_at(2)->is_defeated());
    // B played C's last order, relayed by A.
    bool relayed = false;
    for (const auto& cmd : b.recorded_replay().commands)
        relayed |= cmd.source == 2 && cmd.command.type == CommandType::Move;
    CHECK(relayed);
}

TEST_CASE("A drop report's frame number can't make a survivor work forever", "[lockstep][drop]") {
    // B's report about C claims C's last frame is ~4 billion. Survivors walk
    // the frames the reports carried, not the frame numbers.
    LuaGuard g;
    SimState a(g.L, nullptr);
    for (const char* army : {"ARMY_1", "ARMY_2", "ARMY_3"}) a.add_army(army, army);
    LoopbackHub hub;
    LoopbackTransport ta(hub, hub.add_endpoint());
    LoopbackTransport raw_b(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1, 2});

    std::vector<osc::u8> msg;
    osc::sim::ByteWriter w(msg);
    w.u8v(1);           // a drop report
    w.u32v(1);          // from B
    w.u32v(2);          // about C
    w.u32v(0xFFFFFFF0); // its "last frame"
    w.u32v(0);          // no frames relayed
    raw_b.broadcast(msg);
    sa.send_frame();
    sa.receive_and_advance(); // returns: that is the test
    CHECK(sa.has_dropped(2));
}
