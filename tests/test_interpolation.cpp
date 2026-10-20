// Render interpolation between sim ticks (M190): the tick clock, per-tick
// world snapshots, and the interpolated view the renderer draws from.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/interned_string.hpp"
#include "core/tick_clock.hpp"
#include "core/worker_pool.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/projectile.hpp"
#include "sim/prop.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"
#include "sim/world_snapshot.hpp"

extern "C" {
#include <lua.h>
}

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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

void end_tick(SimState& sim, WorldHistory& history) {
    sim.entity_registry().advance_coords();
    history.capture(sim);
}

std::array<osc::f32, 16> filled(osc::f32 v) {
    std::array<osc::f32, 16> m{};
    m.fill(v);
    return m;
}

} // namespace

// --- TickClock ---------------------------------------------------------------

namespace {

/// A game played at `speed` and 60 fps against the clock: each frame owes
/// the sim its time, and a job of the ticks owed runs on the sim thread for
/// `job_frames(n)` frames (n counting jobs), published as it ends. Returns
/// how the drawn time went once settled: frames it stood still in, and its
/// step's worst stray from the frame's time at the game's speed.
struct Drawn {
    int held = 0;
    double worst_stray = 0.0;
    double behind = 0.0; ///< the newest tick less the drawn time, at the end
};

template <typename JobFrames> Drawn play(double speed, JobFrames job_frames, int frames = 600) {
    TickClock clock(kTick);
    const double frame = speed / 60.0;
    double owed = 0.0;
    int job_frames_left = 0;
    int job_ticks = 0;
    int jobs = 0;
    std::array<double, 3> kept{0.0, 0.0, 0.0}; // older, previous, newest
    clock.published(0.0, 0.0, 1.0);
    Drawn out;
    double last = 0.0;
    for (int f = 0; f < frames; ++f) {
        clock.advance(frame);
        owed += frame;
        const bool job_running = job_frames_left > 0;
        if (job_running) --job_frames_left;
        if (job_running && job_frames_left == 0) {
            kept = {kept[1], kept[2], kept[2] + job_ticks};
            clock.published(kept[2], kept[0], job_ticks);
        }
        if (job_frames_left == 0) {
            job_ticks = static_cast<int>(owed / kTick);
            owed -= job_ticks * kTick;
            job_frames_left = job_ticks > 0 ? job_frames(jobs++) : 0;
        }
        const double drawn = clock.drawn();
        CHECK(drawn >= kept[0]); // always between two published ticks
        CHECK(drawn <= kept[2]);
        if (f >= frames / 4) { // settled
            out.held += drawn <= last ? 1 : 0;
            out.worst_stray = std::max(out.worst_stray, std::abs(drawn - last - frame / kTick));
        }
        last = drawn;
        out.behind = kept[2] - drawn;
    }
    return out;
}

} // namespace

TEST_CASE("TickClock draws one tick a publish evenly, about half a tick behind",
          "[interp][clock]") {
    // Normal speed: a tick every six frames, the drawn time between the
    // last two as Moho draws them
    const Drawn d = play(1.0, [](int) { return 1; });
    CHECK(d.held == 0);
    CHECK(d.worst_stray < 0.02);
    CHECK(d.behind <= 1.0);
}

TEST_CASE("TickClock draws a sped-up game's jobs of ticks on evenly, never standing still",
          "[interp][clock]") {
    // 4x: each frame owes two thirds of a tick, run as jobs of three frames
    // (two ticks each). Drawn up to the newest at the game's speed, it
    // stood still a frame in three, then leapt.
    const Drawn even = play(4.0, [](int) { return 3; });
    CHECK(even.held == 0);
    CHECK(even.worst_stray < 0.1);
    // And jobs of one frame and two in turn (one tick, then two, as the
    // engine's own trace showed): still no frame standing still
    const Drawn uneven = play(4.0, [](int n) { return 1 + n % 2; });
    CHECK(uneven.held == 0);
    CHECK(uneven.worst_stray < 0.25);
}

TEST_CASE("TickClock slows to the pace of a sim that can't keep up, never standing still",
          "[interp][clock]") {
    // Asked 4x, a late game's sim makes 1x: a job of the ticks owed (at
    // most 8, the rest let go, as the engine does) takes four frames a
    // tick. Held to half the asked pace at the slowest, the drawn time ran
    // into the newest tick and stood (WOPC's long run, 2026-10-19: 45% of
    // frames held from 35 minutes on).
    TickClock clock(kTick);
    const double frame = 4.0 / 60.0;
    double owed = 0.0;
    int job_frames_left = 0;
    int job_ticks = 0;
    std::array<double, 3> kept{0.0, 0.0, 0.0};
    clock.published(0.0, 0.0, 1.0);
    double last = 0.0;
    double last_step = -1.0;
    int held = 0;
    double worst_change = 0.0;
    for (int f = 0; f < 2400; ++f) {
        clock.advance(frame);
        owed += frame;
        const bool job_running = job_frames_left > 0;
        if (job_running) --job_frames_left;
        if (job_running && job_frames_left == 0) {
            kept = {kept[1], kept[2], kept[2] + job_ticks};
            clock.published(kept[2], kept[0], job_ticks);
        }
        if (job_frames_left == 0) {
            job_ticks = std::min(static_cast<int>(owed / kTick), 8);
            if (job_ticks > 0) owed = 0.0; // what the job doesn't take is let go
            job_frames_left = job_ticks * 4;
        }
        const double drawn = clock.drawn();
        if (f >= 1200) { // settled
            const double step = drawn - last;
            held += step <= 0.0 ? 1 : 0;
            if (last_step >= 0.0) worst_change = std::max(worst_change, std::abs(step - last_step));
            last_step = step;
        }
        last = drawn;
    }
    CHECK(held == 0);
    CHECK(worst_change < 0.05); // a quarter tick a frame, steady
}

