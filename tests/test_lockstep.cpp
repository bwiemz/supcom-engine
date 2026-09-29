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

TEST_CASE("A dropped player's own army is defeated, not the one numbered as its source",
          "[lockstep][drop]") {
    // A lobby's game: player A (source 0) plays army 0, an AI army 1, player
    // B (source 1) army 2, and C (source 2) watches. B goes silent, then C.
    LuaGuard ga, gb, gc;
    SimState a(ga.L, nullptr), b(gb.L, nullptr), c(gc.L, nullptr);
    for (SimState* s : {&a, &b, &c}) {
        s->set_victory_condition("sandbox");
        for (const char* army : {"ARMY_1", "ARMY_2", "ARMY_3"}) s->add_army(army, army);
        s->set_source_army(0, 0);
        s->set_source_army(1, 2);
        s->set_source_army(2, -1);
    }
    LoopbackHub hub;
    LoopbackTransport ta(hub, hub.add_endpoint()), tb(hub, hub.add_endpoint()),
        tc(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1, 2});
    LockstepSession sb(b, tb, 1, {0, 1, 2});
    LockstepSession sc(c, tc, 2, {0, 1, 2});
    sa.set_drop_timeout(5);
    sc.set_drop_timeout(5);
    for (int round = 0; round < 5; ++round) {
        for (LockstepSession* s : {&sa, &sb, &sc}) s->send_frame();
        for (LockstepSession* s : {&sa, &sb, &sc}) s->receive_and_advance();
    }
    // B goes silent: A and C drop it, and its army 2 goes, not army 1
    for (int round = 0; round < 30; ++round) {
        sa.send_frame();
        sc.send_frame();
        sa.receive_and_advance();
        sc.receive_and_advance();
    }
    REQUIRE(sa.has_dropped(1));
    REQUIRE(sc.has_dropped(1));
    CHECK(a.army_at(2)->is_defeated());
    CHECK_FALSE(a.army_at(1)->is_defeated());
    CHECK(c.army_at(2)->is_defeated());
    // The watcher goes silent too: A drops it, and no army goes with it
    for (int round = 0; round < 30; ++round) {
        sa.send_frame();
        sa.receive_and_advance();
    }
    REQUIRE(sa.has_dropped(2));
    CHECK_FALSE(a.army_at(0)->is_defeated());
    CHECK_FALSE(a.army_at(1)->is_defeated());
    CHECK(a.tick_count() > 30); // A plays on alone
}

TEST_CASE("A player ejects another still playing; the survivors agree (M218e)",
          "[lockstep][drop]") {
    LuaGuard ga, gb, gc;
    SimState a(ga.L, nullptr), b(gb.L, nullptr), c(gc.L, nullptr);
    for (SimState* s : {&a, &b, &c}) {
        s->set_victory_condition("sandbox");
        for (const char* army : {"ARMY_1", "ARMY_2", "ARMY_3"}) s->add_army(army, army);
    }
    LoopbackHub hub;
    LoopbackTransport ta(hub, hub.add_endpoint()), tb(hub, hub.add_endpoint()),
        tc(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1, 2});
    LockstepSession sb(b, tb, 1, {0, 1, 2});
    LockstepSession sc(c, tc, 2, {0, 1, 2});
    const auto round = [&] {
        for (LockstepSession* s : {&sa, &sb, &sc}) s->send_frame();
        for (LockstepSession* s : {&sa, &sb, &sc}) s->receive_and_advance();
    };
    for (int i = 0; i < 5; ++i) round();

    // A ejects C, who still plays: A's vote first
    CHECK_FALSE(sa.eject(0)); // not itself
    CHECK_FALSE(sa.eject(7)); // no such player
    REQUIRE(sa.eject(2));
    CHECK_FALSE(sa.eject(2)); // once
    CHECK(sa.ejectors(2) == std::vector<osc::u32>{0});
    CHECK_FALSE(sa.has_dropped(2));
    // B hears of it and votes too; then both drop C, on the same tick
    for (int i = 0; i < 10; ++i) round();
    REQUIRE(sa.has_dropped(2));
    REQUIRE(sb.has_dropped(2));
    CHECK(sa.ejectors(2) == std::vector<osc::u32>{0, 1});
    CHECK(sb.ejectors(2) == std::vector<osc::u32>{0, 1});
    CHECK(sc.ejected()); // C knows it is out
    CHECK_FALSE(sa.ejected());
    CHECK(a.tick_count() == b.tick_count());
    CHECK(a.compute_sync_checksum() == b.compute_sync_checksum());
    CHECK(a.army_at(2)->is_defeated());
    CHECK(b.army_at(2)->is_defeated());
    CHECK_FALSE(a.army_at(1)->is_defeated());
    // A and B play on without C; C's game is over: it ticks no further,
    // and cut off, never drops them to play on alone
    const osc::u32 at = a.tick_count();
    const osc::u32 c_at = c.tick_count();
    for (int i = 0; i < 5; ++i) round();
    CHECK(a.tick_count() > at);
    CHECK(a.tick_count() == b.tick_count());
    sc.set_drop_timeout(5);
    for (int i = 0; i < 40; ++i) {
        sc.send_frame();
        sc.receive_and_advance();
    }
    CHECK(c.tick_count() == c_at);
    CHECK_FALSE(sc.has_dropped(0));
    CHECK_FALSE(sc.has_dropped(1));
}

