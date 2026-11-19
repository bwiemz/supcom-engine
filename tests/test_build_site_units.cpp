// Units standing on a build site hold up a mobile build, as Moho's
// CUnitMobileBuildTask does in TASKSTATE_Processing (faf-re
// CUnitMobileBuildTask.cpp; Steam 0x005FE4DB): Sim::LocationIsFree asks
// whether any unit stands on the site's cells, SIM_TryToBuild sends the
// army's own resting units off, and the task looks again 49 ticks later,
// failing once it has found the site taken 11 times.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/footprint.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/build_site_units.hpp"
#include "sim/manipulator.hpp"
#include "sim/occupancy.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "support/tick_until.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using osc::f32;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::test::tick_until;

namespace {

/// A flat map, two armies, an engineer of the first, and a land factory's
/// blueprint (ueb0101: a 5 by 5 footprint, an 8 by 8 skirt) to build.
struct Site {
    lua_State* L = lua_open();
    SimState sim{L, nullptr};
    Unit* engineer = nullptr;
    osc::sim::EntityId built_id{};

    Site() {
        luaopen_base(L);
        luaopen_table(L);
        lua_settop(L, 0);
        const std::string code = R"(
            __blueprints = {
                uel0105 = {
                    BlueprintId = 'uel0105',
                    Economy = { BuildableCategory = { 'ueb0101' } },
                },
                ueb0101 = {
                    BlueprintId = 'ueb0101',
                    Footprint = { SizeX = 5, SizeZ = 5 },
                    Physics = { SkirtSizeX = 8, SkirtSizeZ = 8, SkirtOffsetX = -1.5,
                                SkirtOffsetZ = -1.5 },
                    Economy = { BuildTime = 100, BuildCostMass = 1, BuildCostEnergy = 1 },
                },
            }
            __failed = 0
            Engineer = {
                OnFailedToBuild = function(self) __failed = __failed + 1 end,
                CheckBuildRestriction = function(self, bp) return true end,
            }
            Engineer.__index = Engineer
        )";
        REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "stub") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
        lua_pushstring(L, "__osc_create_building_unit");
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, &Site::create_building_unit, 1);
        lua_rawset(L, LUA_REGISTRYINDEX);

        std::vector<osc::u16> heights(129 * 129, 0);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.build_spatial_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_fog_of_war("none");
        sim.set_victory_condition("sandbox");

        engineer = walker(0, 50.5f, 62.5f, 1.9f);
        engineer->set_blueprint_id("uel0105");
        engineer->set_build_rate(5.0f);
        engineer->set_max_build_distance(5.0f);
        bind(*engineer, "Engineer");
    }
    ~Site() { lua_close(L); }
    Site(const Site&) = delete;
    Site& operator=(const Site&) = delete;

    static int create_building_unit(lua_State* L) {
        auto* self = static_cast<Site*>(lua_touserdata(L, lua_upvalueindex(1)));
        auto u = std::make_unique<Unit>();
        u->set_blueprint_id(lua_tostring(L, 1));
        u->set_unit_id(lua_tostring(L, 1));
        u->set_position(
            {static_cast<f32>(lua_tonumber(L, 3)), 0.0f, static_cast<f32>(lua_tonumber(L, 5))});
        u->set_max_health(100.0f);
        u->set_health(0.0f);
        u->set_fraction_complete(0.0f);
        u->set_is_being_built(true);
        Unit* raw = u.get();
        self->sim.entity_registry().register_entity(std::move(u));
        self->bind(*raw, nullptr);
        self->built_id = raw->entity_id();
        osc::sim::push_entity_key(L, raw->entity_id());
        lua_rawgeti(L, LUA_REGISTRYINDEX, raw->lua_table_ref());
        return 2;
    }

    void bind(osc::sim::Entity& e, const char* cls) {
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, &e);
        lua_rawset(L, -3);
        if (cls) {
            lua_getglobal(L, cls);
            lua_setmetatable(L, -2);
        }
        e.set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    }

    /// A land unit of `army` with a one-cell footprint and a Striker's
    /// collision box (0.7 by 0.5 by 0.9).
    Unit* walker(int army, f32 x, f32 z, f32 speed = 3.4f) {
        auto u = std::make_unique<Unit>();
        u->set_army(army);
        u->set_motion_type("RULEUMT_Land");
        u->set_max_speed(speed);
        u->set_position({x, 0.0f, z});
        u->set_max_health(100.0f);
        u->set_health(100.0f);
        Unit::Drive drive;
        drive.max_accel = 50.0f;
        u->set_drive(drive);
        u->set_footprint_size(1.0f, 1.0f);
        osc::blueprints::UnitFootprints footprints;
        footprints.main.size_x = 1;
        footprints.main.size_z = 1;
        footprints.main.caps = osc::blueprints::occupancy::kLand;
        u->set_footprints(footprints);
        u->set_collision_shape(
            {osc::sim::CollisionShapeType::BOX, 0.0f, 0.25f, 0.0f, 0.35f, 0.25f, 0.45f});
        Unit* raw = u.get();
        sim.entity_registry().register_entity(std::move(u));
        return raw;
    }

    /// A T1 land factory's shape (ueb0101) of the first army: its footprint the
    /// cells round (x, z) 5 by 5, its box 4.2 by 0.6 by 4.4.
    Unit* factory(f32 x, f32 z) {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        u->set_position({x, 0.0f, z});
        u->set_max_health(100.0f);
        u->set_health(100.0f);
        u->set_footprint_size(5.0f, 5.0f);
        u->set_skirt(8.0f, 8.0f, -1.5f, -1.5f);
        u->set_build_rate(10.0f);
        u->add_category("FACTORY");
        u->set_collision_shape(
            {osc::sim::CollisionShapeType::BOX, 0.0f, 0.3f, 0.0f, 2.1f, 0.3f, 2.2f});
        Unit* raw = u.get();
        sim.entity_registry().register_entity(std::move(u));
        bind(*raw, "Engineer");
        return raw;
    }

    /// The engineer ordered to build the factory on the cells 48-53 by 48-53.
    void build() {
        UnitCommand c;
        c.type = CommandType::BuildMobile;
        c.blueprint_id = "ueb0101";
        c.target_pos = {50.5f, 0.0f, 50.5f};
        engineer->push_command(c, true);
    }

    bool built() const { return static_cast<bool>(built_id); }
    bool ordered() const { return !engineer->command_queue().empty(); }
    bool held_by_units() const {
        return ordered() && engineer->command_queue().front().site_tries > 0;
    }
    int failed() {
        lua_getglobal(L, "__failed");
        const int n = static_cast<int>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return n;
    }

    /// Whether `u`'s collision box reaches the site's cells.
    static bool on_site(const Unit& u) {
        const osc::sim::Vector3& p = u.position();
        constexpr f32 kReach = 0.58f; // the box's half-diagonal, turned any way
        return p.x + kReach > 48.0f && p.x - kReach < 53.0f && p.z + kReach > 48.0f &&
               p.z - kReach < 53.0f;
    }
};

} // namespace

