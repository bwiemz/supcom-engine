// The user state's globals no retail script calls, as Moho defines them
// (faf-re, the help texts from the Steam executable): GetBlueprint,
// GetResourceSharing, GetSpecialFolder, the key code conversions, the movie
// volume, the retired overlay toggles, the path debugger, PlayTutorialVO and
// EngineStartSplashScreens.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "core/game_state.hpp"
#include "log_capture.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/sim_sync.hpp"
#include "lua/special_files.hpp"
#include "lua/user_bindings.hpp"
#include "sim/army_brain.hpp"
#include "sim/bone_data.hpp"
#include "sim/manipulator.hpp"
#include "sim/pose.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/weapon.hpp"
#include "ui/key_codes.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lua.h>
}

#include <filesystem>
#include <memory>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

/// A sim of two armies with one unit, and a UI state bound to it in a game,
/// as the game sets them up.
struct UiGame {
    osc::lua::LuaState sim_lua;
    osc::sim::SimState sim{sim_lua.raw(), nullptr};
    osc::lua::LuaState ui;
    osc::blueprints::BlueprintStore store{ui.raw()};
    osc::ui::UIControlRegistry registry;
    osc::GameStateManager game;

    UiGame() {
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        auto unit = std::make_unique<osc::sim::Unit>();
        unit->set_army(0);
        unit->set_blueprint_id("uel0001");
        const auto id = sim.entity_registry().register_entity(std::move(unit));
        osc::lua::register_moho_bindings(ui, sim);
        osc::lua::register_ui_bindings(ui, registry);
        osc::lua::register_user_bindings(ui);
        lua_State* L = ui.raw();
        ui.set_blueprint_store(&store);
        REQUIRE(ui.do_string("return {BlueprintId = 'uel0001', Description = 'Commander'}").ok());
        store.register_blueprint(L, osc::blueprints::BlueprintType::Unit, lua_gettop(L));
        lua_pop(L, 1);
        osc::lua::push_units_for_ui(L, {id});
        lua_setglobal(L, "units");
        lua_pushstring(L, "__osc_game_state_mgr");
        lua_pushlightuserdata(L, &game);
        lua_rawset(L, LUA_REGISTRYINDEX);
        game.transition_to(osc::GameState::GAME, nullptr);
        osc::lua::set_focus_army(nullptr, L, 0);
    }
};

/// Run Lua; the error message, or empty.
std::string run(osc::lua::LuaState& lua, const std::string& code) {
    auto r = lua.do_string(code);
    return r ? std::string() : r.error().message;
}

/// A fresh folder, removed afterwards.
struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("osc-special-folder-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

} // namespace

TEST_CASE("The key code conversions are wx's own tables", "[ui][input]") {
    using osc::ui::windows_key_code_of_wx;
    using osc::ui::wx_key_code_of_windows;
    // Windows -> wx: the keys wx names, F1, the arrows, Delete, Alt (VK_MENU
    // is WXK_MENU), keypad * (WXK_NUMPAD_MULTIPLY) and the OEM keys by a
    // character each (= as +, ` as ~).
    CHECK(wx_key_code_of_windows(0x70) == 342);
    CHECK(wx_key_code_of_windows(0x25) == 316);
    CHECK(wx_key_code_of_windows(0x2E) == 127);
    CHECK(wx_key_code_of_windows(0x12) == 309);
    CHECK(wx_key_code_of_windows(0x6A) == 391);
    CHECK(wx_key_code_of_windows(0xBB) == '+');
    CHECK(wx_key_code_of_windows(0xC0) == '~');
    CHECK(wx_key_code_of_windows(0x0D) == 13);
    // A letter or a digit is left to its character, and keys wx has no code
    // for (the Windows keys, VK_SEPARATOR) are 0 too.
    CHECK(wx_key_code_of_windows('A') == 0);
    CHECK(wx_key_code_of_windows('7') == 0);
    CHECK(wx_key_code_of_windows(0x5B) == 0);
    CHECK(wx_key_code_of_windows(0x6C) == 0);

    // wx -> Windows: what it names, and anything else as it came.
    CHECK(windows_key_code_of_wx(342) == 0x70);
    CHECK(windows_key_code_of_wx(127) == 0x2E);
    CHECK(windows_key_code_of_wx(303) == 3);
    CHECK(windows_key_code_of_wx(391) == 0x6A);
    CHECK(windows_key_code_of_wx('A') == 'A');
    CHECK(windows_key_code_of_wx(13) == 13);
    CHECK(windows_key_code_of_wx(307) == 307); // WXK_ALT: no key of its own
    CHECK(windows_key_code_of_wx(336) == 336); // WXK_MULTIPLY: likewise

    // Every key wx gives a code of its own to comes back to itself.
    for (int vk = 0; vk < 256; ++vk) {
        const int wx = wx_key_code_of_windows(vk);
        if (wx >= 300 || wx == 127) {
            CHECK(windows_key_code_of_wx(wx) == vk);
        }
    }
}