TEST_CASE("TickClock holds at the newest tick through a stall and recovers", "[interp][clock]") {
    TickClock clock(kTick);
    clock.published(10.0, 9.0, 1.0);
    // "Waiting for players": a second passes and no tick arrives
    for (int i = 0; i < 60; ++i) clock.advance(1.0 / 60.0);
    CHECK(clock.drawn() == Approx(10.0));
    // The ticks come again: the drawn time goes on, faster for a while,
    // until it is back behind the newest
    double t = 10.0;
    for (int i = 0; i < 120; ++i) {
        clock.advance(1.0 / 60.0);
        if (i % 6 == 5) {
            t += 1.0;
            clock.published(t, t - 2.0, 1.0);
        }
    }
    CHECK(clock.drawn() < t);
    CHECK(clock.drawn() > t - 1.5);
}

TEST_CASE("TickClock stays put while paused and resets on a new session", "[interp][clock]") {
    TickClock clock(kTick);
    clock.published(5.0, 3.0, 1.0);
    clock.advance(0.03);
    // Paused frames don't advance the clock
    const double paused = clock.drawn();
    CHECK(clock.drawn() == paused);
    clock.reset();
    CHECK(clock.drawn() == 0.0);
    clock.advance(0.05); // nothing published yet: nothing to draw between
    CHECK(clock.drawn() == 0.0);
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
    sim.set_beat_observer([&](const SimState& s) { history.capture(s); });

    sim.tick();
    sim.tick();
    CHECK(history.prev().tick == 1);
    CHECK(history.cur().tick == 2);
    CHECK(history.ticks_captured() == 2);

    history.clear();
    CHECK(history.ticks_captured() == 0);
    CHECK(history.cur().entities.empty());
}

TEST_CASE("A capture staged on the sim thread shows only once published (OE-134)",
          "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 id = spawn_at(sim, {1, 0, 1});
    sim.set_tick_observer([&](const SimState& s) { history.capture(s); });
    sim.tick(); // tick 1, published at once
    sim.tick(); // tick 2

    // The sim thread's next two ticks: staged, the frame's two untouched
    sim.set_tick_observer([&](const SimState& s) { history.capture_staged(s); });
    entity(sim, id).set_position({5, 0, 5});
    sim.tick();
    sim.tick();
    CHECK(history.prev().tick == 1);
    CHECK(history.cur().tick == 2);
    CHECK(history.cur().find(id)->position.x == 1.0f);
    CHECK(history.ticks_captured() == 2);

    // Published between frames: the newest is current, the last current
    // previous, and each record knows its previous self
    CHECK(history.publish());
    CHECK(history.older().tick == 1); // kept, for a frame drawn behind
    CHECK(history.prev().tick == 2);
    CHECK(history.cur().tick == 4);
    CHECK(history.cur().find(id)->position.x == 5.0f);
    CHECK(history.cur().previous_serial == history.prev().serial);
    CHECK(history.ticks_captured() == 3);
    CHECK_FALSE(history.publish()); // nothing new
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
        end_tick(sim, history);
        entity(sim, id).set_position(to);
        end_tick(sim, history);
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
    end_tick(w.sim, w.history); // prev: no `fresh`; cur: `fresh` at 50,50

    const FrameView view(&w.history.prev(), &w.history.cur(), 0.25f);
    CHECK(view.position(entity(w.sim, fresh)).x == Approx(50.0f));
}

TEST_CASE("FrameView draws a projectile's first step from where it was made", "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    end_tick(sim, history);
    auto made = std::make_unique<osc::sim::Projectile>();
    made->set_position({100, 30, 100});
    made->velocity = {35, 0, 0};
    const osc::u32 id = sim.entity_registry().register_entity(std::move(made));
    sim.tick();
    end_tick(sim, history);

    const auto x = [&](float alpha) {
        return FrameView(&history.prev(), &history.cur(), alpha).position(entity(sim, id)).x;
    };
    CHECK(x(0.0f) == Approx(100.0f));
    CHECK(x(0.5f) == Approx(101.75f));
    CHECK(x(1.0f) == Approx(103.5f));
}

