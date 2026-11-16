#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/engine_bindings.hpp"
#include "lua/game_mods.hpp"
#include "lua/init_loader.hpp"
#include "lua/lua_state.hpp"
#include "lua/session_manager.hpp"
#include "sim/lua_bytes.hpp"
#include "support/memory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <memory>
#include <string>

extern "C" {
#include <lua.h>
}

using namespace osc::lua;

namespace {

bool lua_true(LuaState& state, const std::string& expr) {
    REQUIRE(state.do_string("__check = " + expr).ok());
    lua_getglobal(state.raw(), "__check");
    const bool value = lua_toboolean(state.raw(), -1) != 0;
    lua_pop(state.raw(), 1);
    return value;
}

/// read_session_config of the global table `name`.
osc::sim::GameSetup setup_of(LuaState& state, const char* name) {
    lua_getglobal(state.raw(), name);
    auto setup = read_session_config(state.raw(), lua_gettop(state.raw()));
    lua_pop(state.raw(), 1);
    return setup;
}

} // namespace

TEST_CASE("A launch's mods: the lobby's GameMods, else scenarioMods", "[lua][mods]") {
    LuaState launch;
    const auto made = launch.do_string(R"(
        lobby = { GameOptions = {}, PlayerOptions = {},
                  GameMods = { { uid = 'b-mod', location = '/mods/b' },
                               { uid = 'a-mod', location = '/mods/a', hookdir = '/h' } },
                  scenarioMods = { { uid = 'not-this-one' } } }
        campaign = { scenarioMods = { { uid = 'campaign', location = '/mods/c' } } }
        none = { PlayerOptions = {} }
        cyclic = { GameMods = {} }
        cyclic.GameMods[1] = cyclic.GameMods
    )");
    REQUIRE(made.ok());

    const auto lobby = setup_of(launch, "lobby");
    const auto campaign = setup_of(launch, "campaign");
    CHECK(setup_of(launch, "none").mods.empty());
    CHECK(setup_of(launch, "cyclic").mods.empty()); // reported; no mods

    // Each game state reads them back as its __active_mods, in order
    LuaState game;
    set_active_mods(game.raw(), lobby.mods);
    CHECK(lua_true(game, "table.getn(__active_mods) == 2"));
    CHECK(lua_true(game, "__active_mods[1].uid == 'b-mod' and __active_mods[2].hookdir == '/h'"));
    set_active_mods(game.raw(), campaign.mods);
    CHECK(lua_true(game, "table.getn(__active_mods) == 1 and __active_mods[1].uid == 'campaign'"));
}

TEST_CASE("set_active_mods: the game's list, or an empty one", "[lua][mods]") {
    LuaState state;
    set_active_mods(state.raw(), "");
    CHECK(lua_true(state, "type(__active_mods) == 'table' and next(__active_mods) == nil"));

    // Bytes that hold no list, or nothing at all: no mods, never a crash
    REQUIRE(state.do_string("__value = 'a string'").ok());
    lua_getglobal(state.raw(), "__value");
    const auto not_a_list = osc::sim::lua_to_bytes(state.raw(), -1);
    lua_pop(state.raw(), 1);
    REQUIRE(not_a_list);
    const int top = lua_gettop(state.raw());
    for (const std::string& bytes : {*not_a_list, std::string("\x05\xff", 2)}) {
        set_active_mods(state.raw(), bytes);
        CHECK(lua_true(state, "type(__active_mods) == 'table' and next(__active_mods) == nil"));
        CHECK(lua_gettop(state.raw()) == top);
    }
}

TEST_CASE("The bindings keep a launch's __active_mods", "[lua][mods]") {
    LuaState state;
    REQUIRE(state.do_string("__active_mods = { { uid = 'kept' } }").ok());
    register_blueprint_bindings(state); // as load_blueprints does, after the launch set them
    CHECK(lua_true(state, "__active_mods[1].uid == 'kept'"));

    LuaState bare; // ...and give one to a state without
    register_blueprint_bindings(bare);
    CHECK(lua_true(bare, "type(__active_mods) == 'table' and next(__active_mods) == nil"));
    REQUIRE(bare.do_string("__active_mods = 'not a list'").ok());
    ensure_active_mods(bare.raw());
    CHECK(lua_true(bare, "type(__active_mods) == 'table'"));
}