TEST_CASE("KeycodeMauiToMSW and KeycodeMSWToMaui", "[ui][input]") {
    UiGame g;
    CHECK(run(g.ui, "assert(KeycodeMSWToMaui(112) == 342)\n"
                    "assert(KeycodeMSWToMaui(65) == 0)\n"
                    "assert(KeycodeMauiToMSW(342) == 112)\n"
                    "assert(KeycodeMauiToMSW(65) == 65)\n")
              .empty());
    CHECK(run(g.ui, "KeycodeMSWToMaui()").find("expected 1 args, but got 0") != std::string::npos);
    CHECK(run(g.ui, "KeycodeMauiToMSW('a')").find("attempt to use as integer a string value") !=
          std::string::npos);
}

TEST_CASE("GetBlueprint gives a UI unit's blueprint", "[ui]") {
    UiGame g;
    CHECK(run(g.ui, "assert(GetBlueprint(units[1]).BlueprintId == 'uel0001')\n"
                    "assert(GetBlueprint(units[1]) == units[1]:GetBlueprint())\n")
              .empty());
    CHECK(run(g.ui, "GetBlueprint()").find("blueprint = GetBlueprint(entity)") !=
          std::string::npos);
    CHECK(run(g.ui, "GetBlueprint(5)").find("Expected a game object") != std::string::npos);
}

TEST_CASE("GetResourceSharing asks of the focus army", "[ui][economy]") {
    UiGame g;
    // An army shares its overflow until its brain says otherwise.
    CHECK(run(g.ui, "assert(GetResourceSharing() == true)").empty());
    g.sim.get_army(0)->set_resource_sharing(false);
    CHECK(run(g.ui, "assert(GetResourceSharing() == false)").empty());
    g.sim.get_army(1)->set_resource_sharing(true);
    osc::lua::set_focus_army(nullptr, g.ui.raw(), 1);
    CHECK(run(g.ui, "assert(GetResourceSharing() == true)").empty());
    // An observer has none.
    osc::lua::set_focus_army(nullptr, g.ui.raw(), -1);
    CHECK(run(g.ui, "assert(GetResourceSharing() == false)").empty());
    CHECK(run(g.ui, "GetResourceSharing(1)").find("expected 0 args, but got 1") !=
          std::string::npos);

    // Outside a game Moho's warns as its GetEconomyTotals does, and answers
    // nothing.
    g.game.transition_to(osc::GameState::FRONT_END, nullptr);
    osc::test::LogCapture log;
    CHECK(run(g.ui, "local function count(...) return arg.n end\n"
                    "assert(count(GetResourceSharing()) == 0)")
              .empty());
    CHECK(log.count("Attempt to call GetEconomyTotals before world sessions exists.") == 1);
}

TEST_CASE("GetSpecialFolder answers a special file type's folder", "[specialfiles]") {
    TempDir dir;
    osc::lua::SpecialFiles files(dir.path);
    osc::lua::LuaState lua;
    osc::lua::register_special_file_bindings(lua, &files);
    const std::string root = dir.path.generic_string();
    // As GetSpecialFiles' directory: with its separator; the name with or
    // without Moho's SFT_ prefix, in any case.
    CHECK(run(lua, "assert(GetSpecialFolder('Replay') == '" + root +
                       "/replays/')\n"
                       "assert(GetSpecialFolder('Replay') == GetSpecialFiles('Replay').directory)\n"
                       "assert(GetSpecialFolder('SFT_SaveGame') == '" +
                       root +
                       "/savegames/')\n"
                       "assert(GetSpecialFolder('campaignsave') == '" +
                       root + "/savegames/')\n")
              .empty());
    // Moho's fourth type: its folder is made as it is asked for.
    CHECK_FALSE(fs::exists(dir.path / "screenshots"));
    CHECK(
        run(lua, "assert(GetSpecialFolder('Screenshot') == '" + root + "/screenshots/')").empty());
    CHECK(fs::is_directory(dir.path / "screenshots"));

    CHECK(run(lua, "GetSpecialFolder('Movie')").find("Invalid special file type Movie") !=
          std::string::npos);
    CHECK(run(lua, "GetSpecialFolder()").find("expected 1 args, but got 0") != std::string::npos);
    CHECK(run(lua, "GetSpecialFolder({})").find("attempt to use as string a table value") !=
          std::string::npos);
}

