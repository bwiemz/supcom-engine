// --mod <uid or path>: the mods of a game the command line starts (--map).
// A mod on disk mounts at /mods/<name>, as retail's init file mounts the
// player's mods folder; the list resolves through the data's /lua/mods.lua
// (here a stand-in with retail's interface: AllMods, GetGameMods sorting by
// uid, GetDependencies).

#include <catch2/catch_test_macros.hpp>

#include "app/app_internal.hpp"
#include "core/preferences.hpp"
#include "lua/command_line_mods.hpp"
#include "lua/game_mods.hpp"
#include "lua/lua_state.hpp"
#include "support/memory_mount.hpp"
#include "support/temp_path.hpp"
#include "vfs/virtual_file_system.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <lua.h>
}

using osc::lua::ModArg;
using osc::test::MemoryMount;
namespace fs = std::filesystem;

namespace {

std::optional<osc::app::Options> parse(std::vector<std::string> args) {
    args.insert(args.begin(), "opensupcom");
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& a : args) argv.push_back(a.data());
    return osc::app::parse_options(static_cast<int>(argv.size()), argv.data(), {});
}

/// A folder with a mod folder (MyMod), a packed mod (Packed.ZIP) and a file
/// that is neither (notes.txt).
struct ModsOnDisk {
    osc::test::TempDir dir{"osc_mods_on_disk"};
    fs::path folder = dir.path() / "MyMod";
    fs::path packed = dir.path() / "Packed.ZIP";
    fs::path notes = dir.path() / "notes.txt";
    ModsOnDisk() {
        fs::create_directories(folder);
        std::ofstream(packed) << "PK";
        std::ofstream(notes) << "notes";
    }
};

// A stand-in for the data's import.lua and mods.lua: retail's interface,
// and what its functions do that the resolution relies on.
constexpr const char* kImport = R"(
function import(name)
    name = string.lower(name)
    local module = __modules[name]
    if module then return module end
    module = {}
    setmetatable(module, { __index = _G })
    __modules[name] = module
    doscript(name, module)
    return module
end
)";

constexpr const char* kMods = R"(
local cache
function AllMods()
    if not cache then
        cache = {}
        for _, file in ipairs(DiskFindFiles('/mods', '*mod_info.lua')) do
            local env = { location = Dirname(file), name = file, uid = file }
            if pcall(doscript, file, env) and env.enabled ~= false then
                env.location = Dirname(file)
                cache[env.uid] = env
            end
        end
    end
    return cache
end
-- Retail's: the selected mods that aren't UI-only, sorted by uid
function GetGameMods(selected)
    local uids = {}
    for uid, mod in pairs(AllMods()) do
        if selected[uid] and not mod.ui_only then table.insert(uids, uid) end
    end
    table.sort(uids)
    local list = {}
    for _, uid in ipairs(uids) do table.insert(list, cache[uid]) end
    return list
end
-- Retail's: what a mod requires (installed or missing), and conflicts with
function GetDependencies(uid)
    local all = AllMods()
    local found = { requires = {}, missing = {}, conflicts = {} }
    local function walk(id)
        for _, other in ipairs(all[id].conflicts or {}) do
            if all[other] then found.conflicts[other] = true end
        end
        for other, mod in pairs(all) do
            for _, against in ipairs(mod.conflicts or {}) do
                if against == id and other ~= id then found.conflicts[other] = true end
            end
        end
        for _, need in ipairs(all[id].requires or {}) do
            if not all[need] then
                found.missing[need] = true
            elseif not found.requires[need] then
                found.requires[need] = true
                walk(need)
            end
        end
    end
    walk(uid)
    for key, set in pairs(found) do
        if next(set) == nil then found[key] = nil end
    end
    return found
end
)";

/// A VFS with the stand-ins and these installed mods (each at /mods/<name>),
/// and one on disk at /mods/devmod (as --mod <path> mounts it).
struct ModsVfs {
    osc::vfs::VirtualFileSystem vfs;
    explicit ModsVfs(bool with_mods_lua = true) {
        auto dev = std::make_unique<MemoryMount>();
        dev->add("/mod_info.lua", "name = 'Dev'\nuid = 'DEV'\n");
        vfs.mount("/mods/devmod", std::move(dev));
        auto data = std::make_unique<MemoryMount>();
        data->add("/lua/system/import.lua", kImport);
        if (with_mods_lua) data->add("/lua/mods.lua", kMods);
        data->add("/mods/beta/mod_info.lua", "name = 'Beta'\nuid = 'B-GAME'\n");
        data->add("/mods/alpha/mod_info.lua", "name = 'Alpha'\nuid = 'A-GAME'\n");
        data->add("/mods/charlie/mod_info.lua",
                  "name = 'Charlie UI'\nuid = 'C-UI'\nui_only = true\n");
        data->add("/mods/delta/mod_info.lua",
                  "name = 'Delta'\nuid = 'D-NEEDS-A'\nrequires = { 'A-GAME' }\n");
        data->add("/mods/echo/mod_info.lua",
                  "name = 'Echo'\nuid = 'E-CONFLICTS-B'\nconflicts = { 'B-GAME' }\n");
        data->add("/mods/foxtrot/mod_info.lua",
                  "name = 'Foxtrot'\nuid = 'F-MISSING'\nrequires = { 'NOPE' }\n"
                  "requiresNames = { NOPE = 'Nope Mod' }\n");
        data->add("/mods/off/mod_info.lua", "name = 'Off'\nuid = 'OFF'\nenabled = false\n");
        data->add("/mods/empty/readme.txt", "no mod here");
        vfs.mount("/", std::move(data));
    }
};

