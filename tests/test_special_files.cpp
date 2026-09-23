// FA's special files (M199b): replays and saved games, one folder per
// profile, as retail's file picker and replay dialog use them.

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/special_files.hpp"
#include "sim/replay.hpp"

extern "C" {
#include <lua.h>
}

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using osc::lua::SpecialFiles;

namespace {

/// A fresh folder, removed afterwards.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("osc-special-files-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "x";
}

osc::sim::Replay playable_replay() {
    osc::sim::Replay r;
    r.has_setup = true;
    r.setup.scenario = "/maps/SCMP_009/SCMP_009_scenario.lua";
    r.final_tick = 10;
    return r;
}

/// Run Lua; the error message, or empty.
std::string run(osc::lua::LuaState& lua, const char* code) {
    auto r = lua.do_string(code);
    return r ? std::string() : r.error().message;
}

} // namespace

TEST_CASE("Special files live per profile, with our own extension", "[specialfiles]") {
    TempDir dir;
    SpecialFiles files(dir.path);
    const auto* replay = SpecialFiles::find_type("Replay");
    REQUIRE(replay);
    CHECK(SpecialFiles::find_type("SaveGame"));
    CHECK_FALSE(SpecialFiles::find_type("Screenshot"));

    CHECK(files.path(*replay, "Player", "LastGame") ==
          dir.path / "replays" / "Player" / "LastGame.oscreplay");

    touch(files.path(*replay, "Player", "b"));
    touch(files.path(*replay, "Player", "a"));
    touch(files.path(*replay, "Other", "c"));
    touch(dir.path / "replays" / "Player" / "fa.SCFAReplay");  // FA's own: not ours
    touch(dir.path / "replays" / "FAOnly" / "old.SCFAReplay"); // no profile of ours
    const auto listed = files.list(*replay);
    REQUIRE(listed.size() == 2);
    CHECK(listed.at("Player") == std::vector<std::string>{"a", "b"});
    CHECK(listed.at("Other") == std::vector<std::string>{"c"});
}

TEST_CASE("The special-file globals, as retail's file picker uses them", "[specialfiles]") {
    TempDir dir;
    SpecialFiles files(dir.path);
    osc::lua::LuaState lua;
    osc::lua::register_special_file_bindings(lua, &files);
    const auto* type = SpecialFiles::find_type("Replay");
    touch(files.path(*type, "Player", "match"));

    CHECK(run(lua, R"(
        local data = GetSpecialFiles('Replay')
        if data.extension ~= 'oscreplay' then error('extension ' .. data.extension) end
        if string.sub(data.directory, -1) ~= '/' then error('no trailing slash') end
        local names = data.files.Player
        if not names or names[1] ~= 'match' then error('Player files not listed') end
        -- the file picker's own fspec
        local fspec = data.directory .. 'Player/' .. names[1] .. '.' .. data.extension
        if fspec ~= GetSpecialFilePath('Player', 'match', 'Replay') then
            error(fspec .. ' vs ' .. GetSpecialFilePath('Player', 'match', 'Replay'))
        end
        local info = GetSpecialFileInfo('Player', 'match', 'Replay')
        if not info or not info.TimeStamp or not info.WriteTime.year then error('no info') end
        if GetSpecialFileInfo('Player', 'missing', 'Replay') then error('info for no file') end
        -- names are one path component: no climbing out of the folder
        if GetSpecialFilePath('..', 'x', 'Replay') ~= '' then error('.. accepted') end
        if GetSpecialFilePath('Player', 'a/b', 'Replay') ~= '' then error('/ accepted') end
        RemoveSpecialFile('Player', 'match', 'Replay')
        if GetSpecialFileInfo('Player', 'match', 'Replay') then error('not removed') end
    )")
              .empty());
    CHECK_FALSE(run(lua, "GetSpecialFiles('Screenshot')").empty()); // unknown type
}

TEST_CASE("LaunchReplaySession asks the game loop to play a replay", "[specialfiles][replay]") {
    TempDir dir;
    SpecialFiles files(dir.path);
    osc::lua::LuaState lua;
    osc::lua::register_special_file_bindings(lua, &files);
    const fs::path good = dir.path / "good.oscreplay";
    REQUIRE(osc::lua::write_replay_file(playable_replay(), good));
    osc::sim::Replay no_setup = playable_replay();
    no_setup.has_setup = false; // can't start a game
    const fs::path bare = dir.path / "bare.oscreplay";
    REQUIRE(osc::lua::write_replay_file(no_setup, bare));
    touch(dir.path / "junk.oscreplay");

    lua_State* L = lua.raw();
    for (const auto& refused : {dir.path / "junk.oscreplay", bare, dir.path / "none"}) {
        lua_pushstring(L, "__f");
        lua_pushstring(L, refused.string().c_str());
        lua_rawset(L, LUA_GLOBALSINDEX);
        CHECK(run(lua, "if LaunchReplaySession(__f) then error('launched') end").empty());
    }
    lua_pushstring(L, "__osc_launch_requested");
    lua_rawget(L, LUA_REGISTRYINDEX);
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);

    lua_pushstring(L, "__f");
    lua_pushstring(L, good.string().c_str());
    lua_rawset(L, LUA_GLOBALSINDEX);
    CHECK(run(lua, "if LaunchReplaySession(__f) ~= true then error('refused') end").empty());
    lua_pushstring(L, "__osc_launch_requested");
    lua_rawget(L, LUA_REGISTRYINDEX);
    CHECK(lua_toboolean(L, -1));
    lua_pushstring(L, "__osc_launch_scenario");
    lua_rawget(L, LUA_REGISTRYINDEX);
    CHECK(std::string(lua_tostring(L, -1)) == "/maps/SCMP_009/SCMP_009_scenario.lua");
    lua_pushstring(L, "__osc_launch_replay");
    lua_rawget(L, LUA_REGISTRYINDEX);
    CHECK(std::string(lua_tostring(L, -1)) == good.string());
    lua_pop(L, 3);
}

TEST_CASE("Replay files round-trip, and one without a setup is refused", "[specialfiles][replay]") {
    TempDir dir;
    const fs::path path = dir.path / "deep" / "folder" / "r.oscreplay"; // folders made
    REQUIRE(osc::lua::write_replay_file(playable_replay(), path));
    const auto back = osc::lua::read_replay_file(path);
    REQUIRE(back);
    CHECK(back->setup.scenario == "/maps/SCMP_009/SCMP_009_scenario.lua");
    CHECK(back->final_tick == 10);

    osc::sim::Replay no_setup;
    REQUIRE(osc::lua::write_replay_file(no_setup, dir.path / "old.oscreplay"));
    CHECK_FALSE(osc::lua::read_replay_file(dir.path / "old.oscreplay"));
}
