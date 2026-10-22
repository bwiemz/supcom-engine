#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/econ_request.hpp"
#include "sim/economy_event.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "sim/weapon.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cstring>
#include <memory>
#include <tuple>
#include <vector>

using Catch::Approx;
using osc::sim::EconomyEvent;
using osc::sim::EconomyEventRegistry;
using osc::sim::SimState;
using osc::sim::Unit;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

struct Armies {
    LuaGuard g;
    SimState sim{g.L, nullptr};

    Armies() {
        std::vector<osc::u16> heights(129 * 129, 1000);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.set_victory_condition("sandbox");
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
    }

    Unit* add(osc::f32 x, osc::f32 z) {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        u->set_position({x, 0.0f, z});
        u->set_max_health(100.0f);
        u->set_health(100.0f);
        Unit* raw = u.get();
        sim.entity_registry().register_entity(std::move(u));
        return raw;
    }
};

enum class Work { Silo, Capture, DockRepair, Event };

struct Stall : Armies {
    Work work;
    Unit* worker = nullptr;
    EconomyEvent* event = nullptr;

    Stall(Work w, double energy_income) : work(w) {
        const char* code = "__blueprints = {"
                           " missile = {Economy = {BuildTime = 1000, BuildCostEnergy = 1000,"
                           " BuildCostMass = 100}},"
                           " prize = {Economy = {BuildTime = 2000, BuildCostEnergy = 1000}}}";
        REQUIRE(luaL_loadbuffer(g.L, code, std::strlen(code), "bp") == 0);
        REQUIRE(lua_pcall(g.L, 0, 0, 0) == 0);
        Unit* drain = add(60.0f, 60.0f);
        drain->economy().consumption_energy = 1000.0;
        drain->economy().consumption_active = true;
        Unit* source = add(70.0f, 60.0f);
        source->economy().production_energy = energy_income;
        source->economy().production_mass = 1000.0;
        source->economy().production_active = true;
        worker = add(10.0f, 10.0f);
        worker->set_build_rate(10.0f);
        switch (w) {
        case Work::Silo: {
            auto launcher = std::make_unique<osc::sim::Weapon>();
            launcher->counted_projectile = true;
            launcher->nuke_weapon = true;
            launcher->max_projectile_storage = 5;
            launcher->projectile_bp_id = "missile";
            worker->add_weapon(std::move(launcher));
            worker->order_silo_build(true);
            break;
        }
        case Work::Capture: {
            Unit* prize = add(11.0f, 10.0f);
            prize->set_army(1);
            prize->set_unit_id("prize");
            osc::sim::UnitCommand capture;
            capture.type = osc::sim::CommandType::Capture;
            capture.target_id = prize->entity_id();
            capture.target_pos = prize->position();
            worker->push_command(capture, true);
            break;
        }
        case Work::DockRepair: {
            Unit* pad = add(30.0f, 30.0f);
            pad->add_category("AIRSTAGINGPLATFORM");
            osc::sim::StagingRules rules;
            rules.repair_amount = 10.0f;
            rules.repair_energy = 1.0f;
            rules.repair_mass = 0.1f;
            pad->set_staging_rules(rules);
            worker->set_transport_id(pad->entity_id());
            worker->set_fuel_use_time(100.0f);
            worker->set_fuel_ratio(0.5f);
            worker->set_max_health(100000.0f);
            break;
        }
        case Work::Event:
            event = sim.economy_events().create(worker->entity_id(), 0.0, 1000.0, 100.0);
            break;
        }
    }

    double done() const {
        switch (work) {
        case Work::Silo: return worker->silo_build().progress;
        case Work::Capture: return worker->work_progress();
        case Work::DockRepair: return worker->health();
        case Work::Event: return event->progress();
        }
        return 0.0;
    }
};

} // namespace

TEST_CASE("Silo builds, captures, dock repairs and economy events step only once their army has "
          "granted a tick's whole cost",
          "[economy_event][economy]") {
    const Work work = GENERATE(Work::Silo, Work::Capture, Work::DockRepair, Work::Event);
    const double share = GENERATE(1.0, 0.9, 0.25);
    CAPTURE(static_cast<int>(work), share);
    Stall s(work, share < 1.0 ? share * 1010.0 : 2000.0);
    const int ticks = 60;
    int steps = 0;
    for (int i = 0; i < ticks; ++i) {
        const double before = s.done();
        s.sim.tick();
        if (s.done() > before) {
            ++steps;
        }
    }
    if (share == 1.0) {
        CHECK(steps == ticks - 1);
    } else if (share > 0.5) {
        CHECK(steps > 0);
        CHECK(steps < ticks / 4);
    } else {
        CHECK(steps == 0);
    }
}

