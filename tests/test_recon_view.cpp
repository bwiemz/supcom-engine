#include <catch2/catch_test_macros.hpp>

#include "map/visibility_grid.hpp"
#include "renderer/recon_view.hpp"
#include "sim/world_snapshot.hpp"

#include <span>
#include <utility>
#include <vector>

using namespace osc;
using map::VisFlag;
using renderer::ReconView;
using renderer::Sight;

namespace {

/// A 256 x 256 world of three armies (0 the player's), whose snapshot the
/// tests edit between ticks.
struct World {
    sim::WorldSnapshot snap;

    World() {
        snap.visibility.emplace(256, 256);
        snap.armies.resize(3);
        for (auto& a : snap.armies) a.valid = true;
    }

    sim::EntityRecord& add(u32 id, i32 army, f32 x, f32 z) {
        sim::EntityRecord e;
        e.id = id;
        e.army = army;
        e.position = {x, 0, z};
        snap.entities.push_back(e);
        return snap.entities.back();
    }
    sim::EntityRecord& unit(u32 id, i32 army, f32 x, f32 z, bool mobile) {
        sim::EntityRecord& e = add(id, army, x, z);
        e.is_unit = true;
        e.is_mobile = mobile;
        return e;
    }
    sim::EntityRecord* find(u32 id) {
        for (auto& e : snap.entities)
            if (e.id == id) return &e;
        return nullptr;
    }

    /// Army 0's senses over (x, z)'s cell this tick: nothing else.
    void sense(f32 x, f32 z, VisFlag flag) {
        snap.visibility->clear_transient();
        if (flag != VisFlag::None) snap.visibility->paint_circle(0, x, z, 12.0f, flag);
    }

    /// Units' recon masks from the grid, as the sim leaves them with nothing
    /// to counter (no cloak, stealth or water); off, a test sets its own.
    bool derive_masks = true;

    /// The next tick, as `recon` sees it (with the FlushIntelInRects since
    /// the last).
    void tick(ReconView& recon, std::span<const sim::IntelFlushRecord> flushes = {}) {
        ++snap.tick;
        if (derive_masks && snap.visibility)
            for (auto& e : snap.entities) {
                if (!e.is_unit) continue;
                e.los_now = e.detected = 0;
                u32 gx = 0;
                u32 gz = 0;
                snap.visibility->world_to_grid(e.position.x, e.position.z, gx, gz);
                for (u32 a = 0; a < 3; ++a) {
                    const VisFlag f = snap.visibility->get(gx, gz, a);
                    if (map::has_flag(f, VisFlag::Vision)) e.los_now |= 1u << a;
                    if (map::has_flag(f, VisFlag::Vision | VisFlag::Radar | VisFlag::Sonar |
                                             VisFlag::Omni))
                        e.detected |= 1u << a;
                }
            }
        recon.update(sim::FrameView(&snap, &snap, 1.0f), flushes);
    }
};

} // namespace

TEST_CASE("ReconView: an observer, or a world without a grid, sees everything",
          "[renderer][recon]") {
    World w;
    w.unit(1, 1, 100, 100, true);
    w.add(2, 1, 100, 100).is_projectile = true;

    ReconView recon;
    recon.set_focus_army(-1);
    w.tick(recon);
    CHECK(recon.sees_everything());
    CHECK(recon.sight(*w.find(1)) == Sight::Seen);
    CHECK(recon.sight(*w.find(2)) == Sight::Seen);

    recon.set_focus_army(0);
    w.tick(recon);
    CHECK_FALSE(recon.sees_everything());
    CHECK(recon.sight(*w.find(1)) == Sight::Hidden);

    w.snap.visibility.reset();
    w.tick(recon);
    CHECK(recon.sees_everything());
    CHECK(recon.sight(*w.find(1)) == Sight::Seen);
}

TEST_CASE("ReconView: its own and its allies' units, props and wrecks show anywhere",
          "[renderer][recon]") {
    World w;
    w.snap.armies[0].allies = 1u << 1;
    w.unit(1, 0, 100, 100, true);          // its own
    w.unit(2, 1, 100, 100, true);          // an ally's
    w.unit(3, 2, 100, 100, true);          // an enemy's
    w.add(4, -1, 100, 100).is_prop = true; // a tree
    w.add(5, 1, 100, 100).is_projectile = true;

    ReconView recon;
    recon.set_focus_army(0);
    w.tick(recon);
    CHECK(recon.sight(*w.find(1)) == Sight::Seen);
    CHECK(recon.sight(*w.find(2)) == Sight::Seen);
    CHECK(recon.sight(*w.find(3)) == Sight::Hidden);
    CHECK(recon.sight(*w.find(4)) == Sight::Seen);
    CHECK(recon.sight(*w.find(5)) == Sight::Seen);

    // An alliance broken: the former ally's unit is judged by intel.
    w.snap.armies[0].allies = 0;
    w.tick(recon);
    CHECK(recon.sight(*w.find(2)) == Sight::Hidden);
    CHECK(recon.sight(*w.find(5)) == Sight::Hidden);
}

