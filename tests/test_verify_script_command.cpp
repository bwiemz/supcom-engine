// A Script order's click goes through /lua/user/UserScriptCommand.lua's
// VerifyScriptCommand, as Moho's UI_VerifyScriptCommand hands it there (faf-re
// CWldSession.cpp, 0x0083E640; the order's table from
// BuildUserScriptCommandObject, 0x0083E150): only an answer that sets
// UserValidated lets it go, to its AuthorizedUnits, the rest of the answer
// the task's data.

#include <catch2/catch_test_macros.hpp>

#include "app/app_internal.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/lua_bytes.hpp"
#include "sim/manipulator.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <lua.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

struct VerifyWorld {
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    osc::lua::LuaState ui;
    osc::sim::EntityId id{};

    VerifyWorld() {
        sim.add_army("ARMY_1", "ARMY_1");
        auto unit = std::make_unique<osc::sim::Unit>();
        unit->set_army(0);
        unit->set_blueprint_id("uel0001");
        id = sim.entity_registry().register_entity(std::move(unit));
        osc::lua::register_moho_bindings(ui, sim);
        // The module as import() finds it; each test gives the answer
        REQUIRE(ui.do_string(R"(
            modules = { ['/lua/user/UserScriptCommand.lua'] = {
                VerifyScriptCommand = function(data) seen = data; return answer(data) end } }
            function import(name) return modules[name] end
        )")
                    .ok());
    }

    bool check(const std::string& code) {
        auto r = ui.do_string(code);
        UNSCOPED_INFO((r.ok() ? std::string() : r.error().message));
        return r.ok();
    }

    std::optional<osc::renderer::VerifiedScript> verify(osc::sim::EntityId target,
                                                        osc::sim::Vector3 at, bool clear) {
        return osc::app::verify_script_command(ui.raw(), {id}, target, at, clear);
    }
};

} // namespace

TEST_CASE("VerifyScriptCommand hears a Script order as Moho describes it", "[ui][script]") {
    VerifyWorld w;
    REQUIRE(w.check("answer = function(data) return {} end"));
    // The unit's id as scripts get it
    osc::sim::push_entity_id(w.ui.raw(), w.id);
    lua_setglobal(w.ui.raw(), "id");
    CHECK_FALSE(w.verify({}, {1, 2, 3}, true));
    CHECK(w.check("assert(table.getn(seen.Units) == 1 and seen.Units[1]:GetEntityId() == id, "
                  "'units')"
                  " assert(seen.Blueprint == '' and seen.CommandType == 'Script', 'kind')"
                  " assert(seen.Clear == true, 'clear')"
                  " assert(seen.Target.Type == 'Position' and seen.Target.EntityId == nil, 'type')"
                  " local p = seen.Target.Position"
                  " assert(p[1] == 1 and p[2] == 2 and p[3] == 3, 'position')"));

    // On a unit: its id, and where it stands
    CHECK_FALSE(w.verify(w.id, {4, 5, 6}, false));
    CHECK(w.check("assert(seen.Target.Type == 'Entity' and seen.Target.EntityId == id, 'entity')"
                  " assert(seen.Target.Position[1] == 4 and seen.Clear == false, 'where')"));
}

TEST_CASE("Only a VerifyScriptCommand answer with UserValidated lets the order go, to its "
          "AuthorizedUnits",
          "[ui][script]") {
    VerifyWorld w;
    REQUIRE(w.check("answer = function(data) return { UserValidated = false } end"));
    CHECK_FALSE(w.verify({}, {1, 2, 3}, true));
    REQUIRE(w.check("answer = function(data) return nil end"));
    CHECK_FALSE(w.verify({}, {1, 2, 3}, true));

    // Retail's own answer: the mode's task, the target as its Location
    REQUIRE(w.check("answer = function(data) return { TaskName = 'TargetLocation', "
                    "UserValidated = true, Location = data.Target.Position, "
                    "AuthorizedUnits = data.Units } end"));
    const auto verdict = w.verify({}, {1, 2, 3}, true);
    REQUIRE(verdict);
    CHECK(verdict->units == std::vector<osc::sim::EntityId>{w.id});
    // The task's data: the answer without AuthorizedUnits
    lua_State* L = w.ui.raw();
    REQUIRE(osc::sim::push_lua_bytes(L, verdict->args));
    lua_setglobal(L, "task");
    CHECK(w.check("assert(task.TaskName == 'TargetLocation' and task.UserValidated == true)"
                  " assert(task.AuthorizedUnits == nil, 'authorized')"
                  " assert(task.Location[1] == 1 and task.Location[3] == 3, 'location')"));

    // None authorized: it goes to no one
    REQUIRE(w.check("answer = function(data) return { UserValidated = true, "
                    "AuthorizedUnits = {} } end"));
    const auto none = w.verify({}, {1, 2, 3}, true);
    REQUIRE(none);
    CHECK(none->units.empty());
}

TEST_CASE("A VerifyScriptCommand that fails, or isn't there, lets nothing go", "[ui][script]") {
    VerifyWorld w;
    REQUIRE(w.check("answer = function(data) error('no such ability') end"));
    CHECK_FALSE(w.verify({}, {1, 2, 3}, true));
    REQUIRE(w.check("modules = {}"));
    CHECK_FALSE(w.verify({}, {1, 2, 3}, true));
}
