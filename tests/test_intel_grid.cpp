// Moho's intel grids (M215g): CIntelGrid's raster and queries, and the
// recon an army reads from its grids (CAiReconDBImpl::ReconCanDetect), by
// the decompile (docs/plans/2026-10-01-m215g-intel-grids-design.md).

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "map/heightmap.hpp"
#include "map/intel_grid.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace osc;
using map::IntelGrid;
using sim::SimState;
using sim::Unit;

namespace {

size_t painted(const IntelGrid& g) {
    size_t n = 0;
    for (const i8 c : g.cells()) n += c != 0 ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("A circle is rastered as CIntelGrid::Raster does", "[intel]") {
    IntelGrid vision(256, 256, 2);
    CHECK((vision.width() == 128 && vision.height() == 128));
    // Vision 20 at 2-unit cells: 10 cells across, 296 of them
    vision.add_circle(100, 100, 20);
    CHECK(painted(vision) == 296);
    // Columns gx-r to gx+r exclusive, each of rows gz-leg to gz+leg
    // exclusive: cells 41 to 59 across (the first column has no rows), 40
    // to 59 down, leaning a cell to -z.
    CHECK_FALSE(vision.visible(80.5f, 100));
    CHECK(vision.visible(82.5f, 100));
    CHECK(vision.visible(118.5f, 100));
    CHECK(vision.visible(100, 80.5f));
    CHECK(vision.visible(100, 118.5f));
    CHECK_FALSE(vision.visible(100, 120.5f));
    // Rubbed out, it's gone
    vision.sub_circle(100, 100, 20);
    CHECK(painted(vision) == 0);

    // The default WaterVision, 10 at 4-unit cells: 8 cells
    IntelGrid water(256, 256, 4);
    water.add_circle(100, 100, 10);
    CHECK(painted(water) == 8);
    // Under a cell's size, nothing
    water.add_circle(100, 100, 3);
    CHECK(painted(water) == 8);
}

TEST_CASE("An intel grid counts its circles", "[intel]") {
    IntelGrid g(64, 64, 4);
    g.add_circle(32, 32, 8);
    g.add_circle(32, 32, 8);
    CHECK(g.count(8, 8) == 2);
    g.sub_circle(32, 32, 8);
    CHECK(g.visible(32, 32)); // one still over it
    g.sub_circle(32, 32, 8);
    CHECK_FALSE(g.visible(32, 32));
    // Off the grid, nothing: no clamping to its edge
    g.add_circle(2, 2, 12);
    CHECK(g.visible(0, 0));
    CHECK_FALSE(g.visible(-1, 0));
    CHECK_FALSE(g.visible(64, 0));
}

TEST_CASE("A rectangle's sight is any cell it covers, counted above 0", "[intel]") {
    IntelGrid g(128, 128, 2);
    g.add_circle(41, 41, 2); // cells (20, 19) and (20, 20): x 40-42, z 38-42
    CHECK(g.any_in(40.5f, 40.5f, 41, 41));
    CHECK(g.any_in(30, 30, 40.5f, 40.5f)); // the ceiled corner reaches cell 20
    CHECK_FALSE(g.any_in(30, 30, 40, 40)); // ends where cell 20 starts
    CHECK_FALSE(g.any_in(50, 50, 60, 60));
    CHECK_FALSE(g.any_in(-40, -40, -20, -20));
}

namespace {

/// A 128 x 128 map, land for x < 64 and sea beyond (the water at 5, the
/// sea floor below it), with three enemy armies; units are bare (no
/// script), their intel switched on by hand.
struct World {
    lua::LuaState lua;
    SimState sim{lua.raw(), nullptr};

    World() {
        constexpr u32 kSize = 128;
        std::vector<u16> heights((kSize + 1) * (kSize + 1), 1000);
        for (u32 z = 0; z <= kSize; ++z)
            for (u32 x = 64; x <= kSize; ++x) heights[z * (kSize + 1) + x] = 100;
        map::Heightmap hm(kSize, kSize, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<map::Terrain>(std::move(hm), 5.0f, true));
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.add_army("ARMY_3", "ARMY_3");
    }

    Unit* unit(i32 army, f32 x, f32 z, const char* layer, f32 y = 6.0f) {
        auto u = std::make_unique<Unit>();
        u->set_army(army);
        u->set_layer(layer);
        u->set_position({x, y, z});
        Unit* raw = u.get();
        sim.entity_registry().register_entity(std::move(u));
        return raw;
    }
    static void give(Unit* u, const char* type, f32 radius) {
        u->add_intel(type, radius);
        u->enable_intel(type);
    }
    u8 recon(const Unit* u, u32 army) const { return sim.recon_of(*u, army); }
};

constexpr u8 kLOS = SimState::kReconLOS;
constexpr u8 kRadar = SimState::kReconRadar;
constexpr u8 kSonar = SimState::kReconSonar;
constexpr u8 kOmni = SimState::kReconOmni;

} // namespace

TEST_CASE("Recon reads the grid each layer asks", "[intel]") {
    World w;
    Unit* eye = w.unit(0, 70, 64, "Water");
    World::give(eye, "Vision", 30);
    World::give(eye, "Radar", 60);
    World::give(eye, "Sonar", 60);
    Unit* tank = w.unit(1, 50, 64, "Land");
    Unit* ship = w.unit(1, 90, 64, "Water");
    Unit* sub = w.unit(1, 90, 70, "Sub", -2.0f);
    w.sim.tick();

    // Sight, radar and omni see land and air; sonar never
    CHECK(w.recon(tank, 0) == (kLOS | kRadar));
    // On the water: sight, radar and sonar
    CHECK(w.recon(ship, 0) == (kLOS | kRadar | kSonar));
    // Under it: only the water grid's sight (WaterVision), sonar and omni
    CHECK(w.recon(sub, 0) == kSonar);
    World::give(eye, "WaterVision", 30);
    w.sim.tick();
    CHECK(w.recon(sub, 0) == (kLOS | kSonar));
    // ...and WaterVision sees nothing above the water
    eye->disable_intel("Vision");
    w.sim.tick();
    CHECK(w.recon(tank, 0) == kRadar);
    // Its own and an ally's: everything
    CHECK(w.recon(eye, 0) == SimState::kReconAll);
    w.sim.set_alliance(0, 2, sim::Alliance::Ally);
    Unit* friend_ = w.unit(2, 20, 20, "Land");
    CHECK(w.recon(friend_, 0) == SimState::kReconAll);
}

TEST_CASE("Fields counter an army's senses; omni beats them", "[intel]") {
    World w;
    Unit* eye = w.unit(0, 40, 40, "Land");
    World::give(eye, "Vision", 40);
    World::give(eye, "Radar", 80);
    Unit* tank = w.unit(1, 40, 60, "Land");
    Unit* field = w.unit(1, 40, 64, "Land");
    w.sim.tick();
    CHECK(w.recon(tank, 0) == (kLOS | kRadar));

    // A radar stealth field over it: no radar, but it's still in sight
    World::give(field, "RadarStealthField", 12);
    w.sim.tick();
    CHECK(w.recon(tank, 0) == kLOS);
    // A cloak field: out of sight too
    World::give(field, "CloakField", 12);
    w.sim.tick();
    CHECK(w.recon(tank, 0) == 0);
    // The field's owner isn't fooled by its own field
    Unit* own = w.unit(1, 40, 40, "Land");
    World::give(own, "Vision", 30);
    w.sim.tick();
    CHECK(w.sim.recon_at({40, 6, 60}, 1) != 0);
    // Omni beats every counter
    World::give(eye, "Omni", 40);
    w.sim.tick();
    CHECK(w.recon(tank, 0) == (kLOS | kRadar | kOmni));
}

TEST_CASE("A unit's own stealth hides it only out of sight", "[intel]") {
    World w;
    Unit* eye = w.unit(0, 40, 40, "Land");
    World::give(eye, "Vision", 20);
    World::give(eye, "Radar", 80);
    Unit* near = w.unit(1, 40, 50, "Land");
    Unit* far = w.unit(1, 40, 90, "Land");
    for (Unit* u : {near, far}) World::give(u, "RadarStealth", 0);
    w.sim.tick();
    CHECK(w.recon(near, 0) == (kLOS | kRadar)); // in sight: the stealth doesn't count
    CHECK(w.recon(far, 0) == 0);
    // A cloak hides it from sight, and then its stealth from radar
    World::give(near, "Cloak", 0);
    w.sim.tick();
    CHECK(w.recon(near, 0) == 0);
}

TEST_CASE("Allies' grids count for an army that they call an ally", "[intel]") {
    World w;
    Unit* scout = w.unit(2, 40, 40, "Land");
    World::give(scout, "Vision", 30);
    Unit* tank = w.unit(1, 40, 50, "Land");
    w.sim.tick();
    CHECK(w.recon(tank, 0) == 0);
    // Army 3 calls army 1 its ally: army 1 reads its grids, not the reverse
    w.sim.get_army(2)->set_alliance(0, sim::Alliance::Ally);
    CHECK(w.recon(tank, 0) == kLOS);
    Unit* eye = w.unit(0, 100, 100, "Land");
    World::give(eye, "Vision", 30);
    Unit* boat = w.unit(1, 100, 110, "Land");
    w.sim.tick();
    CHECK(w.recon(boat, 0) == kLOS);
    CHECK(w.recon(boat, 2) == 0);
}

TEST_CASE("A moving source repaints as Moho's handles do", "[intel]") {
    World w;
    Unit* radar = w.unit(0, 30, 64, "Land");
    World::give(radar, "Radar", 30); // 7 cells; repaints after 10 units
    w.sim.tick();
    const map::IntelGrids& grids = *w.sim.intel_grids();
    const map::IntelGrid& g = grids.grid(0, map::IntelLayer::Radar);
    // About cell 7: columns 1 to 13
    CHECK(g.visible(6, 64));
    CHECK_FALSE(g.visible(66, 64));

    // Moving: painted where it was until it has gone a third of its radius
    radar->set_position({36, 6, 64});
    w.sim.tick();
    radar->set_position({38, 6, 64});
    w.sim.tick();
    CHECK(g.visible(6, 64));
    radar->set_position({41, 6, 64});
    w.sim.tick();
    // About cell 10: columns 4 to 16
    CHECK_FALSE(g.visible(6, 64));
    CHECK(g.visible(66, 64));

    // Stopping, at once
    radar->set_position({45, 6, 64});
    w.sim.tick();
    CHECK(g.visible(18, 64));
    CHECK_FALSE(g.visible(70, 64));
    w.sim.tick(); // it stood still: the motion stopped
    // About cell 11: columns 5 to 17
    CHECK_FALSE(g.visible(18, 64));
    CHECK(g.visible(70, 64));

    // Off, it is rubbed out; gone, too
    radar->disable_intel("Radar");
    w.sim.tick();
    size_t lit = 0;
    for (const i8 c : g.cells()) lit += c != 0 ? 1 : 0;
    CHECK(lit == 0);
    radar->enable_intel("Radar");
    w.sim.tick();
    CHECK(g.visible(45, 64));
    radar->mark_destroyed();
    w.sim.tick();
    CHECK_FALSE(g.visible(45, 64));
}

TEST_CASE("Without fog of war every army is in sight of everything but a cloak", "[intel][fow]") {
    World w;
    w.sim.set_fog_of_war("none");
    Unit* tank = w.unit(1, 40, 60, "Land");
    Unit* cloaked = w.unit(1, 40, 70, "Land");
    World::give(cloaked, "Cloak", 0);
    w.sim.tick();
    CHECK(w.recon(tank, 0) == kLOS);
    CHECK(w.recon(cloaked, 0) == 0);
    CHECK(w.sim.intel_grids()->grid(0, map::IntelLayer::Vision).empty());
}
