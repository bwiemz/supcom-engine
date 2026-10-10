#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <memory>
#include <string>

using osc::sim::AimManipulator;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct AssistSim {
    lua_State* L = lua_open();
    SimState sim{L, nullptr};
    Unit* site = nullptr;

    AssistSim() {
        luaopen_base(L);
        luaopen_table(L);
        lua_settop(L, 0);
        const char* code = R"(
            log = {}
            __blueprints = {site = {Economy = {BuildTime = 100, BuildCostMass = 10, BuildCostEnergy = 10}}}
            Builder = {
                OnStartBuild = function(self, unit, order)
                    table.insert(log, self.name .. ' start ' .. tostring(order))
                end,
                OnStopBuild = function(self, unit)
                    table.insert(log, self.name .. ' stop')
                end,
                OnPrepareArmToBuild = function(self)
                    table.insert(log, self.name .. ' arm')
                end,
            }
            Builder.__index = Builder
        )";
        REQUIRE(luaL_loadbuffer(L, code, std::string(code).size(), "stub") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);

        auto s = std::make_unique<Unit>();
        s->set_unit_id("site");
        s->set_position({20.0f, 0.0f, 20.0f});
        s->set_max_health(100.0f);
        s->set_health(10.0f);
        s->set_fraction_complete(0.1f);
        s->set_is_being_built(true);
        site = s.get();
        sim.entity_registry().register_entity(std::move(s));
        bind(*site, nullptr);
    }
    ~AssistSim() { lua_close(L); }
    AssistSim(const AssistSim&) = delete;
    AssistSim& operator=(const AssistSim&) = delete;

    void bind(osc::sim::Entity& e, const char* name) {
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, &e);
        lua_rawset(L, -3);
        if (name) {
            lua_pushstring(L, "name");
            lua_pushstring(L, name);
            lua_rawset(L, -3);
            lua_getglobal(L, "Builder");
            lua_setmetatable(L, -2);
        }
        e.set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    }

    Unit* engineer(const char* name, osc::f32 x) {
        auto u = std::make_unique<Unit>();
        u->set_position({x, 0.0f, 20.0f});
        u->set_build_rate(10.0f);
        u->set_max_build_distance(10.0f);
        u->add_category("REPAIR");
        auto arm = std::make_unique<AimManipulator>();
        arm->set_builder_arm(true);
        u->add_manipulator(std::move(arm));
        Unit* raw = u.get();
        sim.entity_registry().register_entity(std::move(u));
        bind(*raw, name);
        return raw;
    }

    std::string log() {
        const std::string code = "local s = table.concat(log, ','); log = {}; return s";
        REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "l") == 0);
        REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
        std::string v = lua_tostring(L, -1);
        lua_pop(L, 1);
        return v;
    }
};

UnitCommand order(CommandType type, const Unit& target, osc::u32 id) {
    UnitCommand c;
    c.type = type;
    c.target_id = target.entity_id();
    c.target_pos = target.position();
    c.command_id = id;
    return c;
}

const AimManipulator& arm_of(const Unit& u) {
    return static_cast<const AimManipulator&>(*u.manipulators().front());
}

} // namespace

TEST_CASE("A repair of a unit under construction aims the arm and builds it as a repair",
          "[build][assist]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    w.sim.tick();
    CHECK(eng->is_building());
    CHECK(w.log() == "eng arm,eng start Repair");
    CHECK(arm_of(*eng).has_target());

    eng->push_command(order(CommandType::Stop, *w.site, 2), true);
    w.sim.tick();
    CHECK(w.log() == "eng stop");
    CHECK_FALSE(arm_of(*eng).has_target());
}

TEST_CASE("A guard helping a build aims its arm at what it helps build", "[build][assist]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    Unit* helper = w.engineer("helper", 14.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    helper->push_command(order(CommandType::Guard, *eng, 2), true);
    w.sim.tick();
    w.sim.tick();
    CHECK(helper->is_building());
    CHECK(arm_of(*helper).has_target());
}

