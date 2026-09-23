// SimCallbacks in the command stream (M198): a UI script's request runs
// inside a tick, on every lockstep peer at the same tick, and is recorded in
// replays -- never applied locally between ticks.

#include <catch2/catch_test_macros.hpp>

#include "sim/command_codec.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/manipulator.hpp"
#include "sim/net_transport.hpp"
#include "sim/replay.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <memory>
#include <optional>
#include <string>
#include <vector>

using osc::sim::LockstepSession;
using osc::sim::LoopbackHub;
using osc::sim::LoopbackTransport;
using osc::sim::Replay;
using osc::sim::ScheduledCommand;
using osc::sim::SimCallbackEntry;
using osc::sim::SimState;
using osc::sim::Unit;

namespace {

/// A sim whose Lua state has a stand-in /lua/SimCallbacks.lua: DoCallback
/// records each call (tick, name, args, units, whether human input was
/// active) in the global `calls`.
struct CallbackSim {
    lua_State* L = lua_open();
    SimState sim{L, nullptr};

    static int l_tick(lua_State* L) {
        auto* sim = static_cast<SimState*>(lua_touserdata(L, lua_upvalueindex(1)));
        lua_pushnumber(L, sim->tick_count());
        return 1;
    }
    static int l_human(lua_State* L) {
        auto* sim = static_cast<SimState*>(lua_touserdata(L, lua_upvalueindex(1)));
        lua_pushboolean(L, sim->human_input_active() ? 1 : 0);
        return 1;
    }

    CallbackSim() {
        luaopen_base(L);  // tostring
        luaopen_table(L); // table.insert, table.getn
        lua_settop(L, 0);
        lua_pushstring(L, "sim_tick");
        lua_pushlightuserdata(L, &sim);
        lua_pushcclosure(L, l_tick, 1);
        lua_rawset(L, LUA_GLOBALSINDEX);
        lua_pushstring(L, "sim_human_input");
        lua_pushlightuserdata(L, &sim);
        lua_pushcclosure(L, l_human, 1);
        lua_rawset(L, LUA_GLOBALSINDEX);
        const char* code = R"(
            calls = {}
            local module = {
                DoCallback = function(name, args, units)
                    local n = 0
                    if units then n = table.getn(units) end
                    table.insert(calls, {tick = sim_tick(), name = name, args = args,
                                         units = n, human = sim_human_input()})
                end,
            }
            import = function(path) return module end
            -- A unit's script object, recording the hooks the engine calls.
            hooks = {}
            function make_unit()
                local u = {}
                for _, name in ipairs({'OnPaused', 'OnUnpaused', 'OnAutoModeOn', 'OnAutoModeOff',
                                       'OnScriptBitSet', 'OnScriptBitClear'}) do
                    local hook = name
                    u[hook] = function(self, bit)
                        table.insert(hooks, bit and (hook .. bit) or hook)
                    end
                end
                return u
            end
        )";
        REQUIRE(luaL_loadbuffer(L, code, std::string(code).size(), "stub") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
    }
    ~CallbackSim() { lua_close(L); }

    osc::u32 spawn() {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        // its script object, as the sim gives every unit
        lua_pushstring(L, "make_unit");
        lua_rawget(L, LUA_GLOBALSINDEX);
        REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
        u->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
        return sim.entity_registry().register_entity(std::move(u));
    }

    Unit& unit(osc::u32 id) { return *static_cast<Unit*>(sim.entity_registry().find(id)); }

    /// The hooks called so far, in order, comma-separated.
    std::string hooks() {
        const std::string code = "return table.concat(hooks, ',')";
        REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "h") == 0);
        REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
        std::string v = lua_tostring(L, -1);
        lua_pop(L, 1);
        return v;
    }

    int call_count() {
        auto r = luaL_loadbuffer(L, "return table.getn(calls)", 24, "n");
        REQUIRE(r == 0);
        REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
        const int n = static_cast<int>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return n;
    }
    /// A field of call `i` (1-based), as text.
    std::string field(int i, const std::string& expr) {
        const std::string code =
            "local c = calls[" + std::to_string(i) + "] return tostring(" + expr + ")";
        REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "f") == 0);
        REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
        std::string v = lua_tostring(L, -1);
        lua_pop(L, 1);
        return v;
    }
};

