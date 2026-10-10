#include <catch2/catch_test_macros.hpp>

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/moho_bindings_internal.hpp"
#include "sim/command_codec.hpp"
#include "sim/lua_bytes.hpp"
#include "sim/manipulator.hpp"
#include "sim/replay.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

/// A sim whose `import` hands out test task classes (M206w), and units with
/// plain script objects.
struct TaskSim {
    lua_State* L = lua_open();
    SimState sim{L, nullptr};

    static int l_tick(lua_State* L) {
        auto* sim = static_cast<SimState*>(lua_touserdata(L, lua_upvalueindex(1)));
        lua_pushnumber(L, sim->tick_count());
        return 1;
    }

    TaskSim() {
        luaopen_base(L);
        luaopen_table(L);
        luaopen_string(L);
        lua_settop(L, 0);
        lua_pushstring(L, "sim_tick");
        lua_pushlightuserdata(L, &sim);
        lua_pushcclosure(L, l_tick, 1);
        lua_rawset(L, LUA_GLOBALSINDEX);
        const char* code = R"(
            log = {}
            function note(s) table.insert(log, s) end
            local function class(t) t.__index = t return t end
            -- Retail's ScriptTask, as the fallback: it fails its order.
            ScriptTask = class({
                OnCreate = function(self, args) self.CommandData = args note('base') end,
                TaskTick = function(self) note('base tick') return -1 end,
            })
            -- Returns the statuses in self.CommandData.Statuses, one a tick
            Seq = class({
                OnCreate = function(self, args)
                    self.CommandData = args
                    self.At = 1
                    note('create ' .. tostring(args.Payload))
                end,
                TaskTick = function(self)
                    local status = self.CommandData.Statuses[self.At]
                    self.At = self.At + 1
                    note('tick ' .. sim_tick())
                    return status
                end,
                OnDestroy = function(self) note('destroy ' .. sim_tick()) end,
            })
            Broken = class({
                TaskTick = function(self) error('broken task') end,
                OnDestroy = function(self) note('destroy') end,
            })
            Forever = class({ TaskTick = function(self) note('x') return 0 end })
            Mute = class({ TaskTick = function(self) note('mute') end })
            Unit = class({
                TaskTick = function(self)
                    note(tostring(self:GetUnit() == unit_object))
                    self:SetAIResult(2)
                    return -1
                end,
            })
            modules = {
                ['/lua/sim/tasks/Seq.lua'] = { Seq = Seq },
                ['/lua/sim/tasks/Broken.lua'] = { Broken = Broken },
                ['/lua/sim/tasks/Forever.lua'] = { Forever = Forever },
                ['/lua/sim/tasks/Mute.lua'] = { Mute = Mute },
                ['/lua/sim/tasks/Unit.lua'] = { Unit = Unit },
                ['/lua/sim/ScriptTask.lua'] = { ScriptTask = ScriptTask },
            }
            function import(path) return modules[path] or error('no module ' .. path) end
        )";
        REQUIRE(luaL_loadbuffer(L, code, std::string(code).size(), "stub") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
        // moho.ScriptTask_Methods, as the engine gives retail's ScriptTask
        for (const auto* m = osc::lua::script_task_methods; m->name; ++m) {
            for (const char* cls : {"ScriptTask", "Seq", "Broken", "Forever", "Mute", "Unit"}) {
                lua_pushstring(L, cls);
                lua_rawget(L, LUA_GLOBALSINDEX);
                lua_pushstring(L, m->name);
                lua_pushcfunction(L, m->func);
                lua_rawset(L, -3);
                lua_pop(L, 1);
            }
        }
    }
    ~TaskSim() { lua_close(L); }
    TaskSim(const TaskSim&) = delete;
    TaskSim& operator=(const TaskSim&) = delete;

