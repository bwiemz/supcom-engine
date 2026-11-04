#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <memory>
#include <string>
#include <vector>

using osc::sim::CommandType;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct UpgradeWorld {
    osc::lua::LuaState lua;
    osc::blueprints::BlueprintStore store{lua.raw()};
    osc::sim::SimState sim{lua.raw(), &store};

    UpgradeWorld() {
        lua_State* L = lua.raw();
        REQUIRE(lua.do_string(R"(
            return {
                {BlueprintId = 'f1', General = {UpgradesTo = 'f2'}},
                {BlueprintId = 'f2', General = {UpgradesFrom = 'f1', UpgradesTo = 'f3'}},
                {BlueprintId = 'f3', General = {UpgradesFrom = 'f2'}},
                {BlueprintId = 'eng', General = {}},
                {BlueprintId = 'b1', General = {UpgradesFromBase = 'b1', UpgradesTo = 'b2'}},
                {BlueprintId = 'b2', General = {UpgradesFrom = 'b1', UpgradesFromBase = 'b1',
                                                UpgradesTo = 'b3'}},
                {BlueprintId = 'b3', General = {UpgradesFrom = 'b2', UpgradesFromBase = 'b1'}},
                {BlueprintId = 's1', General = {SeedUnit = 'f1'}},
                {BlueprintId = 's2', General = {SeedUnit = 'eng'}},
            }
        )")
                    .ok());
        const int list = lua_gettop(L);
        for (int i = 1; i <= luaL_getn(L, list); ++i) {
            lua_rawgeti(L, list, i);
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        REQUIRE(lua.do_string("return {BlueprintId = 'rock'}").ok());
        store.register_blueprint(L, osc::blueprints::BlueprintType::Prop, lua_gettop(L));
        lua_pop(L, 1);
        store.expose_to_lua(L);
    }

    Unit& unit(const char* bp) {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        u->set_blueprint_id(bp);
        u->add_category("FACTORY");
        Unit& made = *u;
        sim.entity_registry().register_entity(std::move(u));
        return made;
    }

    std::string queue_after(Unit& u, const std::vector<std::string>& orders, bool clear = false) {
        std::string text;
        for (const std::string& order : orders) {
            UnitCommand cmd;
            cmd.type = order == "build" ? CommandType::BuildFactory : CommandType::Upgrade;
            cmd.blueprint_id = order == "build" ? "eng" : order;
            sim.route_command({u.entity_id()}, cmd, clear);
            text += std::to_string(u.command_queue().size()) + ' ';
        }
        return text;
    }
};

} // namespace

TEST_CASE("A unit takes an upgrade order its blueprint links it to, and not the one its last order "
          "makes, as Moho's",
          "[sim][upgrade]") {
    struct Case {
        const char* unit;
        std::vector<std::string> orders;
        const char* queue;
    };
    const Case cases[] = {
        {"f1", {"f2", "f2"}, "1 1 "},
        {"f1", {"F2", "f2"}, "1 1 "},
        {"f1", {"f3"}, "1 "},
        {"f1", {"eng"}, "0 "},
        {"eng", {"f2"}, "0 "},
        {"f1", {"f2", "f3"}, "1 2 "},
        {"f1", {"f3", "f2"}, "1 2 "},
        {"f2", {"f2"}, "0 "},
        {"f2", {"f3"}, "1 "},
        {"f2", {"f1"}, "0 "},
        {"f1", {"f2", "build", "f2"}, "1 2 3 "},
        {"b2", {"b2"}, "1 "},
        {"b2", {"b1"}, "1 "},
        {"b1", {"b3"}, "1 "},
        {"b3", {"b2"}, "0 "},
        {"f1", {"s1"}, "1 "},
        {"f2", {"s1"}, "0 "},
        {"eng", {"s2"}, "1 "},
        {"f1", {"rock"}, "0 "},
        {"f1", {"nothing"}, "0 "},
    };
    for (const Case& c : cases) {
        UpgradeWorld w;
        std::string orders;
        for (const std::string& o : c.orders) {
            orders += o + ' ';
        }
        INFO(c.unit << ": " << orders);
        CHECK(w.queue_after(w.unit(c.unit), c.orders) == c.queue);
    }
}

TEST_CASE("A cleared upgrade order to what the last order already makes is refused, and the "
          "queue kept",
          "[sim][upgrade]") {
    UpgradeWorld w;
    Unit& f1 = w.unit("f1");
    CHECK(w.queue_after(f1, {"build", "f2"}) == "1 2 ");
    CHECK(w.queue_after(f1, {"f2"}, true) == "2 ");
    CHECK(f1.command_queue().front().type == CommandType::BuildFactory);
}

TEST_CASE("A player's upgrade orders not yet applied are taken as the sim will take them",
          "[sim][upgrade]") {
    UpgradeWorld w;
    Unit& f1 = w.unit("f1");
    w.sim.set_human_input_active(true);
    for (const char* to : {"f2", "f2", "f3", "f3"}) {
        UnitCommand cmd;
        cmd.type = CommandType::Upgrade;
        cmd.blueprint_id = to;
        w.sim.route_player_command({f1.entity_id()}, cmd, false);
    }
    w.sim.set_human_input_active(false);
    CHECK(f1.command_queue().empty());
    const auto pending = w.sim.queues_with_pending();
    REQUIRE(pending.count(f1.entity_id()) == 1);
    std::string orders;
    for (const UnitCommand& c : pending.at(f1.entity_id()).orders) {
        orders += c.blueprint_id + ' ';
    }
    CHECK(orders == "f2 f3 ");
}
