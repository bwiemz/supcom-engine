// SimCallbacks in the command stream (M198): a UI script's request runs
// in a beat, on every lockstep peer in the same beat, and is recorded in
// replays -- never applied locally between beats.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "core/game_state.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/user_bindings.hpp"
#include "renderer/input_handler.hpp"
#include "sim/army_brain.hpp"
#include "sim/command_codec.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/manipulator.hpp"
#include "sim/net_transport.hpp"
#include "sim/replay.hpp"
#include "sim/sim_callback_queue.hpp"
#include "sim/sim_state.hpp"
#include "sim/state_io.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
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
                                       'OnScriptBitSet', 'OnScriptBitClear', 'OnFailedToBuild',
                                       'Destroy'}) do
                    local hook = name
                    u[hook] = function(self, bit)
                        table.insert(hooks, bit and (hook .. bit) or hook)
                    end
                end
                u.OnFailedToBeBuilt = function(self)
                    table.insert(hooks, 'OnFailedToBeBuilt')
                    self:Destroy()
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
        u->add_command_cap("RULEUCC_Pause");
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

TEST_CASE("A SimCallback runs in the next beat, before its tick, not when submitted",
          "[simcallback]") {
    CallbackSim w;
    const osc::u32 unit = w.spawn();
    w.sim.tick();                       // tick 1
    w.sim.set_human_input_active(true); // as while the UI is handled

    w.sim.submit_callback(toggle(unit));
    CHECK(w.call_count() == 0); // not between ticks

    w.sim.tick(); // tick 2
    REQUIRE(w.call_count() == 1);
    CHECK(w.field(1, "c.tick") == "1");
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
        rec.sim.submit_callback(toggle(unit)); // runs before tick 5
        for (int i = 0; i < 3; ++i) rec.sim.tick();
        REQUIRE(rec.field(1, "c.tick") == "4");
        REQUIRE(Replay::deserialize(rec.sim.recorded_replay().serialize(), replay));
    }
    REQUIRE(replay.commands.size() == 1);
    REQUIRE(replay.commands[0].callback);

    CallbackSim play;
    play.spawn();
    play.sim.queue_replay(replay);
    for (int i = 0; i < 8; ++i) play.sim.tick();
    REQUIRE(play.call_count() == 1);
    CHECK(play.field(1, "c.tick") == "4");
    CHECK(play.field(1, "c.args.Mode") == "fast");
}