TEST_CASE("Its army's idle tanks on a build site are sent off it, with no order, and then it "
          "is built",
          "[build][build_site]") {
    Site s;
    const std::vector<Unit*> tanks = {s.walker(0, 49.5f, 49.5f), s.walker(0, 51.5f, 51.5f),
                                      s.walker(0, 52.2f, 49.0f)};
    s.sim.tick();
    s.build();
    const int ticks =
        tick_until([&] { s.sim.tick(); }, "the frame stands", 200, [&] { return s.built(); });
    // Not before a look, the 49 ticks after it, and another.
    CHECK(ticks > 49);
    for (const Unit* tank : tanks) {
        CAPTURE(tank->position().x, tank->position().z);
        CHECK_FALSE(Site::on_site(*tank));
        CHECK(tank->command_queue().empty());
    }
    CHECK(s.failed() == 0);
}

TEST_CASE("Another army's tank on a build site stays, and the build waits until it is gone",
          "[build][build_site]") {
    Site s;
    Unit* foe = s.walker(1, 50.5f, 50.5f);
    s.sim.tick();
    s.build();
    for (int i = 0; i < 150; ++i) {
        s.sim.tick();
    }
    CHECK_FALSE(s.built());
    CHECK(s.ordered());
    CHECK(foe->position().x == 50.5f);
    CHECK(foe->position().z == 50.5f);
    CHECK_FALSE(foe->navigator().busy());

    foe->set_position({50.5f, 0.0f, 30.5f});
    tick_until([&] { s.sim.tick(); }, "the frame stands once the site is clear", 49,
               [&] { return s.built(); });
    CHECK(s.failed() == 0);
}

TEST_CASE("A build whose site never clears fails at its 12th look, 11 waits of 49 ticks after "
          "the first",
          "[build][build_site]") {
    Site s;
    s.walker(1, 50.5f, 50.5f);
    s.sim.tick();
    s.build();
    // Its first look, a tick after it prepares (it stands in reach, facing the site).
    s.sim.tick();
    s.sim.tick();
    const int ticks =
        tick_until([&] { s.sim.tick(); }, "the build fails", 600, [&] { return !s.ordered(); });
    CHECK(ticks == 11 * 49);
    CHECK_FALSE(s.built());
    CHECK(s.failed() == 1);
}

