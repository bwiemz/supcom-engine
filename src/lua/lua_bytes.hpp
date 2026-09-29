#pragma once

// Plain Lua data as bytes (M221b), as Moho's SCR_ToString / SCR_FromString
// keep a launch's mod list: the launch and the replay carry it, and each
// game's Lua states read it back as their `__active_mods`.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

struct lua_State;

namespace osc::lua {

/// How deep tables may nest, written or read.
inline constexpr int kLuaBytesMaxDepth = 32;
/// The most bytes lua_to_bytes writes (a mod list is a few kilobytes).
inline constexpr size_t kLuaBytesMaxSize = size_t{16} << 20;

/// The value at `idx` as bytes: nil, a boolean, a number, a string, or a
/// table of them keyed by booleans, numbers or strings. Anything else -- and
/// a table entry holding or keyed by it -- is left out, as for any data that
/// crosses Lua states (core::copy_lua_value). A table's entries are written
/// in one order (number keys ascending, then strings, then false and true),
/// so the same data gives the same bytes. A table found twice is written
/// twice. Nothing for data nested deeper than kLuaBytesMaxDepth (so for a
/// table inside itself, which nests without end) or longer than
/// kLuaBytesMaxSize (tables shared at every level double at each).
std::optional<std::string> lua_to_bytes(lua_State* L, int idx);

/// Push the value `bytes` hold (as lua_to_bytes wrote it). False, with
/// nothing pushed, when they aren't exactly one such value.
bool push_lua_bytes(lua_State* L, std::string_view bytes);

} // namespace osc::lua
