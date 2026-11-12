#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/sim_sync.hpp"
#include "sim/lua_sim.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

#include <string>
#include <thread>
#include <vector>

using osc::lua::LuaState;

namespace {

double global_number(lua_State* L, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : -999.0;
    lua_pop(L, 1);
    return v;
}

bool run(LuaState& state, const char* code) {
    auto r = state.do_string(code);
    if (!r) FAIL(r.error().message);
    return static_cast<bool>(r);
}

} // namespace

TEST_CASE("copy_lua_value deep-copies plain data between states", "[lua][sync]") {
    LuaState from;
    LuaState to;
    run(from, R"(
        shared = { n = 1 }
        src = {
            num = 4, str = 'hi', flag = true,
            list = { 10, 20, 30 },
            a = shared, b = shared,        -- shared: copied once
            fn = function() end,           -- code is not synced
            [true] = 'boolean key',
        }
        src.self = src                     -- cycle
    )");
    lua_State* F = from.raw();
    lua_State* T = to.raw();
    lua_pushstring(F, "src");
    lua_rawget(F, LUA_GLOBALSINDEX);
    const int top_before = lua_gettop(F);
    osc::lua::copy_lua_value(F, -1, T);
    CHECK(lua_gettop(F) == top_before); // source stack untouched
    lua_pop(F, 1);
    lua_pushstring(T, "dst");
    lua_insert(T, -2);
    lua_rawset(T, LUA_GLOBALSINDEX);

    run(to, R"(
        ok_num = dst.num == 4 and 1 or 0
        ok_str = dst.str == 'hi' and 1 or 0
        ok_flag = dst.flag == true and 1 or 0
        ok_list = (dst.list[1] + dst.list[2] + dst.list[3] == 60) and 1 or 0
        ok_shared = (dst.a == dst.b and dst.a.n == 1) and 1 or 0
        ok_fn = dst.fn == nil and 1 or 0
        ok_bool_key = dst[true] == 'boolean key' and 1 or 0
        ok_cycle = dst.self == dst and 1 or 0
    )");
    for (const char* name : {"ok_num", "ok_str", "ok_flag", "ok_list", "ok_shared",
                             "ok_fn", "ok_bool_key", "ok_cycle"}) {
        INFO(name);
        CHECK(global_number(T, name) == 1.0);
    }
}

TEST_CASE("A delivered beat carries Sync to the user state, then resets it", "[lua][sync]") {
    LuaState sim;
    LuaState ui;
    run(sim, R"(
        function ResetSyncTable() Sync = { Score = {} } end
        ResetSyncTable()
        Sync.Score[1] = 250
        Sync.Tag = 'beat1'
        noted_new, noted_old = 0, 0
        function NoteFocusArmyChanged(new, old) noted_new, noted_old = new, old end
    )");
    run(ui, R"(
        Sync = { Tag = 'before' }
        seen_score, seen_tag, seen_focus_new, seen_focus_old, prev_tag = 0, '', 0, 0, ''
        function OnSync()
            seen_score = Sync.Score[1] or 0
            seen_tag = Sync.Tag or ''
            prev_tag = PreviousSync.Tag or ''
            if Sync.FocusArmyChanged then
                seen_focus_new = Sync.FocusArmyChanged.new
                seen_focus_old = Sync.FocusArmyChanged.old
            end
        end
    )");

    // Focus army 0 (Lua 1) -> observer requested from the UI.
    lua_State* U = ui.raw();
    lua_pushstring(U, "__osc_focus_army");
    lua_pushnumber(U, 0);
    lua_rawset(U, LUA_REGISTRYINDEX);
    lua_pushstring(U, osc::lua::kFocusArmyRequestKey);
    lua_pushnumber(U, -1);
    lua_rawset(U, LUA_REGISTRYINDEX);

    osc::lua::SyncQueue sync;
    sync.deliver(sim.raw(), U);

    CHECK(global_number(U, "seen_score") == 250.0);
    run(ui, "tag_ok = (seen_tag == 'beat1' and prev_tag == 'before') and 1 or 0");
    CHECK(global_number(U, "tag_ok") == 1.0);
    CHECK(global_number(U, "seen_focus_new") == -1.0);
    CHECK(global_number(U, "seen_focus_old") == 1.0);
    CHECK(global_number(sim.raw(), "noted_new") == -1.0);
    CHECK(global_number(sim.raw(), "noted_old") == 1.0);

    // Both states now agree on the focus; the request is consumed.
    lua_pushstring(sim.raw(), "__osc_focus_army");
    lua_rawget(sim.raw(), LUA_REGISTRYINDEX);
    CHECK(lua_tonumber(sim.raw(), -1) == -1.0);
    lua_pop(sim.raw(), 1);
    lua_pushstring(U, osc::lua::kFocusArmyRequestKey);
    lua_rawget(U, LUA_REGISTRYINDEX);
    CHECK(lua_isnil(U, -1));
    lua_pop(U, 1);

    // The sim's table was reset: the next beat carries nothing old.
    sync.deliver(sim.raw(), U);
    CHECK(global_number(U, "seen_score") == 0.0);
    run(ui, "cleared = (Sync.Tag == nil and Sync.FocusArmyChanged == nil) and 1 or 0");
    CHECK(global_number(U, "cleared") == 1.0);
}