TEST_CASE("ReconView: an enemy mobile unit shows in sight, as a blip when detected",
          "[renderer][recon]") {
    World w;
    const sim::EntityRecord& tank = w.unit(1, 1, 100, 100, true);
    ReconView recon;
    recon.set_focus_army(0);

    w.tick(recon);
    CHECK(recon.sight(tank) == Sight::Hidden);

    // Radar alone: a blip, never seen.
    w.sense(100, 100, VisFlag::Radar);
    w.tick(recon);
    CHECK(recon.sight(tank) == Sight::Blip);
    CHECK(recon.frozen_pose(1) == nullptr);

    // In sight: itself.
    w.sense(100, 100, VisFlag::Vision);
    w.tick(recon);
    CHECK(recon.sight(tank) == Sight::Seen);

    // Out of sight, still on radar: a blip it has seen.
    w.sense(100, 100, VisFlag::Radar);
    w.tick(recon);
    CHECK(recon.sight(tank) == Sight::SeenBlip);

    // Lost to every sense, then found again: never seen, as far as the blip
    // knows.
    w.sense(100, 100, VisFlag::None);
    w.tick(recon);
    CHECK(recon.sight(tank) == Sight::Hidden);
    w.sense(100, 100, VisFlag::Radar);
    w.tick(recon);
    CHECK(recon.sight(tank) == Sight::Blip);

    // Sonar and omni detect as radar does.
    w.sense(100, 100, VisFlag::Sonar);
    w.tick(recon);
    CHECK(recon.sight(tank) == Sight::Blip);
    w.sense(100, 100, VisFlag::Omni);
    w.tick(recon);
    CHECK(recon.sight(tank) == Sight::Blip);

    // Senses elsewhere don't count.
    w.sense(200, 200, VisFlag::Vision);
    w.tick(recon);
    CHECK(recon.sight(tank) == Sight::Hidden);
}

TEST_CASE("ReconView: an enemy structure, once seen, is remembered as it was",
          "[renderer][recon]") {
    World w;
    sim::EntityRecord& pd = w.unit(1, 1, 100, 100, false);
    pd.bone_count = 1;
    pd.bone_offset = 0;
    pd.fraction_complete = 0.5f;
    sim::BoneMatrix seen{};
    seen[12] = 1.0f; // the pose it was seen in
    w.snap.bones.push_back(seen);

    ReconView recon;
    recon.set_focus_army(0);
    w.sense(100, 100, VisFlag::Radar);
    w.tick(recon);
    CHECK(recon.sight(pd) == Sight::Blip);

    w.sense(100, 100, VisFlag::Vision);
    w.tick(recon);
    CHECK(recon.sight(pd) == Sight::Seen);
    CHECK(recon.frozen_pose(1) == nullptr); // live while in sight
    CHECK(recon.frozen_fraction(1, 0.7f) == 0.7f);

    // Out of sight, it turns and builds on; the player's army sees it as it
    // was, with or without a sense on it.
    w.snap.bones[0][12] = 2.0f;
    pd.fraction_complete = 0.9f;
    w.sense(100, 100, VisFlag::Radar);
    w.tick(recon);
    CHECK(recon.sight(pd) == Sight::Remembered);
    REQUIRE(recon.frozen_pose(1) != nullptr);
    CHECK((*recon.frozen_pose(1))[0][12] == 1.0f);
    CHECK(recon.frozen_fraction(1, pd.fraction_complete) == 0.5f);

    w.sense(100, 100, VisFlag::None);
    w.tick(recon);
    CHECK(recon.sight(pd) == Sight::Remembered);

    // Seen again: live again.
    w.sense(100, 100, VisFlag::Vision);
    w.tick(recon);
    CHECK(recon.sight(pd) == Sight::Seen);
    CHECK(recon.frozen_pose(1) == nullptr);
}

TEST_CASE("ReconView: a unit is judged by the sim's recon of it, not the cells",
          "[renderer][recon]") {
    // The sim counts what a unit's cell can't show: a cloak defeats vision,
    // stealth radar, the water radar or sonar (M215d). A unit in a lit cell
    // the sim says isn't in sight isn't; one it says is detected is a blip.
    World w;
    w.derive_masks = false;
    sim::EntityRecord& cloaked = w.unit(1, 1, 100, 100, true);
    ReconView recon;
    recon.set_focus_army(0);
    w.sense(100, 100, VisFlag::Vision);
    cloaked.los_now = 0;
    cloaked.detected = 0;
    w.tick(recon);
    CHECK(recon.sight(cloaked) == Sight::Hidden);
    cloaked.detected = 1u; // omni, say, or sonar under water
    w.tick(recon);
    CHECK(recon.sight(cloaked) == Sight::Blip);
    cloaked.los_now = 1u;
    w.tick(recon);
    CHECK(recon.sight(cloaked) == Sight::Seen);
    // Another army's bits are not the player's.
    cloaked.los_now = 1u << 2;
    cloaked.detected = 1u << 2;
    w.tick(recon);
    CHECK(recon.sight(cloaked) == Sight::Hidden);
}

