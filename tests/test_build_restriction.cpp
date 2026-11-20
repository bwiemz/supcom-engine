#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/category_expr.hpp"
#include "sim/manipulator.hpp"
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

#include <algorithm>
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

struct Builds {
    lua_State* L = lua_open();
    SimState sim{L, nullptr};
    Unit* engineer = nullptr;
    osc::sim::EntityId built_id{};
    std::vector<osc::sim::Vector3> frames;

    Builds() {
        luaopen_base(L);
        luaopen_table(L);
        lua_settop(L, 0);
        const std::string code = R"(
            __blueprints = {
                uel0001 = {
                    BlueprintId = 'uel0001',
                    Economy = { BuildableCategory = { 'ueb1101', 'ueb1103', 'uel0105' } },
                },
                uel0105 = {
                    BlueprintId = 'uel0105',
                    Economy = { BuildTime = 100, BuildCostMass = 1, BuildCostEnergy = 1 },
                },
                ueb0101 = {
                    BlueprintId = 'ueb0101',
                    Footprint = { SizeX = 2, SizeZ = 2 },
                    Economy = { BuildTime = 100, BuildCostMass = 1, BuildCostEnergy = 1,
                                BuildableCategory = { 'uel0105' } },
                },
                ueb1103 = {
                    BlueprintId = 'ueb1103',
                    Physics = { BuildRestriction = 'RULEUBR_OnMassDeposit', SkirtSizeX = 2,
                                SkirtSizeZ = 2, SkirtOffsetX = -0.5, SkirtOffsetZ = -0.5 },
                    Economy = { BuildTime = 100, BuildCostMass = 1, BuildCostEnergy = 1 },
                },
                ueb1101 = {
                    BlueprintId = 'ueb1101',
                    Footprint = { SizeX = 2, SizeZ = 2 },
                    Physics = { SkirtSizeX = 3, SkirtSizeZ = 3, SkirtOffsetX = -0.5,
                                SkirtOffsetZ = -0.5 },
                    Economy = { BuildTime = 100, BuildCostMass = 1, BuildCostEnergy = 1 },
                },
            }
            __failed = 0
            __refuse = false
            Engineer = {
                OnFailedToBuild = function(self) __failed = __failed + 1 end,
                CheckBuildRestriction = function(self, bp) return not __refuse end,
            }
            Engineer.__index = Engineer
            Factory = { CheckBuildRestriction = function(self, bp) return true end }
            Factory.__index = Factory
        )";
        REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "stub") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
        lua_pushstring(L, "__osc_create_building_unit");
        lua_pushlightuserdata(L, this);
        lua_pushcclosure(L, &Builds::create_building_unit, 1);
        lua_rawset(L, LUA_REGISTRYINDEX);

        std::vector<osc::u16> heights(129 * 129, 0);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.build_spatial_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.set_fog_of_war("none");
        sim.set_victory_condition("sandbox");
        sim.add_resource_deposit({50.5f, 0.0f, 50.5f, 1.0f, osc::sim::ResourceDeposit::Mass});

        auto u = std::make_unique<Unit>();
        u->set_blueprint_id("uel0001");
        u->set_army(0);
        u->set_position({50.5f, 0.0f, 54.5f});
        u->set_max_health(100.0f);
        u->set_health(100.0f);
        u->set_build_rate(5.0f);
        u->set_max_build_distance(5.0f);
        engineer = u.get();
        sim.entity_registry().register_entity(std::move(u));
        bind(*engineer, "Engineer");
    }
    ~Builds() { lua_close(L); }
    Builds(const Builds&) = delete;
    Builds& operator=(const Builds&) = delete;

    static int create_building_unit(lua_State* L) {
        auto* self = static_cast<Builds*>(lua_touserdata(L, lua_upvalueindex(1)));
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
        self->frames.push_back(raw->position());
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

    Unit* factory(f32 x, f32 z) {
        auto u = std::make_unique<Unit>();
        u->set_blueprint_id("ueb0101");
        u->set_army(0);
        u->set_position({x, 0.0f, z});
        u->set_max_health(100.0f);
        u->set_health(100.0f);
        u->set_build_rate(10.0f);
        u->add_category("FACTORY");
        Unit* raw = u.get();
        sim.entity_registry().register_entity(std::move(u));
        bind(*raw, "Factory");
        return raw;
    }

    bool frame_at(const osc::sim::Vector3& at) const {
        return std::any_of(frames.begin(), frames.end(),
                           [&](const osc::sim::Vector3& f) { return f.x == at.x && f.z == at.z; });
    }

    void build(const char* blueprint, f32 x, f32 z) {
        UnitCommand c;
        c.type = CommandType::BuildMobile;
        c.blueprint_id = blueprint;
        c.target_pos = {x, 0.0f, z};
        engineer->push_command(c, true);
    }

    bool built() const { return static_cast<bool>(built_id); }
    bool ordered() const { return !engineer->command_queue().empty(); }
    int failed() {
        lua_getglobal(L, "__failed");
        const int n = static_cast<int>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return n;
    }
};

} // namespace