TEST_CASE("A player alone with the one it ejects drops it at once (M218e)", "[lockstep][drop]") {
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr), b(gb.L, nullptr);
    for (SimState* s : {&a, &b}) {
        s->set_victory_condition("sandbox");
        for (const char* army : {"ARMY_1", "ARMY_2"}) s->add_army(army, army);
    }
    LoopbackHub hub;
    LoopbackTransport ta(hub, hub.add_endpoint()), tb(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1});
    LockstepSession sb(b, tb, 1, {0, 1});
    for (int i = 0; i < 3; ++i) {
        sa.send_frame();
        sb.send_frame();
        sa.receive_and_advance();
        sb.receive_and_advance();
    }
    REQUIRE(sa.eject(1));
    CHECK(sa.has_dropped(1)); // no other survivor to wait for
    CHECK(sa.ejectors(1) == std::vector<osc::u32>{0});
    sb.receive_and_advance();
    CHECK(sb.ejected());
}

namespace {

/// Two sims with a mover each, over a hub: the pause tests' game.
struct PausePair {
    LuaGuard ga, gb;
    SimState a{ga.L, nullptr}, b{gb.L, nullptr};
    LoopbackHub hub;
    LoopbackTransport ta{hub, hub.add_endpoint()}, tb{hub, hub.add_endpoint()};
    LockstepSession sa{a, ta, 0, {0, 1}};
    LockstepSession sb{b, tb, 1, {0, 1}};
    PausePair() {
        for (SimState* s : {&a, &b}) {
            s->set_victory_condition("sandbox");
            for (const char* army : {"ARMY_1", "ARMY_2"}) s->add_army(army, army);
            spawn_mover(*s, 5.0f);
        }
        sa.set_drop_timeout(5);
        sb.set_drop_timeout(5);
    }
    void round() {
        sa.send_frame();
        sb.send_frame();
        sa.receive_and_advance();
        sb.receive_and_advance();
    }
};

} // namespace

