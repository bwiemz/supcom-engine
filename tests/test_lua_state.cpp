#include <catch2/catch_test_macros.hpp>
#include "core/front_end_data.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"
#include "vfs/mount_point.hpp"
#include "vfs/virtual_file_system.hpp"
#include "support/memory_mount.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <stdexcept>

using namespace osc::lua;

namespace {

using osc::test::MemoryMount;

bool global_bool(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    const bool value = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return value;
}

std::string global_string(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    std::string value = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return value;
}

double global_number(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    double value = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : 0.0;
    lua_pop(L, 1);
    return value;
}

} // namespace

TEST_CASE("LuaState creation and basic execution", "[lua]") {
    LuaState state;
    REQUIRE(state.raw() != nullptr);

    auto result = state.do_string("x = 1 + 2");
    REQUIRE(result.ok());
}

TEST_CASE("LuaState register and call C function", "[lua]") {
    LuaState state;

    static int called = 0;
    state.register_function("test_fn", [](lua_State* L) -> int {
        called++;
        lua_pushnumber(L, 42);
        return 1;
    });

    called = 0;
    auto result = state.do_string("result = test_fn()");
    REQUIRE(result.ok());
    CHECK(called == 1);
}

namespace {
int g_probes_destroyed = 0;
struct Probe {
    std::string payload = "heap allocated so a skipped destructor leaks";
    ~Probe() { ++g_probes_destroyed; }
};
} // namespace

TEST_CASE("lua_error runs destructors of C++ objects it unwinds", "[lua]") {
    // Bindings routinely hold std::string / RAII locks when they raise a Lua
    // error. With longjmp (GCC/Clang, Lua built as C) those destructors are
    // skipped; Lua built as C++ throws an exception instead.
    LuaState state;
    state.register_function("raise_with_probe", [](lua_State* L) -> int {
        Probe probe;
        return luaL_error(L, "boom from %s", "binding");
    });
    g_probes_destroyed = 0;
    auto result = state.do_string(R"(
        ok, msg = pcall(raise_with_probe)
    )");
    REQUIRE(result.ok());
    CHECK(g_probes_destroyed == 1);
    lua_getglobal(state.raw(), "ok");
    CHECK(lua_toboolean(state.raw(), -1) == 0);
    lua_pop(state.raw(), 1);
    lua_getglobal(state.raw(), "msg");
    CHECK(std::string(lua_tostring(state.raw(), -1)).find("boom from binding") !=
          std::string::npos);
    lua_pop(state.raw(), 1);
}

TEST_CASE("C++ exceptions escaping a binding become Lua errors", "[lua]") {
    LuaState state;
    state.register_function("throw_std", [](lua_State*) -> int {
        throw std::runtime_error("binding blew up");
    });
    auto result = state.do_string(R"(
        ok, msg = pcall(throw_std)
        after = 1  -- the state is still usable
    )");
    REQUIRE(result.ok());
    lua_getglobal(state.raw(), "ok");
    CHECK(lua_toboolean(state.raw(), -1) == 0);
    lua_pop(state.raw(), 1);
    lua_getglobal(state.raw(), "msg");
    CHECK(std::string(lua_tostring(state.raw(), -1)) == "binding blew up");
    lua_pop(state.raw(), 1);
    lua_getglobal(state.raw(), "after");
    CHECK(lua_tonumber(state.raw(), -1) == 1);
    lua_pop(state.raw(), 1);
}

TEST_CASE("LuaState != operator (LuaPlus patch)", "[lua]") {
    LuaState state;

    auto result = state.do_string(R"(
        x = 5
        if x != 3 then
            result = true
        else
            result = false
        end
    )");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "result");
    CHECK(lua_toboolean(state.raw(), -1) == 1);
    lua_pop(state.raw(), 1);
}

TEST_CASE("LuaState # line comments (LuaPlus patch)", "[lua]") {
    LuaState state;

    // Retail FA's Lua uses '#' comments both on their own line and trailing
    // code; '#' inside strings must stay literal.
    auto result = state.do_string(R"(
        # full-line comment
            # indented comment
        x = 1 # trailing comment
        s = "a#b" # comment after a string containing '#'
        y = x + 1#no space before the comment
    )");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "y");
    CHECK(lua_tonumber(state.raw(), -1) == 2);
    lua_pop(state.raw(), 1);
    lua_getglobal(state.raw(), "s");
    CHECK(std::string(lua_tostring(state.raw(), -1)) == "a#b");
    lua_pop(state.raw(), 1);
}