TEST_CASE("An extractor is built on its deposit, and off it the build fails at once",
          "[build][build_restriction]") {
    const bool on_deposit = GENERATE(true, false);
    CAPTURE(on_deposit);
    Builds b;
    b.build("ueb1103", on_deposit ? 50.5f : 52.5f, 50.5f);
    if (on_deposit) {
        tick_until([&] { b.sim.tick(); }, "the frame stands", 40, [&] { return b.built(); });
        CHECK(b.failed() == 0);
    } else {
        b.sim.tick();
        CHECK_FALSE(b.ordered());
        CHECK(b.failed() == 1);
        for (int i = 0; i < 40; ++i) {
            b.sim.tick();
        }
        CHECK_FALSE(b.built());
    }
}

TEST_CASE("A build its builder may not make is dropped as it comes up, with no OnFailedToBuild",
          "[build][build_restriction]") {
    enum class Why { Allowed, NotBuildable, ArmyRestricted, UnitRestricted, ScriptRefuses };
    const Why why = GENERATE(Why::Allowed, Why::NotBuildable, Why::ArmyRestricted,
                             Why::UnitRestricted, Why::ScriptRefuses);
    CAPTURE(static_cast<int>(why));
    Builds b;
    const char* blueprint = why == Why::NotBuildable ? "ueb0101" : "ueb1101";
    if (why == Why::ArmyRestricted) {
        b.sim.get_army(0)->add_build_restriction({"ueb1101"});
    } else if (why == Why::UnitRestricted) {
        b.engineer->add_build_restriction(osc::sim::CategoryExpr::name("ueb1101"));
    } else if (why == Why::ScriptRefuses) {
        lua_pushboolean(b.L, 1);
        lua_setglobal(b.L, "__refuse");
    }
    b.build(blueprint, 46.0f, 54.0f);
    b.sim.tick();
    if (why == Why::Allowed) {
        tick_until([&] { b.sim.tick(); }, "the frame stands", 40, [&] { return b.built(); });
    } else {
        CHECK_FALSE(b.ordered());
        for (int i = 0; i < 40; ++i) {
            b.sim.tick();
        }
        CHECK_FALSE(b.built());
    }
    CHECK(b.failed() == 0);
}

TEST_CASE("A build its army may not make once its order is under way goes on",
          "[build][build_restriction]") {
    Builds b;
    b.build("ueb1101", 46.0f, 54.0f);
    b.sim.tick();
    REQUIRE(b.ordered());
    b.sim.get_army(0)->add_build_restriction({"ueb1101"});
    tick_until([&] { b.sim.tick(); }, "the frame stands", 40, [&] { return b.built(); });
    CHECK(b.failed() == 0);
}

TEST_CASE("A factory build is dropped as it comes up where its unit's script refuses it, and the "
          "factory ordered with that unit builds",
          "[build][build_restriction]") {
    const bool refuses = GENERATE(true, false);
    CAPTURE(refuses);
    Builds b;
    Unit* factory = b.factory(40.0f, 40.0f);
    b.engineer->add_category("ENGINEER");
    lua_pushboolean(b.L, refuses ? 1 : 0);
    lua_setglobal(b.L, "__refuse");
    const osc::sim::Vector3 at = b.engineer->position();
    UnitCommand build;
    build.type = CommandType::BuildFactory;
    build.blueprint_id = "uel0105";
    b.sim.route_command({b.engineer->entity_id(), factory->entity_id()}, build, false);
    CHECK(b.ordered());
    b.sim.tick();
    CHECK(b.ordered() != refuses);
    for (int i = 0; i < 9; ++i) {
        b.sim.tick();
    }
    CHECK(b.frame_at(factory->position()));
    CHECK(b.frame_at(at) != refuses);
    if (refuses) {
        CHECK_FALSE(b.ordered());
        CHECK(b.engineer->position().x == at.x);
        CHECK(b.engineer->position().z == at.z);
    }
    CHECK(b.failed() == 0);
}

TEST_CASE("A factory build its factory may not make is dropped as its task prepares, with nothing "
          "built",
          "[build][build_restriction]") {
    enum class Why { Allowed, NotBuildable, ArmyRestricted, UnitRestricted };
    const Why why =
        GENERATE(Why::Allowed, Why::NotBuildable, Why::ArmyRestricted, Why::UnitRestricted);
    CAPTURE(static_cast<int>(why));
    Builds b;
    Unit* factory = b.factory(40.0f, 40.0f);
    const char* blueprint = why == Why::NotBuildable ? "ueb1101" : "uel0105";
    if (why == Why::ArmyRestricted) {
        b.sim.get_army(0)->add_build_restriction({"uel0105"});
    } else if (why == Why::UnitRestricted) {
        factory->add_build_restriction(osc::sim::CategoryExpr::name("uel0105"));
    }
    UnitCommand build;
    build.type = CommandType::BuildFactory;
    build.blueprint_id = blueprint;
    factory->push_command(build, true);
    for (int i = 0; i < 10; ++i) {
        b.sim.tick();
    }
    CHECK(b.frame_at(factory->position()) == (why == Why::Allowed));
    CHECK(factory->command_queue().empty() == (why != Why::Allowed));
}
