#pragma once

#include "core/result.hpp"

#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::lua {

/// Run a script from the VFS the way Moho's `doscript` does (M221a): the
/// file, then its hooks (script_hooks), loaded as ONE chunk named
/// "@<path>", so a hook sees the file's locals. A newline joins two files
/// when the first doesn't end in one.
///
/// `env_index` is a stack index of the environment table, or 0 for the
/// globals table. The VFS comes from the state's registry (LuaState::set_vfs).
/// On failure returns an error naming the script; the Lua stack is left as
/// it was either way.
///
/// Every engine entry point that executes game scripts must go through this
/// (not LuaState::do_buffer), or hooks silently stop applying.
Result<void> run_vfs_script(lua_State* L, std::string_view path, int env_index = 0);

/// The hooks of the script at `path`, in the order they run, as normalised
/// VFS paths: `<dir><path>` for each init-file hook directory (e.g.
/// /schook/lua/simInit.lua), then `<location><hookdir><path>` for each entry
/// of L's `__active_mods` (hookdir "/hook" when absent or empty; an entry
/// without a location has none). Only files that exist.
std::vector<std::string> script_hooks(lua_State* L, const vfs::VirtualFileSystem& vfs,
                                      std::string_view path);

/// Append a file's bytes to a script's chunk as Moho's concatenating reader
/// does: a newline follows a file that doesn't end in one. A UTF-8 BOM is
/// dropped (e.g. loc/us/strings_db.lua); an empty file adds nothing.
void append_script_file(std::string& chunk, const std::vector<char>& data);

} // namespace osc::lua