TEST_CASE("An economy event takes its duration's whole ticks, a tick at least, asking for its "
          "cost over them",
          "[economy_event][economy]") {
    const auto [duration, ticks] = GENERATE(table<double, int>({{0.0, 1},
                                                                {0.05, 1},
                                                                {0.1, 1},
                                                                {0.15, 1},
                                                                {0.25, 2},
                                                                {0.3, 3},
                                                                {0.35, 3},
                                                                {0.5, 5},
                                                                {0.7, 7},
                                                                {0.99, 9},
                                                                {1.0, 10},
                                                                {1.05, 10},
                                                                {2.5, 25}}));
    CAPTURE(duration);
    Armies a;
    a.sim.get_army(0)->set_extra_storage(0.0, 1000.0);
    a.sim.get_army(0)->set_stored_resources(0.0, 1000.0);
    Unit* holder = a.add(10.0f, 10.0f);
    lua_State* L = a.g.L;
    lua_newtable(L);
    holder->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    const char* code = "heard = {} n = 0 return function(u, p) n = n + 1 heard[n] = p end";
    REQUIRE(luaL_loadbuffer(L, code, std::strlen(code), "heard") == 0);
    REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
    a.sim.economy_events()
        .create(holder->entity_id(), 0.0, 10.0, duration)
        ->set_callback_ref(luaL_ref(L, LUA_REGISTRYINDEX));
    a.sim.tick();
    CHECK(a.sim.get_army(0)->economy().energy.requested == Approx(100.0 / ticks));
    int taken = 1;
    while (a.sim.economy_events().count() != 0 && taken < 100) {
        a.sim.tick();
        ++taken;
    }
    CHECK(taken == ticks);
    lua_getglobal(L, "heard");
    lua_rawgeti(L, -1, 1);
    CHECK(lua_tonumber(L, -1) == Approx(1.0 / ticks));
    lua_settop(L, 0);
}

TEST_CASE("A unit's consumed rate is what it spent in the tick, as Moho's GetEconData reports it",
          "[economy_event][economy]") {
    enum class Spender { Request, Event };
    const auto [spender, share] = GENERATE(table<Spender, double>({{Spender::Request, 1.0},
                                                                   {Spender::Request, 0.5},
                                                                   {Spender::Request, 0.0},
                                                                   {Spender::Event, 1.0},
                                                                   {Spender::Event, 0.0}}));
    CAPTURE(static_cast<int>(spender), share);
    Armies a;
    Unit* source = a.add(20.0f, 20.0f);
    source->economy().production_energy = 20.0 * share;
    source->economy().production_active = true;
    Unit* spending = a.add(10.0f, 10.0f);
    if (spender == Spender::Request) {
        spending->economy().consumption_energy = 20.0;
        spending->economy().consumption_active = true;
    } else {
        a.sim.economy_events().create(spending->entity_id(), 0.0, 200.0, 10.0);
    }
    for (int i = 0; i < 5; ++i) {
        a.sim.tick();
    }
    CHECK(spending->economy().energy_consumed() == Approx(20.0 * share).margin(1e-6));
}

TEST_CASE("An economy event asks for a tick's share of its cost and moves on a tick a step",
          "[economy_event]") {
    EconomyEvent evt(1, 50.0, 1000.0, 1.0);
    CHECK(evt.request().requested.mass == 5.0f);
    CHECK(evt.request().requested.energy == 100.0f);
    for (int i = 0; i < 9; ++i) {
        evt.step();
    }
    CHECK_FALSE(evt.is_done());
    CHECK(evt.progress() == Approx(0.9));
    evt.step();
    CHECK(evt.is_done());
    CHECK(evt.progress() == 1.0f);
}

TEST_CASE("A request's grants close on its cost a share of the shortfall at a time, reaching it "
          "only from half a share",
          "[economy_event][economy]") {
    const float share = GENERATE(1.0f, 0.9f, 0.6f, 0.4f, 0.25f);
    CAPTURE(share);
    osc::sim::EconRequest q;
    q.ask(0.5, 12.0);
    int taken = 0;
    for (int tick = 0; tick < 200; ++tick) {
        q.grant(share);
        if (q.take()) {
            ++taken;
        }
    }
    if (share == 1.0f) {
        CHECK(taken == 200);
    } else if (share > 0.5f) {
        CHECK(taken > 0);
        CHECK(taken < 50);
    } else {
        CHECK(taken == 0);
        CHECK(q.granted.energy < 12.0f);
        CHECK(q.outstanding().energy > 0.0f);
    }
}

TEST_CASE("Events made during a pass wait for the next", "[economy_event]") {
    // A progress callback may make events; enough of them move the list.
    // Only the first visit makes them: a pass that visited new events would
    // otherwise make more without end, filling memory instead of failing.
    EconomyEventRegistry registry;
    registry.create(1, 0.0, 10.0, 1.0);
    int visits = 0;
    registry.for_each([&](EconomyEvent&) {
        if (++visits > 1) return;
        for (int i = 0; i < 64; ++i) registry.create(2, 0.0, 1.0, 1.0);
    });
    CHECK(visits == 1);
    CHECK(registry.count() == 65);

    visits = 0;
    registry.for_each([&](EconomyEvent&) { ++visits; });
    CHECK(visits == 65);
}

TEST_CASE("Finished and cancelled events are collected", "[economy_event]") {
    EconomyEventRegistry registry;
    EconomyEvent* done = registry.create(1, 0.0, 1.0, 0.1);
    EconomyEvent* cancelled = registry.create(1, 0.0, 1.0, 1.0);
    registry.create(1, 0.0, 1.0, 1.0);
    done->step();
    cancelled->cancel();
    CHECK_FALSE(cancelled->active());
    registry.gc();
    CHECK(registry.count() == 1);
}