TEST_CASE("FrameView draws a projectile reaching its impact point at the share of the step",
          "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    sim.set_beat_observer([&](const SimState& s) { history.capture(s); });
    const osc::u32 target = spawn_at(sim, {110, 10, 100});
    osc::sim::CollisionShape box;
    box.type = osc::sim::CollisionShapeType::BOX;
    box.cy = 1;
    box.sx = 1;
    box.sy = 1;
    box.sz = 1;
    entity(sim, target).set_collision_shape(box);
    auto made = std::make_unique<osc::sim::Projectile>();
    made->set_position({100, 11, 100});
    made->velocity = {35, 0, 0};
    const osc::u32 id = sim.entity_registry().register_entity(std::move(made));
    for (int t = 0; t < 3; ++t) {
        sim.tick();
    }
    REQUIRE(entity(sim, id).position().x == Approx(109.0f));

    const auto x = [&](float alpha) {
        return FrameView(&history.prev(), &history.cur(), alpha).position(entity(sim, id)).x;
    };
    const float share = (109.0f - (107.0f - 0.35f)) / 3.5f;
    CHECK(x(0.0f) == Approx(107.0f));
    CHECK(x(share * 0.5f) == Approx(108.0f));
    CHECK(x(share) == Approx(109.0f));
    CHECK(x(1.0f) == Approx(109.0f));
}

TEST_CASE("FrameView jumps a teleported entity instead of sliding it", "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 id = spawn_at(sim, {0, 0, 0});
    end_tick(sim, history);
    entity(sim, id).set_position({500, 0, 500});
    entity(sim, id).note_snap(); // Warp / SetPosition(pos, true)
    end_tick(sim, history);

    const FrameView view(&history.prev(), &history.cur(), 0.5f);
    CHECK(view.position(entity(sim, id)).x == Approx(500.0f));
}

TEST_CASE("FrameView pairs records through the history's index as a search would",
          "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 moving = spawn_at(sim, {0, 0, 0});
    const osc::u32 dying = spawn_at(sim, {5, 0, 5});
    const osc::u32 warped = spawn_at(sim, {9, 0, 9});
    end_tick(sim, history);
    entity(sim, moving).set_position({10, 0, 0});
    entity(sim, dying).mark_destroyed();
    entity(sim, warped).set_position({500, 0, 500});
    entity(sim, warped).note_snap();
    const osc::u32 fresh = spawn_at(sim, {50, 0, 50});
    end_tick(sim, history);
    REQUIRE(history.cur().previous_serial == history.prev().serial);

    // The same pair, the index dropped: a search.
    WorldSnapshot prev = history.prev();
    WorldSnapshot cur = history.cur();
    cur.previous.clear();
    const FrameView indexed(&history.prev(), &history.cur(), 0.5f);
    const FrameView searched(&prev, &cur, 0.5f);
    for (const osc::u32 id : {moving, warped, fresh}) {
        const auto* a = indexed.find(id);
        const auto* b = searched.find(id);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        CHECK(indexed.position(*a).x == Approx(searched.position(*b).x));
        CHECK(indexed.position(*a).z == Approx(searched.position(*b).z));
    }
    CHECK(indexed.position(*indexed.find(moving)).x == Approx(5.0f));
    CHECK(indexed.position(*indexed.find(warped)).x == Approx(500.0f));
    CHECK(indexed.position(*indexed.find(fresh)).x == Approx(50.0f));
}

TEST_CASE("A capture's record reused for another entity keeps nothing of the last one's",
          "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const osc::u32 builder = spawn_at(sim, {1, 0, 1});
    Unit& u = unit(sim, builder);
    u.set_unit_id("uel0105");
    u.set_blueprint_id("uel0105");
    u.set_custom_name("Bob");
    u.set_vet_level(3);
    u.set_build_target_id(77);
    u.animated_bone_matrices().assign(2, filled(1.0f));
    auto rock = std::make_unique<Entity>();
    rock->set_position({9, 0, 9});
    rock->set_blueprint_id("/env/common/props/rocks/rock01_prop.bp");
    const osc::u32 prop = sim.entity_registry().register_entity(std::move(rock));

    WorldSnapshot snap;
    osc::sim::capture_world(sim, snap);
    REQUIRE(snap.entities.size() == 2);
    CHECK(snap.entities[0].custom_name == "Bob");
    CHECK(snap.entities[0].build_target_id == 77);

    // The builder goes: the rock takes its record, which must hold no unit's
    entity(sim, builder).mark_destroyed();
    osc::sim::capture_world(sim, snap);
    REQUIRE(snap.entities.size() == 1);
    const auto& r = snap.entities[0];
    CHECK(r.id == prop);
    CHECK_FALSE(r.is_unit);
    CHECK(r.blueprint_id == "/env/common/props/rocks/rock01_prop.bp");
    CHECK(r.unit_id.empty());
    CHECK(r.custom_name.empty());
    CHECK(r.vet_level == 0);
    CHECK(r.build_target_id == 0);
    CHECK(r.bone_count == 0);
    CHECK(r.position.x == 9.0f);
}

TEST_CASE("A prop's record reused for another prop keeps no pose of the last one's",
          "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto wreck = std::make_unique<osc::sim::Prop>();
    wreck->set_blueprint_id("/env/wrecks/uel0101_wreck.bp");
    wreck->pose.assign(2, filled(3.0f)); // the pose its unit died in
    const osc::u32 posed = sim.entity_registry().register_entity(std::move(wreck));
    auto tree = std::make_unique<osc::sim::Prop>();
    tree->set_blueprint_id("/env/evergreen/props/trees/pine06_prop.bp");
    tree->set_position({4, 0, 4});
    const osc::u32 plain = sim.entity_registry().register_entity(std::move(tree));

    WorldSnapshot snap;
    osc::sim::capture_world(sim, snap);
    REQUIRE(snap.entities.size() == 2);
    CHECK(snap.entities[0].bone_count == 2);

    // The wreck goes: the tree takes its record
    entity(sim, posed).mark_destroyed();
    osc::sim::capture_world(sim, snap);
    REQUIRE(snap.entities.size() == 1);
    const auto& r = snap.entities[0];
    CHECK(r.id == plain);
    CHECK(r.is_prop);
    CHECK(r.bone_count == 0);
    CHECK(r.bone_offset == 0);
    CHECK(r.blueprint_id == "/env/evergreen/props/trees/pine06_prop.bp");
    CHECK(r.position.x == 4.0f);
}

