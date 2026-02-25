#pragma once

#include <filesystem>
#include <spdlog/spdlog.h>

// Forward declare lua_State to avoid pulling in Lua headers everywhere
struct lua_State;

namespace osc::log {

/// Initialize logging with console + file sinks.
void init(const std::filesystem::path& log_file = "opensupcom.log");

/// Flush and shutdown logging.
void shutdown();

// Lua-side logging functions (C functions registered into Lua)
int l_LOG(lua_State* L);
int l_WARN(lua_State* L);
int l_SPEW(lua_State* L);
int l_ALERT(lua_State* L);

} // namespace osc::log