TEST_CASE("A pause holds every peer on one tick; a resume releases them together (M218f)",
          "[lockstep][pause]") {
    PausePair g;
    for (int i = 0; i < 5; ++i) g.round();
    g.sa.request_pause();
    for (int i = 0; i < 3; ++i) g.round();
    REQUIRE(g.a.network_paused());
    REQUIRE(g.b.network_paused());
    CHECK(g.a.paused_by() == 0);
    CHECK(g.b.paused_by() == 0);
    const osc::u32 held = g.a.tick_count();
    CHECK(g.b.tick_count() == held);
    // Held there, however long, and no one is dropped: no frames flow
    for (int i = 0; i < 40; ++i) g.round();
    CHECK(g.a.tick_count() == held);
    CHECK(g.b.tick_count() == held);
    CHECK_FALSE(g.sa.has_dropped(1));
    CHECK_FALSE(g.sb.has_dropped(0));
    // Asking again while paused does nothing
    g.sa.request_pause();
    // The other player resumes (any may): both go on from there together,
    // a tick a round -- no burst of the rounds spent paused
    REQUIRE(g.sb.request_resume());
    CHECK_FALSE(g.sb.request_resume()); // not paused any more
    g.round();
    CHECK_FALSE(g.a.network_paused());
    CHECK_FALSE(g.b.network_paused());
    for (int i = 0; i < 5; ++i) g.round();
    CHECK(g.a.tick_count() <= held + 7);
    CHECK(g.a.tick_count() > held);
    CHECK(g.a.tick_count() == g.b.tick_count());
    CHECK(g.a.compute_sync_checksum() == g.b.compute_sync_checksum());
    CHECK(g.a.pause_serial() == 1); // the second ask found it paused
}

TEST_CASE("A pause holds on its own tick, though later frames are in (M218f)",
          "[lockstep][pause]") {
    PausePair g;
    for (int i = 0; i < 3; ++i) g.round();
    const osc::u32 pause_tick = g.sa.current_frame(); // the frame it goes in
    g.sa.request_pause();
    // Both send frames well past it before either runs a tick
    for (int i = 0; i < 4; ++i) {
        g.sa.send_frame();
        g.sb.send_frame();
    }
    g.sa.receive_and_advance();
    g.sb.receive_and_advance();
    REQUIRE(g.a.network_paused());
    REQUIRE(g.b.network_paused());
    CHECK(g.a.tick_count() == pause_tick);
    CHECK(g.b.tick_count() == pause_tick);
}

TEST_CASE("A source's pause timeouts run out; others may still pause (M218f)",
          "[lockstep][pause]") {
    PausePair g;
    for (SimState* s : {&g.a, &g.b}) s->set_pause_timeouts(0, 1);
    for (int i = 0; i < 3; ++i) g.round();
    g.sa.request_pause();
    for (int i = 0; i < 3; ++i) g.round();
    REQUIRE(g.a.network_paused());
    CHECK(g.a.pause_timeouts(0) == 0);
    CHECK(g.b.pause_timeouts(0) == 0); // spent on every peer
    g.sa.request_resume();
    for (int i = 0; i < 3; ++i) g.round();
    // A has none left: refused, and the game plays on
    const osc::u32 at = g.a.tick_count();
    g.sa.request_pause();
    for (int i = 0; i < 5; ++i) g.round();
    CHECK_FALSE(g.a.network_paused());
    CHECK(g.a.tick_count() > at);
    // B's are unlimited
    g.sb.request_pause();
    for (int i = 0; i < 3; ++i) g.round();
    CHECK(g.a.network_paused());
    CHECK(g.a.paused_by() == 1);
    CHECK(g.b.pause_timeouts(1) == -1);
}

TEST_CASE("A resume that comes before a peer reaches the pause releases it there (M218f)",
          "[lockstep][pause]") {
    PausePair g;
    for (int i = 0; i < 3; ++i) g.round();
    // B falls behind: it sends its frames but reads nothing, while A pauses
    // and resumes
    g.sa.request_pause();
    for (int i = 0; i < 4; ++i) {
        g.sa.send_frame();
        g.sb.send_frame();
        g.sa.receive_and_advance();
    }
    REQUIRE(g.a.network_paused());
    const osc::u32 held = g.a.tick_count();
    CHECK(g.b.tick_count() < held);
    REQUIRE(g.sa.request_resume());
    // B reads it all at once: it reaches the pause after its resume
    g.sb.receive_and_advance();
    CHECK_FALSE(g.b.network_paused());
    CHECK(g.b.pause_serial() == 1);
    CHECK(g.b.tick_count() >= held);
    for (int i = 0; i < 5; ++i) g.round();
    CHECK(g.a.tick_count() == g.b.tick_count());
    CHECK(g.a.compute_sync_checksum() == g.b.compute_sync_checksum());
}

