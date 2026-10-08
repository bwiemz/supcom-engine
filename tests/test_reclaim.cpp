#include <catch2/catch_test_macros.hpp>

#include "sim/manipulator.hpp"
#include "sim/prop.hpp"
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

using osc::sim::CommandType;
using osc::sim::Prop;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct ReclaimSim {
    lua_State* L = lua_open();
    SimState sim{L, nullptr};
    Unit* target = nullptr;
    Prop* wreck = nullptr;

    ReclaimSim() {
        luaopen_base(L);
        luaopen_table(L);
        lua_settop(L, 0);
        const char* code = R"(
            log = {}
            Reclaimer = {
                GetReclaimCosts = function(self, target)
                    if target == wreck then return 1, 0, 30 end
                    return 0.5, 100, 50
                end,
            }
            Reclaimer.__index = Reclaimer
            Target = {
                OnReclaimed = function(self, by)
                    table.insert(log, 'reclaimed')
                end,
                CreateWreckageProp = function(self, overkill)
                    table.insert(log, 'wreck ' .. tostring(overkill))
                    return wreck
                end,
            }
            Target.__index = Target
        )";
        REQUIRE(luaL_loadbuffer(L, code, std::string(code).size(), "stub") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);

        auto t = std::make_unique<Unit>();
        t->set_position({10.0f, 0.0f, 12.0f});
        t->set_max_health(100.0f);
        t->set_health(100.0f);
        t->add_category("RECLAIMABLE");
        target = t.get();
        sim.entity_registry().register_entity(std::move(t));
        bind(*target, "Target", nullptr);

        auto w = std::make_unique<Prop>();
        w->set_position({10.0f, 0.0f, 12.0f});
        w->reclaim_mass_max = 30.0f;
        w->reclaimable_category = true;
        wreck = w.get();
        sim.entity_registry().register_entity(std::move(w));
        bind(*wreck, nullptr, "wreck");
    }
    ~ReclaimSim() { lua_close(L); }
    ReclaimSim(const ReclaimSim&) = delete;
    ReclaimSim& operator=(const ReclaimSim&) = delete;

    void bind(osc::sim::Entity& e, const char* cls, const char* global) {
        lua_newtable(L);
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, &e);
        lua_rawset(L, -3);
        if (cls) {
            lua_getglobal(L, cls);
            lua_setmetatable(L, -2);
        }
        if (global) {
            lua_pushvalue(L, -1);
            lua_setglobal(L, global);
        }
        e.set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    }

    Unit* engineer(osc::f32 x) {
        auto u = std::make_unique<Unit>();
        u->set_position({x, 0.0f, 10.0f});
        u->set_build_rate(10.0f);
        u->set_max_build_distance(10.0f);
        u->add_category("RECLAIM");
        Unit* raw = u.get();
        sim.entity_registry().register_entity(std::move(u));
        bind(*raw, "Reclaimer", nullptr);
        return raw;
    }

    std::string log() {
        const std::string code = "return table.concat(log, ',')";
        REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "l") == 0);
        REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
        std::string v = lua_tostring(L, -1);
        lua_pop(L, 1);
        return v;
    }
};

UnitCommand order(CommandType type, osc::u32 target_id) {
    UnitCommand c;
    c.type = type;
    c.target_id = target_id;
    return c;
}

} // namespace

TEST_CASE("A reclaimed unit wears down for nothing, then its wreck is reclaimed", "[reclaim]") {
    ReclaimSim w;
    Unit* eng = w.engineer(10.0f);
    const osc::u32 target_id = w.target->entity_id();
    eng->push_command(order(CommandType::Reclaim, target_id), true);
    for (const osc::f32 health : {80.0f, 60.0f, 40.0f, 20.0f}) {
        w.sim.tick();
        CHECK(w.target->health() == health);
        CHECK(w.target->fraction_complete() == 1.0f);
        CHECK(eng->economy().reclaim_mass == 0.0);
        CHECK(w.log().empty());
    }
    w.sim.tick();
    CHECK(w.log() == "reclaimed,wreck 0");
    CHECK(w.sim.entity_registry().find(target_id) == nullptr);
    CHECK(eng->reclaim_target_id() == w.wreck->entity_id());
    REQUIRE_FALSE(eng->command_queue().empty());
    CHECK(eng->command_queue().front().target_id == w.wreck->entity_id());
    CHECK(eng->economy().reclaim_mass == 0.0);
    w.sim.tick();
    CHECK(eng->economy().reclaim_mass == 30.0);
    CHECK(w.wreck->fraction_complete() == 1.0f);
}

TEST_CASE("A guard helps wear a reclaimed unit down and leaves the finish to the reclaimer",
          "[reclaim][guard]") {
    ReclaimSim w;
    Unit* eng = w.engineer(10.0f);
    Unit* helper = w.engineer(12.0f);
    const osc::u32 target_id = w.target->entity_id();
    eng->push_command(order(CommandType::Reclaim, target_id), true);
    helper->push_command(order(CommandType::Guard, eng->entity_id()), true);
    int ticks = 0;
    while (w.log().empty() && ticks < 10) {
        w.sim.tick();
        ++ticks;
        if (w.log().empty()) {
            CHECK(w.target->fraction_complete() == 1.0f);
        }
    }
    CHECK(ticks < 5);
    CHECK(w.log() == "reclaimed,wreck 0");
    CHECK(eng->reclaim_target_id() == w.wreck->entity_id());
}

TEST_CASE("A prop's reclaim takes it down from the second tick after it starts", "[reclaim]") {
    ReclaimSim w;
    Unit* eng = w.engineer(10.0f);
    eng->push_command(order(CommandType::Reclaim, w.wreck->entity_id()), true);
    w.sim.tick();
    CHECK(eng->reclaim_target_id() == w.wreck->entity_id());
    CHECK(w.wreck->fraction_complete() == 1.0f);
    CHECK(eng->economy().reclaim_mass == 0.0);
    w.sim.tick();
    CHECK(w.wreck->fraction_complete() == 1.0f);
    CHECK(eng->economy().reclaim_mass == 30.0);
    w.sim.tick();
    CHECK(w.wreck->fraction_complete() < 1.0f);
}

TEST_CASE("A reclaim takes nothing while the reclaimer's arm is off its target", "[reclaim][arm]") {
    ReclaimSim w;
    Unit* eng = w.engineer(10.0f);
    auto arm = std::make_unique<osc::sim::AimManipulator>();
    arm->set_builder_arm(true);
    arm->set_enabled(false);
    osc::sim::Manipulator* raw = eng->add_manipulator(std::move(arm));
    eng->set_builder_on_target(false);
    eng->push_command(order(CommandType::Reclaim, w.wreck->entity_id()), true);
    for (int i = 0; i < 4; ++i) {
        w.sim.tick();
        CHECK(eng->reclaim_target_id() == w.wreck->entity_id());
        CHECK(w.wreck->fraction_complete() == 1.0f);
        CHECK(eng->economy().reclaim_mass == 0.0);
    }
    raw->set_enabled(true);
    w.sim.tick();
    CHECK(w.wreck->fraction_complete() == 1.0f);
    w.sim.tick();
    CHECK(eng->economy().reclaim_mass == 30.0);
    w.sim.tick();
    CHECK(w.wreck->fraction_complete() < 1.0f);
}
