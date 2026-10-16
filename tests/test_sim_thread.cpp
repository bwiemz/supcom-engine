// The sim's own thread (OE-134): one job at a time, waited for between
// frames; what a job throws reaches the waiting thread; a Lua state used off
// its owner's thread is caught.

#include <catch2/catch_test_macros.hpp>

#include "app/sim_thread.hpp"
#include "lua/lua_thread_check.hpp"

extern "C" {
#include <lua.h>
}

#include <atomic>
#include <stdexcept>
#include <thread>

TEST_CASE("The sim thread runs a job off the caller's thread, one at a time", "[simthread]") {
    osc::app::SimThread sim;
    CHECK_FALSE(sim.busy());
    sim.wait(); // nothing to wait for

    std::atomic<int> runs{0};
    std::thread::id ran_on;
    for (int i = 0; i < 50; ++i) {
        sim.start([&] {
            ran_on = std::this_thread::get_id();
            ++runs;
        });
        CHECK(sim.busy());
        sim.wait();
        CHECK_FALSE(sim.busy());
        CHECK(runs == i + 1); // done, and seen, once waited for
    }
    CHECK(ran_on != std::this_thread::get_id());

    // What a job throws is thrown where it is waited for, and the thread runs on
    sim.start([] { throw std::runtime_error("a tick failed"); });
    CHECK_THROWS_AS(sim.wait(), std::runtime_error);
    sim.start([&] { ++runs; });
    sim.wait();
    CHECK(runs == 51);
}

TEST_CASE("A watched Lua state used off its owner's thread is counted", "[simthread]") {
    lua_State* L = lua_open();
    osc::lua::install_lua_thread_check();
    const auto before = osc::lua::lua_thread_violations();

    // Owned by another thread (the sim thread, say): this one may not use it
    std::thread other([] {});
    const std::thread::id other_id = other.get_id();
    other.join();
    osc::lua::watch_lua_state(L, other_id);
    lua_pushnil(L);
    lua_pop(L, 1);
    CHECK(osc::lua::lua_thread_violations() > before);

    // Anyone's again: no more counted
    osc::lua::watch_lua_state(L, {});
    const auto after = osc::lua::lua_thread_violations();
    lua_pushnil(L);
    lua_pop(L, 1);
    CHECK(osc::lua::lua_thread_violations() == after);
    osc::lua::watch_lua_state(nullptr, {});
    lua_close(L);
}