TEST_CASE("An interned string is one copy per text", "[interp][snapshot]") {
    const osc::InternedString a("uel0105");
    const osc::InternedString b(std::string("uel") + "0105");
    CHECK(a == b);
    CHECK(&a.str() == &b.str()); // the same copy
    CHECK(a == "uel0105");
    CHECK_FALSE(a == osc::InternedString("uel0106"));
    CHECK(osc::InternedString().empty());
    CHECK(&osc::InternedString("").str() == &osc::InternedString().str()); // one empty
    CHECK(fmt::format("{}", a) == "uel0105");
}

TEST_CASE("An entity counts its mesh override's changes", "[interp][snapshot]") {
    Unit u;
    CHECK(u.mesh_changes() == 0);
    u.set_mesh_override("/units/ueb1101/ueb1101_mesh_build");
    u.set_mesh_override("/units/ueb1101/ueb1101_mesh_build");
    CHECK(u.mesh_changes() == 1);
    u.set_mesh_override("");
    CHECK(u.mesh_changes() == 2);
}

TEST_CASE("FrameView jumps an attachment with its teleported parent", "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 parent = spawn_at(sim, {0, 0, 0});
    const osc::u32 child = spawn_at(sim, {0, 0, 0});
    entity(sim, child).set_parent(parent, -1, -1);
    end_tick(sim, history);

    entity(sim, parent).set_position({500, 0, 500});
    entity(sim, parent).note_snap();
    sim.tick();
    end_tick(sim, history);

    const FrameView view(&history.prev(), &history.cur(), 0.5f);
    CHECK(view.position(entity(sim, child)).x == Approx(500.0f));
}

TEST_CASE("A chain of attachments jumps with its teleported root", "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 root = spawn_at(sim, {0, 0, 0});
    const osc::u32 mid = spawn_at(sim, {0, 0, 0});
    const osc::u32 leaf = spawn_at(sim, {0, 0, 0});
    entity(sim, mid).set_parent(root, -1, -1);
    entity(sim, leaf).set_parent(mid, -1, -1);
    sim.tick();
    end_tick(sim, history);

    entity(sim, root).set_position({500, 0, 500});
    entity(sim, root).note_snap();
    sim.tick();
    end_tick(sim, history);

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
    end_tick(sim, history);

    entity(sim, child).set_parent(parent, -1, -1); // AttachTo / AttachBoneTo
    sim.tick();
    end_tick(sim, history);

    const FrameView view(&history.prev(), &history.cur(), 0.5f);
    CHECK(view.position(entity(sim, child)).x == Approx(100.0f));

    // Riding along afterwards interpolates as usual.
    entity(sim, parent).set_position({110, 0, 100});
    sim.tick();
    end_tick(sim, history);
    const FrameView riding(&history.prev(), &history.cur(), 0.5f);
    CHECK(riding.position(entity(sim, child)).x == Approx(105.0f));
}

TEST_CASE("FrameView interpolates collision-beam endpoints", "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 id = spawn_at(sim, {0, 0, 0});
    entity(sim, id).set_beam_endpoint({0, 0, 0});
    end_tick(sim, history);
    entity(sim, id).set_beam_endpoint({0, 0, 20});
    end_tick(sim, history);

    const FrameView view(&history.prev(), &history.cur(), 0.5f);
    CHECK(view.beam_end(entity(sim, id)).z == Approx(10.0f));
}

