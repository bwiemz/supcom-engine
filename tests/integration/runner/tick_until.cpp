#include "tick_until.hpp"

#include "core/test_status.hpp"
#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lua.h>
}

namespace osc::test {

namespace {

std::optional<int> until(const std::function<void()>& tick, std::string_view what, int limit,
                         const std::function<bool()>& done, const std::string* last_error) {
    for (int ticks = 0;; ++ticks) {
        if (done()) {
            return ticks;
        }
        if (ticks == limit) {
            break;
        }
        tick();
    }
    if (last_error && !last_error->empty()) {
        osc::test_status::fail("[FAIL] {}: not within {} ticks ({})", what, limit, *last_error);
    } else {
        osc::test_status::fail("[FAIL] {}: not within {} ticks", what, limit);
    }
    return std::nullopt;
}

bool lua_true(lua::LuaState& lua, const std::string& condition, std::string& error) {
    lua_State* L = lua.raw();
    const int top = lua_gettop(L);
    const auto r = lua.do_string("return (" + condition + ")");
    error = r ? std::string() : r.error().message;
    const bool held = r && lua_gettop(L) > top && lua_toboolean(L, top + 1) != 0;
    lua_settop(L, top);
    return held;
}

} // namespace

std::optional<int> tick_until(const std::function<void()>& tick, std::string_view what, int limit,
                              const std::function<bool()>& done) {
    return until(tick, what, limit, done, nullptr);
}

std::optional<int> tick_until(TestContext& ctx, std::string_view what, int limit,
                              const std::function<bool()>& done) {
    return until([&] { ctx.sim.tick(); }, what, limit, done, nullptr);
}

std::optional<int> tick_until(TestContext& ctx, std::string_view what, int limit,
                              const std::string& condition) {
    std::string error;
    return until([&] { ctx.sim.tick(); }, what, limit,
                 [&] { return lua_true(ctx.lua_state, condition, error); }, &error);
}

bool lua_true(lua::LuaState& lua, const std::string& condition) {
    std::string error;
    return lua_true(lua, condition, error);
}

void tick_for(TestContext& ctx, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        ctx.sim.tick();
    }
}

} // namespace osc::test