TEST_CASE("ReconView: an enemy projectile shows only in sight", "[renderer][recon]") {
    World w;
    w.add(1, 1, 100, 100).is_projectile = true;
    ReconView recon;
    recon.set_focus_army(0);

    w.tick(recon);
    CHECK(recon.sight(*w.find(1)) == Sight::Hidden);
    w.sense(100, 100, VisFlag::Radar);
    w.tick(recon);
    CHECK(recon.sight(*w.find(1)) == Sight::Hidden);
    w.sense(100, 100, VisFlag::Vision);
    w.tick(recon);
    CHECK(recon.sight(*w.find(1)) == Sight::Seen);
}

TEST_CASE("ReconView: memory is per army, per entity, per tick", "[renderer][recon]") {
    World w;
    w.unit(1, 1, 100, 100, false);
    ReconView recon;
    recon.set_focus_army(0);
    w.sense(100, 100, VisFlag::Vision);
    w.tick(recon);
    w.sense(100, 100, VisFlag::None);
    w.tick(recon);
    CHECK(recon.sight(*w.find(1)) == Sight::Remembered);

    // A tick already seen isn't judged again.
    w.sense(100, 100, VisFlag::Radar);
    recon.update(sim::FrameView(&w.snap, &w.snap, 0.5f));
    CHECK(recon.sight(*w.find(1)) == Sight::Remembered);

    // Another army's view starts from nothing (army 2 senses nothing).
    recon.set_focus_army(2);
    w.tick(recon);
    CHECK(recon.sight(*w.find(1)) == Sight::Hidden);
    recon.set_focus_army(0);
    w.sense(100, 100, VisFlag::None);
    w.tick(recon);
    CHECK(recon.sight(*w.find(1)) == Sight::Hidden);

    // An entity gone from the world while its spot is in sight is
    // forgotten; one with its id later is new.
    w.sense(100, 100, VisFlag::Vision);
    w.tick(recon);
    w.snap.entities.clear();
    w.tick(recon);
    CHECK(recon.ghosts().empty());
    w.unit(1, 1, 100, 100, false);
    w.sense(100, 100, VisFlag::None);
    w.tick(recon);
    CHECK(recon.sight(*w.find(1)) == Sight::Hidden);
}

TEST_CASE("ReconView: a structure gone unseen is maybe dead, until its spot is seen",
          "[renderer][recon]") {
    World w;
    sim::EntityRecord& pd = w.unit(7, 1, 100, 100, false);
    pd.blueprint_id = "ueb2101";
    pd.bone_count = 1;
    w.snap.bones.push_back(sim::BoneMatrix{});
    w.unit(8, 1, 200, 200, true); // a tank: mobile, never a ghost
    ReconView recon;
    recon.set_focus_army(0);
    w.snap.visibility->paint_circle(0, 200, 200, 12.0f, VisFlag::Vision);
    w.snap.visibility->paint_circle(0, 100, 100, 12.0f, VisFlag::Vision);
    w.tick(recon);
    w.sense(0, 0, VisFlag::None);
    w.tick(recon);
    CHECK(recon.sight(*w.find(7)) == Sight::Remembered);

    // Both die out of sight: the structure stays, as last seen; the tank is
    // gone.
    w.snap.entities.clear();
    w.tick(recon);
    REQUIRE(recon.ghosts().size() == 1);
    const sim::EntityRecord& ghost = recon.ghosts().front();
    CHECK(ghost.id == 7);
    CHECK(ghost.blueprint_id == "ueb2101");
    CHECK(recon.maybe_dead(7));
    CHECK_FALSE(recon.maybe_dead(8));
    CHECK(recon.sight(ghost) == Sight::Remembered);
    CHECK(recon.frozen_pose(7) != nullptr);

    // Radar there isn't sight: still maybe dead.
    w.sense(100, 100, VisFlag::Radar);
    w.tick(recon);
    CHECK(recon.ghosts().size() == 1);

    // Seen, the spot is empty: forgotten.
    w.sense(100, 100, VisFlag::Vision);
    w.tick(recon);
    CHECK(recon.ghosts().empty());
    CHECK_FALSE(recon.maybe_dead(7));

    // A new focus forgets them too.
    w.unit(9, 1, 100, 100, false);
    w.tick(recon);
    w.snap.entities.clear();
    w.sense(0, 0, VisFlag::None);
    w.tick(recon);
    CHECK(recon.ghosts().size() == 1);
    recon.set_focus_army(2);
    CHECK(recon.ghosts().empty());
}