TEST_CASE("FrameView blends bone poses between ticks", "[interp][view]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 id = spawn_at(sim, {0, 0, 0});
    unit(sim, id).animated_bone_matrices().assign(2, filled(0.0f));
    end_tick(sim, history);
    unit(sim, id).animated_bone_matrices().assign(2, filled(4.0f));
    end_tick(sim, history);

    const FrameView view(&history.prev(), &history.cur(), 0.25f);
    std::vector<std::array<osc::f32, 16>> out;
    REQUIRE(view.bones(id, out));
    REQUIRE(out.size() == 2);
    CHECK(out[1][7] == Approx(1.0f));

    // A pose whose bone count changed (a new mesh) is taken as it is.
    unit(sim, id).animated_bone_matrices().assign(3, filled(8.0f));
    end_tick(sim, history);
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
    end_tick(sim, history);
    end_tick(sim, history);
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
    CHECK(e.position().x == 0.0f);
    sim.entity_registry().advance_coords();
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

// --- The rest of the snapshot (M190b) ----------------------------------------

TEST_CASE("capture_world records what the renderer draws a unit with", "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.add_army("ARMY_1", "ARMY_1").set_color(10, 20, 30);
    sim.add_army("ARMY_2", "ARMY_2");

    const osc::u32 target = spawn_at(sim, {5, 0, 5});
    const osc::u32 id = spawn_at(sim, {1, 0, 1});
    Unit& u = unit(sim, id);
    u.set_unit_id("uel0105");
    u.set_custom_name("Bob");
    u.add_category("ENGINEER");
    u.add_category("LAND");
    u.set_max_health(200);
    u.set_health(150);
    u.set_is_being_built(true);
    u.set_fraction_complete(0.5f);
    u.set_build_target_id(target);
    u.set_work_progress(0.25f);
    u.set_vet_level(2);
    u.add_cargo(target);
    u.give_nuke_silo_ammo(3);
    u.add_adjacent(target);
    u.add_intel("Radar", 40);
    u.enable_intel("Radar");
    u.add_intel("Sonar", 30); // off: ringed all the same
    u.add_intel("Omni", 0.5f);
    u.enable_intel("Omni"); // too small to ring
    osc::sim::UnitCommand move{};
    move.type = osc::sim::CommandType::Move;
    move.target_pos = {9, 0, 9};
    u.push_command(move, true);
    osc::sim::UnitCommand attack{};
    attack.type = osc::sim::CommandType::Attack;
    attack.target_id = target;
    u.push_command(attack, false);
    // Its second blueprint weapon, its reach changed (ChangeMaxRadius)
    auto gun = std::make_unique<osc::sim::Weapon>();
    gun->weapon_index = 1;
    gun->min_range = 3;
    gun->max_range = 26;
    u.add_weapon(std::move(gun));
    u.set_script_bit(3, true);
    sim.army_at(0)->set_start_position({100, 0, 200});
    sim.set_no_rush(300, 60);

    WorldSnapshot snap;
    osc::sim::capture_world(sim, snap);
    const auto* r = snap.find(id);
    REQUIRE(r != nullptr);
    CHECK(r->is_unit);
    CHECK(r->army == 0);
    CHECK(r->unit_id == "uel0105");
    CHECK(r->custom_name == "Bob");
    CHECK(r->icon == osc::sim::IconClass::Engineer);
    CHECK(r->health == 150.0f);
    CHECK(r->max_health == 200.0f);
    CHECK(r->is_being_built);
    CHECK(r->fraction_complete == 0.5f);
    CHECK(r->is_building());
    CHECK(r->build_target_id == target);
    CHECK(r->work_progress == 0.25f);
    CHECK(r->vet_level == 2);
    CHECK(r->cargo_count == 1);
    CHECK(r->nuke_silo_ammo == 3);

    const auto cmds = snap.commands_of(*r);
    REQUIRE(cmds.size() == 2);
    CHECK(cmds[0].type == osc::sim::CommandType::Move);
    CHECK(cmds[0].target_pos.x == 9.0f);
    CHECK(cmds[1].target_id == target);

    // Radar on and sonar off: both ringable; omni's too small.
    const auto intel = snap.intel_of(*r);
    REQUIRE(intel.size() == 2);
    const auto& radar = intel[0].type == "Radar" ? intel[0] : intel[1];
    const auto& sonar = intel[0].type == "Radar" ? intel[1] : intel[0];
    CHECK(radar.type == "Radar");
    CHECK(radar.radius == 40.0f);
    CHECK(radar.enabled);
    CHECK(sonar.type == "Sonar");
    CHECK(sonar.radius == 30.0f);
    CHECK_FALSE(sonar.enabled);

    const auto adj = snap.adjacent_of(*r);
    REQUIRE(adj.size() == 1);
    CHECK(adj[0] == target);

    const auto reach = snap.weapon_ranges_of(*r);
    REQUIRE(reach.size() == 1);
    CHECK(reach[0].index == 1);
    CHECK(reach[0].min_radius == 3.0f);
    CHECK(reach[0].max_radius == 26.0f);
    CHECK(r->script_bits == 8);
    CHECK(snap.armies[0].start_x == 100.0f);
    CHECK(snap.armies[0].start_z == 200.0f);
    CHECK(snap.no_rush_radius == 60.0f);

    REQUIRE(snap.armies.size() == 2);
    CHECK(snap.armies[0].valid);
    CHECK(snap.armies[0].has_color);
    CHECK(snap.armies[0].g == 20);
    CHECK_FALSE(snap.armies[1].has_color);
    CHECK(snap.army(2) == nullptr);
}

TEST_CASE("capture_world records shields, live effects and the economy", "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    auto& brain = sim.add_army("ARMY_1", "ARMY_1");
    brain.economy().mass.stored = 123;
    brain.economy().energy.max_storage = 5000;

    const osc::u32 owner = spawn_at(sim, {0, 0, 0});
    auto shield = std::make_unique<osc::sim::Shield>();
    shield->owner_id = owner;
    shield->is_on = true;
    shield->size = 18;
    const osc::u32 shield_id = sim.entity_registry().register_entity(std::move(shield));

    auto* beam = sim.effect_registry().create();
    beam->set_type(osc::sim::EffectType::ATTACHED_BEAM);
    beam->set_entity_id(owner);
    beam->set_param("LENGTH", 12);
    beam->set_param("THICKNESS", 3);
    auto* gone = sim.effect_registry().create();
    gone->mark_destroyed();

    WorldSnapshot snap;
    osc::sim::capture_world(sim, snap);

    const auto* s = snap.find(shield_id);
    REQUIRE(s != nullptr);
    CHECK(s->is_shield);
    CHECK(s->shield_on);
    CHECK(s->shield_owner_id == owner);
    CHECK(s->shield_size == 18.0f);

    REQUIRE(snap.effects.size() == 1);
    CHECK(snap.effects[0].id == beam->id());
    CHECK(snap.effects[0].type == osc::sim::EffectType::ATTACHED_BEAM);
    CHECK(snap.effects[0].entity_id == owner);
    CHECK(snap.effects[0].length == 12.0f);
    CHECK(snap.effects[0].thickness == 3.0f);

    CHECK(snap.armies[0].mass.stored == 123.0);
    CHECK(snap.armies[0].energy.max_storage == 5000.0);
}

TEST_CASE("Death flashes and camera shakes reach the renderer once", "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    sim.set_beat_observer([&](const SimState& s) { history.capture(s); });

    // Raised between ticks (a SimCallback): shown with the next tick.
    sim.add_death_event(1, 2, 3, 4, 0);
    osc::sim::CameraShakeEvent shake;
    shake.x = 7;
    sim.add_camera_shake(shake);
    sim.tick();
    REQUIRE(history.events().deaths.size() == 1);
    CHECK(history.events().deaths[0].z == 3.0f);
    REQUIRE(history.events().shakes.size() == 1);
    CHECK(history.events().shakes[0].x == 7.0f);
    // The sim is done with them once the tick is captured.
    CHECK(sim.death_events().empty());
    CHECK(sim.camera_shake_events().empty());

    // Two ticks before the renderer looks: both ticks' events wait for it.
    sim.add_death_event(5, 5, 5, 1, 0);
    sim.tick();
    CHECK(history.events().deaths.size() == 2);
    history.events().clear(); // the renderer took them
    sim.tick();
    CHECK(history.events().deaths.empty());
}

TEST_CASE("A capture shared over the worker pool is the one a walk in id order makes",
          "[interp][snapshot][pool]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    // Enough units for many chunks, each with orders and a weapon (lists the
    // records point into), between props with poses
    for (int i = 0; i < 3000; ++i) {
        const osc::u32 id =
            spawn_at(sim, {static_cast<float>(i % 200), 0, static_cast<float>(i / 200)});
        auto& u = unit(sim, id);
        for (int c = 0; c < i % 4; ++c) {
            osc::sim::UnitCommand move;
            move.type = osc::sim::CommandType::Move;
            move.target_pos = {static_cast<float>(c), 0, static_cast<float>(i)};
            u.push_command(move, false);
        }
        if (i % 7 == 0) {
            auto prop = std::make_unique<osc::sim::Prop>();
            prop->pose.assign(static_cast<size_t>(1 + i % 3), osc::sim::BoneMatrix{});
            sim.entity_registry().register_entity(std::move(prop));
        }
    }
    const auto capture = [&](bool pool) {
        osc::WorkerPool::instance().set_enabled(pool);
        WorldSnapshot snap;
        osc::sim::capture_world(sim, snap, -1);
        osc::WorkerPool::instance().set_enabled(true);
        return snap;
    };
    const WorldSnapshot shared = capture(true);
    const WorldSnapshot alone = capture(false);
    REQUIRE(shared.entities.size() == alone.entities.size());
    for (size_t i = 0; i < alone.entities.size(); ++i) {
        const auto& a = shared.entities[i];
        const auto& b = alone.entities[i];
        const bool same =
            a.id == b.id && a.position.x == b.position.x && a.is_unit == b.is_unit &&
            a.command_offset == b.command_offset && a.command_count == b.command_count &&
            a.rally_offset == b.rally_offset && a.bone_offset == b.bone_offset &&
            a.bone_count == b.bone_count && a.weapon_range_offset == b.weapon_range_offset &&
            a.intel_offset == b.intel_offset && a.adjacent_offset == b.adjacent_offset;
        CHECK(same);
        if (!same) break;
    }
    CHECK(shared.commands.size() == alone.commands.size());
    CHECK(shared.bones.size() == alone.bones.size());
    for (size_t i = 0; i < alone.commands.size(); ++i) {
        CHECK(shared.commands[i].target_pos.z == alone.commands[i].target_pos.z);
        if (shared.commands[i].target_pos.z != alone.commands[i].target_pos.z) break;
    }
    // And a record finds its own orders
    const auto& last = alone.entities.back();
    CHECK(last.command_offset + last.command_count <= alone.commands.size());
}

TEST_CASE("A job of several ticks is captured once, with every tick's events",
          "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    WorldHistory history;
    const osc::u32 id = spawn_at(sim, {1, 0, 1});
    sim.set_tick_observer([&](const SimState& s) { history.capture(s); });
    sim.tick(); // tick 1

    // The sim thread's job of three ticks: each tick's events kept, the
    // world captured once, as the last left it
    sim.set_tick_observer([&](const SimState& s) { history.stage_events(s); });
    for (int t = 0; t < 3; ++t) {
        sim.add_death_event(static_cast<osc::f32>(t), 0, 0, 1, 0);
        entity(sim, id).set_position({static_cast<osc::f32>(t + 2), 0, 1});
        sim.tick();
    }
    history.stage_world(sim);
    REQUIRE(history.publish());
    CHECK(history.prev().tick == 1);
    CHECK(history.cur().tick == 4);
    CHECK(history.cur().find(id)->position.x == 4.0f);
    CHECK(history.cur().previous_serial == history.prev().serial);
    REQUIRE(history.events().deaths.size() == 3);
    CHECK(history.events().deaths[2].x == 2.0f);

    // Drawn halfway across the job, it is halfway across its three ticks
    const FrameView view(&history.prev(), &history.cur(), 0.5f);
    CHECK(view.time() == Approx(3.5));
    CHECK(view.position(*history.cur().find(id)).x == Approx(2.5f));
}

TEST_CASE("A light reaches the renderer where its entity was and with its particle textures",
          "[interp][snapshot][lua]") {
    osc::lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    osc::lua::register_sim_bindings(lua, sim);
    osc::lua::register_moho_bindings(lua, sim);
    const osc::u32 id = spawn_at(sim, {4, 5, 6});

    lua_State* L = lua.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &entity(sim, id));
    lua_rawset(L, -3);
    lua_setglobal(L, "ent");

    REQUIRE(lua.do_string("CreateLightParticle(ent, -1, 1, 7, 8, 'glow_02', 'ramp_blue_22')\n"
                          "CreateLightParticleIntel(ent, -1, 1, 4, 6, '', 'ramp_flare_02')\n"
                          "CreateLightParticle(ent, -1, 1, 4, 6, 'glow_02', '')")
                .ok());
    entity(sim, id).set_position({40, 5, 60});
    WorldSnapshot snap;
    osc::sim::capture_world(sim, snap);
    REQUIRE(snap.effects.size() == 2);

    const osc::sim::EffectRecord& light = snap.effects[0];
    CHECK(light.type == osc::sim::EffectType::LIGHT_PARTICLE);
    CHECK(light.framed);
    CHECK(light.frame_position.x == 4.0f);
    CHECK(light.frame_position.z == 6.0f);
    CHECK(light.light_size == 7.0f);
    CHECK(light.light_lifetime == 8.0f);
    CHECK(light.glow_texture == "/textures/particles/glow_02.dds");
    CHECK(light.ramp_texture == "/textures/particles/ramp_blue_22.dds");

    const osc::sim::EffectRecord& intel = snap.effects[1];
    CHECK(intel.type == osc::sim::EffectType::LIGHT_PARTICLE_INTEL);
    CHECK(intel.glow_texture == "/textures/particles/beam_white_01.dds");
}