    osc::u32 spawn() {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        Unit* raw = u.get();
        const osc::u32 id = sim.entity_registry().register_entity(std::move(u));
        lua_newtable(L); // its script object, as the sim gives every unit
        lua_pushstring(L, "_c_object");
        lua_pushlightuserdata(L, raw);
        lua_rawset(L, -3);
        lua_pushstring(L, "unit_object");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_GLOBALSINDEX);
        raw->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
        return id;
    }
    Unit& unit(osc::u32 id) { return *static_cast<Unit*>(sim.entity_registry().find(id)); }

    /// A Script order of `table` (Lua source of its table).
    UnitCommand script(const std::string& table) {
        REQUIRE(luaL_loadbuffer(L, ("return " + table).c_str(), table.size() + 7, "t") == 0);
        REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
        UnitCommand cmd;
        cmd.type = CommandType::Script;
        auto bytes = osc::sim::lua_to_bytes(L, -1);
        REQUIRE(bytes);
        cmd.script_args = *bytes;
        lua_pop(L, 1);
        return cmd;
    }

    std::string log() {
        const std::string code = "return table.concat(log, ',')";
        REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "l") == 0);
        REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
        std::string v = lua_tostring(L, -1);
        lua_pop(L, 1);
        return v;
    }
    void clear_log() {
        REQUIRE(luaL_loadbuffer(L, "log = {}", 8, "c") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
    }
};

/// Test-mode failure counting, on for a scope.
struct CountingLuaFailures {
    CountingLuaFailures() {
        osc::test_status::reset();
        osc::test_status::set_count_lua_failures(true);
    }
    ~CountingLuaFailures() {
        osc::test_status::set_count_lua_failures(false);
        osc::test_status::reset();
    }
    CountingLuaFailures(const CountingLuaFailures&) = delete;
    CountingLuaFailures& operator=(const CountingLuaFailures&) = delete;
};

} // namespace

TEST_CASE("A Script order runs its task: TaskTick's statuses in ticks, then OnDestroy",
          "[script_orders]") {
    TaskSim w;
    const osc::u32 id = w.spawn();
    // Wait, then wait 2 ticks (3), then repeat (0) in the same tick, wait,
    // done: ticks 1, 2, 4, 4, 5
    w.unit(id).push_command(w.script("{ TaskName = 'Seq', Payload = 'p', "
                                     "Statuses = { 1, 3, 0, 1, -1 } }"),
                            false);
    UnitCommand stop;
    stop.type = CommandType::Stop;
    w.unit(id).push_command(stop, false);
    for (int i = 0; i < 6; ++i) w.sim.tick();
    CHECK(w.log() == "create p,tick 1,tick 2,tick 4,tick 4,tick 5,destroy 5");
    CHECK_FALSE(w.unit(id).has_script_task());
    CHECK(w.unit(id).command_queue().empty()); // the next order ran at once
}

TEST_CASE("A Script task ends with its order: cleared, replaced, suspended, aborted",
          "[script_orders]") {
    TaskSim w;
    const osc::u32 id = w.spawn();
    Unit& u = w.unit(id);
    // Cleared while it waits: its OnDestroy runs the next tick
    u.push_command(w.script("{ TaskName = 'Seq', Statuses = { 1, 1, 1 } }"), false);
    w.sim.tick();
    u.clear_commands();
    w.sim.tick();
    CHECK(w.log() == "create nil,tick 1,destroy 2");
    CHECK_FALSE(u.has_script_task());

    // Suspended: no more ticks, and what comes after waits, until replaced
    w.clear_log();
    u.push_command(w.script("{ TaskName = 'Seq', Statuses = { -2 } }"), false);
    UnitCommand stop;
    stop.type = CommandType::Stop;
    u.push_command(stop, false);
    for (int i = 0; i < 3; ++i) w.sim.tick();
    CHECK(w.log() == "create nil,tick 3");
    CHECK(u.command_queue().size() == 2);
    u.push_command(stop, true);
    w.sim.tick();
    CHECK(w.log() == "create nil,tick 3,destroy 6");

    // Aborted: the order ends, as done
    w.clear_log();
    u.push_command(w.script("{ TaskName = 'Seq', Statuses = { -3 } }"), false);
    w.sim.tick();
    CHECK(w.log() == "create nil,tick 7,destroy 7");
    CHECK(u.command_queue().empty());
}

TEST_CASE("A Script task's unit dying or destroyed ends it", "[script_orders]") {
    TaskSim w;
    const osc::u32 dying = w.spawn();
    const osc::u32 gone = w.spawn();
    w.unit(dying).push_command(w.script("{ TaskName = 'Seq', Statuses = { 1, 1, 1 } }"), false);
    w.unit(gone).push_command(w.script("{ TaskName = 'Seq', Statuses = { 1, 1, 1 } }"), false);
    w.sim.tick();
    w.clear_log();
    w.unit(dying).begin_dying();
    w.sim.tick();                         // its first dying tick
    CHECK(w.log() == "destroy 2,tick 2"); // the dying one's, then the other's tick
    CHECK_FALSE(w.unit(dying).has_script_task());
    w.clear_log();
    w.sim.entity_registry().unregister_entity(gone);
    CHECK(w.log() == "destroy 2");
}