ModArg uid(const std::string& given) {
    return ModArg{given, {}, {}};
}
ModArg on_disk(const std::string& mount_point) {
    return ModArg{"~/dev" + mount_point, fs::path("/dev") / mount_point, mount_point};
}

osc::Result<osc::lua::CommandLineMods> resolve(const std::vector<ModArg>& mods,
                                               bool with_mods_lua = true) {
    ModsVfs data(with_mods_lua);
    return osc::lua::resolve_command_line_mods(data.vfs, mods);
}

using Uids = std::vector<std::string>;

} // namespace

TEST_CASE("--mod's argument is a mod on disk when it names one, else a uid", "[app][mods]") {
    ModsOnDisk disk;
    const auto by_uid = osc::lua::mod_arg("74A9EAB2-E851-11DB-A1F1-F2C755D89593");
    CHECK(by_uid.path.empty());
    CHECK(by_uid.mount_point.empty());

    const auto folder = osc::lua::mod_arg(disk.folder.string());
    CHECK(folder.path.is_absolute());
    CHECK(fs::equivalent(folder.path, disk.folder));
    CHECK(folder.mount_point == "/mods/mymod"); // retail's init: the name, lower case
    CHECK(osc::lua::mod_arg(disk.folder.string() + "/").mount_point == "/mods/mymod");
    CHECK(osc::lua::mod_arg(disk.packed.string()).mount_point == "/mods/packed"); // less .zip

    // ...as retail's init file names a mods folder entry
    CHECK(osc::lua::mod_mount_point("a/Units.SCD") == "/mods/units");
    CHECK(osc::lua::mod_mount_point("a/x.zip.scd") == "/mods/x");
    CHECK(osc::lua::mod_mount_point("a/x.scd.zip") == "/mods/x.scd");
    CHECK(osc::lua::mod_mount_point("a/Some.Mod") == "/mods/some.mod");

    // A mod on disk is a folder or an archive, and two can't share a mount point
    CHECK(osc::lua::check_mod_args({folder, osc::lua::mod_arg(disk.packed.string()), by_uid})
              .empty());
    CHECK_FALSE(osc::lua::check_mod_args({osc::lua::mod_arg(disk.notes.string())}).empty());
    CHECK_FALSE(
        osc::lua::check_mod_args({folder, osc::lua::mod_arg(disk.folder.string() + "/")}).empty());
}

TEST_CASE("--mod picks the mods of a game --map starts; elsewhere it only mounts", "[app][mods]") {
    ModsOnDisk disk;
    const std::string map = "/maps/SCMP_009/SCMP_009_scenario.lua";
    const auto game = parse({"--map", map, "--mod", "B-GAME", "--mod", disk.folder.string()});
    REQUIRE(game);
    REQUIRE(game->mods.size() == 2); // in order
    CHECK(game->mods[0].given == "B-GAME");
    CHECK(game->mods[0].path.empty());
    CHECK(game->mods[1].mount_point == "/mods/mymod");
    CHECK(game->mods_picked);
    CHECK_FALSE(parse({"--map", map})->mods_picked);

    // The front end and its lobby: a mod on disk is mounted for the mod
    // manager to list, but a uid would pick nothing
    const auto front_end = parse({"--mod", disk.folder.string()});
    REQUIRE(front_end);
    CHECK(front_end->mods.size() == 1);
    CHECK_FALSE(front_end->mods_picked);
    CHECK_FALSE(parse({"--mod", "B-GAME"}));

    // A recorded game plays with its own mods: likewise
    const auto watched = parse({"--watch", "game.oscreplay", "--mod", disk.folder.string()});
    REQUIRE(watched);
    CHECK_FALSE(watched->mods_picked);
    CHECK_FALSE(parse({"--watch", "game.oscreplay", "--mod", "B-GAME"}));

    // Refused: no value, a file that isn't a mod, one mount point twice
    CHECK_FALSE(parse({"--map", map, "--mod"}));
    CHECK_FALSE(parse({"--map", map, "--mod", disk.notes.string()}));
    CHECK_FALSE(
        parse({"--map", map, "--mod", disk.folder.string(), "--mod", disk.folder.string()}));
}