TEST_CASE("An order given and not yet run is in its unit's orders before its tick",
          "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const osc::u32 id = spawn_at(sim, {10, 0, 10});
    osc::sim::UnitCommand move;
    move.type = osc::sim::CommandType::Move;
    move.target_pos = {20, 0, 10};
    unit(sim, id).push_command(move, true);
    WorldHistory history;
    end_tick(sim, history);
    const auto orders = [&] {
        history.refresh_pending(sim);
        std::vector<osc::f32> xs;
        for (const auto& c : history.cur().orders_of(*history.cur().find(id))) {
            xs.push_back(c.target_pos.x);
        }
        return xs;
    };

    move.target_pos = {30, 0, 10};
    sim.schedule_command(0, {id}, move, false);
    CHECK(orders() == std::vector<osc::f32>{20, 30});
    CHECK(history.cur().commands_of(*history.cur().find(id)).size() == 1);

    move.target_pos = {40, 0, 10};
    sim.schedule_command(0, {id}, move, true);
    CHECK(orders() == std::vector<osc::f32>{40});

    osc::sim::UnitCommand stop;
    stop.type = osc::sim::CommandType::Stop;
    sim.schedule_command(0, {id}, stop, true);
    CHECK(orders().empty());
    CHECK(unit(sim, id).command_queue().size() == 1);

    sim.set_playback(true);
    sim.schedule_command(0, {id}, move, false);
    CHECK(orders() == std::vector<osc::f32>{20});
}