TEST_CASE("Threaded Sync queues each beat without touching UI focus until delivery",
          "[lua][sync][thread]") {
    LuaState sim;
    LuaState ui;
    run(sim, "function ResetSyncTable() Sync = {} end ResetSyncTable() "
             "function NoteFocusArmyChanged(new, old) noted = new end");
    run(ui, "Sync = {} seen = 0 function OnSync() seen = seen + 1 end");
    osc::lua::set_focus_army(sim.raw(), ui.raw(), 0);
    const auto request = [](lua_State* L, int army) {
        lua_pushstring(L, osc::lua::kFocusArmyRequestKey);
        lua_pushnumber(L, army);
        lua_rawset(L, LUA_REGISTRYINDEX);
    };
    request(sim.raw(), 1);
    request(ui.raw(), 2); // UI wins a simultaneous sim request
    osc::lua::SyncQueue sync;
    sync.prepare_threaded(sim.raw(), ui.raw());
    std::thread worker([&] {
        sync.queue(sim.raw(), nullptr);
        request(sim.raw(), -1); // a second beat changes to observer
        sync.queue(sim.raw(), nullptr);
    });
    worker.join();
    CHECK(osc::lua::focus_army(ui.raw()) == 0);
    REQUIRE(sync.pending());
    sync.deliver(sim.raw(), ui.raw());
    CHECK(osc::lua::focus_army(ui.raw()) == 2);
    CHECK(global_number(ui.raw(), "seen") == 1);
    sync.deliver(sim.raw(), ui.raw());
    CHECK(osc::lua::focus_army(ui.raw()) == -1);
    CHECK(global_number(ui.raw(), "seen") == 2);
    CHECK_FALSE(sync.pending());
}

TEST_CASE("A threaded replay's first focus request uses the UI observer as its old focus",
          "[lua][sync][thread]") {
    for (const int army : {0, 1}) {
        LuaState sim;
        LuaState ui;
        run(sim, "function ResetSyncTable() Sync = {} end ResetSyncTable() "
                 "function NoteFocusArmyChanged(new, old) noted_new = new noted_old = old end");
        run(ui, "Sync = {} function OnSync() end");
        osc::lua::set_focus_army(sim.raw(), ui.raw(), 0);
        lua_pushstring(ui.raw(), "__osc_focus_army");
        lua_pushnumber(ui.raw(), -1);
        lua_rawset(ui.raw(), LUA_REGISTRYINDEX);
        lua_pushstring(ui.raw(), osc::lua::kFocusArmyRequestKey);
        lua_pushnumber(ui.raw(), army);
        lua_rawset(ui.raw(), LUA_REGISTRYINDEX);
        osc::lua::SyncQueue sync;
        sync.prepare_threaded(sim.raw(), ui.raw());
        std::thread worker([&] { sync.queue(sim.raw(), nullptr); });
        worker.join();
        CHECK(osc::lua::focus_army(ui.raw()) == -1);
        CHECK(global_number(sim.raw(), "noted_old") == -1);
        CHECK(global_number(sim.raw(), "noted_new") == army + 1);
        sync.deliver(sim.raw(), ui.raw());
        CHECK(osc::lua::focus_army(ui.raw()) == army);
    }
}