TEST_CASE("LuaState continue statement (LuaPlus patch)", "[lua]") {
    LuaState state;

    auto result = state.do_string(R"(
        sum = 0
        for i = 1, 10 do
            if i == 5 then continue end
            sum = sum + i
        end
    )");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "sum");
    // Sum of 1..10 minus 5 = 55 - 5 = 50
    CHECK(lua_tonumber(state.raw(), -1) == 50);
    lua_pop(state.raw(), 1);
}

TEST_CASE("LuaState table size hints (LuaPlus patch)", "[lua]") {
    LuaState state;

    // Retail MultiEvent.lua: `{&1&1 n=0}`. Hints are discarded; the items that
    // follow must still be parsed, with or without a separator.
    auto result = state.do_string(R"(
        a = {&1&1 n=0}
        b = {&1&4 10, 20; x = 3}
        c = {&2&0}
        d = {&1&1, 7}
        total = a.n + b[1] + b[2] + b.x + table.getn(c) + d[1]
    )");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "total");
    CHECK(lua_tonumber(state.raw(), -1) == 40);
    lua_pop(state.raw(), 1);
}

TEST_CASE("LuaState hex literals (LuaPlus patch)", "[lua]") {
    LuaState state;

    auto result = state.do_string("x = 0xFF");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "x");
    CHECK(lua_tonumber(state.raw(), -1) == 255);
    lua_pop(state.raw(), 1);
}

TEST_CASE("LuaState per-type metatables (LuaPlus patch)", "[lua]") {
    LuaState state;

    // getmetatable(nil) should return a table
    auto result = state.do_string(R"(
        local mt = getmetatable(nil)
        mt_type = type(mt)
    )");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "mt_type");
    CHECK(std::string(lua_tostring(state.raw(), -1)) == "table");
    lua_pop(state.raw(), 1);

    // Full metacleanup pattern from config.lua
    result = state.do_string(R"(
        local function metacleanup(obj)
            local name = type(obj)
            local mmt = {
                __newindex = function(_, key, _)
                    error(("Attempt to set attribute '%s' on %s"):format(tostring(key), name), 2)
                end,
            }
            setmetatable(getmetatable(obj), mmt)
        end
        metacleanup(nil)
        metacleanup(false)
        metacleanup(0)
        metacleanup('')
        cleanup_ok = true
    )");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "cleanup_ok");
    CHECK(lua_toboolean(state.raw(), -1) == 1);
    lua_pop(state.raw(), 1);
}

TEST_CASE("LuaState table iteration without pairs()", "[lua]") {
    LuaState state;

    auto result = state.do_string(R"(
        t = {a = 1, b = 2, c = 3}
        sum = 0
        for k, v in t do
            sum = sum + v
        end
    )");
    REQUIRE(result.ok());

    lua_getglobal(state.raw(), "sum");
    CHECK(lua_tonumber(state.raw(), -1) == 6);
    lua_pop(state.raw(), 1);
}

TEST_CASE("MapPreview constructor returns controls for lobby preview swaps", "[lua][ui]") {
    LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    register_moho_bindings(state, sim);
    register_ui_bindings(state, ui_registry);

    auto result = state.do_string(R"(
        parent = {}
        InternalCreateGroup(parent)

        first_preview = MapPreview(parent)
        first_is_table = type(first_preview) == 'table'
        first_has_control = first_preview and first_preview._c_object != nil
        first_can_clear = first_preview and type(first_preview.ClearTexture) == 'function'
        if first_preview then
            first_preview:ClearTexture()
        end

        active_preview = MapPreview(parent)
        swapped_without_nil = active_preview != nil
        swapped_to_new_control = active_preview != first_preview
        second_has_control = active_preview and active_preview._c_object != nil
        second_can_clear = active_preview and type(active_preview.ClearTexture) == 'function'
    )");
    if (!result.ok()) {
        UNSCOPED_INFO(result.error().message);
    }
    REQUIRE(result.ok());

    lua_State* L = state.raw();
    CHECK(global_bool(L, "first_is_table"));
    CHECK(global_bool(L, "first_has_control"));
    CHECK(global_bool(L, "first_can_clear"));
    CHECK(global_bool(L, "swapped_without_nil"));
    CHECK(global_bool(L, "swapped_to_new_control"));
    CHECK(global_bool(L, "second_has_control"));
    CHECK(global_bool(L, "second_can_clear"));
    CHECK(ui_registry.count() == 4);
}