TEST_CASE("A builder pushed out of reach while units hold up its site builds from there once "
          "they are gone",
          "[build][build_site]") {
    for (const bool walked : {false, true}) {
        DYNAMIC_SECTION((walked ? "after its walk to the site" : "from where it stood")) {
            Site s;
            Unit* foe = s.walker(1, 50.5f, 50.5f);
            if (walked) {
                s.engineer->set_position({50.5f, 0.0f, 80.5f});
            }
            s.sim.tick();
            s.build();
            tick_until([&] { s.sim.tick(); }, "the builder finds a unit on its site", 300,
                       [&] { return s.held_by_units(); });
            s.engineer->set_position({50.5f, 0.0f, 70.5f});
            foe->set_position({50.5f, 0.0f, 30.5f});
            tick_until([&] { s.sim.tick(); }, "the frame stands", 49, [&] { return s.built(); });
            CHECK(s.ordered());
            CHECK(s.failed() == 0);
            CHECK(s.engineer->position().z == 70.5f);
        }
    }
}

TEST_CASE("A builder that must face its site turns to it once, not again after a wait for units "
          "on it",
          "[build][build_site][face]") {
    Site s;
    Unit::Drive drive;
    drive.max_accel = 50.0f;
    drive.turn_rate = 1.5f;
    s.engineer->set_drive(drive);
    s.engineer->set_need_to_face_target_to_build(true);
    Unit* foe = s.walker(1, 50.5f, 50.5f);
    s.sim.tick();
    s.build();
    tick_until([&] { s.sim.tick(); }, "the builder turns and finds a unit on its site", 60,
               [&] { return s.held_by_units(); });
    s.engineer->set_orientation(osc::sim::euler_to_quat(0.0f, 0.0f, 0.0f));
    foe->set_position({50.5f, 0.0f, 30.5f});
    tick_until([&] { s.sim.tick(); }, "the frame stands", 49, [&] { return s.built(); });
    CHECK(osc::sim::quat_yaw(s.engineer->orientation()) == 0.0f);
}

TEST_CASE("A builder out of reach that cannot move fails its build the tick after it takes it, "
          "with OnFailedToBuild",
          "[build][build_site]") {
    Site s;
    s.engineer->set_position({50.5f, 0.0f, 80.5f});
    s.engineer->set_max_speed(0.0f);
    s.sim.tick();
    s.build();
    s.sim.tick();
    CHECK(s.ordered());
    CHECK(s.failed() == 0);
    s.sim.tick();
    CHECK_FALSE(s.ordered());
    CHECK_FALSE(s.built());
    CHECK(s.failed() == 1);
}

TEST_CASE("Sim::LocationIsFree counts a unit whose shape meets the cells, not the builder, a "
          "flier, the dead or, asked for idle ones, one with an order",
          "[build][build_site]") {
    Site s;
    const osc::sim::OccupancyRect cells{48, 48, 53, 53};
    CHECK(osc::sim::location_is_free(s.sim, nullptr, cells, false));
    // The engineer stands off the site; another just outside it, its box clear.
    Unit* beside = s.walker(0, 53.5f, 50.5f);
    CHECK(osc::sim::location_is_free(s.sim, nullptr, cells, false));
    // Its box reaching the cells, once its move is committed.
    beside->set_position({53.3f, 0.0f, 50.5f});
    s.sim.entity_registry().advance_coords();
    CHECK_FALSE(osc::sim::location_is_free(s.sim, nullptr, cells, false));
    CHECK(osc::sim::location_is_free(s.sim, beside, cells, false));
    beside->set_layer("Air");
    CHECK(osc::sim::location_is_free(s.sim, nullptr, cells, false));
    beside->set_layer("Land");
    UnitCommand move;
    move.type = CommandType::Move;
    move.target_pos = {53.3f, 0.0f, 50.5f};
    beside->push_command(move, true);
    CHECK(osc::sim::location_is_free(s.sim, nullptr, cells, true));
    CHECK_FALSE(osc::sim::location_is_free(s.sim, nullptr, cells, false));
}

TEST_CASE("Sim::LocationIsFree counts a structure only where its skirt overlaps the cells",
          "[build][build_site]") {
    Site s;
    // A one-cell structure whose skirt is its footprint, (54, 50) to (55, 51), and whose box
    // reaches 0.1 past it, onto the cells 48-54.
    Unit* post = s.walker(1, 54.5f, 50.5f);
    post->set_motion_type("RULEUMT_None");
    post->set_skirt(1.0f, 1.0f, 0.0f, 0.0f);
    post->set_collision_shape(
        {osc::sim::CollisionShapeType::BOX, 0.0f, 0.4f, 0.0f, 0.6f, 0.4f, 0.6f});
    CHECK(osc::sim::location_is_free(s.sim, nullptr, {48, 48, 54, 53}, false));
    CHECK_FALSE(osc::sim::location_is_free(s.sim, nullptr, {48, 48, 55, 53}, false));
    // A mobile unit there counts by its box alone.
    post->set_motion_type("RULEUMT_Land");
    CHECK_FALSE(osc::sim::location_is_free(s.sim, nullptr, {48, 48, 54, 53}, false));
}