TEST_CASE("A beat's Sync is the table as its beat left it", "[lua][sync]") {
    LuaState sim;
    LuaState ui;
    run(sim, "function ResetSyncTable() Sync = {} end ResetSyncTable() "
             "Sync.Tag = 'beat1' kept = Sync");
    run(ui, "Sync = {} seen = '' function OnSync() seen = Sync.Tag end");
    osc::lua::SyncQueue sync;
    sync.queue(sim.raw(), nullptr);
    run(sim, "kept.Tag = 'later'");
    sync.deliver(sim.raw(), ui.raw());
    run(ui, "as_queued = seen == 'beat1' and 1 or 0");
    CHECK(global_number(ui.raw(), "as_queued") == 1.0);
}

TEST_CASE("The UI's selection reaches the sim at a beat's Sync, threaded or not",
          "[lua][sync][selection]") {
    // Moho's CWldSession::SetSelection hands a changed selection to the sim
    // driver (SetSyncFilterMaskB); the next Sim::Sync copies it in, where
    // DebugGetSelection reads it (Sim::mSyncFilter.maskB).
    using osc::sim::EntityId;
    LuaState sim_lua;
    LuaState ui;
    osc::sim::SimState world{sim_lua.raw(), nullptr};
    osc::sim::set_sim(sim_lua.raw(), &world);
    run(sim_lua, "function ResetSyncTable() Sync = {} end ResetSyncTable()");
    run(ui, "Sync = {} function OnSync() end");
    const auto keys = [&world] {
        std::vector<osc::u64> out;
        for (const EntityId id : world.debug_selection()) out.push_back(id.key());
        return out;
    };
    const EntityId a = EntityId::of(3, 1);
    const EntityId b = EntityId::of(7, 1);
    const EntityId c = EntityId::of(9, 2);
    osc::lua::SyncQueue sync;

    // Serial: the next beat's Sync takes it, in ascending ids
    osc::lua::request_debug_selection(ui.raw(), {b, a});
    CHECK(world.debug_selection().empty()); // not before the Sync
    sync.queue(sim_lua.raw(), ui.raw());
    CHECK(keys() == std::vector<osc::u64>{a.key(), b.key()});
    lua_pushstring(ui.raw(), osc::lua::kDebugSelectionKey);
    lua_rawget(ui.raw(), LUA_REGISTRYINDEX);
    CHECK(lua_isnil(ui.raw(), -1)); // taken
    lua_pop(ui.raw(), 1);
    sync.queue(sim_lua.raw(), ui.raw()); // a beat with no change keeps it
    CHECK(keys() == std::vector<osc::u64>{a.key(), b.key()});
    while (sync.pending()) sync.deliver(sim_lua.raw(), ui.raw());
    // With none queued, a delivery runs the Sync's steps itself
    osc::lua::request_debug_selection(ui.raw(), {c});
    sync.deliver(sim_lua.raw(), ui.raw());
    CHECK(keys() == std::vector<osc::u64>{c.key()});

    // Threaded: taken between jobs, applied by the sim thread's Sync
    osc::lua::request_debug_selection(ui.raw(), {c, a});
    sync.prepare_threaded(sim_lua.raw(), ui.raw());
    osc::lua::request_debug_selection(ui.raw(), {}); // made during the job: the next job's
    std::thread worker([&] {
        sync.queue(sim_lua.raw(), nullptr);
        sync.queue(sim_lua.raw(), nullptr); // a second beat, nothing new
    });
    worker.join();
    CHECK(keys() == std::vector<osc::u64>{a.key(), c.key()});
    sync.prepare_threaded(sim_lua.raw(), ui.raw());
    std::thread next([&] { sync.queue(sim_lua.raw(), nullptr); });
    next.join();
    CHECK(world.debug_selection().empty());
}
