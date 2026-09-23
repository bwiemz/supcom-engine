// Render interpolation between sim ticks (M190): the tick clock, per-tick
// world snapshots, and the interpolated view the renderer draws from.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/fixed_step.hpp"
#include "core/tick_clock.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/world_snapshot.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>

using Catch::Approx;
using osc::TickClock;
using osc::sim::Entity;
using osc::sim::FrameView;
using osc::sim::Quaternion;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::Vector3;
using osc::sim::WorldHistory;
using osc::sim::WorldSnapshot;

namespace {

constexpr double kTick = 0.1;

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

osc::u32 spawn_at(SimState& sim, Vector3 pos) {
    auto unit = std::make_unique<Unit>();
    unit->set_army(0);
    unit->set_position(pos);
    return sim.entity_registry().register_entity(std::move(unit));
}

Entity& entity(SimState& sim, osc::u32 id) {
    auto* e = sim.entity_registry().find(id);
    REQUIRE(e != nullptr);
    return *e;
}

Unit& unit(SimState& sim, osc::u32 id) { return static_cast<Unit&>(entity(sim, id)); }

std::array<osc::f32, 16> filled(osc::f32 v) {
    std::array<osc::f32, 16> m{};
    m.fill(v);
    return m;
}

} // namespace

// --- TickClock ---------------------------------------------------------------

TEST_CASE("TickClock tracks the fixed-step accumulator at a steady cadence", "[interp][clock]") {
    TickClock clock(kTick);
    double acc = 0.0;
    for (int frame = 0; frame < 100; ++frame) {
        const double dt = 0.016;
        clock.advance(dt);
        const int ticks = osc::consume_fixed_steps(acc, dt, kTick, 8);
        for (int t = 0; t < ticks; ++t) clock.on_tick();
        CHECK(clock.alpha() == Approx(acc / kTick).margin(1e-9));
    }
}

TEST_CASE("TickClock holds at the newest tick through a stall and recovers",
          "[interp][clock]") {
    TickClock clock(kTick);
    clock.advance(0.05);
    CHECK(clock.alpha() == Approx(0.5));

    // "Waiting for players": a second passes and no tick arrives.
    for (int i = 0; i < 60; ++i) clock.advance(1.0 / 60.0);
    CHECK(clock.alpha() == Approx(1.0));

    // The next tick lands; the one after it starts interpolating again.
    clock.on_tick();
    clock.advance(0.02);
    CHECK(clock.alpha() == Approx(1.0));
    clock.on_tick();
    CHECK(clock.alpha() == Approx(0.2));
}

TEST_CASE("TickClock stays put while paused and resets on a new session", "[interp][clock]") {
    TickClock clock(kTick);
    clock.advance(0.03);
    const float paused_alpha = clock.alpha();
    // Paused frames don't advance the clock.
    CHECK(clock.alpha() == paused_alpha);
    clock.reset();
    CHECK(clock.alpha() == 0.0f);
}

// --- capture_world -----------------------------------------------------------

TEST_CASE("capture_world records live entities in id order with their bone poses",
          "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const osc::u32 a = spawn_at(sim, {1, 2, 3});
    const osc::u32 b = spawn_at(sim, {4, 5, 6});
    const osc::u32 gone = spawn_at(sim, {7, 8, 9});
    entity(sim, gone).mark_destroyed();
    unit(sim, b).animated_bone_matrices().assign(3, filled(2.0f));

    WorldSnapshot snap;
    osc::sim::capture_world(sim, snap);

    CHECK(snap.tick == sim.tick_count());
    REQUIRE(snap.entities.size() == 2);
    CHECK(snap.entities[0].id == a);
    CHECK(snap.entities[1].id == b);
    CHECK(snap.find(gone) == nullptr);

    const auto* pa = snap.find(a);
    REQUIRE(pa != nullptr);
    CHECK(pa->position.x == 1.0f);
    CHECK(pa->bone_count == 0);

    const auto* pb = snap.find(b);
    REQUIRE(pb != nullptr);
    REQUIRE(pb->bone_count == 3);
    CHECK(snap.bones_of(*pb)[2][0] == 2.0f);

    // A second capture into the same snapshot replaces the first.
    entity(sim, a).mark_destroyed();
    osc::sim::capture_world(sim, snap);
    CHECK(snap.entities.size() == 1);
    CHECK(snap.bones.size() == 3);
}

TEST_CASE("WorldHistory keeps the last two ticks and follows the sim's ticks",
          "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    sim.set_tick_observer([&](const SimState& s) { history.capture(s); });

    sim.tick();
    sim.tick();
    CHECK(history.prev().tick == 1);
    CHECK(history.cur().tick == 2);
    CHECK(history.ticks_captured() == 2);

    history.clear();
    CHECK(history.ticks_captured() == 0);
    CHECK(history.cur().entities.empty());
}