TEST_CASE("SIM_TryToBuild sends only its army's resting land units, each to its own place off "
          "the cells",
          "[build][build_site]") {
    Site s;
    const osc::sim::OccupancyRect cells{48, 47, 54, 53};
    Unit* a = s.walker(0, 50.5f, 50.5f);
    Unit* b = s.walker(0, 50.5f, 51.5f);
    Unit* foe = s.walker(1, 52.5f, 49.5f);
    s.sim.tick();
    s.sim.tick();
    osc::sim::SimContext ctx = s.sim.sim_context();
    osc::sim::try_to_build(ctx, 0, cells, false);
    CHECK(a->stepping_aside());
    CHECK(b->stepping_aside());
    CHECK_FALSE(foe->stepping_aside());
    CHECK_FALSE(foe->navigator().busy());
    const osc::sim::Vector3 ga = a->navigator().goal();
    const osc::sim::Vector3 gb = b->navigator().goal();
    CHECK((ga.x != gb.x || ga.z != gb.z));
    for (const osc::sim::Vector3& g : {ga, gb}) {
        CAPTURE(g.x, g.z);
        // A one-cell footprint's corner cell, clear of the cells and their edges.
        CHECK(
            (g.x - 0.5f < 47.0f || g.x - 0.5f > 54.0f || g.z - 0.5f < 46.0f || g.z - 0.5f > 53.0f));
    }
    // Nothing stays reserved.
    CHECK_FALSE(s.sim.occupancy().reserved_any({0, 0, 128, 128}));
    tick_until([&] { s.sim.tick(); }, "both walk off", 120,
               [&] { return !a->stepping_aside() && !b->stepping_aside(); });
    // Off it but for the other army's tank.
    CHECK_FALSE(osc::sim::location_is_free(s.sim, nullptr, cells, false));
    CHECK(osc::sim::location_is_free(s.sim, foe, cells, false));
    CHECK(a->command_queue().empty());
    CHECK(b->command_queue().empty());
}

TEST_CASE("A factory waits for its army's idle tank to be sent off its footprint, then builds",
          "[build][build_site]") {
    Site s;
    // Its footprint the cells 78-83 by 48-53.
    Unit* factory = s.factory(80.5f, 50.5f);
    Unit* tank = s.walker(0, 81.0f, 50.5f);
    s.sim.tick();
    UnitCommand c;
    c.type = CommandType::BuildFactory;
    c.blueprint_id = "ueb0101";
    factory->push_command(c, true);
    s.sim.tick();
    CHECK_FALSE(s.built());
    CHECK(tank->stepping_aside());
    const int ticks =
        tick_until([&] { s.sim.tick(); }, "the factory builds", 100, [&] { return s.built(); });
    // Its next look, 49 ticks after the first.
    CHECK(ticks == 49);
    CHECK(tank->command_queue().empty());
    const osc::sim::Vector3& p = tank->position();
    CAPTURE(p.x, p.z);
    CHECK((p.x < 78.0f || p.x > 83.0f || p.z < 48.0f || p.z > 53.0f));
}

TEST_CASE("A factory's footprint holds it up only by idle units: one with an order, or another "
          "army's that never goes",
          "[build][build_site]") {
    Site s;
    Unit* factory = s.factory(80.5f, 50.5f);
    Unit* tank = s.walker(0, 81.0f, 50.5f);
    UnitCommand c;
    c.type = CommandType::BuildFactory;
    c.blueprint_id = "ueb0101";
    SECTION("its own tank, given an order") {
        UnitCommand move;
        move.type = CommandType::Move;
        move.target_pos = {81.0f, 0.0f, 20.5f};
        tank->push_command(move, true);
        factory->push_command(c, true);
        s.sim.tick();
        CHECK(s.built());
        CHECK_FALSE(tank->stepping_aside());
    }
    SECTION("another army's, which waits it out for good") {
        tank->set_army(1);
        s.sim.tick();
        factory->push_command(c, true);
        for (int i = 0; i < 600; ++i) {
            s.sim.tick();
        }
        CHECK_FALSE(s.built());
        CHECK(factory->command_queue().size() == 1);
        CHECK_FALSE(tank->stepping_aside());
        CHECK(tank->position().x == 81.0f);
    }
}
