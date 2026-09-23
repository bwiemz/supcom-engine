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