TEST_CASE("MapPreview texture state survives lobby clear and replacement", "[lua][ui]") {
    LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;
    osc::vfs::VirtualFileSystem vfs;
    auto mount = std::make_unique<MemoryMount>();
    mount->add("/maps/alpha/lobby/preview.dds", {'D', 'D', 'S', ' ', 'a'});
    mount->add("/maps/bravo/lobby/preview.dds", {'D', 'D', 'S', ' ', 'b'});
    vfs.mount("/", std::move(mount));
    state.set_vfs(&vfs);

    register_moho_bindings(state, sim);
    register_ui_bindings(state, ui_registry);

    auto result = state.do_string(R"(
        parent = {}
        InternalCreateGroup(parent)

        first = MapPreview(parent)
        alpha_loaded = first:SetTexture('/maps/alpha/lobby/preview.dds')
        first:ClearTexture()

        second = MapPreview(parent)
        bravo_loaded = second:SetTexture('/maps/bravo/lobby/preview.dds')
    )");
    if (!result.ok()) {
        UNSCOPED_INFO(result.error().message);
    }
    REQUIRE(result.ok());

    lua_State* L = state.raw();
    CHECK(global_bool(L, "alpha_loaded"));
    CHECK(global_bool(L, "bravo_loaded"));
    REQUIRE(ui_registry.count() == 4);
    CHECK(ui_registry.all()[2]->texture_path().empty());
    CHECK(ui_registry.all()[3]->texture_path() == "/maps/bravo/lobby/preview.dds");
}

TEST_CASE("Lobby peer methods maintain single-process peer state", "[lua][ui]") {
    LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    register_moho_bindings(state, sim);
    register_ui_bindings(state, ui_registry);

    auto result = state.do_string(R"(
        LobbyClass = {}
        for k, v in moho.lobby_methods do LobbyClass[k] = v end
        lobby = InternalCreateLobby(LobbyClass, 'UDP', 6112, 16, 'Host')

        connected = lobby:ConnectToPeer('2', 'RemotePlayer', 6113)
        peers_after_connect = lobby:GetPeers()
        peer_after_connect = lobby:GetPeer('2')
        peer_name_after_connect = peer_after_connect and peer_after_connect.Name
        peer_quiet_after_connect = peer_after_connect and peer_after_connect.quiet

        disconnected = lobby:DisconnectFromPeer('2')
        peer_after_disconnect = lobby:GetPeer('2')

        lobby:ConnectToPeer('3', 'EjectedPlayer', 6114)
        ejected = lobby:EjectPeer('3')
        peer_after_eject = lobby:GetPeer('3')
    )");
    if (!result.ok()) {
        UNSCOPED_INFO(result.error().message);
    }
    REQUIRE(result.ok());

    lua_State* L = state.raw();
    CHECK(global_bool(L, "connected"));
    CHECK(global_string(L, "peer_name_after_connect") == "RemotePlayer");
    CHECK(global_number(L, "peer_quiet_after_connect") == 0.0);
    CHECK(global_bool(L, "disconnected"));
    CHECK(global_bool(L, "ejected"));

    lua_getglobal(L, "peer_after_disconnect");
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);
    lua_getglobal(L, "peer_after_eject");
    CHECK(lua_isnil(L, -1));
    lua_pop(L, 1);
}