TEST_CASE("Outside a lockstep game a pause resumes at once (M218f)", "[lockstep][pause]") {
    // A replay playing a network game back: no message would resume it
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.set_pause_timeouts(0, 3);
    sim.request_pause(0);
    CHECK_FALSE(sim.network_paused());
    CHECK(sim.pause_serial() == 1);
    CHECK(sim.pause_timeouts(0) == 2); // spent as in the game
}

TEST_CASE("A drop report fits one wire message, keeping the newest frames", "[lockstep][drop]") {
    // The dropped peer's last frames each name 600,000 units (2.4 MB): all
    // three would make a report over the wire limit, and a survivor that
    // sent it would be dropped as hostile. The report keeps the newest.
    LuaGuard ga, gc;
    SimState a(ga.L, nullptr), c(gc.L, nullptr);
    for (SimState* s : {&a, &c}) {
        s->set_victory_condition("sandbox");
        for (const char* army : {"ARMY_1", "ARMY_2"}) s->add_army(army, army);
    }
    LoopbackHub hub;
    LoopbackTransport ta(hub, hub.add_endpoint()), tc(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1});
    LockstepSession sc(c, tc, 1, {0, 1});
    sa.set_drop_timeout(5);

    std::vector<osc::u32> crowd(600000);
    for (size_t i = 0; i < crowd.size(); ++i) crowd[i] = static_cast<osc::u32>(1000000 + i);
    for (int round = 0; round < 3; ++round) {
        sc.submit_local(crowd, move_to(10.0f * static_cast<osc::f32>(round), 0.0f), true);
        sa.send_frame();
        sc.send_frame();
        sa.receive_and_advance();
        sc.receive_and_advance();
    }
    hub.drain(1); // what C was sent so far
    for (int round = 0; round < 30 && !sa.has_dropped(1); ++round) {
        sa.send_frame();
        sa.receive_and_advance();
    }
    REQUIRE(sa.has_dropped(1));
    size_t largest = 0;
    for (const auto& msg : hub.drain(1)) {
        CHECK(msg.size() <= osc::sim::kMaxWireMessage);
        largest = std::max(largest, msg.size());
    }
    CHECK(largest > crowd.size() * sizeof(osc::u32)); // the newest frame went
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

// ── Pace (M218h) ──────────────────────────────────────────────────────────

namespace {

/// Peers on a line with latency: what one sends reaches the others `delay`
/// rounds later (a round: every peer's send_frame and receive_and_advance).
struct DelayedHub {
    osc::u32 now = 0;
    osc::u32 delay = 0;
    std::vector<std::vector<std::pair<osc::u32, std::vector<osc::u8>>>> inboxes;
    int add_endpoint() {
        inboxes.emplace_back();
        return static_cast<int>(inboxes.size()) - 1;
    }
};

class DelayedTransport : public osc::sim::INetTransport {
public:
    DelayedTransport(DelayedHub& hub, int id) : hub_(hub), id_(id) {}
    void broadcast(const std::vector<osc::u8>& msg) override {
        for (size_t i = 0; i < hub_.inboxes.size(); ++i)
            if (static_cast<int>(i) != id_)
                hub_.inboxes[i].emplace_back(hub_.now + hub_.delay, msg);
    }
    std::vector<std::vector<osc::u8>> receive() override {
        auto& inbox = hub_.inboxes[static_cast<size_t>(id_)];
        std::vector<std::vector<osc::u8>> out;
        auto due = std::stable_partition(inbox.begin(), inbox.end(),
                                         [&](const auto& m) { return m.first <= hub_.now; });
        for (auto it = inbox.begin(); it != due; ++it) out.push_back(std::move(it->second));
        inbox.erase(inbox.begin(), due);
        return out;
    }

private:
    DelayedHub& hub_;
    int id_;
};

/// Frames a peer has sent and not yet run.
osc::u32 lead(const LockstepSession& s, const SimState& sim) {
    return s.current_frame() - 1 - sim.tick_count();
}

} // namespace