TEST_CASE("An order moved or removed and not yet run is so in its unit's orders before its tick",
          "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const osc::u32 id = spawn_at(sim, {10, 0, 10});
    osc::sim::UnitCommand move;
    move.type = osc::sim::CommandType::Move;
    move.target_pos = {20, 0, 10};
    move.command_id = 7;
    unit(sim, id).push_command(move, true);
    move.target_pos = {30, 0, 10};
    move.command_id = 8;
    unit(sim, id).push_command(move, false);
    WorldHistory history;
    end_tick(sim, history);
    const auto orders = [&] {
        history.refresh_pending(sim);
        std::vector<std::pair<osc::u32, osc::f32>> out;
        for (const auto& c : history.cur().orders_of(*history.cur().find(id))) {
            out.emplace_back(c.command_id, c.target_pos.x);
        }
        return out;
    };
    const auto edit = [&](const char* func, osc::u32 command) {
        osc::sim::SimCallbackEntry cb;
        cb.func_name = func;
        cb.args["Command"] = static_cast<osc::f64>(command);
        cb.args["X"] = 50.0;
        cb.args["Y"] = 0.0;
        cb.args["Z"] = 10.0;
        cb.unit_ids = {id};
        return cb;
    };

    sim.schedule_callback(0, edit(osc::sim::kSetCommandTargetCallback, 8));
    using Shown = std::vector<std::pair<osc::u32, osc::f32>>;
    CHECK(orders() == Shown{{7, 20.0f}, {8, 50.0f}});
    CHECK(unit(sim, id).command_queue().back().target_pos.x == 30.0f);

    sim.schedule_callback(0, edit(osc::sim::kRemoveCommandCallback, 7));
    CHECK(orders() == Shown{{8, 50.0f}});
    CHECK(unit(sim, id).command_queue().size() == 2);
}