TEST_CASE("Lobby LaunchGame preserves lobby config for skirmish launch", "[lua][ui]") {
    LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;
    osc::FrontEndData front_end_data;

    register_moho_bindings(state, sim);
    register_ui_bindings(state, ui_registry);

    lua_State* L = state.raw();
    lua_pushstring(L, "__osc_front_end_data");
    lua_pushlightuserdata(L, &front_end_data);
    lua_rawset(L, LUA_REGISTRYINDEX);

    auto result = state.do_string(R"(
        LobbyClass = {}
        for k, v in moho.lobby_methods do LobbyClass[k] = v end
        lobby = InternalCreateLobby(LobbyClass, 'UDP', 6112, 16, 'Host')

        lobby:LaunchGame({
            GameOptions = {
                ScenarioFile = '/maps/SCMP_009/SCMP_009_scenario.lua',
                Victory = 'sandbox',
                UnitCap = 750,
            },
            PlayerOptions = {
                [1] = {
                    Human = true,
                    PlayerName = 'Player',
                    Faction = 1,
                    Team = 1,
                    StartSpot = 1,
                },
                [2] = {
                    Human = false,
                    PlayerName = 'AI: Turtle',
                    AIPersonality = 'turtle',
                    Faction = 2,
                    Team = 2,
                    StartSpot = 2,
                },
            },
        })

        launch_requested = false
        saved_scenario = nil
        normalized_scenario = nil
        saved_ai_personality = nil

        saved_config = GetFrontEndData('sessionConfig')
        if saved_config then
            normalized_scenario = saved_config.ScenarioFile
            if saved_config.PlayerOptions and saved_config.PlayerOptions[2] then
                saved_ai_personality = saved_config.PlayerOptions[2].AIPersonality
            end
        end
    )");
    if (!result.ok()) {
        UNSCOPED_INFO(result.error().message);
    }
    REQUIRE(result.ok());

    lua_pushstring(L, "__osc_launch_requested");
    lua_rawget(L, LUA_REGISTRYINDEX);
    CHECK(lua_toboolean(L, -1) != 0);
    lua_pop(L, 1);

    lua_pushstring(L, "__osc_launch_scenario");
    lua_rawget(L, LUA_REGISTRYINDEX);
    std::string launch_scenario = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);

    CHECK(launch_scenario == "/maps/SCMP_009/SCMP_009_scenario.lua");
    CHECK(global_string(L, "normalized_scenario") == "/maps/SCMP_009/SCMP_009_scenario.lua");
    CHECK(global_string(L, "saved_ai_personality") == "turtle");

    front_end_data.clear(L);
}

TEST_CASE("Discovery service tracks advertised lobby entries", "[lua][ui]") {
    LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    register_moho_bindings(state, sim);
    register_ui_bindings(state, ui_registry);

    auto result = state.do_string(R"(
        DiscoveryClass = {}
        for k, v in moho.discovery_service_methods do DiscoveryClass[k] = v end
        discovery = InternalCreateDiscoveryService(DiscoveryClass)

        discovery:AddGame({Name = 'Setons Test', Host = 'Player', Map = '/maps/setons.scmap'})
        discovery:AddGame({Name = 'Theta Test', Host = 'AI', Map = '/maps/theta.scmap'})
        game_count_before_reset = discovery:GetGameCount()
        first_game = discovery:GetGame(1)
        first_game_name = first_game and first_game.Name

        discovery:Reset()
        game_count_after_reset = discovery:GetGameCount()
    )");
    if (!result.ok()) {
        UNSCOPED_INFO(result.error().message);
    }
    REQUIRE(result.ok());

    lua_State* L = state.raw();
    lua_getglobal(L, "game_count_before_reset");
    CHECK(lua_tonumber(L, -1) == 2);
    lua_pop(L, 1);
    CHECK(global_string(L, "first_game_name") == "Setons Test");
    lua_getglobal(L, "game_count_after_reset");
    CHECK(lua_tonumber(L, -1) == 0);
    lua_pop(L, 1);
}

TEST_CASE("Front-end options persist skin layout and key binding overrides", "[lua][ui]") {
    LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    register_moho_bindings(state, sim);
    register_ui_bindings(state, ui_registry);

    auto result = state.do_string(R"(
        UIUtil.SetCurrentSkin('cybran')
        UIUtil.SetLayoutPreference('right')
        SetKeyBinding('attack', 'Q')

        selected_skin = UIUtil.GetCurrentSkin()
        selected_layout = UIUtil.GetLayoutPreference()
        key_bindings = GetKeyBindings()
        attack_binding = key_bindings.attack
    )");
    if (!result.ok()) {
        UNSCOPED_INFO(result.error().message);
    }
    REQUIRE(result.ok());

    lua_State* L = state.raw();
    CHECK(global_string(L, "selected_skin") == "cybran");
    CHECK(global_string(L, "selected_layout") == "right");
    CHECK(global_string(L, "attack_binding") == "Q");
}