TEST_CASE("--mod resolves through mods.lua: game mods sorted, UI-only ones apart", "[app][mods]") {
    // (b-game: a uid matches whatever its case)
    const auto mods = resolve({uid("b-game"), uid("C-UI"), uid("A-GAME")});
    REQUIRE(mods.ok());
    CHECK(mods.value().game_uids == Uids{"A-GAME", "B-GAME"}); // GetGameMods' order
    CHECK(mods.value().ui_uids == Uids{"C-UI"});
    CHECK(mods.value().required.empty());

    // The game's GameMods: mods.lua's own mod_info tables, which each Lua
    // state of the game reads back as its __active_mods
    osc::lua::LuaState game;
    osc::lua::set_active_mods(game.raw(), mods.value().game);
    REQUIRE(game.do_string("assert(table.getn(__active_mods) == 2)"
                           " assert(__active_mods[1].uid == 'A-GAME')"
                           " assert(__active_mods[1].location == '/mods/alpha')"
                           " assert(__active_mods[2].name == 'Beta')")
                .ok());

    // A mod on disk: the mod at its mount point
    const auto dev = resolve({on_disk("/mods/devmod"), uid("C-UI")});
    REQUIRE(dev.ok());
    CHECK(dev.value().game_uids == Uids{"DEV"});
    CHECK(dev.value().ui_uids == Uids{"C-UI"});

    // UI mods alone: an empty game list, which the game still records
    const auto ui_only = resolve({uid("C-UI")});
    REQUIRE(ui_only.ok());
    CHECK(ui_only.value().game_uids.empty());
    CHECK_FALSE(ui_only.value().game.empty());
}

TEST_CASE("--mod brings in what a mod requires, and refuses what the mod manager would",
          "[app][mods]") {
    const auto needs = resolve({uid("D-NEEDS-A")});
    REQUIRE(needs.ok());
    CHECK(needs.value().game_uids == Uids{"A-GAME", "D-NEEDS-A"});
    CHECK(needs.value().required == Uids{"A-GAME"});

    const auto missing = resolve({uid("F-MISSING")});
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message.find("Foxtrot (F-MISSING)") != std::string::npos);
    CHECK(missing.error().message.find("Nope Mod (NOPE)") != std::string::npos);

    const auto conflict = resolve({uid("B-GAME"), uid("E-CONFLICTS-B")});
    REQUIRE_FALSE(conflict.ok());
    CHECK(conflict.error().message.find("conflicts with") != std::string::npos);
}

TEST_CASE("An unknown --mod fails, naming the installed mods", "[app][mods]") {
    const auto unknown = resolve({uid("A-GAME"), uid("NO-SUCH-MOD")});
    REQUIRE_FALSE(unknown.ok());
    const std::string& why = unknown.error().message;
    CHECK(why.find("--mod NO-SUCH-MOD") != std::string::npos);
    for (const char* installed : {"Alpha (A-GAME)", "Beta (B-GAME)", "Charlie UI (C-UI)",
                                  "Dev (DEV)", "Foxtrot (F-MISSING)"})
        CHECK(why.find(installed) != std::string::npos);
    CHECK(why.find("A-GAME") < why.find("B-GAME")); // by uid
    CHECK(why.find("OFF") == std::string::npos);    // (disabled: not installed)

    const auto nothing_there = resolve({on_disk("/mods/empty")});
    REQUIRE_FALSE(nothing_there.ok());
    CHECK(nothing_there.error().message.find("no mod at /mods/empty") != std::string::npos);

    const auto no_mods_lua = resolve({uid("A-GAME")}, false);
    REQUIRE_FALSE(no_mods_lua.ok());
    CHECK(no_mods_lua.error().message.find("/lua/mods.lua") != std::string::npos);
}

TEST_CASE("A preference stood in for reads as the stand-in, is never saved, and comes back",
          "[preferences][mods]") {
    osc::test::TempDir dir("osc_prefs_stand_in");
    const fs::path file = dir.path() / "Game.prefs";
    osc::core::Preferences prefs;
    prefs.set_path(file);
    osc::lua::LuaState script;
    lua_State* L = script.raw();
    REQUIRE(script.do_string("players = { ['B-GAME'] = true }").ok());
    lua_getglobal(L, "players");
    prefs.set("active_mods", L, -1);
    lua_pop(L, 1);

    const auto read = [&](const char* check) {
        prefs.push("active_mods", L);
        lua_setglobal(L, "selected");
        return script.do_string(check).ok();
    };
    {
        const osc::core::Preferences::StandIn stand_in(prefs, "active_mods", {"C-UI", "DEV"});
        CHECK(read("assert(selected['C-UI'] == true and selected.DEV == true)"
                   " assert(selected['B-GAME'] == nil)"));
        CHECK_FALSE(prefs.save()); // written by nobody while it stands in
        CHECK_FALSE(fs::exists(file));
    }
    CHECK(read("assert(selected['B-GAME'] == true and selected['C-UI'] == nil)"));
    CHECK(prefs.save());

    // Standing in for nothing leaves nothing
    osc::core::Preferences empty;
    {
        const osc::core::Preferences::StandIn stand_in(empty, "active_mods", {"C-UI"});
        CHECK(empty.get_bool("active_mods.C-UI", false));
    }
    empty.push("active_mods", L);
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);
}