TEST_CASE("A peer runs at most two seconds of frames ahead of its sim, and a dead peer still "
          "drops (M218h)",
          "[lockstep][pace]") {
    LoopbackHub hub;
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr), b(gb.L, nullptr);
    LoopbackTransport ta(hub, hub.add_endpoint());
    LoopbackTransport tb(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1});
    LockstepSession sb(b, tb, 1, {0, 1});
    for (int r = 0; r < 3; ++r) {
        sa.send_frame();
        sb.send_frame();
        sa.receive_and_advance();
        sb.receive_and_advance();
    }
    hub.drain(1);

    // B goes silent: A runs ahead to the cap, then only says it's there
    for (osc::u32 r = 0; r < LockstepSession::kMaxLead + 5; ++r) {
        sa.send_frame();
        sa.receive_and_advance();
        CHECK(lead(sa, a) <= LockstepSession::kMaxLead);
    }
    CHECK(lead(sa, a) == LockstepSession::kMaxLead);
    int frames = 0, alive = 0;
    for (const auto& msg : hub.drain(1)) {
        frames += msg[0] == 0 ? 1 : 0;
        alive += msg[0] == 3 ? 1 : 0;
    }
    CHECK(frames == static_cast<int>(LockstepSession::kMaxLead));
    CHECK(alive == 5);
    CHECK_FALSE(sa.has_dropped(1));

    // Held at the cap, A still counts B's silence: past the timeout, B drops
    for (int r = 0; r < 10 && !sa.has_dropped(1); ++r) {
        sa.send_frame();
        sa.receive_and_advance();
    }
    CHECK(sa.has_dropped(1));
}

TEST_CASE("Latency costs a local order's delay, not the game's pace (M218h)", "[lockstep][pace]") {
    for (const osc::u32 delay : {4u, 30u}) {
        DYNAMIC_SECTION("a delay of " << delay << " rounds") {
            DelayedHub hub;
            hub.delay = delay;
            LuaGuard ga, gb;
            SimState a(ga.L, nullptr), b(gb.L, nullptr);
            const osc::u32 ida = spawn_mover(a, 5.0f);
            spawn_mover(b, 5.0f);
            DelayedTransport ta(hub, hub.add_endpoint()), tb(hub, hub.add_endpoint());
            LockstepSession sa(a, ta, 0, {0, 1});
            LockstepSession sb(b, tb, 1, {0, 1});
            osc::u32 order_frame = 0, delay_seen = 0;
            for (osc::u32 r = 1; r <= 300; ++r) {
                hub.now = r;
                if (r == 150) {
                    order_frame = sa.current_frame();
                    delay_seen = order_frame - a.tick_count();
                    sa.submit_local({ida}, move_to(500.0f, 0.0f), true);
                }
                sa.send_frame();
                sb.send_frame();
                sa.receive_and_advance();
                sb.receive_and_advance();
                CHECK(lead(sa, a) <= LockstepSession::kMaxLead);
            }
            CHECK_FALSE(sa.has_dropped(1));
            CHECK_FALSE(sb.has_dropped(0));
            CHECK_FALSE(sa.desynced());
            CHECK_FALSE(sb.desynced());
            if (delay < LockstepSession::kMaxLead) {
                // Frames lead the sim by the latency: an order waits for it,
                // and the game still runs a tick a round
                CHECK(delay_seen == delay + 1);
                CHECK(a.tick_count() == 300 - delay);
            } else {
                // Past two seconds, the game slows rather than drops anyone
                CHECK(delay_seen <= LockstepSession::kMaxLead + 1);
                CHECK(a.tick_count() < 200);
                CHECK(a.tick_count() > 50);
            }
            // The order ran on both, on its frame's tick
            auto* ua = static_cast<Unit*>(a.entity_registry().find(ida));
            auto* ub = static_cast<Unit*>(b.entity_registry().find(ida));
            if (a.tick_count() > order_frame && b.tick_count() > order_frame) {
                CHECK(ua->position().x > 0.0f);
                CHECK(ub->position().x > 0.0f);
            }
        }
    }
}