TEST_CASE("Chat handlers receive lobby messages and history", "[lua][ui]") {
    LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    register_moho_bindings(state, sim);
    register_ui_bindings(state, ui_registry);

    auto result = state.do_string(R"(
        received_count = 0
        last_text = nil
        RegisterChatFunc(function(msg)
            received_count = received_count + 1
            last_text = msg.text
        end, 'test')

        SessionSendChatMessage({'1'}, {text = 'hello lobby', from = 'Player'})
        SendSystemMessage('system ready')

        history = GetChatHistory()
        history_count = table.getn(history)
        first_history_text = history[1] and history[1].text
        second_history_text = history[2] and history[2].text
    )");
    if (!result.ok()) {
        UNSCOPED_INFO(result.error().message);
    }
    REQUIRE(result.ok());

    lua_State* L = state.raw();
    lua_getglobal(L, "received_count");
    CHECK(lua_tonumber(L, -1) == 2);
    lua_pop(L, 1);
    CHECK(global_string(L, "last_text") == "system ready");
    lua_getglobal(L, "history_count");
    CHECK(lua_tonumber(L, -1) == 2);
    lua_pop(L, 1);
    CHECK(global_string(L, "first_history_text") == "hello lobby");
    CHECK(global_string(L, "second_history_text") == "system ready");
}

TEST_CASE("ReturnToLobby clears chat history before the next lobby", "[lua][ui]") {
    LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    register_moho_bindings(state, sim);
    register_ui_bindings(state, ui_registry);

    auto result = state.do_string(R"(
        SessionSendChatMessage({'1'}, {text = 'previous lobby', from = 'Player'})
        SendSystemMessage('previous system')

        ReturnToLobby()
        after_return_count = table.getn(GetChatHistory())

        SessionSendChatMessage({'1'}, {text = 'next lobby', from = 'Player'})
        next_history = GetChatHistory()
        next_count = table.getn(next_history)
        next_first_text = next_history[1] and next_history[1].text
        next_second_text = next_history[2] and next_history[2].text
    )");
    if (!result.ok()) {
        UNSCOPED_INFO(result.error().message);
    }
    REQUIRE(result.ok());

    lua_State* L = state.raw();
    lua_getglobal(L, "after_return_count");
    CHECK(lua_tonumber(L, -1) == 0);
    lua_pop(L, 1);
    lua_getglobal(L, "next_count");
    CHECK(lua_tonumber(L, -1) == 1);
    lua_pop(L, 1);
    CHECK(global_string(L, "next_first_text") == "next lobby");
    CHECK(global_string(L, "next_second_text").empty());
}

TEST_CASE("Fresh front-end boot clears stale chat history", "[lua][ui]") {
    LuaState state;
    osc::sim::SimState sim(state.raw(), nullptr);
    osc::ui::UIControlRegistry ui_registry;

    register_moho_bindings(state, sim);
    register_ui_bindings(state, ui_registry);

    auto seed_result = state.do_string(R"(
        SessionSendChatMessage({'1'}, {text = 'old boot message', from = 'Player'})
        SendSystemMessage('old boot system')
    )");
    if (!seed_result.ok()) {
        UNSCOPED_INFO(seed_result.error().message);
    }
    REQUIRE(seed_result.ok());

    register_ui_bindings(state, ui_registry);

    auto result = state.do_string(R"(
        boot_history = GetChatHistory()
        boot_history_count = table.getn(boot_history)
        boot_first_text = boot_history[1] and boot_history[1].text
    )");
    if (!result.ok()) {
        UNSCOPED_INFO(result.error().message);
    }
    REQUIRE(result.ok());

    lua_State* L = state.raw();
    lua_getglobal(L, "boot_history_count");
    CHECK(lua_tonumber(L, -1) == 0);
    lua_pop(L, 1);
    CHECK(global_string(L, "boot_first_text").empty());
}
