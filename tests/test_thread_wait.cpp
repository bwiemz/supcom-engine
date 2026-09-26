#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "sim/thread_manager.hpp"
#include "sim/waitable.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {

struct TestWait : osc::sim::Waitable {
    bool is_done() const override { return false; }
    bool is_cancelled() const override { return false; }
};

TestWait* g_wait = nullptr;

/// Park() -- WaitFor(g_wait)
int park(lua_State* L) {
    lua_pushlightuserdata(L, static_cast<osc::sim::Waitable*>(g_wait));
    return lua_yield(L, 1);
}

/// Sleep(n) -- WaitTicks(n)
int sleep_ticks(lua_State* L) {
    lua_pushnumber(L, luaL_checknumber(L, 1));
    return lua_yield(L, 1);
}

/// Fork global function `name`; returns the thread's registry ref.
int fork(osc::sim::ThreadManager& tm, lua_State* L, const char* name) {
    lua_settop(L, 0);
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    tm.fork_thread(L);
    lua_pushstring(L, "_c_ref");
    lua_rawget(L, -2);
    const int ref = static_cast<int>(lua_tonumber(L, -1));
    lua_settop(L, 0);
    return ref;
}

bool global_true(lua_State* L, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}

} // namespace

TEST_CASE("WaitFor: a wake reaches only the thread that waited", "[threads]") {
    osc::lua::LuaState state;
    lua_State* L = state.raw();
    osc::sim::ThreadManager tm(L);
    TestWait wait;
    g_wait = &wait;
    lua_register(L, "Park", park);
    lua_register(L, "Sleep", sleep_ticks);
    REQUIRE(state.do_string(R"(
        function waiter() Park() waiter_resumed = true end
        function sleeper() Sleep(1000) sleeper_resumed = true end
    )"));

    // A thread parks on the waitable, then is killed: its registry ref
    // goes back to Lua's free list...
    const int killed = fork(tm, L, "waiter");
    tm.resume_all(1);
    REQUIRE(wait.has_waiting_thread());
    tm.kill_thread(killed);
    tm.resume_all(2); // releases the dead thread's ref

    // ... and a new, unrelated thread gets it.
    const int reused = fork(tm, L, "sleeper");
    REQUIRE(reused == killed);
    tm.resume_all(3); // sleeps 1000 ticks

    // The old wait ends: it must not wake the sleeper.
    tm.wake(wait, 4);
    tm.resume_all(5);
    CHECK_FALSE(global_true(L, "sleeper_resumed"));
    CHECK_FALSE(wait.has_waiting_thread());

    // A live waiter is woken.
    fork(tm, L, "waiter");
    tm.resume_all(6);
    REQUIRE(wait.has_waiting_thread());
    tm.wake(wait, 7);
    tm.resume_all(7);
    CHECK(global_true(L, "waiter_resumed"));
    g_wait = nullptr;
}

TEST_CASE("KillThread: a stale handle can't kill the thread that took its ref", "[threads]") {
    // A trash bag keeps a finished thread's handle; its ref goes back to
    // Lua's free list and a new thread gets it. Destroying the bag must not
    // kill the new thread (a unit's death thread was killed this way, so
    // the unit never died).
    osc::lua::LuaState state;
    lua_State* L = state.raw();
    osc::sim::ThreadManager tm(L);
    tm.register_in_registry(L);
    lua_register(L, "Sleep", sleep_ticks);
    REQUIRE(state.do_string(R"(
        function quick() end
        function sleeper() Sleep(1000) sleeper_resumed = true end
    )"));

    lua_settop(L, 0);
    lua_pushstring(L, "quick");
    lua_rawget(L, LUA_GLOBALSINDEX);
    tm.fork_thread(L);
    lua_pushstring(L, "stale");
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_GLOBALSINDEX); // stale = the quick thread's handle
    lua_pushstring(L, "_c_ref");
    lua_rawget(L, -2);
    const int quick_ref = static_cast<int>(lua_tonumber(L, -1));
    lua_settop(L, 0);
    tm.resume_all(1); // it finishes
    tm.resume_all(2); // and its ref is released

    const int sleeper_ref = fork(tm, L, "sleeper");
    REQUIRE(sleeper_ref == quick_ref); // the ref is reused
    tm.resume_all(3);
    REQUIRE(tm.active_count() == 1);

    REQUIRE(state.do_string("stale:Destroy()"));
    tm.resume_all(4);
    CHECK(tm.active_count() == 1); // the sleeper lives on
}

TEST_CASE("A thread's handle lives as long as the thread, even in a weak table", "[threads]") {
    // Retail keeps threads in trash bags, which are weak tables. A handle
    // collected from one was never killed when the bag was destroyed: a
    // disbanded platoon's threads ran on.
    osc::lua::LuaState state;
    lua_State* L = state.raw();
    osc::sim::ThreadManager tm(L);
    tm.register_in_registry(L);
    lua_register(L, "Sleep", sleep_ticks);
    REQUIRE(state.do_string(R"(
        function sleeper() Sleep(1000) end
        bag = setmetatable({}, {__mode = 'v'})
    )"));
    lua_settop(L, 0);
    lua_pushstring(L, "sleeper");
    lua_rawget(L, LUA_GLOBALSINDEX);
    tm.fork_thread(L);
    lua_pushstring(L, "bag");
    lua_rawget(L, LUA_GLOBALSINDEX);
    lua_pushvalue(L, -2);
    lua_rawseti(L, -2, 1); // bag[1] = the handle, held nowhere else
    lua_settop(L, 0);
    tm.resume_all(1);

    lua_setgcthreshold(L, 0); // a full collection
    REQUIRE(state.do_string("kept = bag[1] ~= nil; if kept then bag[1]:Destroy() end"));
    CHECK(global_true(L, "kept"));
    tm.resume_all(2); // the killed thread is released
    CHECK(tm.active_count() == 0);

    lua_setgcthreshold(L, 0);
    REQUIRE(state.do_string("gone = bag[1] == nil"));
    CHECK(global_true(L, "gone")); // and now its handle can go
}