SimCallbackEntry toggle(osc::u32 unit) {
    SimCallbackEntry cb;
    cb.func_name = "ToggleThing";
    cb.args["Mode"] = std::string("fast");
    cb.args["Level"] = 3.0;
    cb.args["On"] = true;
    cb.unit_ids = {unit};
    return cb;
}

/// The request the orders panel's SetPaused, SetFireState, ... make.
SimCallbackEntry setting(const char* name, osc::sim::SimCallbackArg value, osc::u32 unit,
                         std::optional<double> bit = std::nullopt) {
    SimCallbackEntry cb;
    cb.func_name = osc::sim::kUnitSettingCallback;
    cb.args["Setting"] = std::string(name);
    cb.args["Value"] = std::move(value);
    if (bit) cb.args["Bit"] = *bit;
    cb.unit_ids = {unit};
    return cb;
}

} // namespace

TEST_CASE("A SimCallback runs inside the next tick, not when submitted", "[simcallback]") {
    CallbackSim w;
    const osc::u32 unit = w.spawn();
    w.sim.tick();                       // tick 1
    w.sim.set_human_input_active(true); // as while the UI is handled

    w.sim.submit_callback(toggle(unit));
    CHECK(w.call_count() == 0); // not between ticks

    w.sim.tick(); // tick 2
    REQUIRE(w.call_count() == 1);
    CHECK(w.field(1, "c.tick") == "2");
    CHECK(w.field(1, "c.name") == "ToggleThing");
    CHECK(w.field(1, "c.args.Mode") == "fast");
    CHECK(w.field(1, "c.args.Level") == "3");
    CHECK(w.field(1, "c.args.On") == "true");
    CHECK(w.field(1, "c.units") == "1");
    // It runs on every peer, so the orders it issues must not be broadcast
    // again as a local player's.
    CHECK(w.field(1, "c.human") == "false");
    CHECK(w.sim.human_input_active()); // restored afterwards
}

TEST_CASE("A recorded SimCallback replays on the same tick", "[simcallback][replay]") {
    Replay replay;
    {
        CallbackSim rec;
        const osc::u32 unit = rec.spawn();
        rec.sim.set_recording(true);
        for (int i = 0; i < 4; ++i) rec.sim.tick();
        rec.sim.submit_callback(toggle(unit)); // runs at tick 5
        for (int i = 0; i < 3; ++i) rec.sim.tick();
        REQUIRE(rec.field(1, "c.tick") == "5");
        REQUIRE(Replay::deserialize(rec.sim.recorded_replay().serialize(), replay));
    }
    REQUIRE(replay.commands.size() == 1);
    REQUIRE(replay.commands[0].callback);

    CallbackSim play;
    play.spawn();
    play.sim.queue_replay(replay);
    for (int i = 0; i < 8; ++i) play.sim.tick();
    REQUIRE(play.call_count() == 1);
    CHECK(play.field(1, "c.tick") == "5");
    CHECK(play.field(1, "c.args.Mode") == "fast");
}

TEST_CASE("Commands with callbacks survive the codec, and bad ones are refused", "[simcallback]") {
    ScheduledCommand c;
    c.exec_tick = 9;
    c.source = 2;
    c.callback = toggle(41);
    std::vector<osc::u8> bytes;
    osc::sim::ByteWriter w(bytes);
    osc::sim::write_command(w, c);

    osc::sim::ByteReader r(bytes);
    ScheduledCommand back;
    REQUIRE(osc::sim::read_command(r, back));
    REQUIRE(back.callback);
    CHECK(back.exec_tick == 9);
    CHECK(back.callback->func_name == "ToggleThing");
    CHECK(std::get<std::string>(back.callback->args.at("Mode")) == "fast");
    CHECK(std::get<double>(back.callback->args.at("Level")) == 3.0);
    CHECK(std::get<bool>(back.callback->args.at("On")));
    CHECK(back.callback->unit_ids == std::vector<osc::u32>{41});

    // An unknown value tag, or bytes that run out, is refused whole.
    std::vector<osc::u8> truncated(bytes.begin(), bytes.end() - 3);
    osc::sim::ByteReader rt(truncated);
    CHECK_FALSE(osc::sim::read_command(rt, back));
}