TEST_CASE("The slowest peer sets the game's pace, and isn't dropped (M218h)", "[lockstep][pace]") {
    LoopbackHub hub;
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr), b(gb.L, nullptr);
    const osc::u32 ida = spawn_mover(a, 5.0f);
    spawn_mover(b, 5.0f);
    LoopbackTransport ta(hub, hub.add_endpoint()), tb(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1});
    LockstepSession sb(b, tb, 1, {0, 1});

    // B manages a round for every three of A's (a slow machine). Pacing by
    // the clock alone, A would run ever further ahead and time B out.
    osc::u32 most = 0;
    for (int r = 0; r < 300; ++r) {
        if (r == 100) sa.submit_local({ida}, move_to(500.0f, 0.0f), true);
        sa.send_frame();
        if (r % 3 == 0) sb.send_frame();
        sa.receive_and_advance();
        if (r % 3 == 0) sb.receive_and_advance();
        most = std::max(most, lead(sa, a));
    }
    CHECK(most == LockstepSession::kMaxLead);
    CHECK_FALSE(sa.has_dropped(1));
    CHECK_FALSE(sb.has_dropped(0));
    CHECK(a.tick_count() > 90); // B's pace: about a tick a round of its
    CHECK(a.tick_count() < 110);
    // Both run what both have sent, in step
    for (int r = 0; r < 60 && a.tick_count() != b.tick_count(); ++r) {
        sb.send_frame();
        sa.receive_and_advance();
        sb.receive_and_advance();
    }
    REQUIRE(a.tick_count() == b.tick_count());
    CHECK(a.compute_sync_checksum() == b.compute_sync_checksum());
    CHECK_FALSE(sa.desynced());
    CHECK_FALSE(sb.desynced());
}

TEST_CASE("A peer held at the cap still says it's there, so a slow one isn't dropped (M218h)",
          "[lockstep][pace]") {
    // C, slower still, sends a frame every 29 of A's rounds: both A and B
    // wait on it at the cap, and each sends a frame only as C's lets the
    // sim move. B, slow too (a round in seven of A's), then sends one only
    // on a round of its own after C's, 28 or 35 of A's apart: the 35 is
    // past A's timeout. Its word each round it has keeps it in.
    LuaGuard ga, gb, gc;
    SimState a(ga.L, nullptr), b(gb.L, nullptr), c(gc.L, nullptr);
    LoopbackHub hub;
    LoopbackTransport ta(hub, hub.add_endpoint()), tb(hub, hub.add_endpoint()),
        tc(hub, hub.add_endpoint());
    LockstepSession sa(a, ta, 0, {0, 1, 2});
    LockstepSession sb(b, tb, 1, {0, 1, 2});
    LockstepSession sc(c, tc, 2, {0, 1, 2});
    for (int r = 0; r < 600; ++r) {
        sa.send_frame();
        if (r % 7 == 0) sb.send_frame();
        if (r % 29 == 0) sc.send_frame();
        sa.receive_and_advance();
        if (r % 7 == 0) sb.receive_and_advance();
        if (r % 29 == 0) sc.receive_and_advance();
    }
    for (osc::u32 s : {0u, 1u, 2u}) {
        CHECK_FALSE(sa.has_dropped(s));
        CHECK_FALSE(sb.has_dropped(s));
        CHECK_FALSE(sc.has_dropped(s));
    }
    CHECK(a.tick_count() > 15); // a tick for each of C's frames
    CHECK_FALSE(sa.desynced());
    CHECK_FALSE(sb.desynced());
}

