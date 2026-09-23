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

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <memory>
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
        )";
        REQUIRE(luaL_loadbuffer(L, code, std::string(code).size(), "stub") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
    }
    ~CallbackSim() { lua_close(L); }

    osc::u32 spawn() {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        lua_newtable(L); // its script object, as the sim gives every unit
        u->set_lua_table_ref(luaL_ref(L, LUA_REGISTRYINDEX));
        return sim.entity_registry().register_entity(std::move(u));
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