TEST_CASE("Lockstep peers run a SimCallback on the same tick", "[simcallback][lockstep]") {
    CallbackSim a, b;
    const osc::u32 ua = a.spawn();
    REQUIRE(b.spawn() == ua);

    LoopbackHub hub;
    LoopbackTransport ta(hub, hub.add_endpoint());
    LoopbackTransport tb(hub, hub.add_endpoint());
    LockstepSession sa(a.sim, ta, 0, {0, 1});
    LockstepSession sb(b.sim, tb, 1, {0, 1});
    a.sim.set_local_callback_sink(
        [&sa](SimCallbackEntry cb) { sa.submit_local_callback(std::move(cb)); });

    for (int round = 0; round < 10; ++round) {
        if (round == 3) a.sim.submit_callback(toggle(ua)); // the UI on peer A
        sa.send_frame();
        sb.send_frame();
        sa.receive_and_advance();
        sb.receive_and_advance();
    }
    REQUIRE(a.call_count() == 1);
    REQUIRE(b.call_count() == 1);                        // peer B ran A's callback too...
    CHECK(a.field(1, "c.tick") == b.field(1, "c.tick")); // ...on the same tick
    CHECK(b.field(1, "c.args.Mode") == "fast");
    CHECK(b.field(1, "c.units") == "1");
    CHECK(a.sim.compute_sync_checksum() == b.sim.compute_sync_checksum());
}

TEST_CASE("The UI's unit settings apply inside a tick, with their script hooks", "[simcallback]") {
    CallbackSim w;
    const osc::u32 id = w.spawn();
    w.unit(id).economy().consumption_active = true;

    w.sim.submit_callback(setting("Paused", true, id));
    CHECK_FALSE(w.unit(id).is_paused()); // not between ticks
    w.sim.tick();
    CHECK(w.unit(id).is_paused());
    CHECK_FALSE(w.unit(id).economy().consumption_active); // as unit:SetPaused does
    CHECK(w.hooks() == "OnPaused");

    // A setting that doesn't change calls no hook.
    w.sim.submit_callback(setting("Paused", true, id));
    w.sim.tick();
    CHECK(w.hooks() == "OnPaused");

    w.sim.submit_callback(setting("Paused", false, id));
    w.sim.submit_callback(setting("AutoMode", true, id));
    w.sim.submit_callback(setting("ScriptBit", true, id, 3.0));
    w.sim.submit_callback(setting("FireState", 2.0, id));
    w.sim.submit_callback(setting("AutoSurfaceMode", true, id));
    w.sim.tick();
    CHECK_FALSE(w.unit(id).is_paused());
    CHECK(w.unit(id).auto_mode());
    CHECK(w.unit(id).get_script_bit(3));
    CHECK(w.unit(id).fire_state() == 2);
    CHECK(w.unit(id).auto_surface_mode());
    CHECK(w.hooks() == "OnPaused,OnUnpaused,OnAutoModeOn,OnScriptBitSet3");

    w.sim.submit_callback(setting("ScriptBit", false, id, 3.0));
    w.sim.tick();
    CHECK_FALSE(w.unit(id).get_script_bit(3));
    CHECK(w.hooks() == "OnPaused,OnUnpaused,OnAutoModeOn,OnScriptBitSet3,OnScriptBitClear3");
}

TEST_CASE("Unit settings out of range are ignored", "[simcallback]") {
    CallbackSim w;
    const osc::u32 id = w.spawn();
    w.sim.submit_callback(setting("FireState", 7.0, id));
    w.sim.submit_callback(setting("FireState", 1.5, id));
    w.sim.submit_callback(setting("ScriptBit", true, id, 9.0));
    w.sim.submit_callback(setting("ScriptBit", true, id, -1.0));
    w.sim.submit_callback(setting("Paused", std::string("yes"), id)); // not a bool
    w.sim.submit_callback(setting("Unheard", true, id));
    w.sim.tick();
    CHECK(w.unit(id).fire_state() == 0);
    CHECK(w.unit(id).script_bits() == 0);
    CHECK_FALSE(w.unit(id).is_paused());
    CHECK(w.hooks().empty());
}

