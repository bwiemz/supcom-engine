#pragma once

#include "core/result.hpp"
#include "core/types.hpp"

#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::blueprints {
class BlueprintStore;
}

namespace osc::lua {

/// Registry keys for storing engine pointers accessible from C bindings.
constexpr const char* REG_VFS = "osc_vfs";
constexpr const char* REG_BLUEPRINT_STORE = "osc_blueprint_store";

/// RAII wrapper around a Lua 5.0 state.
class LuaState {
public:
    LuaState();
    ~LuaState();

    // Move-only
    LuaState(const LuaState&) = delete;
    LuaState& operator=(const LuaState&) = delete;
    LuaState(LuaState&& other) noexcept;
    LuaState& operator=(LuaState&& other) noexcept;

    lua_State* raw() const { return L_; }

    /// Register a C function as a global.
    void register_function(const char* name, int (*fn)(lua_State*));

    /// Register a C function inside a table (e.g., "io.dir").
    void register_table_function(const char* table_name,
                                  const char* func_name,
                                  int (*fn)(lua_State*));

    /// Set a global string variable.
    void set_global_string(const char* name, const char* value);

    /// Set a global boolean.
    void set_global_bool(const char* name, bool value);

    /// Set a global to an empty table.
    void set_global_table(const char* name);

    /// Execute a string of Lua code.
    Result<void> do_string(std::string_view code);

    /// Execute a file from the real filesystem (for init context).
    Result<void> do_file(const fs::path& path);

    /// Execute a buffer with a given chunk name.
    Result<void> do_buffer(const char* buf, size_t len, const char* name);

    /// Store a VFS pointer in the Lua registry for access from C bindings.
    void set_vfs(vfs::VirtualFileSystem* vfs);

    /// Store a BlueprintStore pointer in the Lua registry.
    void set_blueprint_store(blueprints::BlueprintStore* store);

    /// Retrieve the VFS pointer from a lua_State (static, for use in C bindings).
    static vfs::VirtualFileSystem* get_vfs(lua_State* L);

    /// Retrieve the BlueprintStore pointer from a lua_State.
    static blueprints::BlueprintStore* get_blueprint_store(lua_State* L);

private:
    lua_State* L_ = nullptr;
};

} // namespace osc::lua