TEST_CASE("A repair of a unit under construction starts once the builder's arm is on it",
          "[build][assist][arm]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    auto& arm = *eng->manipulators().front();
    arm.set_enabled(false);
    eng->set_builder_on_target(false);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    w.sim.tick();
    CHECK(w.log() == "eng arm");
    for (int i = 0; i < 3; ++i) {
        w.sim.tick();
        CHECK_FALSE(eng->is_building());
        CHECK(arm_of(*eng).has_target());
    }
    CHECK(w.log().empty());
    CHECK(w.site->fraction_complete() <= 0.1f);

    arm.set_enabled(true);
    w.sim.tick();
    CHECK_FALSE(eng->is_building());
    w.sim.tick();
    CHECK(eng->is_building());
    CHECK(w.log() == "eng start Repair");
}

TEST_CASE("A paused repairer of a unit under construction holds it without work",
          "[build][assist][pause]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    w.sim.tick();
    w.sim.tick();
    REQUIRE(eng->is_building());
    const osc::f32 frac = w.site->fraction_complete();
    eng->set_paused(true);
    for (int i = 0; i < 20; ++i) {
        w.sim.tick();
    }
    CHECK(eng->is_building());
    CHECK(eng->command_queue().size() == 1);
    CHECK(w.site->fraction_complete() == frac);
    eng->set_paused(false);
    w.sim.tick();
    CHECK(w.site->fraction_complete() > frac);
}

TEST_CASE("A repairer paused before it starts waits at its target until a retry after unpause",
          "[build][assist][pause]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    eng->set_paused(true);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    for (int i = 0; i < 15; ++i) {
        w.sim.tick();
        CHECK_FALSE(eng->is_building());
    }
    CHECK(eng->command_queue().size() == 1);
    CHECK(arm_of(*eng).has_target());
    eng->set_paused(false);
    for (int i = 0; i < 5; ++i) {
        w.sim.tick();
        CHECK_FALSE(eng->is_building());
    }
    w.sim.tick();
    CHECK(eng->is_building());
}

TEST_CASE("A guard paused before it helps a build waits until a retry after unpause",
          "[build][assist][pause]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    Unit* helper = w.engineer("helper", 14.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    helper->set_paused(true);
    helper->push_command(order(CommandType::Guard, *eng, 2), true);
    for (int i = 0; i < 15; ++i) {
        w.sim.tick();
        CHECK_FALSE(helper->is_building());
    }
    helper->set_paused(false);
    for (int i = 0; i < 5; ++i) {
        w.sim.tick();
        CHECK_FALSE(helper->is_building());
    }
    w.sim.tick();
    CHECK(helper->is_building());
}

TEST_CASE("A paused guard helping a build holds it and adds nothing", "[build][assist][pause]") {
    AssistSim w;
    Unit* eng = w.engineer("eng", 16.0f);
    Unit* helper = w.engineer("helper", 14.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    helper->push_command(order(CommandType::Guard, *eng, 2), true);
    w.sim.tick();
    w.sim.tick();
    REQUIRE(helper->is_building());
    helper->set_paused(true);
    const osc::f32 before = w.site->fraction_complete();
    w.sim.tick();
    const osc::f32 step = w.site->fraction_complete() - before;
    w.sim.tick();
    CHECK(helper->is_building());
    CHECK(w.site->fraction_complete() - before == Catch::Approx(2 * step));
    CHECK(step == Catch::Approx(0.01f));
}

TEST_CASE("A paused repairer of a damaged unit holds it without work", "[build][assist][pause]") {
    AssistSim w;
    w.site->set_unit_id("site");
    w.site->set_is_being_built(false);
    w.site->set_fraction_complete(1.0f);
    w.site->set_health(50.0f);
    Unit* eng = w.engineer("eng", 16.0f);
    eng->push_command(order(CommandType::Repair, *w.site, 1), true);
    w.sim.tick();
    w.sim.tick();
    REQUIRE(eng->is_repairing());
    const osc::f32 hp = w.site->health();
    REQUIRE(hp > 50.0f);
    eng->set_paused(true);
    for (int i = 0; i < 5; ++i) {
        w.sim.tick();
    }
    CHECK(eng->is_repairing());
    CHECK(w.site->health() == hp);
    eng->set_paused(false);
    w.sim.tick();
    CHECK(w.site->health() > hp);
}