TEST_CASE("ProcessInfo's pause and auto mode call the same hooks", "[simcallback]") {
    CallbackSim w;
    const osc::u32 id = w.spawn();
    SimCallbackEntry cb;
    cb.func_name = osc::sim::kProcessInfoCallback;
    cb.args["Action"] = std::string("SetPaused");
    cb.args["Value"] = std::string("true");
    cb.unit_ids = {id};
    w.sim.submit_callback(cb);
    cb.args["Action"] = std::string("SetAutoMode");
    w.sim.submit_callback(cb);
    cb.args["Action"] = std::string("SetRepeatQueue");
    w.sim.submit_callback(cb);
    cb.args["Action"] = std::string("Destroy"); // only settings, never any method
    w.sim.submit_callback(cb);
    w.sim.tick();
    CHECK(w.unit(id).is_paused());
    CHECK(w.unit(id).auto_mode());
    CHECK(w.unit(id).repeat_queue());
    CHECK(w.hooks() == "OnPaused,OnAutoModeOn");
}

TEST_CASE("The sync checksum sees a unit setting", "[simcallback][sync]") {
    CallbackSim a, b;
    const osc::u32 ua = a.spawn();
    b.spawn();
    CHECK(a.sim.compute_sync_checksum() == b.sim.compute_sync_checksum());
    a.unit(ua).set_fire_state(1);
    CHECK(a.sim.compute_sync_checksum() != b.sim.compute_sync_checksum());
    a.unit(ua).set_fire_state(0);
    a.unit(ua).set_paused(true);
    CHECK(a.sim.compute_sync_checksum() != b.sim.compute_sync_checksum());
    a.unit(ua).set_paused(false);
    a.unit(ua).set_script_bit(5, true);
    CHECK(a.sim.compute_sync_checksum() != b.sim.compute_sync_checksum());
}

TEST_CASE("A factory's queue is its build orders; decreasing takes the newest", "[simcallback]") {
    CallbackSim w;
    const osc::u32 id = w.spawn();
    Unit& f = w.unit(id);
    osc::sim::UnitCommand build;
    build.type = osc::sim::CommandType::BuildFactory;
    auto queue = [&](const char* bp, int n) {
        build.blueprint_id = bp;
        for (int i = 0; i < n; ++i) f.push_command(build, false);
    };
    queue("a", 3);
    queue("b", 2);
    queue("a", 1);
    auto q = f.factory_queue();
    REQUIRE(q.size() == 3);
    CHECK((q[0].blueprint_id == "a" && q[0].count == 3));
    CHECK((q[1].blueprint_id == "b" && q[1].count == 2));
    CHECK((q[2].blueprint_id == "a" && q[2].count == 1));

    // DecreaseBuildCountInQueue(2, 1), as the sim runs it (straight away
    // here: a tick would also start the factory's first build).
    SimCallbackEntry cb;
    cb.func_name = osc::sim::kDecreaseBuildCountCallback;
    cb.args["Index"] = 2.0;
    cb.args["Count"] = 1.0;
    cb.unit_ids = {id};
    w.sim.run_sim_callback(cb);
    q = f.factory_queue();
    REQUIRE(q.size() == 3);
    CHECK(q[1].count == 1);

    // Out of range, or nonsense: nothing changes.
    cb.args["Index"] = 7.0;
    w.sim.run_sim_callback(cb);
    cb.args["Index"] = 0.0;
    w.sim.run_sim_callback(cb);
    cb.args["Index"] = std::string("2");
    w.sim.run_sim_callback(cb);
    CHECK(f.factory_queue().size() == 3);

    // More than the group holds: it goes, and the runs either side join.
    cb.args["Index"] = 2.0;
    cb.args["Count"] = 5.0;
    w.sim.run_sim_callback(cb);
    q = f.factory_queue();
    REQUIRE(q.size() == 1);
    CHECK((q[0].blueprint_id == "a" && q[0].count == 4));
}