// --- FrameView ---------------------------------------------------------------

namespace {

// Two captures with a move of `sim` in between; the unit starts at `from`.
struct TwoTicks {
    LuaGuard g;
    SimState sim{g.L, nullptr};
    WorldHistory history;
    osc::u32 id = 0;

    TwoTicks(Vector3 from, Vector3 to) {
        id = spawn_at(sim, from);
        history.capture(sim);
        entity(sim, id).set_position(to);
        history.capture(sim);
    }
    FrameView view(float alpha) const { return {&history.prev(), &history.cur(), alpha}; }
};

} // namespace

TEST_CASE("FrameView interpolates position between the last two ticks", "[interp][view]") {
    TwoTicks w({0, 0, 0}, {10, 2, -4});
    const Entity& e = entity(w.sim, w.id);

    CHECK(w.view(0.0f).position(e).x == Approx(0.0f));
    const Vector3 mid = w.view(0.5f).position(e);
    CHECK(mid.x == Approx(5.0f));
    CHECK(mid.y == Approx(1.0f));
    CHECK(mid.z == Approx(-2.0f));
    CHECK(w.view(1.0f).position(e).x == Approx(10.0f));
}

TEST_CASE("FrameView turns orientation along the shorter arc", "[interp][view]") {
    TwoTicks w({0, 0, 0}, {0, 0, 0});
    // A quarter turn about Y, stored with the opposite sign in the second
    // tick: q and -q are the same rotation, and the halfway pose must be the
    // eighth turn, not a spin the long way round.
    const float s = std::sqrt(0.5f);
    WorldSnapshot prev = w.history.prev();
    WorldSnapshot cur = w.history.cur();
    prev.entities[0].orientation = {0, 0, 0, 1};
    cur.entities[0].orientation = {0, -s, 0, -s};
    const FrameView view(&prev, &cur, 0.5f);

    const Quaternion q = view.orientation(entity(w.sim, w.id));
    const float eighth = std::sin(0.3926991f); // sin(pi/8)
    CHECK(std::abs(q.y) == Approx(eighth).margin(1e-4));
    CHECK(std::abs(q.w) == Approx(std::cos(0.3926991f)).margin(1e-4));
    CHECK(q.y * q.w > 0.0f);
}

TEST_CASE("FrameView draws a new entity where it is, not where it wasn't", "[interp][view]") {
    TwoTicks w({0, 0, 0}, {10, 0, 0});
    const osc::u32 fresh = spawn_at(w.sim, {50, 0, 50});
    w.history.capture(w.sim); // prev: no `fresh`; cur: `fresh` at 50,50

    const FrameView view(&w.history.prev(), &w.history.cur(), 0.25f);
    CHECK(view.position(entity(w.sim, fresh)).x == Approx(50.0f));
}

TEST_CASE("FrameView jumps a teleported entity instead of sliding it", "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 id = spawn_at(sim, {0, 0, 0});
    history.capture(sim);
    entity(sim, id).set_position({500, 0, 500});
    entity(sim, id).note_snap(); // Warp / SetPosition(pos, true)
    history.capture(sim);

    const FrameView view(&history.prev(), &history.cur(), 0.5f);
    CHECK(view.position(entity(sim, id)).x == Approx(500.0f));
}

TEST_CASE("FrameView jumps an attachment with its teleported parent", "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 parent = spawn_at(sim, {0, 0, 0});
    const osc::u32 child = spawn_at(sim, {0, 0, 0});
    entity(sim, child).set_parent(parent, -1, -1);
    history.capture(sim);

    entity(sim, parent).set_position({500, 0, 500});
    entity(sim, parent).note_snap();
    sim.follow_attachments();
    history.capture(sim);

    const FrameView view(&history.prev(), &history.cur(), 0.5f);
    CHECK(view.position(entity(sim, child)).x == Approx(500.0f));
}

TEST_CASE("A chain of attachments jumps link by link as each one follows", "[interp][view]") {
    // follow_attachments moves one link per tick (a lockstep-safe order), so
    // a grandchild reaches the teleported grandparent a tick after the child.
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 root = spawn_at(sim, {0, 0, 0});
    const osc::u32 mid = spawn_at(sim, {0, 0, 0});
    const osc::u32 leaf = spawn_at(sim, {0, 0, 0});
    entity(sim, mid).set_parent(root, -1, -1);
    entity(sim, leaf).set_parent(mid, -1, -1);
    sim.follow_attachments();
    history.capture(sim);

    entity(sim, root).set_position({500, 0, 500});
    entity(sim, root).note_snap();
    sim.follow_attachments(); // mid jumps
    history.capture(sim);
    sim.follow_attachments(); // leaf jumps
    history.capture(sim);

    const FrameView view(&history.prev(), &history.cur(), 0.5f);
    CHECK(view.position(entity(sim, mid)).x == Approx(500.0f));
    CHECK(view.position(entity(sim, leaf)).x == Approx(500.0f));
}