TEST_CASE("A Script task: the fallback class, errors, endless repeats, no status",
          "[script_orders]") {
    TaskSim w;
    CountingLuaFailures counting;
    const osc::u32 id = w.spawn();
    Unit& u = w.unit(id);

    // No such task: retail's ScriptTask, which fails its order
    u.push_command(w.script("{ TaskName = 'Nope' }"), false);
    w.sim.tick();
    CHECK(w.log() == "base,base tick");
    CHECK(u.command_queue().empty());
    CHECK(osc::test_status::failure_count() == 0);

    // An error ends the order, reported
    w.clear_log();
    u.push_command(w.script("{ TaskName = 'Broken' }"), false);
    w.sim.tick();
    CHECK(w.log() == "destroy");
    CHECK(u.command_queue().empty());
    CHECK(osc::test_status::failure_count() == 1);

    // Repeating for ever: 64 a tick, reported, then again the next
    w.clear_log();
    u.push_command(w.script("{ TaskName = 'Forever' }"), false);
    w.sim.tick();
    const std::string repeats = w.log();
    CHECK(std::count(repeats.begin(), repeats.end(), 'x') == 64);
    CHECK(osc::test_status::failure_count() == 2);
    CHECK(u.has_script_task());
    u.clear_commands();
    w.sim.tick();

    // No status: it ends, reported
    w.clear_log();
    u.push_command(w.script("{ TaskName = 'Mute' }"), false);
    w.sim.tick();
    CHECK(w.log() == "mute");
    CHECK(u.command_queue().empty());
    CHECK(osc::test_status::failure_count() == 3);
}

TEST_CASE("A Script task reaches its unit, and gives the order's AI result", "[script_orders]") {
    TaskSim w;
    const osc::u32 id = w.spawn();
    w.unit(id).push_command(w.script("{ TaskName = 'Unit' }"), false);
    w.sim.tick();
    CHECK(w.log() == "true");
    CHECK(w.unit(id).script_task_result() == 2);
}

TEST_CASE("A Script order's table crosses the codec; older replays have none", "[script_orders]") {
    TaskSim w;
    osc::sim::ScheduledCommand c;
    c.command = w.script("{ TaskName = 'EnhanceTask', Enhancement = 'Shield' }");
    std::vector<osc::u8> bytes;
    osc::sim::ByteWriter wr(bytes);
    osc::sim::write_command(wr, c);
    osc::sim::ByteReader r(bytes);
    osc::sim::ScheduledCommand back;
    REQUIRE(osc::sim::read_command(r, back));
    CHECK(back.command.type == CommandType::Script);
    CHECK(back.command.script_args == c.command.script_args);
    CHECK(r.position() == bytes.size());

    // Before version 11 a command has no table
    c.command.script_args.clear();
    std::vector<osc::u8> v10;
    osc::sim::ByteWriter w10(v10);
    osc::sim::write_command(w10, c);
    // (the empty table's length, after the factory byte, v10 never wrote)
    std::vector<osc::u8> marked;
    osc::sim::ByteWriter wm(marked);
    c.command.script_args = "x";
    osc::sim::write_command(wm, c);
    const auto differ = std::mismatch(v10.begin(), v10.end(), marked.begin());
    v10.erase(differ.first, differ.first + 4);
    osc::sim::ByteReader r10(v10);
    REQUIRE(osc::sim::read_command(r10, back, true, true, true, true, true, /*with_script=*/false));
    CHECK(back.command.script_args.empty());
    CHECK(r10.position() == v10.size());
}