TEST_CASE("Commands with callbacks survive the codec, and bad ones are refused", "[simcallback]") {
    ScheduledCommand c;
    c.exec_beat = 9;
    c.source = 2;
    c.callback = toggle(41);
    std::vector<osc::u8> bytes;
    osc::sim::ByteWriter w(bytes);
    osc::sim::write_command(w, c);

    osc::sim::ByteReader r(bytes);
    ScheduledCommand back;
    REQUIRE(osc::sim::read_command(r, back));
    REQUIRE(back.callback);
    CHECK(back.exec_beat == 9);
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

TEST_CASE("A SimCallback's Args may be one value, recorded and replayed", "[simcallback]") {
    // SimCamera's move callback: {Func = "OnCameraFinish", Args = name}
    SimCallbackEntry cb;
    cb.func_name = "OnCameraFinish";
    cb.value = std::string("WorldCamera");
    Replay replay;
    {
        CallbackSim rec;
        rec.sim.set_recording(true);
        rec.sim.tick();
        rec.sim.submit_callback(cb);
        rec.sim.submit_callback(SimCallbackEntry{"Count", {}, 7.0, {}, {}});
        rec.sim.tick();
        REQUIRE(rec.call_count() == 2);
        CHECK(rec.field(1, "c.args") == "WorldCamera");
        CHECK(rec.field(2, "type(c.args)") == "number");
        CHECK(rec.field(2, "c.args") == "7");
        REQUIRE(Replay::deserialize(rec.sim.recorded_replay().serialize(), replay));
    }
    CallbackSim play;
    play.sim.queue_replay(replay);
    for (int i = 0; i < 3; ++i) play.sim.tick();
    REQUIRE(play.call_count() == 2);
    CHECK(play.field(1, "c.args") == "WorldCamera");
    CHECK(play.field(2, "c.args") == "7");
}

TEST_CASE("A callback's one value survives the codec; older replays have none",
          "[simcallback][replay]") {
    ScheduledCommand c;
    c.callback = SimCallbackEntry{"OnCameraFinish", {}, true, {}, {}};
    std::vector<osc::u8> bytes;
    osc::sim::ByteWriter w(bytes);
    osc::sim::write_command(w, c);
    osc::sim::ByteReader r(bytes);
    ScheduledCommand back;
    REQUIRE(osc::sim::read_command(r, back));
    REQUIRE(back.callback);
    REQUIRE(back.callback->value);
    CHECK(std::get<bool>(*back.callback->value));
    CHECK(r.position() == bytes.size());

    // A v7 command ends at its callback's units: read without the value
    c.callback->value.reset();
    std::vector<osc::u8> v7;
    osc::sim::ByteWriter w7(v7);
    osc::sim::write_command(w7, c);
    v7.pop_back(); // v13's "no Lua args" byte
    v7.pop_back(); // v8's "no value" byte
    osc::sim::ByteReader r7(v7);
    REQUIRE(osc::sim::read_command(r7, back, true, true, true, true, /*with_value=*/false,
                                   /*with_script=*/true, /*with_lua_args=*/false));
    CHECK_FALSE(back.callback->value);
    CHECK(r7.position() == v7.size());

    // An unknown value tag is refused
    c.callback->value = std::string("x");
    std::vector<osc::u8> bad;
    osc::sim::ByteWriter wb(bad);
    osc::sim::write_command(wb, c);
    bad[bad.size() - 7] = 9; // the tag, before the string's length and byte
    osc::sim::ByteReader rb(bad);
    CHECK_FALSE(osc::sim::read_command(rb, back));
}

TEST_CASE("A v7 replay's callbacks load without the value byte", "[simcallback][replay]") {
    std::vector<osc::u8> bytes;
    size_t script_at = 0; // where the command's Script table sits in the file
    size_t form_move_at = 0;
    {
        CallbackSim rec;
        const osc::u32 unit = rec.spawn();
        rec.sim.set_recording(true);
        rec.sim.tick();
        rec.sim.submit_callback(toggle(unit));
        rec.sim.tick();
        const Replay& recorded = rec.sim.recorded_replay();
        bytes = recorded.serialize();
        // The command again, and with a Script table: the first byte they
        // differ at is the table's (v11), which v7 didn't write either.
        ScheduledCommand c = recorded.commands.back();
        std::vector<osc::u8> plain, marked;
        osc::sim::ByteWriter wp(plain), wm(marked);
        osc::sim::write_command(wp, c);
        c.command.script_args = "x";
        osc::sim::write_command(wm, c);
        const auto differ = std::mismatch(plain.begin(), plain.end(), marked.begin());
        script_at = bytes.size() - plain.size() + static_cast<size_t>(differ.first - plain.begin());
        c.command.form_move = !c.command.form_move;
        std::vector<osc::u8> paced;
        osc::sim::ByteWriter wf(paced);
        osc::sim::write_command(wf, c);
        const auto form = std::mismatch(marked.begin(), marked.end(), paced.begin());
        form_move_at =
            bytes.size() - plain.size() + static_cast<size_t>(form.first - marked.begin());
    }
    // The callback is the last command, its "no value" and "no Lua args"
    // bytes the file's last: v7 wrote neither, nor its (empty) Script table
    REQUIRE(bytes.back() == 0);
    bytes.pop_back();
    REQUIRE(bytes.back() == 0);
    bytes.pop_back();
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(script_at),
                bytes.begin() + static_cast<std::ptrdiff_t>(script_at) + 4);
    bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(form_move_at));
    const osc::u32 v7 = 7;
    std::memcpy(bytes.data() + 4, &v7, 4); // after "OSCR"
    Replay back;
    REQUIRE(Replay::deserialize(bytes, back));
    CHECK(back.version == 7);
    REQUIRE(back.commands.size() == 1);
    REQUIRE(back.commands[0].callback);
    CHECK(back.commands[0].callback->func_name == "ToggleThing");
    CHECK_FALSE(back.commands[0].callback->value);
}

