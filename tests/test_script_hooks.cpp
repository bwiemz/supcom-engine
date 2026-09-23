#include <catch2/catch_test_macros.hpp>

#include "lua/engine_bindings.hpp"
#include "lua/init_loader.hpp"
#include "lua/lua_state.hpp"
#include "lua/script_loader.hpp"
#include "support/memory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <random>

extern "C" {
#include <lua.h>
}

using namespace osc::lua;
using osc::test::MemoryMount;
namespace fs = std::filesystem;

namespace {

double global_number(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    double value = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : -1.0;
    lua_pop(L, 1);
    return value;
}

/// VFS with a base script and (optionally) its /schook hook.
std::unique_ptr<osc::vfs::VirtualFileSystem> make_vfs(bool with_hook) {
    auto mount = std::make_unique<MemoryMount>();
    mount->add("/lua/a.lua", "x = 1");
    if (with_hook) mount->add("/schook/lua/a.lua", "x = x + 10");
    mount->add("/lua/broken.lua", "this is not lua");
    auto vfs = std::make_unique<osc::vfs::VirtualFileSystem>();
    vfs->mount("/", std::move(mount));
    return vfs;
}

} // namespace

TEST_CASE("run_vfs_script applies hook directories after the base file",
          "[lua][hooks]") {
    auto vfs = make_vfs(true);
    vfs->set_hook_dirs({"/schook"});
    LuaState state;
    state.set_vfs(vfs.get());
    const int top = lua_gettop(state.raw()); // luaopen_* leave tables behind

    auto result = run_vfs_script(state.raw(), "/lua/a.lua");
    REQUIRE(result.ok());
    CHECK(global_number(state.raw(), "x") == 11);
    CHECK(lua_gettop(state.raw()) == top);
}

TEST_CASE("run_vfs_script without hook dirs runs only the base file",
          "[lua][hooks]") {
    auto vfs = make_vfs(true);
    LuaState state;
    state.set_vfs(vfs.get());

    REQUIRE(run_vfs_script(state.raw(), "/lua/a.lua").ok());
    CHECK(global_number(state.raw(), "x") == 1);
}

TEST_CASE("run_vfs_script reports missing files and syntax errors",
          "[lua][hooks]") {
    auto vfs = make_vfs(false);
    LuaState state;
    state.set_vfs(vfs.get());
    const int top = lua_gettop(state.raw());

    auto missing = run_vfs_script(state.raw(), "/lua/nope.lua");
    CHECK_FALSE(missing.ok());
    auto broken = run_vfs_script(state.raw(), "/lua/broken.lua");
    REQUIRE_FALSE(broken.ok());
    CHECK(broken.error().message.find("/lua/broken.lua") != std::string::npos);
    CHECK(lua_gettop(state.raw()) == top);
}

TEST_CASE("doscript runs hooks in the caller-supplied environment",
          "[lua][hooks]") {
    auto vfs = make_vfs(true);
    vfs->set_hook_dirs({"/schook"});
    LuaState state;
    state.set_vfs(vfs.get());
    register_blueprint_bindings(state);

    auto result = state.do_string(R"(
        env = { x = 100 }
        setmetatable(env, { __index = _G })
        doscript('/lua/a.lua', env)
        env_x = env.x
    )");
    REQUIRE(result.ok());
    // Base file sets env.x = 1, hook adds 10 -- both in env, not globals.
    CHECK(global_number(state.raw(), "env_x") == 11);
    CHECK(global_number(state.raw(), "x") == -1.0);
}

TEST_CASE("init loader records the init script's hook table", "[lua][hooks]") {
    std::random_device rd;
    fs::path dir = fs::temp_directory_path() /
                   ("osc_hook_init_" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(dir / "bin");
    fs::path init = dir / "bin" / "init.lua";
    std::ofstream(init) << "path = {}\nhook = { '/schook', '\\\\myhooks\\\\' }\n";

    LuaState state;
    osc::vfs::VirtualFileSystem vfs;
    InitConfig config;
    config.init_file = init;
    config.fa_path = dir;
    InitLoader loader;
    auto result = loader.execute_init(state, config, vfs);
    std::error_code ec;
    fs::remove_all(dir, ec);

    REQUIRE(result.ok());
    REQUIRE(vfs.hook_dirs().size() == 2);
    CHECK(vfs.hook_dirs()[0] == "/schook");
    CHECK(vfs.hook_dirs()[1] == "/myhooks"); // normalised like any VFS path
}
