#pragma once

// A sampling profile of the sim's Lua (under --profile): every
// kInstructions Lua instructions, the function running is counted. At the
// end of the run, the functions that ran the most instructions, by source
// and the line they start on: which scripts the Lua threads' and callbacks'
// time goes to (docs/performance.md). And each engine function the scripts
// call (a C binding: GetUnitsAroundPoint, say), timed from its call to its
// return: the time the samples, counting Lua instructions, can't see.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct lua_State;

namespace osc::sim::lua_profile {

/// How often a sample is taken, in Lua instructions.
constexpr int kInstructions = 1000;

/// Sample `L` from now on: the sim's state (its callbacks), and each
/// thread's coroutine (ThreadManager attaches them as it resumes them; a
/// new coroutine doesn't take its creator's hook).
void attach(lua_State* L);
/// The `count` functions with the most samples: where (source:line it
/// starts on, and its name), and how many.
std::vector<std::pair<std::string, uint64_t>> top(size_t count);
/// All the samples taken.
uint64_t total();
/// An engine function the scripts called: its calls, and their time.
struct EngineCall {
    std::string name;
    uint64_t calls = 0;
    double ms = 0;
};
/// The `count` engine functions that took the most time, a wait's
/// (WaitTicks, a yield: the ticks it sleeps) left out.
std::vector<EngineCall> engine_calls(size_t count);
/// Log the `count` functions with the most samples, and their share.
void log_report(size_t count);

} // namespace osc::sim::lua_profile