TEST_CASE("ReconView: a FlushIntelInRect forgets what it took from the player's army",
          "[renderer][recon]") {
    World w;
    w.unit(1, 1, 100, 100, false); // a structure the flush takes from army 0
    w.unit(2, 1, 140, 100, false); // one it took from army 1 alone
    w.unit(3, 1, 200, 200, false); // one that dies unseen, in the rect
    w.unit(4, 1, 240, 100, false); // one outside it
    ReconView recon;
    recon.set_focus_army(0);
    for (const auto& [x, z] :
         {std::pair{100.0f, 100.0f}, {140.0f, 100.0f}, {200.0f, 200.0f}, {240.0f, 100.0f}})
        w.snap.visibility->paint_circle(0, x, z, 12.0f, VisFlag::Vision);
    w.tick(recon);
    w.sense(0, 0, VisFlag::None);
    w.tick(recon);
    std::erase_if(w.snap.entities, [](const sim::EntityRecord& e) { return e.id == 3; });
    w.tick(recon);
    REQUIRE(recon.ghosts().size() == 1);
    CHECK(recon.sight(*w.find(1)) == Sight::Remembered);

    const sim::IntelFlushRecord flush{90, 90, 210, 210, {{1, 1u << 0}, {2, 1u << 1}}};
    w.tick(recon, {&flush, 1});
    CHECK(recon.sight(*w.find(1)) == Sight::Hidden);
    CHECK(recon.frozen_pose(1) == nullptr);
    CHECK(recon.sight(*w.find(2)) == Sight::Remembered);
    CHECK(recon.sight(*w.find(4)) == Sight::Remembered);
    CHECK(recon.ghosts().empty());
    CHECK_FALSE(recon.maybe_dead(3));

    // Seen again, it is remembered again.
    w.sense(100, 100, VisFlag::Vision);
    w.tick(recon);
    w.sense(0, 0, VisFlag::None);
    w.tick(recon);
    CHECK(recon.sight(*w.find(1)) == Sight::Remembered);
}

TEST_CASE("ReconView: effects show where the player's army sees", "[renderer][recon]") {
    World w;
    w.snap.armies[0].allies = 1u << 1;
    ReconView recon;
    recon.set_focus_army(0);
    w.sense(100, 100, VisFlag::Vision);
    w.tick(recon);
    const sim::FrameView view(&w.snap, &w.snap, 1.0f);
    // Its own and an ally's anywhere; another army's, or none's, in sight.
    CHECK(recon.sees_at(view, 0, 200, 200));
    CHECK(recon.sees_at(view, 1, 200, 200));
    CHECK_FALSE(recon.sees_at(view, 2, 200, 200));
    CHECK_FALSE(recon.sees_at(view, -1, 200, 200));
    CHECK(recon.sees_at(view, 2, 100, 100));
    // Radar isn't sight.
    w.sense(200, 200, VisFlag::Radar);
    w.tick(recon);
    CHECK_FALSE(recon.sees_at(view, 2, 200, 200));
    // A beam, where either end is in sight.
    w.sense(100, 100, VisFlag::Vision);
    w.tick(recon);
    CHECK(recon.sees_beam(view, {100, 0, 100}, {200, 0, 200}));
    CHECK(recon.sees_beam(view, {200, 0, 200}, {100, 0, 100}));
    CHECK_FALSE(recon.sees_beam(view, {200, 0, 200}, {220, 0, 200}));
    // An observer sees all.
    recon.set_focus_army(-1);
    w.tick(recon);
    CHECK(recon.sees_at(view, 2, 200, 200));
    CHECK(recon.sees_beam(view, {200, 0, 200}, {220, 0, 200}));
}

// Shields and beams aren't judged as units are: the overlay places them and
// asks sees_at where they are (M215b).
TEST_CASE("ReconView: shields and beams aren't judged by sight", "[renderer][recon]") {
    World w;
    w.add(1, 1, 100, 100).is_shield = true;
    w.add(2, 1, 100, 100).is_collision_beam = true;
    ReconView recon;
    recon.set_focus_army(0);
    w.tick(recon);
    CHECK(recon.sight(*w.find(1)) == Sight::Seen);
    CHECK(recon.sight(*w.find(2)) == Sight::Seen);
}

TEST_CASE("ReconView: the unidentified colour's channels", "[renderer][recon]") {
    ReconView recon;
    recon.set_unidentified_color(0xFF336699u);
    const auto [r, g, b] = recon.unidentified_rgb();
    CHECK(r == 0x33 / 255.0f);
    CHECK(g == 0x66 / 255.0f);
    CHECK(b == 0x99 / 255.0f);
}