TEST_CASE("Attaching an entity jumps it onto its parent", "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 parent = spawn_at(sim, {100, 0, 100});
    const osc::u32 child = spawn_at(sim, {0, 0, 0});
    history.capture(sim);

    entity(sim, child).set_parent(parent, -1, -1); // AttachTo / AttachBoneTo
    sim.follow_attachments();
    history.capture(sim);

    const FrameView view(&history.prev(), &history.cur(), 0.5f);
    CHECK(view.position(entity(sim, child)).x == Approx(100.0f));

    // Riding along afterwards interpolates as usual.
    entity(sim, parent).set_position({110, 0, 100});
    sim.follow_attachments();
    history.capture(sim);
    const FrameView riding(&history.prev(), &history.cur(), 0.5f);
    CHECK(riding.position(entity(sim, child)).x == Approx(105.0f));
}

TEST_CASE("FrameView interpolates collision-beam endpoints", "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 id = spawn_at(sim, {0, 0, 0});
    entity(sim, id).set_beam_endpoint({0, 0, 0});
    history.capture(sim);
    entity(sim, id).set_beam_endpoint({0, 0, 20});
    history.capture(sim);

    const FrameView view(&history.prev(), &history.cur(), 0.5f);
    CHECK(view.beam_end(entity(sim, id)).z == Approx(10.0f));
}

TEST_CASE("FrameView blends bone poses between ticks", "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 id = spawn_at(sim, {0, 0, 0});
    unit(sim, id).animated_bone_matrices().assign(2, filled(0.0f));
    history.capture(sim);
    unit(sim, id).animated_bone_matrices().assign(2, filled(4.0f));
    history.capture(sim);

    const FrameView view(&history.prev(), &history.cur(), 0.25f);
    std::vector<std::array<osc::f32, 16>> out;
    REQUIRE(view.bones(id, out));
    REQUIRE(out.size() == 2);
    CHECK(out[1][7] == Approx(1.0f));

    // A pose whose bone count changed (a new mesh) is taken as it is.
    unit(sim, id).animated_bone_matrices().assign(3, filled(8.0f));
    history.capture(sim);
    const FrameView view2(&history.prev(), &history.cur(), 0.25f);
    REQUIRE(view2.bones(id, out));
    REQUIRE(out.size() == 3);
    CHECK(out[0][0] == Approx(8.0f));
}

TEST_CASE("FrameView without history reads the live sim", "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const osc::u32 id = spawn_at(sim, {3, 4, 5});
    const FrameView live;
    CHECK(live.position(entity(sim, id)).z == 5.0f);

    // An entity spawned after the last capture also reads live.
    WorldHistory history;
    history.capture(sim);
    history.capture(sim);
    const osc::u32 late = spawn_at(sim, {9, 9, 9});
    const FrameView view(&history.prev(), &history.cur(), 0.5f);
    CHECK(view.position(entity(sim, late)).x == 9.0f);
    std::vector<std::array<osc::f32, 16>> out;
    CHECK_FALSE(view.bones(late, out));
}

// --- Snap bindings -----------------------------------------------------------

TEST_CASE("Warp and an immediate SetPosition teleport; a plain SetPosition moves",
          "[interp][lua]") {
    osc::lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    osc::lua::register_sim_bindings(lua, sim);
    osc::lua::register_moho_bindings(lua, sim);
    const osc::u32 id = spawn_at(sim, {0, 0, 0});
    Entity& e = entity(sim, id);

    lua_State* L = lua.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &e);
    lua_rawset(L, -3);
    lua_setglobal(L, "ent");

    REQUIRE(lua.do_string("moho.entity_methods.SetPosition(ent, {1, 0, 1})").ok());
    CHECK(e.snap_serial() == 0);
    CHECK(e.position().x == 1.0f);

    REQUIRE(lua.do_string("moho.entity_methods.SetPosition(ent, {2, 0, 2}, true)").ok());
    CHECK(e.snap_serial() == 1);
    CHECK(e.position().x == 2.0f);

    // Warp(entity, position, [orientation]) -- FA's teleport.
    REQUIRE(lua.do_string("Warp(ent, {300, 5, 400}, {0, 1, 0, 0})").ok());
    CHECK(e.snap_serial() == 2);
    CHECK(e.position().z == 400.0f);
    CHECK(e.orientation().y == 1.0f);
    CHECK(e.orientation().w == 0.0f);

    REQUIRE(lua.do_string("Warp(ent, {10, 0, 10})").ok());
    CHECK(e.snap_serial() == 3);
    CHECK(e.orientation().y == 1.0f); // no orientation given: kept
}
