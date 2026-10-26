// /hostgame and /joingame open a lobby at once, as Moho's CScApp does (faf-re CScApp.cpp): the
// arguments after the option (CFG_GetArgOption, any case), the map through FindMapScenario, and
// uimain.lua's StartHostLobbyUI or StartJoinLobbyUI in the lobby's UI state.

#include <catch2/catch_test_macros.hpp>

#include "app/app_internal.hpp"
#include "core/game_state.hpp"
#include "lua/lua_state.hpp"

#include <optional>
#include <string>
#include <vector>

namespace {

std::optional<osc::app::Options> parse(std::vector<std::string> args) {
    args.insert(args.begin(), "opensupcom");
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& a : args) argv.push_back(a.data());
    return osc::app::parse_options(static_cast<int>(argv.size()), argv.data(), {});
}

} // namespace

// (A name that starts with '/' is an option to Catch2 on Windows, where ctest
// passes the name as its filter.)
TEST_CASE("The /hostgame and /joingame options take the arguments after them, in any case",
          "[app][lobby]") {
    const auto host =
        parse({"/windowed", "/HostGame", "UDP", "15000", "Ann", "Ann's game", "SCMP_009"});
    REQUIRE(host);
    CHECK(host->host_game ==
          std::vector<std::string>{"UDP", "15000", "Ann", "Ann's game", "SCMP_009"});
    CHECK(host->join_game.empty());

    const auto join = parse({"/joingame", "UDP", "192.168.1.5:15000", "Bob"});
    REQUIRE(join);
    CHECK(join->join_game == std::vector<std::string>{"UDP", "192.168.1.5:15000", "Bob"});

    // Too few after it: as if it weren't given (CFG_GetArgOption looks on)
    const auto short_of = parse({"/hostgame", "UDP", "15000"});
    REQUIRE(short_of);
    CHECK(short_of->host_game.empty());
}

TEST_CASE("A map's name is its scenario under /maps; a path stays as it is", "[app][lobby]") {
    CHECK(osc::core::find_map_scenario("SCMP_009") == "/maps/SCMP_009/SCMP_009_scenario.lua");
    CHECK(osc::core::find_map_scenario("/maps/X1MP_001/X1MP_001_scenario.lua") ==
          "/maps/X1MP_001/X1MP_001_scenario.lua");
    CHECK(osc::core::find_map_scenario("maps\\a\\a_scenario.lua") == "maps\\a\\a_scenario.lua");
    CHECK(osc::core::find_map_scenario("").empty());
}

TEST_CASE("The lobby's entry points hear their arguments in the lobby's UI state", "[app][lobby]") {
    const osc::UIState before = osc::ui_state();
    osc::lua::LuaState state;
    REQUIRE(state
                .do_string("function StartHostLobbyUI(protocol, port, player, game, map)"
                           "  host = {protocol, port, player, game, map, type(port)} end"
                           " function StartJoinLobbyUI(protocol, address, player)"
                           "  join = {protocol, address, player} end")
                .ok());
    osc::core::call_start_host_lobby_ui(state.raw(), "UDP", 15000, "Ann", "Ann's game",
                                        "/maps/SCMP_009/SCMP_009_scenario.lua");
    CHECK(osc::ui_state() == osc::UIState::Lobby);
    CHECK(state
              .do_string("assert(host[1] == 'UDP' and host[2] == 15000 and host[3] == 'Ann'"
                         " and host[4] == \"Ann's game\""
                         " and host[5] == '/maps/SCMP_009/SCMP_009_scenario.lua'"
                         " and host[6] == 'number')")
              .ok());
    osc::core::call_start_join_lobby_ui(state.raw(), "UDP", "192.168.1.5:15000", "Bob");
    CHECK(state
              .do_string("assert(join[1] == 'UDP' and join[2] == '192.168.1.5:15000'"
                         " and join[3] == 'Bob')")
              .ok());
    osc::set_ui_state(before);
}
