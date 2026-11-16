#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/prop.hpp"
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
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using osc::sim::CommandType;
using osc::sim::EntityId;
using osc::sim::Prop;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;
using osc::test::tick_until;

namespace {

struct ReclaimSim {
    osc::lua::LuaState lua;
    lua_State* L = lua.raw();
    SimState sim{L, nullptr};
    Unit* target = nullptr;
    Prop* wreck = nullptr;

    ReclaimSim() {
        osc::lua::register_moho_bindings(lua, sim);
        lua_settop(L, 0);
        const char* code = R"(
            log = {}
            -- ueb1101, the T1 power generator
            __blueprints = {pgen = {Economy = {BuildTime = 125, BuildCostMass = 75,
                                               BuildCostEnergy = 750}}}
            Reclaimer = {
                GetReclaimCosts = function(self, target)
                    if target == wreck then return 1, 0, 30 end
                    -- retail's Unit.GetReclaimCosts for a T1 engineer (build
                    -- rate 5): max(750, 75) / 5 / 10 seconds, all the costs
                    if target.frame then return 15, 750, 75 end
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
            Frame = {
                frame = true,
                OnReclaimed = function(self, by)
                    table.insert(log, 'frame reclaimed')
                end,
            }
            Frame.__index = Frame
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

    /// A T1 power generator under construction (ueb1101: 600 health), built
    /// to `fraction` with `health` left. Without its blueprint it never decays.
    Unit* frame(osc::f32 fraction, osc::f32 health, bool decays = true) {
        auto u = std::make_unique<Unit>();
        u->set_position({12.0f, 0.0f, 12.0f});
        if (decays) {
            u->set_unit_id("pgen");
        }
        u->set_max_health(600.0f);
        u->set_health(health);
        u->set_fraction_complete(fraction);
        u->set_is_being_built(true);
        u->set_creation_tick(sim.tick_count());
        u->add_category("RECLAIMABLE");
        Unit* raw = u.get();
        sim.entity_registry().register_entity(std::move(u));
        bind(*raw, "Frame", nullptr);
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

    std::string heard() {
        const std::string take = "local s = table.concat(log, ',') log = {} return s";
        REQUIRE(luaL_loadbuffer(L, take.c_str(), take.size(), "take") == 0);
        REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
        std::string s = lua_tostring(L, -1);
        lua_pop(L, 1);
        return s;
    }

    void run(const std::string& code) {
        REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "test") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
    }

    bool in_state(const Unit& u, const char* state) {
        lua_getglobal(L, "moho");
        lua_pushstring(L, "unit_methods");
        lua_gettable(L, -2);
        lua_pushstring(L, "IsUnitState");
        lua_gettable(L, -2);
        lua_rawgeti(L, LUA_REGISTRYINDEX, u.lua_table_ref());
        lua_pushstring(L, state);
        REQUIRE(lua_pcall(L, 2, 1, 0) == 0);
        const bool in = lua_toboolean(L, -1) != 0;
        lua_pop(L, 3);
        return in;
    }
};

UnitCommand order(CommandType type, EntityId target_id) {
    UnitCommand c;
    c.type = type;
    c.target_id = target_id;
    return c;
}

} // namespace

TEST_CASE("A reclaimed unit wears down for nothing, then its wreck is reclaimed", "[reclaim]") {
    ReclaimSim w;
    Unit* eng = w.engineer(10.0f);
    const EntityId target_id = w.target->entity_id();
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
    eng->add_category("ENGINEER");
    helper->add_category("ENGINEER");
    const EntityId target_id = w.target->entity_id();
    eng->push_command(order(CommandType::Reclaim, target_id), true);
    helper->push_command(order(CommandType::Guard, eng->entity_id()), true);
    const int ticks = tick_until(
        [&] {
            w.sim.tick();
            if (w.log().empty()) {
                CHECK(w.target->fraction_complete() == 1.0f);
            }
        },
        "the target is reclaimed", 10, [&] { return !w.log().empty(); });
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

TEST_CASE("A prop's reclaimer is Reclaiming from its arm's first tick on target, and stops the "
          "tick after the prop goes",
          "[reclaim]") {
    ReclaimSim w;
    w.run(R"(
        function Reclaimer:GetReclaimCosts(target) return 0.8, 0, 8 end
        function Reclaimer:OnStartReclaim(target)
            table.insert(log, 'start ' .. tostring(target ~= nil))
        end
        function Reclaimer:OnStopReclaim(target)
            table.insert(log, 'stop ' .. tostring(target ~= nil))
        end
    )");
    w.sim.add_army("ARMY_1", "ARMY_1");
    Unit* eng = w.engineer(10.0f);
    eng->set_army(0);
    const EntityId prop_id = w.wreck->entity_id();
    eng->push_command(order(CommandType::Reclaim, prop_id), true);
    struct Tick {
        bool reclaiming;
        osc::f32 work;
        double banked;
        const char* heard;
    };
    std::vector<Tick> ticks = {{false, 0.0f, 0.0, "start true"}, {true, 0.0f, 0.0, ""}};
    for (int i = 1; i <= 8; ++i) {
        ticks.push_back({true, 0.125f * static_cast<osc::f32>(i), 10.0, ""});
    }
    ticks.push_back({false, 0.0f, 0.0, "stop false"});
    for (size_t t = 0; t < ticks.size(); ++t) {
        CAPTURE(t + 1);
        w.sim.tick();
        CHECK(w.in_state(*eng, "Reclaiming") == ticks[t].reclaiming);
        CHECK(eng->work_progress() == ticks[t].work);
        CHECK(eng->economy().reclaimed_mass == ticks[t].banked);
        CHECK(w.heard() == ticks[t].heard);
        CHECK((w.sim.entity_registry().find(prop_id) == nullptr) == (t + 1 >= 10));
        CHECK(eng->command_queue().empty() == (t + 1 == ticks.size()));
    }
}

TEST_CASE("A prop's reclaim banks the share it takes each tick, at least a tick's worth, of no "
          "more than its health",
          "[reclaim]") {
    struct Row {
        const char* name;
        const char* costs;
        osc::f32 health;
        std::vector<double> banked;
    };
    const std::vector<Row> rows = {
        {"under a tick", "0.075, 7.5, 0", 50.0f, {75.0}},
        {"two and a half ticks", "0.25, 10, 0", 50.0f, {40.0, 40.0, 20.0}},
        {"at 98% health", "0.1, 10, 0", 49.0f, {98.0}},
    };
    for (const auto& row : rows) {
        DYNAMIC_SECTION(row.name) {
            ReclaimSim w;
            w.run(std::string("function Reclaimer:GetReclaimCosts(target) return ") + row.costs +
                  " end");
            w.sim.add_army("ARMY_1", "ARMY_1");
            w.wreck->set_max_health(50.0f);
            w.wreck->set_health(row.health);
            Unit* eng = w.engineer(10.0f);
            eng->set_army(0);
            eng->push_command(order(CommandType::Reclaim, w.wreck->entity_id()), true);
            std::vector<double> banked;
            for (int t = 0; t < 8; ++t) {
                w.sim.tick();
                if (eng->economy().reclaimed_energy != 0.0) {
                    banked.push_back(eng->economy().reclaimed_energy);
                }
            }
            REQUIRE(banked.size() == row.banked.size());
            for (size_t i = 0; i < banked.size(); ++i) {
                CHECK(banked[i] == Catch::Approx(row.banked[i]));
            }
        }
    }
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

TEST_CASE("A paused engineer reclaims a prop as an unpaused one", "[reclaim][pause]") {
    ReclaimSim w;
    Unit* eng = w.engineer(10.0f);
    eng->set_paused(true);
    eng->push_command(order(CommandType::Reclaim, w.wreck->entity_id()), true);
    w.sim.tick();
    CHECK(eng->reclaim_target_id() == w.wreck->entity_id());
    w.sim.tick();
    CHECK(eng->economy().reclaim_mass == 30.0);
    w.sim.tick();
    CHECK(w.wreck->fraction_complete() < 1.0f);
}

TEST_CASE("On FAF's data a paused engineer's reclaim of a prop holds, as FAF's exe",
          "[reclaim][pause]") {
    // FA-Binary-Patches section/StopReclaimWhenPaused.cpp: no progress and
    // nothing banked while paused; the order is kept, and goes on unpaused
    ReclaimSim w;
    w.sim.set_faf_exe(true);
    w.sim.add_army("ARMY_1", "ARMY_1"); // an army to bank in
    Unit* eng = w.engineer(10.0f);
    eng->set_army(0);
    eng->set_paused(true);
    eng->push_command(order(CommandType::Reclaim, w.wreck->entity_id()), true);
    for (int i = 0; i < 5; ++i) {
        w.sim.tick();
        CHECK(eng->reclaim_target_id() == w.wreck->entity_id());
        CHECK(w.wreck->fraction_complete() == 1.0f);
        CHECK(eng->economy().reclaimed_mass == 0.0);
    }
    REQUIRE_FALSE(eng->command_queue().empty());
    eng->set_paused(false);
    w.sim.tick();
    CHECK(w.wreck->fraction_complete() < 1.0f);
    CHECK(eng->economy().reclaimed_mass == 30.0);
}

TEST_CASE("A reclaimer hears OnAssignedFocusEntity each tick it wears a unit down, and once "
          "more for the unit's wreck",
          "[reclaim][focus]") {
    // CUnitReclaimTask's Starting state calls Unit::SetFocusEntity on the
    // unit every tick it wears it down, after OnStartReclaim and before the
    // health goes, and once more on the wreck that replaces it; a prop's
    // reclaim sets no focus
    ReclaimSim w;
    const std::string code = R"(
        function Reclaimer:OnStartReclaim(target) table.insert(log, 'start') end
        function Reclaimer:OnAssignedFocusEntity(...)
            table.insert(log, 'focus ' .. arg.n .. ' ' .. tostring(health()))
        end
    )";
    REQUIRE(luaL_loadbuffer(w.L, code.c_str(), code.size(), "focus") == 0);
    REQUIRE(lua_pcall(w.L, 0, 0, 0) == 0);
    // health(): the reclaimed unit's, nil once it has left the sim
    EntityId target_id = w.target->entity_id();
    lua_pushlightuserdata(w.L, &w.sim);
    lua_pushlightuserdata(w.L, &target_id);
    lua_pushcclosure(
        w.L,
        [](lua_State* L) -> int {
            auto* sim = static_cast<SimState*>(lua_touserdata(L, lua_upvalueindex(1)));
            const auto* id = static_cast<const EntityId*>(lua_touserdata(L, lua_upvalueindex(2)));
            const osc::sim::Entity* target = sim->entity_registry().find(*id);
            if (target) {
                lua_pushnumber(L, target->health());
            } else {
                lua_pushnil(L);
            }
            return 1;
        },
        2);
    lua_setglobal(w.L, "health");
    Unit* eng = w.engineer(10.0f);
    eng->push_command(order(CommandType::Reclaim, target_id), true);
    w.sim.tick();
    CHECK(w.heard() == "start,focus 0 100");
    for (const char* tick : {"focus 0 80", "focus 0 60", "focus 0 40"}) {
        w.sim.tick();
        CHECK(w.heard() == tick);
    }
    w.sim.tick();
    CHECK(w.heard() == "focus 0 20,reclaimed,wreck 0,focus 0 20");
    REQUIRE(eng->reclaim_target_id() == w.wreck->entity_id());
    for (int i = 0; i < 3; ++i) {
        w.sim.tick();
        CHECK(w.heard().empty());
    }
}

TEST_CASE("A unit under construction loses health with its fraction as it is reclaimed, and is "
          "built on from where the reclaim left it",
          "[reclaim][build]") {
    // CUnitReclaimTask steps a unit under construction by Unit::Materialize
    // with its rate negated, which takes the health down with the fraction.
    // A build step brings the fraction level with the health first, so a T1
    // power generator built to 60% and reclaimed to 20% goes on from 20%.
    ReclaimSim w;
    Unit* frame = w.frame(0.6f, 360.0f);
    Unit* eng = w.engineer(10.0f);
    eng->set_build_rate(5.0f);
    eng->push_command(order(CommandType::Reclaim, frame->entity_id()), true);
    osc::f32 apart = 0.0f; // the furthest its health strays from its fraction's share
    tick_until(
        [&] {
            w.sim.tick();
            apart =
                std::max(apart, std::fabs(frame->health() - 600.0f * frame->fraction_complete()));
        },
        "the frame is reclaimed to 20%", 200, [&] { return frame->fraction_complete() <= 0.2f; });
    CHECK(apart < 0.01f);
    const osc::f32 left = frame->fraction_complete();

    eng->push_command(order(CommandType::Repair, frame->entity_id()), true);
    tick_until([&] { w.sim.tick(); }, "the build goes on", 20,
               [&] { return frame->fraction_complete() > left; });
    // One step of 5 / 125 a second on from what the reclaim (and the decay
    // since) left, not back at 60%
    CHECK(frame->fraction_complete() < left + 0.005f);
    CHECK(frame->health() == Catch::Approx(600.0f * frame->fraction_complete()).margin(0.01f));
}

TEST_CASE("A damaged unit under construction is reclaimed from its health's share, for that "
          "share's mass",
          "[reclaim][economy]") {
    // As CUnitReclaimTask starts on a unit under construction it makes it no
    // more complete than its health's share; its steps then credit the mass of
    // what they take off. A frame built to 60% with half that health left
    // pays 30% of its 75 mass, not 60%.
    ReclaimSim w;
    w.sim.add_army("ARMY_1", "ARMY_1"); // an army to bank in
    Unit* frame = w.frame(0.6f, 180.0f, false);
    const EntityId frame_id = frame->entity_id();
    Unit* eng = w.engineer(10.0f);
    eng->set_army(0);
    eng->set_build_rate(5.0f);
    eng->push_command(order(CommandType::Reclaim, frame_id), true);
    tick_until([&] { w.sim.tick(); }, "the reclaim takes the frame's costs", 10,
               [&] { return eng->economy().reclaim_mass > 0.0; });
    CHECK(frame->fraction_complete() == 0.3f);
    CHECK(frame->health() == 180.0f);
    tick_until([&] { w.sim.tick(); }, "the frame is reclaimed", 200,
               [&] { return w.sim.entity_registry().find(frame_id) == nullptr; });
    CHECK(w.log() == "frame reclaimed");
    w.sim.tick(); // the army takes in the last of it
    CHECK(w.sim.get_army(0)->economy().mass.reclaimed == Catch::Approx(22.5));
}

TEST_CASE("A guard helping reclaim a unit under construction takes its health down with it",
          "[reclaim][guard]") {
    // The guard's own CUnitReclaimTask steps the unit by Unit::Materialize too
    ReclaimSim w;
    Unit* frame = w.frame(0.6f, 360.0f, false);
    Unit* eng = w.engineer(10.0f);
    Unit* helper = w.engineer(14.0f);
    for (Unit* u : {eng, helper}) {
        u->add_category("ENGINEER");
        u->set_build_rate(5.0f);
    }
    eng->push_command(order(CommandType::Reclaim, frame->entity_id()), true);
    helper->push_command(order(CommandType::Guard, eng->entity_id()), true);
    osc::f32 apart = 0.0f;
    const int ticks = tick_until(
        [&] {
            w.sim.tick();
            apart =
                std::max(apart, std::fabs(frame->health() - 600.0f * frame->fraction_complete()));
        },
        "the frame is reclaimed to 20%", 200, [&] { return frame->fraction_complete() <= 0.2f; });
    CHECK(ticks < 55); // alone, 0.4 at 1/150 a tick is 60 steps
    CHECK(apart < 0.01f);
}