TEST_CASE("A UI SimCallback's nested Args reach the sim, recorded, replayed and saved",
          "[simcallback][replay]") {
    osc::lua::LuaState ui;
    osc::lua::register_user_bindings(ui);
    osc::sim::SimCallbackQueue queue;
    lua_pushstring(ui.raw(), "__osc_sim_callback_queue");
    lua_pushlightuserdata(ui.raw(), &queue);
    lua_rawset(ui.raw(), LUA_REGISTRYINDEX);
    REQUIRE(ui.do_string("SimCallback({Func = 'SpawnPing', "
                         "Args = {Owner = 1, Location = {10, 20, 30}}})")
                .ok());
    REQUIRE(ui.do_string("SimCallback({Func = 'ToggleSelfDestruct', "
                         "Args = {units = {7, 9}, owner = 0}})")
                .ok());
    REQUIRE(ui.do_string("SimCallback({Func = 'OnControlGroupAssign', Args = {7, 9}})").ok());
    REQUIRE(ui.do_string("SimCallback({Func = 'OnEndGameFMVFinished'})").ok());
    const auto sent = queue.drain();
    REQUIRE(sent.size() == 4);

    const auto check = [](CallbackSim& s) {
        REQUIRE(s.call_count() == 4);
        CHECK(s.field(1, "c.args.Owner") == "1");
        CHECK(s.field(1, "c.args.Location[2]") == "20");
        CHECK(s.field(2, "c.args.units[2]") == "9");
        CHECK(s.field(2, "c.args.owner") == "0");
        CHECK(s.field(3, "table.getn(c.args)") == "2");
        CHECK(s.field(3, "c.args[1]") == "7");
        CHECK(s.field(4, "c.args") == "nil");
    };
    Replay replay;
    std::vector<osc::u8> saved;
    {
        CallbackSim rec;
        rec.sim.set_recording(true);
        rec.sim.tick();
        for (const auto& cb : sent) {
            rec.sim.submit_callback(cb);
        }
        saved = osc::sim::save_sim_state(rec.sim);
        rec.sim.tick();
        check(rec);
        REQUIRE(Replay::deserialize(rec.sim.recorded_replay().serialize(), replay));
    }
    {
        CallbackSim play;
        play.sim.queue_replay(replay);
        for (int i = 0; i < 3; ++i) {
            play.sim.tick();
        }
        check(play);
    }
    CallbackSim loaded;
    REQUIRE(osc::sim::load_sim_state(loaded.sim, saved).empty());
    loaded.sim.tick();
    check(loaded);
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

TEST_CASE("The UI's unit settings apply in a beat, with their script hooks", "[simcallback]") {
    CallbackSim w;
    const osc::u32 id = w.spawn();
    w.unit(id).economy().consumption_active = true;

    w.sim.submit_callback(setting("Paused", true, id));
    CHECK_FALSE(w.unit(id).is_paused()); // not between ticks
    w.sim.tick();
    CHECK(w.unit(id).is_paused());
    CHECK(w.unit(id).economy().consumption_active);
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
    w.sim.submit_callback(setting("FireState", 2.0, id));
    w.sim.tick();
    CHECK(w.unit(id).fire_state() == 2);
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

TEST_CASE("The UI pauses only a unit with RULEUCC_Pause or RULEUTC_GenericToggle",
          "[simcallback]") {
    CallbackSim w;
    struct Case {
        const char* command_cap;
        const char* toggle_cap;
        bool paused;
    };
    for (const Case c : {Case{nullptr, nullptr, false}, Case{"RULEUCC_Pause", nullptr, true},
                         Case{nullptr, "RULEUTC_GenericToggle", true},
                         Case{"RULEUCC_Stop", "RULEUTC_ProductionToggle", false}}) {
        const osc::u32 by_setting = w.spawn();
        const osc::u32 by_info = w.spawn();
        for (const osc::u32 id : {by_setting, by_info}) {
            w.unit(id).remove_command_cap("RULEUCC_Pause");
            if (c.command_cap) {
                w.unit(id).add_command_cap(c.command_cap);
            }
            if (c.toggle_cap) {
                w.unit(id).add_toggle_cap(c.toggle_cap);
            }
        }
        w.sim.submit_callback(setting("Paused", true, by_setting));
        SimCallbackEntry info;
        info.func_name = osc::sim::kProcessInfoCallback;
        info.args["Action"] = std::string("SetPaused");
        info.args["Value"] = std::string("true");
        info.unit_ids = {by_info};
        w.sim.submit_callback(info);
        w.sim.tick();
        CHECK(w.unit(by_setting).is_paused() == c.paused);
        CHECK(w.unit(by_info).is_paused() == c.paused);
    }
    CHECK(w.hooks() == "OnPaused,OnPaused,OnPaused,OnPaused");
}

TEST_CASE("ProcessInfo's CustomName names the unit at the tick", "[simcallback]") {
    // UserUnit:SetCustomName reaches the sim as this pair, as Moho's does
    // (ProcessInfoPair(id, "CustomName", name)): the rename dialog, and the
    // commander named for its player as the game starts.
    CallbackSim w;
    const osc::u32 id = w.spawn();
    SimCallbackEntry cb;
    cb.func_name = osc::sim::kProcessInfoCallback;
    cb.args["Action"] = std::string("CustomName");
    cb.args["Value"] = std::string("Fred");
    cb.unit_ids = {id};
    w.sim.submit_callback(cb);
    CHECK(w.unit(id).custom_name().empty()); // not between ticks
    w.sim.tick();
    CHECK(w.unit(id).custom_name() == "Fred");
    CHECK(w.hooks().empty()); // a name, no script hook
}

TEST_CASE("ProcessInfo's staging platform voices reach the unit's brain", "[simcallback]") {
    CallbackSim w;
    w.sim.add_army("ARMY_1", "ARMY_1");
    const char* code = "return {OnPlayNoStagingPlatformsVO = function(self) "
                       "table.insert(hooks, 'NoStaging') end, "
                       "OnPlayBusyStagingPlatformsVO = function(self) "
                       "table.insert(hooks, 'BusyStaging') end}";
    REQUIRE(luaL_loadbuffer(w.L, code, std::strlen(code), "brain") == 0);
    REQUIRE(lua_pcall(w.L, 0, 1, 0) == 0);
    w.sim.army_at(0)->set_lua_table_ref(luaL_ref(w.L, LUA_REGISTRYINDEX));
    const osc::u32 id = w.spawn();
    for (const char* action : {"PlayNoStagingPlatformsVO", "PlayBusyStagingPlatformsVO"}) {
        SimCallbackEntry cb;
        cb.func_name = osc::sim::kProcessInfoCallback;
        cb.args["Action"] = std::string(action);
        cb.args["Value"] = std::string("play");
        cb.unit_ids = {id};
        w.sim.submit_callback(cb);
    }
    w.sim.tick();
    CHECK(w.hooks() == "NoStaging,BusyStaging");
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

TEST_CASE("Increasing a factory's queue counts up one group's newest order", "[simcallback]") {
    CallbackSim w;
    const osc::u32 id = w.spawn();
    Unit& f = w.unit(id);
    osc::sim::UnitCommand build;
    build.type = osc::sim::CommandType::BuildFactory;
    auto queue = [&](const char* bp, int n, osc::u32 command_id) {
        build.blueprint_id = bp;
        build.command_id = command_id;
        for (int i = 0; i < n; ++i) f.push_command(build, false);
    };
    queue("a", 3, 11);
    queue("b", 1, 12);
    // The group's last order has run state of its own (say it is under way).
    build.approached = true;
    queue("b", 1, 12);
    build.approached = false;
    queue("a", 1, 13);

    // IncreaseBuildCountInQueue(2, 3), as the sim runs it.
    SimCallbackEntry cb;
    cb.func_name = osc::sim::kIncreaseBuildCountCallback;
    cb.args["Index"] = 2.0;
    cb.args["Count"] = 3.0;
    cb.unit_ids = {id};
    w.sim.run_sim_callback(cb);
    auto q = f.factory_queue();
    REQUIRE(q.size() == 3);
    CHECK((q[0].blueprint_id == "a" && q[0].count == 3));
    CHECK((q[1].blueprint_id == "b" && q[1].count == 5));
    CHECK((q[2].blueprint_id == "a" && q[2].count == 1));
    // Moho's IncreaseCount: the group's newest order makes more, its most
    // with it; no order is added, and its run state stays as it was.
    const auto& orders = f.command_queue();
    REQUIRE(orders.size() == 6);
    CHECK(orders[4].blueprint_id == "b");
    CHECK(orders[4].count == 4);
    CHECK(orders[4].max_count == 4);
    CHECK(orders[4].approached);
    CHECK(orders[3].count == 1);

    // The first group too.
    cb.args["Index"] = 1.0;
    cb.args["Count"] = 1.0;
    w.sim.run_sim_callback(cb);
    CHECK(f.factory_queue()[0].count == 4);

    // Past the queue, or nonsense: nothing changes. A count past 1,000 is
    // refused: the request comes over the network, and FA asks for 1 or 5.
    const auto counts = [&] {
        std::vector<int> c;
        for (const auto& g : f.factory_queue()) c.push_back(g.count);
        return c;
    };
    const auto before = counts();
    for (const auto& [index, count] :
         {std::pair{4.0, 1.0}, std::pair{0.0, 1.0}, std::pair{1.0, 0.0}, std::pair{1.0, 1001.0}}) {
        cb.args["Index"] = index;
        cb.args["Count"] = count;
        w.sim.run_sim_callback(cb);
    }
    cb.args["Index"] = std::string("1");
    cb.args["Count"] = 1.0;
    w.sim.run_sim_callback(cb);
    CHECK(counts() == before);
    cb.args["Index"] = 1.0;
    cb.args["Count"] = 1000.0;
    w.sim.run_sim_callback(cb);
    CHECK(f.factory_queue()[0].count == 1004);
}

TEST_CASE("A callback's NaN or infinite numbers change nothing", "[simcallback]") {
    // They arrive over the network as raw doubles; NaN passes a `< lo || > hi`
    // guard and a NaN cast to an integer is undefined.
    CallbackSim w;
    w.sim.add_army("ARMY_1", "ARMY_1");
    w.sim.add_army("ARMY_2", "ARMY_2");
    const osc::u32 id = w.spawn();
    Unit& f = w.unit(id);
    osc::sim::UnitCommand build;
    build.type = osc::sim::CommandType::BuildFactory;
    build.blueprint_id = "a";
    for (int i = 0; i < 3; ++i) f.push_command(build, false);

    const double bad[] = {std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity(),
                          -std::numeric_limits<double>::infinity()};
    for (const char* func :
         {osc::sim::kDecreaseBuildCountCallback, osc::sim::kIncreaseBuildCountCallback}) {
        for (const double v : bad) {
            SimCallbackEntry cb;
            cb.func_name = func;
            cb.unit_ids = {id};
            cb.args["Index"] = v;
            cb.args["Count"] = 1.0;
            w.sim.run_sim_callback(cb);
            cb.args["Index"] = 1.0;
            cb.args["Count"] = v;
            w.sim.run_sim_callback(cb);
        }
    }
    CHECK(f.command_queue().size() == 3);
    SimCallbackEntry fewer;
    fewer.func_name = osc::sim::kDecreaseBuildCountCallback;
    fewer.unit_ids = {id};
    fewer.args["Index"] = 1.0;
    fewer.args["Count"] = 1.0;
    w.sim.run_sim_callback(fewer);
    CHECK(f.command_queue().size() == 2);

    SimCallbackEntry defeat;
    defeat.func_name = osc::sim::kDefeatArmyCallback;
    for (const double v : bad) {
        defeat.args["Army"] = v;
        w.sim.run_sim_callback(defeat);
    }
    CHECK_FALSE(w.sim.army_at(0)->is_defeated());
    CHECK_FALSE(w.sim.army_at(1)->is_defeated());
    defeat.args["Army"] = 1.0; // and a real one still works
    w.sim.run_sim_callback(defeat);
    CHECK(w.sim.army_at(1)->is_defeated());
}

TEST_CASE("Cancelling a factory's build under way destroys the unit it was building",
          "[simcallback]") {
    CallbackSim w;
    const osc::u32 id = w.spawn();
    const osc::u32 partial = w.spawn(); // the unit on the factory's floor
    Unit& f = w.unit(id);
    w.unit(partial).set_is_being_built(true);
    osc::sim::UnitCommand build;
    build.type = osc::sim::CommandType::BuildFactory;
    build.blueprint_id = "a";
    f.push_command(build, false);
    f.push_command(build, false);
    f.set_build_target_id(partial); // the first order is under way
    REQUIRE(f.building_factory_order());

    // Taking one off takes the newest: the build under way continues.
    SimCallbackEntry cb;
    cb.func_name = osc::sim::kDecreaseBuildCountCallback;
    cb.args["Index"] = 1.0;
    cb.args["Count"] = 1.0;
    cb.unit_ids = {id};
    w.sim.run_sim_callback(cb);
    CHECK(f.factory_queue().at(0).count == 1);
    CHECK(f.build_target_id() == partial);
    CHECK(w.hooks().empty());

    // Taking the last one off cancels it: the factory fails the build and
    // the partial unit's script destroys it.
    w.sim.run_sim_callback(cb);
    CHECK(f.factory_queue().empty());
    CHECK(f.build_target_id() == 0);
    CHECK(w.hooks() == "OnFailedToBuild,OnFailedToBeBuilt,Destroy");
}

TEST_CASE("Taking a factory's build under way off its queue cancels it", "[simcallback]") {
    CallbackSim w;
    const osc::u32 id = w.spawn();
    const osc::u32 partial = w.spawn();
    Unit& f = w.unit(id);
    w.unit(partial).set_is_being_built(true);
    osc::sim::UnitCommand build;
    build.type = osc::sim::CommandType::BuildFactory;
    build.blueprint_id = "a";
    build.command_id = 4;
    f.push_command(build, false);
    build.command_id = 5;
    f.push_command(build, false);
    f.set_build_target_id(partial);

    SimCallbackEntry cb;
    cb.func_name = osc::sim::kRemoveCommandCallback;
    cb.args["Command"] = 4.0;
    cb.unit_ids = {id};
    w.sim.run_sim_callback(cb);
    REQUIRE(f.command_queue().size() == 1);
    CHECK(f.command_queue().front().command_id == 5);
    CHECK(f.build_target_id() == 0);
    CHECK(w.hooks() == "OnFailedToBuild,OnFailedToBeBuilt,Destroy");
}

TEST_CASE("DeleteCommand's DecreaseCommandCount takes one off an order", "[simcallback]") {
    // Moho's Sim::DecreaseCommandCount: a factory build of more makes one
    // fewer; any other order (one unit each) leaves every queue holding it.
    CallbackSim w;
    const osc::u32 factory = w.spawn();
    const osc::u32 a = w.spawn();
    const osc::u32 b = w.spawn();
    osc::sim::UnitCommand build;
    build.type = osc::sim::CommandType::BuildFactory;
    build.blueprint_id = "x";
    build.command_id = 4;
    build.count = 3;
    build.max_count = 3;
    w.unit(factory).push_command(build, false);
    osc::sim::UnitCommand move;
    move.type = osc::sim::CommandType::Move;
    move.command_id = 9;
    w.unit(a).push_command(move, false);
    w.unit(b).push_command(move, false);
    move.command_id = 10;
    w.unit(a).push_command(move, false);

    SimCallbackEntry cb;
    cb.func_name = osc::sim::kDecreaseCommandCountCallback;
    cb.args["Command"] = 4.0;
    cb.args["Count"] = 1.0;
    w.sim.run_sim_callback(cb);
    REQUIRE(w.unit(factory).command_queue().size() == 1);
    CHECK(w.unit(factory).command_queue().front().count == 2);

    cb.args["Command"] = 9.0;
    w.sim.run_sim_callback(cb);
    REQUIRE(w.unit(a).command_queue().size() == 1);
    CHECK(w.unit(a).command_queue().front().command_id == 10);
    CHECK(w.unit(b).command_queue().empty());

    // No such order, or nonsense: nothing changes
    cb.args["Command"] = 77.0;
    w.sim.run_sim_callback(cb);
    cb.args["Command"] = 10.0;
    cb.args["Count"] = std::string("1");
    w.sim.run_sim_callback(cb);
    CHECK(w.unit(a).command_queue().size() == 1);
    CHECK(w.unit(factory).command_queue().front().count == 2);
}

TEST_CASE("CreateUnitAtMouse's unit is a cheat the sim makes", "[simcallback][cheat]") {
    // Moho's Sim::CreateUnit for CMDST_CreateUnit: nothing without cheats, an
    // army in the game and a unit blueprint; a structure on its footprint's
    // cells (its corner's cell, truncated), a mobile unit where it was asked.
    lua_State* L = lua_open();
    luaopen_base(L);
    luaopen_table(L);
    lua_settop(L, 0);
    {
        osc::blueprints::BlueprintStore store(L);
        for (const char* bp : {"{BlueprintId = 'wall', Footprint = {SizeX = 2, SizeZ = 1},"
                               " Physics = {MotionType = 'RULEUMT_None'}}",
                               "{BlueprintId = 'tank', Footprint = {SizeX = 1, SizeZ = 1},"
                               " Physics = {MotionType = 'RULEUMT_Land'}}"}) {
            const std::string code = std::string("return ") + bp;
            REQUIRE(luaL_loadbuffer(L, code.c_str(), code.size(), "bp") == 0);
            REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
            store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
            lua_pop(L, 1);
        }
        store.expose_to_lua(L);
        const char* stub = "created = {}\n"
                           "function CreateUnitHPR(...) table.insert(created, arg) end\n";
        REQUIRE(luaL_loadbuffer(L, stub, std::strlen(stub), "stub") == 0);
        REQUIRE(lua_pcall(L, 0, 0, 0) == 0);
        {
            SimState sim{L, &store};
            sim.add_army("ARMY_1", "ARMY_1");
            const auto made = [&]() {
                lua_pushstring(L, "created");
                lua_rawget(L, LUA_GLOBALSINDEX);
                const int n = luaL_getn(L, -1);
                lua_pop(L, 1);
                return n;
            };
            const auto field = [&](int i, int arg) {
                lua_pushstring(L, "created");
                lua_rawget(L, LUA_GLOBALSINDEX);
                lua_rawgeti(L, -1, i);
                lua_rawgeti(L, -1, arg);
                std::string v = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
                const double n = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : -1.0;
                lua_pop(L, 3);
                return std::pair{v, n};
            };
            SimCallbackEntry cb;
            cb.func_name = osc::sim::kCreateUnitCallback;
            cb.args["Blueprint"] = std::string("WALL");
            cb.args["Army"] = 0.0;
            cb.args["X"] = 10.3;
            cb.args["Z"] = 20.8;
            cb.args["Heading"] = 1.5;
            sim.run_sim_callback(cb);
            CHECK(made() == 0); // cheats off

            sim.set_cheats_enabled(true);
            sim.run_sim_callback(cb);
            REQUIRE(made() == 1);
            CHECK(field(1, 1).first == "wall");
            CHECK(field(1, 2).second == 1.0);  // the 1-based army
            CHECK(field(1, 3).second == 10.0); // its corner in cell 9, 2 wide: 9 + 1
            CHECK(field(1, 5).second == 20.5); // cell 20, 1 deep
            CHECK(field(1, 7).second == 1.5);  // the heading, about Y

            cb.args["Blueprint"] = std::string("tank");
            sim.run_sim_callback(cb);
            REQUIRE(made() == 2);
            CHECK(field(2, 3).second == static_cast<double>(10.3f)); // where asked

            cb.args["Blueprint"] = std::string("nothing");
            sim.run_sim_callback(cb);
            cb.args["Blueprint"] = std::string("tank");
            cb.args["Army"] = 1.0; // no such army
            sim.run_sim_callback(cb);
            cb.args["Army"] = 0.0;
            sim.army_at(0)->set_state(osc::sim::BrainState::Defeat);
            sim.run_sim_callback(cb);
            CHECK(made() == 2);
        }
    } // the store before its state
    lua_close(L);
}

TEST_CASE("GetArmyScore, DeleteCommand and CreateUnitAtMouse check as Moho's do",
          "[simcallback][lua]") {
    // Each errors as Moho's does without a session or with bad arguments;
    // DeleteCommand and CreateUnitAtMouse reach the sim through the command
    // stream alone.
    osc::lua::LuaState sim_lua;
    osc::blueprints::BlueprintStore store(sim_lua.raw());
    SimState sim{sim_lua.raw(), &store};
    sim.add_army("ARMY_1", "ARMY_1");
    REQUIRE(sim_lua.do_string("return {BlueprintId = 'tank'}").ok());
    store.register_blueprint(sim_lua.raw(), osc::blueprints::BlueprintType::Unit,
                             lua_gettop(sim_lua.raw()));
    lua_pop(sim_lua.raw(), 1);
    osc::lua::LuaState ui;
    lua_State* L = ui.raw();
    ui.set_blueprint_store(&store);
    osc::ui::UIControlRegistry registry;
    osc::lua::register_moho_bindings(ui, sim);
    osc::lua::register_ui_bindings(ui, registry);
    osc::lua::register_user_bindings(ui);
    osc::sim::SimCallbackQueue queue;
    lua_pushstring(L, "__osc_sim_callback_queue");
    lua_pushlightuserdata(L, &queue);
    lua_rawset(L, LUA_REGISTRYINDEX);

    const auto fails_with = [&](const char* call, const char* text) {
        const std::string code = std::string("local ok, err = pcall(function() ") + call +
                                 " end) failed = not ok and string.find(err, '" + text +
                                 "', 1, true) ~= nil";
        REQUIRE(ui.do_string(code).ok());
        lua_getglobal(L, "failed");
        const bool failed = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        return failed;
    };
    CHECK(fails_with("GetArmyScore(1)", "No session started."));
    CHECK(fails_with("DeleteCommand(4)", "No active session!"));
    CHECK(fails_with("CreateUnitAtMouse('tank', 0, 0, 0, 0)", "No active session."));
    CHECK(queue.empty());

    osc::GameStateManager game;
    game.transition_to(osc::GameState::GAME, nullptr);
    lua_pushstring(L, "__osc_game_state_mgr");
    lua_pushlightuserdata(L, &game);
    lua_rawset(L, LUA_REGISTRYINDEX);
    CHECK(fails_with("GetArmyScore()", "expected 1 args, but got 0"));
    CHECK(fails_with("DeleteCommand('4')", "integer expected"));
    CHECK(fails_with("CreateUnitAtMouse('tank', 0, 0, 0)", "expected 5 args, but got 4"));
    CHECK(fails_with("CreateUnitAtMouse('none', 0, 0, 0, 0)", "Unknown unit kind: none"));
    CHECK(fails_with("CreateUnitAtMouse('tank', 1, 0, 0, 0)",
                     "Invalid army index, must be >= 0 and < 1 but got 1."));
    CHECK(queue.empty());

    REQUIRE(ui.do_string("local function count(...) return arg.n end "
                         "score_answers = count(GetArmyScore(1)) "
                         "DeleteCommand(4) CreateUnitAtMouse('TANK', 0, 2, 3, 0.5)")
                .ok());
    lua_getglobal(L, "score_answers");
    CHECK(lua_tonumber(L, -1) == 0.0);
    lua_pop(L, 1);
    const auto sent = queue.drain();
    REQUIRE(sent.size() == 2);
    CHECK(sent[0].func_name == osc::sim::kDecreaseCommandCountCallback);
    CHECK(std::get<double>(sent[0].args.at("Command")) == 4.0);
    CHECK(std::get<double>(sent[0].args.at("Count")) == 1.0);
    CHECK(sent[1].func_name == osc::sim::kCreateUnitCallback);
    CHECK(std::get<std::string>(sent[1].args.at("Blueprint")) == "tank");
    CHECK(std::get<double>(sent[1].args.at("Army")) == 0.0);
    CHECK(std::get<double>(sent[1].args.at("X")) == 2.0); // no world view: the cursor at 0, 0
    CHECK(std::get<double>(sent[1].args.at("Z")) == 3.0);
    CHECK(std::get<double>(sent[1].args.at("Heading")) == 0.5);
}

TEST_CASE("A dropped player's defeat is a command in the next beat", "[simcallback][drop]") {
    CallbackSim w;
    w.sim.add_army("ARMY_1", "ARMY_1");
    w.sim.add_army("ARMY_2", "ARMY_2");
    w.sim.set_recording(true);
    SimCallbackEntry defeat;
    defeat.func_name = osc::sim::kDefeatArmyCallback;
    defeat.args["Army"] = 1.0;
    w.sim.schedule_callback(osc::sim::kEngineSource, defeat); // the engine's, never a player's
    CHECK_FALSE(w.sim.army_at(1)->is_defeated()); // not between ticks
    w.sim.tick();
    CHECK(w.sim.army_at(1)->is_defeated());
    CHECK_FALSE(w.sim.army_at(0)->is_defeated());
    CHECK(w.sim.recorded_replay().commands.size() == 1); // a replay defeats it too

    defeat.args["Army"] = 7.0; // no such army: nothing happens
    w.sim.schedule_callback(osc::sim::kEngineSource, defeat);
    w.sim.tick();
    CHECK_FALSE(w.sim.army_at(0)->is_defeated());
}

TEST_CASE("A SimCallback sent at once keeps its place after the order issued before it",
          "[simcallback]") {
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    auto unit = std::make_unique<osc::sim::Unit>();
    unit->set_army(0);
    const osc::u32 id = sim.entity_registry().register_entity(std::move(unit));
    osc::lua::LuaState ui;
    osc::lua::register_moho_bindings(ui, sim);
    osc::lua::register_user_bindings(ui);
    osc::renderer::InputHandler input;
    input.set_selected({id});
    osc::sim::SimCallbackQueue queue;
    queue.set_send([&](osc::sim::SimCallbackEntry& callback) {
        sim.submit_callback(std::move(callback));
        return true;
    });
    lua_State* L = ui.raw();
    lua_pushstring(L, "__osc_input_handler");
    lua_pushlightuserdata(L, &input);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "__osc_sim_callback_queue");
    lua_pushlightuserdata(L, &queue);
    lua_rawset(L, LUA_REGISTRYINDEX);

    REQUIRE(ui.do_string("IssueCommand('UNITCOMMAND_Script', {TaskName = 'AttackMove'})"
                         " SimCallback({Func = 'AttackMove'}, true)")
                .ok());
    CHECK(queue.empty());
    const auto pending = sim.command_scheduler().pending();
    REQUIRE(pending.size() == 2);
    CHECK_FALSE(pending[0].callback);
    CHECK(pending[0].command.type == osc::sim::CommandType::Script);
    REQUIRE(pending[1].callback);
    CHECK(pending[1].callback->func_name == "AttackMove");
    CHECK(pending[1].callback->unit_ids == std::vector<osc::u32>{id});
    CHECK(pending[1].exec_beat == pending[0].exec_beat);

    queue.set_send([](osc::sim::SimCallbackEntry&) { return false; });
    REQUIRE(ui.do_string("SimCallback({Func = 'Later'})").ok());
    CHECK(sim.command_scheduler().pending().size() == 2);
    CHECK(queue.drain().size() == 1);
}