TEST_CASE("The construction panel's IssueCommand gives a Script order", "[script_orders]") {
    // Retail's construction panel: IssueCommand("UNITCOMMAND_Script",
    // {TaskName = 'EnhanceTask', Enhancement = ...}, true) for the selection.
    TaskSim w;
    const osc::u32 id = w.spawn();
    lua_pushstring(w.L, "osc_sim_state");
    lua_pushlightuserdata(w.L, &w.sim);
    lua_rawset(w.L, LUA_REGISTRYINDEX);
    const std::string table = "return { TaskName = 'Seq', Payload = 'panel', Statuses = { -1 } }";
    REQUIRE(luaL_loadbuffer(w.L, table.c_str(), table.size(), "t") == 0);
    REQUIRE(lua_pcall(w.L, 0, 1, 0) == 0);
    osc::lua::issue_targetless_order(w.L, {id}, "Script", lua_gettop(w.L), true);
    lua_pop(w.L, 1);
    w.sim.tick();
    CHECK(w.log() == "create panel,tick 1,destroy 1");
}

TEST_CASE("A version 10 replay's commands load without a Script table", "[script_orders][replay]") {
    TaskSim w;
    osc::sim::Replay r;
    osc::sim::ScheduledCommand c;
    c.exec_tick = 3;
    c.command.type = CommandType::Move;
    c.unit_ids = {7};
    r.commands.push_back(c);
    std::vector<osc::u8> bytes = r.serialize();
    // The command again, and with a table: where they differ is the table's
    // (version 11), which version 10 never wrote
    std::vector<osc::u8> plain, marked;
    osc::sim::ByteWriter wp(plain), wm(marked);
    osc::sim::write_command(wp, c);
    c.command.script_args = "x";
    osc::sim::write_command(wm, c);
    const auto differ = std::mismatch(plain.begin(), plain.end(), marked.begin());
    const size_t at =
        bytes.size() - plain.size() + static_cast<size_t>(differ.first - plain.begin());
    c.command.form_move = false;
    std::vector<osc::u8> paced;
    osc::sim::ByteWriter wf(paced);
    osc::sim::write_command(wf, c);
    const auto form = std::mismatch(marked.begin(), marked.end(), paced.begin());
    const size_t form_move_at =
        bytes.size() - plain.size() + static_cast<size_t>(form.first - marked.begin());
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                bytes.begin() + static_cast<std::ptrdiff_t>(at) + 4);
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(form_move_at));
    const osc::u32 v10 = 10;
    std::memcpy(bytes.data() + 4, &v10, 4); // after "OSCR"
    osc::sim::Replay back;
    REQUIRE(osc::sim::Replay::deserialize(bytes, back));
    REQUIRE(back.commands.size() == 1);
    CHECK(back.commands[0].command.type == CommandType::Move);
    CHECK(back.commands[0].unit_ids == std::vector<osc::u32>{7});
    CHECK(back.commands[0].command.script_args.empty());
}

TEST_CASE("IsUnitState reads the states a script sets", "[script_orders][lua]") {
    // Retail's EnhanceTask marks its unit Enhancing and Upgrading, and its AI
    // (platoon.lua's EnhanceAI) waits while the unit is Upgrading.
    osc::lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    osc::lua::register_moho_bindings(lua, sim);
    Unit unit;
    lua_State* L = lua.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &unit);
    lua_rawset(L, -3);
    lua_setglobal(L, "unit");
    const auto r = lua.do_string(R"(
        local m = moho.unit_methods
        for _, s in ipairs({ 'Enhancing', 'Upgrading' }) do
            if m.IsUnitState(unit, s) then error(s .. ' before') end
            m.SetUnitState(unit, s, true)
            if not m.IsUnitState(unit, s) then error(s .. ' not set') end
            m.SetUnitState(unit, s, false)
            if m.IsUnitState(unit, s) then error(s .. ' still set') end
        end
    )");
    if (!r) FAIL(r.error().message);
}

TEST_CASE("An engineer whose build waits at its site is Building", "[script_orders][lua][pause]") {
    osc::lua::LuaState lua;
    SimState sim(lua.raw(), nullptr);
    osc::lua::register_moho_bindings(lua, sim);
    Unit unit;
    osc::sim::UnitCommand build;
    build.type = CommandType::BuildMobile;
    build.task_wait = 10;
    unit.push_command(build, true);
    lua_State* L = lua.raw();
    lua_newtable(L);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, &unit);
    lua_rawset(L, -3);
    lua_setglobal(L, "unit");
    const auto r = lua.do_string(R"(
        if not moho.unit_methods.IsUnitState(unit, 'Building') then error('not building') end
    )");
    if (!r) {
        FAIL(r.error().message);
    }
}