TEST_CASE("A patrol's break-off isn't in its unit's orders before or after a new order",
          "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const osc::u32 id = spawn_at(sim, {10, 0, 10});
    osc::sim::UnitCommand patrol;
    patrol.type = osc::sim::CommandType::Patrol;
    patrol.target_pos = {20, 0, 10};
    patrol.command_id = 7;
    osc::sim::UnitCommand reclaim;
    reclaim.type = osc::sim::CommandType::Reclaim;
    reclaim.target_pos = {12, 0, 14};
    reclaim.command_id = 7;
    reclaim.from_patrol = true;
    unit(sim, id).push_command(reclaim, true);
    unit(sim, id).append_command(patrol);
    WorldHistory history;
    end_tick(sim, history);
    const auto types = [&](std::span<const osc::sim::CommandRecord> orders) {
        std::vector<osc::sim::CommandType> out;
        for (const auto& c : orders) {
            out.push_back(c.type);
        }
        return out;
    };
    const auto* r = history.cur().find(id);
    using Types = std::vector<osc::sim::CommandType>;
    CHECK(types(history.cur().commands_of(*r)) == Types{osc::sim::CommandType::Patrol});

    osc::sim::UnitCommand move;
    move.type = osc::sim::CommandType::Move;
    move.target_pos = {30, 0, 10};
    sim.schedule_command(0, {id}, move, false);
    history.refresh_pending(sim);
    CHECK(types(history.cur().orders_of(*history.cur().find(id))) ==
          Types{osc::sim::CommandType::Patrol, osc::sim::CommandType::Move});
}

TEST_CASE("A player's order not yet run has its id, and is taken off by it", "[interp][snapshot]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    const osc::u32 id = spawn_at(sim, {10, 0, 10});
    osc::sim::UnitCommand move;
    move.type = osc::sim::CommandType::Move;
    move.target_pos = {20, 0, 10};
    sim.set_human_input_active(true);
    sim.route_player_command({id}, move, true);
    move.target_pos = {30, 0, 10};
    sim.route_player_command({id}, move, false);
    sim.set_human_input_active(false);
    WorldHistory history;
    end_tick(sim, history);
    history.refresh_pending(sim);
    const auto shown = history.cur().orders_of(*history.cur().find(id));
    REQUIRE(shown.size() == 2);
    CHECK(shown[0].command_id != 0);
    CHECK(shown[1].command_id != 0);
    CHECK(shown[0].command_id != shown[1].command_id);

    osc::sim::SimCallbackEntry remove;
    remove.func_name = osc::sim::kRemoveCommandCallback;
    remove.args["Command"] = static_cast<osc::f64>(shown[0].command_id);
    remove.unit_ids = {id};
    sim.schedule_callback(0, remove);
    history.refresh_pending(sim);
    const auto left = history.cur().orders_of(*history.cur().find(id));
    REQUIRE(left.size() == 1);
    CHECK(left[0].target_pos.x == 30.0f);
}

TEST_CASE("Reused captures clear projectile launch and impact state", "[interp][snapshot][reuse]") {
    LuaGuard g;
    SimState sim(g.L, nullptr);
    sim.tick();
    auto projectile = std::make_unique<osc::sim::Projectile>();
    projectile->launch_tick = sim.tick_count();
    projectile->launch_position = {99, 0, 0};
    projectile->impact_interpolation = 0.25f;
    auto* p = projectile.get();
    const auto id = sim.entity_registry().register_entity(std::move(projectile));
    WorldSnapshot snap;
    osc::sim::capture_world(sim, snap);
    REQUIRE(snap.find(id));
    CHECK(snap.find(id)->launched);
    CHECK(snap.find(id)->step_scale == 4.0f);
    p->launch_tick = 0;
    p->impact_interpolation = -1.0f;
    osc::sim::capture_world(sim, snap);
    CHECK_FALSE(snap.find(id)->launched);
    CHECK(snap.find(id)->step_scale == 1.0f);
    // A different kind takes the same vector slot, after a launch/impact.
    p->launch_tick = sim.tick_count();
    p->impact_interpolation = 0.5f;
    osc::sim::capture_world(sim, snap);
    sim.entity_registry().unregister_entity(id);
    auto prop = std::make_unique<osc::sim::Prop>();
    const auto prop_id = sim.entity_registry().register_entity(std::move(prop));
    osc::sim::capture_world(sim, snap);
    REQUIRE(snap.find(prop_id));
    CHECK_FALSE(snap.find(prop_id)->launched);
    CHECK(snap.find(prop_id)->step_scale == 1.0f);
}