TEST_CASE("The blueprints load through /lua/RuleInit.lua, hooked by the game's mods",
          "[lua][mods]") {
    // A rules script shaped as retail's: it loads Blueprints.lua and calls
    // LoadBlueprints() itself. The game's mods hook it (Moho's doscript
    // appends each hook to the script), the second with an error; a mod
    // that isn't in the game hooks it too.
    auto mount = std::make_unique<osc::test::MemoryMount>();
    mount->add("/lua/RuleInit.lua", "__blueprints = {}\n"
                                    "mods_seen = table.getn(__active_mods)\n"
                                    "doscript '/lua/system/Blueprints.lua'\n"
                                    "LoadBlueprints()\n");
    mount->add("/lua/system/Blueprints.lua",
               "function LoadBlueprints()\n"
               "    RegisterUnitBlueprint { BlueprintId = 'UEL0001',\n"
               "                            Source = '/units/uel0001/uel0001_unit.bp' }\n"
               "    loaded = (loaded or 0) + 1\n"
               "end\n");
    mount->add("/mods/game/hook/lua/RuleInit.lua", "hooked = 'game:' .. tostring(loaded)\n");
    mount->add("/mods/broken/hook/lua/RuleInit.lua", "error('a broken hook')\n");
    mount->add("/mods/other/hook/lua/RuleInit.lua", "other_hooked = true\n");
    // The engine loads import() after the rules script: the state goes on
    // to import modules before simInit.lua loads it again.
    mount->add("/lua/system/import.lua", "import_saw = hooked\n");
    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::move(mount));

    LuaState state;
    REQUIRE(state
                .do_string("__active_mods = { { uid = 'game', location = '/mods/game' },"
                           "                  { uid = 'broken', location = '/mods/broken' } }")
                .ok());
    osc::blueprints::BlueprintStore store(state.raw());
    InitLoader loader;
    const auto loaded = loader.load_blueprints(state, vfs, store);
    REQUIRE(loaded.ok());

    // RuleInit.lua ran once, with the game's mods set, then their hooks.
    // The broken one is reported, and the load goes on (Moho's
    // SCR_LuaDoScript): its blueprints are the store's, and __blueprints.
    CHECK(lua_true(state, "mods_seen == 2 and loaded == 1"));
    CHECK(lua_true(state, "hooked == 'game:1'"));
    CHECK(lua_true(state, "rawget(_G, 'other_hooked') == nil"));
    CHECK(lua_true(state, "import_saw == 'game:1'"));
    CHECK(store.find("uel0001") != nullptr);
    CHECK(store.registered_count() == 1);
    CHECK(lua_true(state, "__blueprints.uel0001.BlueprintId == 'UEL0001'"));
    // (a rebind for the next game's state keeps the entry, without its table)
    store.rebind(state.raw());
    CHECK(store.total_count() == 1);
    CHECK(store.registered_count() == 0);

    // A hook that doesn't compile stops the whole script, as in Moho: no
    // blueprints, so no game
    auto unparsed = std::make_unique<osc::test::MemoryMount>();
    unparsed->add("/lua/RuleInit.lua", "doscript '/lua/system/Blueprints.lua'\n"
                                       "LoadBlueprints()\n");
    unparsed->add("/lua/system/Blueprints.lua",
                  "function LoadBlueprints() RegisterUnitBlueprint { BlueprintId = 'a' } end\n");
    unparsed->add("/mods/game/hook/lua/RuleInit.lua", "this is not lua\n");
    osc::vfs::VirtualFileSystem broken;
    broken.mount("/", std::move(unparsed));
    LuaState stopped;
    REQUIRE(stopped.do_string("__active_mods = { { location = '/mods/game' } }").ok());
    osc::blueprints::BlueprintStore nothing(stopped.raw());
    const auto failed = loader.load_blueprints(stopped, broken, nothing);
    REQUIRE_FALSE(failed.ok());
    CHECK(failed.error().message.find("/lua/RuleInit.lua") != std::string::npos);
    CHECK(nothing.registered_count() == 0);

    // Data without a rules script has no blueprints to load
    osc::vfs::VirtualFileSystem empty;
    empty.mount("/", std::make_unique<osc::test::MemoryMount>());
    LuaState bare;
    osc::blueprints::BlueprintStore none(bare.raw());
    const auto missing = loader.load_blueprints(bare, empty, none);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.find("/lua/RuleInit.lua") != std::string::npos);
}

TEST_CASE("A UI state remembers that it ran SessionInit.lua", "[lua][mods]") {
    LuaState state;
    CHECK_FALSE(session_init_ran(state.raw()));
    mark_session_init(state.raw());
    CHECK(session_init_ran(state.raw()));
    CHECK_FALSE(session_init_ran(LuaState().raw()));
}