// ── Game speed (M218i) ────────────────────────────────────────────────────

TEST_CASE("A fixed game speed can't be changed (M218i)", "[lockstep][speed]") {
    PausePair g;
    g.sa.set_speed_option(4, false); // the lobby's 'fast'
    g.sb.set_speed_option(4, false);
    CHECK_FALSE(g.sa.request_speed(7));
    g.round();
    CHECK(g.sa.speed() == 4);
    CHECK(g.sb.speed() == 4);
    CHECK(g.sa.take_speed_changes().empty());
    CHECK(g.sb.take_speed_changes().empty());
}

TEST_CASE("A speed change reaches every peer; the newest wins, a tie the lower source (M218i)",
          "[lockstep][speed]") {
    PausePair g;
    g.sa.set_speed_option(0, true);
    g.sb.set_speed_option(0, true);
    for (int i = 0; i < 3; ++i) g.round();

    // A asks: it has it at once, B once the message comes
    REQUIRE(g.sa.request_speed(5));
    CHECK(g.sa.speed() == 5);
    g.round();
    CHECK(g.sb.speed() == 5);
    for (auto* s : {&g.sa, &g.sb}) {
        const auto changes = s->take_speed_changes();
        REQUIRE(changes.size() == 1);
        CHECK(changes[0].source == 0);
        CHECK(changes[0].rate == 5);
    }

    // Both ask at once (the same clock): source 0's stands on both
    REQUIRE(g.sb.request_speed(7));
    REQUIRE(g.sa.request_speed(3));
    g.round();
    CHECK(g.sa.speed() == 3);
    CHECK(g.sb.speed() == 3);

    // A newer request wins, whoever asks; out of range is held to -10..+50
    REQUIRE(g.sb.request_speed(99));
    g.round();
    CHECK(g.sa.speed() == 50);
    CHECK(g.sb.speed() == 50);
    REQUIRE(g.sb.request_speed(-2));
    g.round();
    CHECK(g.sa.speed() == -2);
    CHECK(g.sb.speed() == -2);
    g.sa.take_speed_changes();

    // A request older than the one applied (delayed on its way) changes nothing
    std::vector<osc::u8> stale;
    osc::sim::ByteWriter w(stale);
    w.u8v(4);  // a speed request
    w.u32v(1); // from B
    w.u32v(1); // its clock: the first
    w.u32v(9);
    g.tb.broadcast(stale);
    g.round();
    CHECK(g.sa.speed() == -2);
    CHECK(g.sa.take_speed_changes().empty());
    CHECK_FALSE(g.sa.desynced());
}

TEST_CASE("At speed, the lead cap and the drop timeout keep their times (M218i)",
          "[lockstep][speed]") {
    for (const osc::i32 speed : {10, -10}) {
        DYNAMIC_SECTION("at " << speed) {
            LoopbackHub hub;
            LuaGuard ga, gb;
            SimState a(ga.L, nullptr), b(gb.L, nullptr);
            LoopbackTransport ta(hub, hub.add_endpoint()), tb(hub, hub.add_endpoint());
            LockstepSession sa(a, ta, 0, {0, 1});
            LockstepSession sb(b, tb, 1, {0, 1});
            sa.set_speed_option(speed, false);
            for (int r = 0; r < 3; ++r) {
                sa.send_frame();
                sb.send_frame();
                sa.receive_and_advance();
                sb.receive_and_advance();
            }
            // B goes silent: A runs ahead two seconds' worth of rounds at
            // this speed, and drops B after three seconds' worth
            const osc::u32 cap = speed > 0 ? 200 : 2;
            const int timeout = speed > 0 ? 300 : 3;
            int rounds = 0;
            osc::u32 most = 0;
            while (!sa.has_dropped(1) && rounds < 1000) {
                sa.send_frame();
                sa.receive_and_advance();
                ++rounds;
                most = std::max(most, lead(sa, a));
            }
            CHECK(most == cap);
            CHECK(rounds == timeout + 1);
        }
    }
}