TEST_CASE("The movie volume is kept as Moho's movie manager keeps it", "[ui][movie]") {
    UiGame g;
    // Stored as DirectSound's attenuation, and answered so: not what was
    // given. Over 1 is full, under 0 silent.
    CHECK(run(g.ui, "assert(GetMovieVolume() == 0)\n"
                    "SetMovieVolume(0.5)\n"
                    "assert(GetMovieVolume() == -5000, GetMovieVolume())\n"
                    "SetMovieVolume(0.25)\n"
                    "assert(GetMovieVolume() == -7500, GetMovieVolume())\n"
                    "SetMovieVolume(2)\n"
                    "assert(GetMovieVolume() == 0)\n"
                    "SetMovieVolume(-1)\n"
                    "assert(GetMovieVolume() == -10000)\n"
                    "SetMovieVolume(1)\n")
              .empty());
    CHECK(run(g.ui, "SetMovieVolume('loud')").find("attempt to use as number a string value") !=
          std::string::npos);
    CHECK(run(g.ui, "GetMovieVolume(1)").find("expected 0 args, but got 1") != std::string::npos);
}

TEST_CASE("RenderOverlayMilitary and RenderOverlayIntel only warn", "[ui]") {
    UiGame g;
    osc::test::LogCapture log;
    CHECK(run(g.ui, "local function count(...) return arg.n end\n"
                    "assert(count(RenderOverlayMilitary(true)) == 0)\n"
                    "RenderOverlayIntel(false)\n")
              .empty());
    CHECK(log.count("RenderOverlayMilitary is deprecated") == 1);
    CHECK(log.count("RenderOverlayIntel is deprecated") == 1);
    CHECK(run(g.ui, "RenderOverlayIntel()").find("RenderOverlayIntel(bool)\n  expected 1 args") !=
          std::string::npos);
}

TEST_CASE("A path debugger is a script object until it is destroyed", "[ui]") {
    UiGame g;
    CHECK(run(g.ui, "local d = setmetatable({}, {__index = moho.PathDebugger_methods})\n"
                    "assert(_c_CreatePathDebugger(d, nil) == d)\n"
                    "assert(not IsDestroyed(d))\n"
                    "d:Destroy()\n"
                    "assert(IsDestroyed(d))\n"
                    "local ok, err = pcall(d.Destroy, d)\n"
                    "assert(not ok and string.find(err, 'Game object has been destroyed'), err)\n"
                    "ok, err = pcall(moho.PathDebugger_methods.Destroy, {})\n"
                    "assert(not ok and string.find(err, 'Expected a game object'), err)\n")
              .empty());
    CHECK(run(g.ui, "_c_CreatePathDebugger({})").find("expected 2 args, but got 1") !=
          std::string::npos);
}

TEST_CASE("PlayTutorialVO plays a cue, or answers nil", "[ui][audio]") {
    UiGame g;
    // No sound here: nothing plays.
    CHECK(run(g.ui, "assert(PlayTutorialVO(Sound{Bank = 'Tutorial', Cue = 'Intro'}) == nil)")
              .empty());
    CHECK(run(g.ui, "PlayTutorialVO(Sound{Bank = 'Tutorial', Cue = 'Intro'}, true)")
              .find("PlayTutorialVO(params)\n  expected 1 args, but got 2") != std::string::npos);
}

TEST_CASE("EngineStartSplashScreens starts the splash state", "[ui]") {
    UiGame g;
    const auto before = osc::ui_state();
    osc::test::LogCapture log; // uimain.lua isn't here: its import warns
    CHECK(run(g.ui, "EngineStartSplashScreens()").empty());
    CHECK(osc::ui_state() == osc::UIState::Splash);
    CHECK(run(g.ui, "EngineStartSplashScreens(1)").find("expected 0 args, but got 1") !=
          std::string::npos);
    osc::set_ui_state(before);
}

TEST_CASE("SoundIsPrepared answers as Moho's for a cue that never waits to prepare",
          "[ui][audio]") {
    UiGame g;
    // Moho's is true for nil or a handle without a cue, and for a cue not
    // XACT_CUESTATE_PREPARING; the engine reads a cue's waves as it
    // prepares it.
    CHECK(run(g.ui, "assert(SoundIsPrepared(nil) == true)\n"
                    "assert(SoundIsPrepared(12) == true)\n")
              .empty());
    CHECK(run(g.ui, "SoundIsPrepared()")
              .ends_with("bool = SoundIsPrepared(handle)\n  expected 1 args, but got 0"));
    CHECK(run(g.ui, "SoundIsPrepared(1, 2)")
              .ends_with("bool = SoundIsPrepared(handle)\n  expected 1 args, but got 2"));
}
