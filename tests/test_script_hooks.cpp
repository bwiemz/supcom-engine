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
#include <string>
#include <utility>
#include <vector>

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

namespace {

std::string global_string(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    std::string value = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return value;
}

/// A VFS of `files` (path -> text), with /schook as its init hook directory.
std::unique_ptr<osc::vfs::VirtualFileSystem>
vfs_of(const std::vector<std::pair<std::string, std::string>>& files) {
    auto mount = std::make_unique<MemoryMount>();
    for (const auto& [path, text] : files) mount->add(path, text);
    auto vfs = std::make_unique<osc::vfs::VirtualFileSystem>();
    vfs->mount("/", std::move(mount));
    vfs->set_hook_dirs({"/schook"});
    return vfs;
}

} // namespace

TEST_CASE("a script and its hooks are one chunk: hooks see its locals", "[lua][hooks]") {
    auto vfs = vfs_of({
        {"/lua/m.lua", "local secret = 5\n"
                       "local function get() return secret end\n"
                       "function api() return get() end\n"},
        // Replaces the file's local function and changes its local: both
        // only reachable from inside the same chunk.
        {"/schook/lua/m.lua", "local old = get\n"
                              "get = function() return old() * 2 end\n"
                              "secret = secret + 1\n"},
    });
    LuaState state;
    state.set_vfs(vfs.get());

    REQUIRE(run_vfs_script(state.raw(), "/lua/m.lua").ok());
    REQUIRE(state.do_string("result = api()").ok());
    CHECK(global_number(state.raw(), "result") == 12);
    CHECK(global_number(state.raw(), "secret") == -1.0); // still a local
}

TEST_CASE("hooks run after the script: the init directories', then each active mod's",
          "[lua][hooks]") {
    auto vfs = vfs_of({
        {"/lua/o.lua", "order = 'base'"},
        {"/schook/lua/o.lua", "order = order .. ',schook'"},
        {"/mods/a/hook/lua/o.lua", "order = order .. ',a'"},
        {"/mods/b/myhook/lua/o.lua", "order = order .. ',b'"},
        {"/mods/c/hook/lua/o.lua", "order = order .. ',c'"},
        {"/mods/e/hook/lua/o.lua", "order = order .. ',e'"},
        // What a mod without a location would name, were it given one
        {"/hook/lua/o.lua", "order = order .. ',nolocation'"},
    });
    LuaState state;
    state.set_vfs(vfs.get());
    // In the list's order, not the alphabet's; b names its hook directory
    // (the paths normalised, as any VFS path), c's is empty (so /hook); d
    // has no location; the list ends at its first nil, so e is not active.
    REQUIRE(state
                .do_string(R"(
        __active_mods = {
            { location = '/Mods/B', hookdir = '/MyHook' },
            { location = '/mods/a' },
            { location = '/mods/c', hookdir = '' },
            { name = 'd' },
            'not a mod',
            nil,
            { location = '/mods/e' },
        }
    )")
                .ok());
    // (the constructor still sets [7])
    REQUIRE(state.do_string("assert(__active_mods[7] ~= nil)").ok());
    const int top = lua_gettop(state.raw());

    CHECK(script_hooks(state.raw(), *vfs, "/LUA/O.lua") ==
          std::vector<std::string>{"/schook/lua/o.lua", "/mods/b/myhook/lua/o.lua",
                                   "/mods/a/hook/lua/o.lua", "/mods/c/hook/lua/o.lua"});
    REQUIRE(run_vfs_script(state.raw(), "/lua/o.lua").ok());
    CHECK(global_string(state.raw(), "order") == "base,schook,b,a,c");
    CHECK(lua_gettop(state.raw()) == top);
}

TEST_CASE("without __active_mods, or with one that isn't a list, only the init hooks run",
          "[lua][hooks]") {
    auto vfs = vfs_of({
        {"/lua/o.lua", "order = 'base'"},
        {"/schook/lua/o.lua", "order = order .. ',schook'"},
        {"/mods/a/hook/lua/o.lua", "order = order .. ',a'"},
    });
    LuaState state;
    state.set_vfs(vfs.get());

    REQUIRE(run_vfs_script(state.raw(), "/lua/o.lua").ok());
    CHECK(global_string(state.raw(), "order") == "base,schook");
    REQUIRE(state.do_string("__active_mods = '/mods/a'").ok());
    REQUIRE(run_vfs_script(state.raw(), "/lua/o.lua").ok());
    CHECK(global_string(state.raw(), "order") == "base,schook");
}

TEST_CASE("files are joined with a newline where one doesn't end in one", "[lua][hooks]") {
    // Without it the hook would be part of the script's last line: the
    // comment's, here, so it would never run.
    auto vfs = vfs_of({
        {"/lua/n.lua", "x = 1 -- no newline at the end"},
        {"/schook/lua/n.lua", "\xEF\xBB\xBFy = 2"}, // with a BOM, dropped
        {"/mods/a/hook/lua/n.lua", ""},             // empty: nothing
        {"/mods/b/hook/lua/n.lua", "z = 3\n"},
    });
    LuaState state;
    state.set_vfs(vfs.get());
    REQUIRE(state
                .do_string("__active_mods = { { location = '/mods/a' }, "
                           "{ location = '/mods/b' } }")
                .ok());

    const int top = lua_gettop(state.raw());
    auto result = run_vfs_script(state.raw(), "/lua/n.lua");
    REQUIRE(result.ok());
    CHECK(lua_gettop(state.raw()) == top);
    CHECK(global_number(state.raw(), "x") == 1);
    CHECK(global_number(state.raw(), "y") == 2);
    CHECK(global_number(state.raw(), "z") == 3);
}

TEST_CASE("an error in a hook fails its script, which a return can't be followed by",
          "[lua][hooks]") {
    auto vfs = vfs_of({
        {"/lua/e.lua", "x = 1"},
        {"/schook/lua/e.lua", "error('from the hook')"},
        {"/lua/r.lua", "return 1"},
        {"/schook/lua/r.lua", "y = 2"},
    });
    LuaState state;
    state.set_vfs(vfs.get());
    const int top = lua_gettop(state.raw());

    auto hooked = run_vfs_script(state.raw(), "/lua/e.lua");
    REQUIRE_FALSE(hooked.ok());
    CHECK(hooked.error().message.find("from the hook") != std::string::npos);
    CHECK(global_number(state.raw(), "x") == 1); // the chunk ran up to it
    // One chunk: Lua 5.0 allows nothing after a return, so a hooked script
    // must not return (Moho's rule too).
    auto returned = run_vfs_script(state.raw(), "/lua/r.lua");
    REQUIRE_FALSE(returned.ok());
    CHECK(returned.error().message.find("/lua/r.lua") != std::string::npos);
    CHECK(lua_gettop(state.raw()) == top);
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
