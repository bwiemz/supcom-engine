#pragma once

// Which thread may use a Lua state (OE-134): the sim's state belongs to the
// sim's own thread while a tick runs there, and to the main thread between
// ticks. A check through the vendored Lua's lua_lock (lstate.h) counts every
// API call on the watched state from any other thread: the UI reading the
// live sim mid-tick, which desynced network games before it was found. Test
// runs fail on any; players' runs install no check.

#include "core/types.hpp"

#include <thread>

struct lua_State;

namespace osc::lua {

/// Install the check (once; it costs a call per Lua API call while
/// installed). Without it, watch() records nothing.
void install_lua_thread_check();
/// From now on only `owner` may use `L`'s state (a default id: anyone).
void watch_lua_state(lua_State* L, std::thread::id owner);
/// The calls made on the watched state from a thread that didn't own it.
u64 lua_thread_violations();

} // namespace osc::lua
