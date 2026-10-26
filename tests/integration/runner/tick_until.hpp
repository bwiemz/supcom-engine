#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace osc::lua {
class LuaState;
}

namespace osc::test {

struct TestContext;

/// Calls `tick` until `done` holds, at most `limit` times, and returns the calls it took (0 if
/// `done` held at once); if `done` never holds, records the failure "<what>: not within <limit>
/// ticks".
std::optional<int> tick_until(const std::function<void()>& tick, std::string_view what, int limit,
                              const std::function<bool()>& done);

/// As above, ticking ctx.sim.
std::optional<int> tick_until(TestContext& ctx, std::string_view what, int limit,
                              const std::function<bool()>& done);

/// As above, until the Lua expression `condition` is true in ctx's state.
std::optional<int> tick_until(TestContext& ctx, std::string_view what, int limit,
                              const std::string& condition);

/// Whether the Lua expression `condition` is true in `lua` (false if it errs).
bool lua_true(lua::LuaState& lua, const std::string& condition);

/// Ticks ctx.sim `ticks` times: a wait whose length is what the test checks.
void tick_for(TestContext& ctx, int ticks);

} // namespace osc::test
