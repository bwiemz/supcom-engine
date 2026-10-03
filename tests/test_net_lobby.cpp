// Networked lobby objects (M218a): what FA's lobby scripts see of Moho's
// CLobby, over the localhost loopback. InternalCreateLobby with "UDP"; the
// methods; the callbacks the frame's pump brings.

#include <catch2/catch_test_macros.hpp>

#include "lua/lobby_wire.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/net_lobby.hpp"
#include "lua/session_clients.hpp"
#include "sim/lan_discovery.hpp"
#include "sim/lobby_net.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lua.h>
}

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using osc::lua::LuaState;

namespace {

/// The scripts' side: a lobby class recording its callbacks, as retail's
/// lobbyComm.lua subclasses moho.lobby_methods.
const char* kTestComm = R"(
    TestComm = {
        Hosting = function(self) self.hosted = true end,
        ConnectionToHostEstablished = function(self, me, name, host)
            self.me, self.myname, self.host = me, name, host
        end,
        EstablishedPeers = function(self, uid, peers)
            self.established = self.established or {}
            self.established[uid] = peers
        end,
        DataReceived = function(self, data)
            self.got = self.got or {}
            table.insert(self.got, data)
        end,
        PeerDisconnected = function(self, name, id) self.left = {name, id} end,
        Ejected = function(self, reason) self.ejected = reason end,
        ConnectionFailed = function(self, reason) self.failed = reason end,
        LaunchFailed = function(self, reason) self.launch_failed = reason end,
    }
    setmetatable(TestComm, {__index = moho.lobby_methods})
    function NewLobby(name) return InternalCreateLobby(TestComm, 'UDP', 0, 8, name, nil, nil) end
    function Has(list, value)
        for _, v in list or {} do if v == value then return true end end
        return false
    end
)";

struct World {
    LuaState state;
    osc::sim::SimState sim{state.raw(), nullptr};
    osc::ui::UIControlRegistry ui_registry;
    World() {
        osc::lua::register_moho_bindings(state, sim);
        osc::lua::register_ui_bindings(state, ui_registry);
        REQUIRE(state.do_string(kTestComm));
    }
    ~World() { osc::lua::close_net_lobbies(state.raw()); }
    bool run(const std::string& code) {
        auto r = state.do_string(code);
        UNSCOPED_INFO(code << (r.ok() ? std::string() : "\n" + r.error().message));
        return r.ok();
    }
    /// Pump the lobbies until `condition` (Lua) holds, or three seconds pass.
    bool until(const std::string& condition) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            osc::lua::pump_net_lobbies(state.raw(), osc::lua::net_lobby_clock_ms());
            if (!run("__done = (" + condition + ") and true or false")) return false;
            lua_getglobal(state.raw(), "__done");
            const bool done = lua_toboolean(state.raw(), -1) != 0;
            lua_pop(state.raw(), 1);
            if (done) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        UNSCOPED_INFO("timed out waiting for: " << condition);
        return false;
    }
};

} // namespace

TEST_CASE("A LAN lobby hosts and is joined, as the lobby scripts see it (M218a)", "[lobby][lua]") {
    World w;
    REQUIRE(w.run("host = NewLobby('Host') host:HostGame()"));
    // Hosting comes with the next frame's pump
    REQUIRE(w.until("host.hosted"));
    REQUIRE(w.run(R"(
        assert(host:IsHost() and host:GetLocalPlayerID() == '0')
        assert(host:GetLocalPlayerName() == 'Host')
        port = host:GetLocalPort()
        assert(type(port) == 'number' and port > 0)
        a = NewLobby('Alice')
        assert(a:IsHost(), 'no host joined yet: Moho says it is one')
        a:JoinGame('127.0.0.1:' .. port, 'Alice', nil)
    )"));
    REQUIRE(w.until("a.me ~= nil"));
    REQUIRE(w.run(R"(
        assert(a.me == '1' and a.myname == 'Alice' and a.host == '0')
        assert(not a:IsHost() and a:GetLocalPlayerID() == '1')
        local peers = host:GetPeers()
        assert(table.getn(peers) == 1, 'the host sees one player')
        local p = peers[1]
        assert(p.name == 'Alice' and p.id == '1' and p.status == 'Established')
        assert(type(p.ping) == 'number' and type(p.quiet) == 'number')
        assert(Has(p.establishedPeers, '0'), 'Alice reaches the host')
        assert(host:GetPeer('1').name == 'Alice' and host:GetPeer('7') == nil)
        -- A second Alice is made unique (JoinGame's name is the host's)
        b = NewLobby('alice')
        b:JoinGame('localhost:' .. port, 'Host', nil)
    )"));
    REQUIRE(w.until("b.me ~= nil and a.established and a.established['2'] ~= nil"));
    REQUIRE(w.run(R"(
        assert(b.me == '2' and b.myname == 'alice1', b.myname)
        -- Everyone reaches everyone (through the host)
        assert(Has(a.established['2'], '0') and Has(a.established['2'], '1'))
        assert(Has(b.established['1'], '2'))
        assert(table.getn(b:GetPeers()) == 2)
        assert(host:MakeValidPlayerName('9', 'ALICE') == 'ALICE2')
        assert(host:MakeValidGameName(string.rep('x', 40)) == string.rep('x', 32))
        -- Ids no u32 holds are no one's (a peer's data can carry any string)
        assert(host:GetPeer('99999999999999999999') == nil and host:GetPeer('4294967296') == nil)
        assert(host:GetPeer(-1) == nil and host:GetPeer('1x') == nil)
        -- (a client lists the host, uid 0: a u32 wrapped round would find it)
        assert(a:GetPeer('0').name == 'Host' and a:GetPeer('4294967296') == nil)
        host:SendData('123456789012345', {Type = 'Nobody'})
        host:EjectPeer('99999999999999999999', 'nobody')
    )"));
}

TEST_CASE("LAN lobby data, ejection, departures and failures (M218a)", "[lobby][lua]") {
    World w;
    REQUIRE(w.run("host = NewLobby('Host') host:HostGame()"));
    REQUIRE(w.until("host.hosted"));
    REQUIRE(w.run(R"(
        port = host:GetLocalPort()
        a = NewLobby('Alice') a:JoinGame('127.0.0.1:' .. port, 'Alice', nil)
        b = NewLobby('Bob') b:JoinGame('127.0.0.1:' .. port, 'Bob', nil)
    )"));
    REQUIRE(w.until("a.me ~= nil and b.me ~= nil and table.getn(a:GetPeers()) == 2"));

    // Data to one player, and to all: tables whole, the sender named
    REQUIRE(w.run(R"(
        a:SendData(b.me, {Type = 'SetPlayerOption', Slot = 2, Options = {Faction = 3}})
        host:BroadcastData({Type = 'Chat', Text = 'hello'})
    )"));
    REQUIRE(w.until("b.got and table.getn(b.got) == 2 and a.got and table.getn(a.got) == 1"));
    REQUIRE(w.run(R"(
        local fromA, fromHost = b.got[1], b.got[2]
        if fromA.Type ~= 'SetPlayerOption' then fromA, fromHost = fromHost, fromA end
        assert(fromA.Slot == 2 and fromA.Options.Faction == 3)
        assert(fromA.SenderID == '1' and fromA.SenderName == 'Alice')
        assert(fromHost.Text == 'hello' and fromHost.SenderID == '0' and fromHost.SenderName == 'Host')
        assert(a.got[1].Text == 'hello')
        assert(host.got == nil, 'the host sent it: not to itself')
    )"));

    // Ejected: told why; the others lose them
    REQUIRE(w.run("host:EjectPeer(b.me, 'KickedByHost')"));
    REQUIRE(w.until("b.ejected ~= nil and a.left ~= nil"));
    REQUIRE(w.run(R"(
        assert(b.ejected == 'KickedByHost')
        assert(a.left[1] == 'Bob' and a.left[2] == '2')
        assert(table.getn(host:GetPeers()) == 1)
    )"));

    // A player leaves; then the host
    REQUIRE(w.run("a:Destroy()"));
    REQUIRE(w.until("host.left ~= nil and host.left[2] == '1'"));
    REQUIRE(w.run(R"(
        c = NewLobby('Carol') c:JoinGame('127.0.0.1:' .. port, 'Carol', nil)
    )"));
    REQUIRE(w.until("c.me ~= nil"));
    REQUIRE(w.run("host:Destroy()"));
    REQUIRE(w.until("c.failed ~= nil"));
    REQUIRE(w.run("assert(c.failed == 'HostLeft')"));

    // No address, no host: failed at the next pump
    REQUIRE(w.run("d = NewLobby('Dave') d:JoinGame('nowhere', 'Dave', nil)"));
    REQUIRE(w.until("d.failed ~= nil"));
    REQUIRE(w.run("assert(d.failed == 'HostLeft')"));

    // A config naming no scenario can't be launched: the scripts hear so
    REQUIRE(w.run(R"(
        e = NewLobby('Eve') e:HostGame() e:LaunchGame({})
        assert(e.launch_failed == '')
    )"));
}

TEST_CASE("The single-player lobby stays a loopback (M218a)", "[lobby][lua]") {
    World w;
    REQUIRE(w.run(R"(
        local sp = InternalCreateLobby(TestComm, 'None', 0, 8, 'Solo', nil, nil)
        sp:SendData('1', {Type = 'Echo'})
        assert(sp.got and sp.got[1].Type == 'Echo', 'SendData calls back at once')
        assert(sp:IsHost())
    )"));
}

TEST_CASE("The single-player lobby's launch tells its script, as a network lobby's does",
          "[lobby][lua]") {
    World w;
    REQUIRE(w.run(R"(
        local calls = {}
        LaunchSinglePlayerSession = function(config) table.insert(calls, config.tag) end
        local comm = setmetatable({GameLaunched = function(self)
            table.insert(calls, 'launched')
        end}, {__index = TestComm})
        local sp = InternalCreateLobby(comm, 'None', 0, 8, 'Solo', nil, nil)
        sp:LaunchGame({tag = 'session'})
        assert(calls[1] == 'session' and calls[2] == 'launched',
               'calls: ' .. tostring(calls[1]) .. ', ' .. tostring(calls[2]))
    )"));
}

TEST_CASE("The LAN's discovery finds a hosted lobby, as the LAN screen sees it (M218b)",
          "[lobby][lua][discovery]") {
    // Discovery asks the loopback, on a port no other test holds
    osc::u16 port = 0;
    {
        osc::sim::DiscoveryResponder probe;
        REQUIRE(probe.open(0));
        port = probe.port();
    }
    osc::lua::set_lan_discovery("127.0.0.1", port);
    World w;
    REQUIRE(w.run(R"(
        TestComm.GameConfigRequested = function(self)
            return {GameName = 'Test Game', HostedBy = 'Host', PlayerCount = 1,
                    Options = {ScenarioFile = '/maps/SCMP_009/SCMP_009_scenario.lua'}}
        end
        host = NewLobby('Host') host:HostGame()
        Finder = {
            GameFound = function(self, index, config) self.found = {index, config} end,
            GameUpdated = function(self, index, config) self.updated = {index, config} end,
            RemoveGame = function(self, index) self.removed = index end,
        }
        setmetatable(Finder, {__index = moho.discovery_service_methods})
        finder = InternalCreateDiscoveryService(Finder)
    )"));
    REQUIRE(w.until("finder.found ~= nil"));
    REQUIRE(w.run(R"(
        local index, config = finder.found[1], finder.found[2]
        assert(index == 0, 'the first game, at 0 (gameselect keeps games[index + 1])')
        assert(config.GameName == 'Test Game' and config.HostedBy == 'Host')
        assert(config.Options.ScenarioFile == '/maps/SCMP_009/SCMP_009_scenario.lua')
        assert(config.Address == '127.0.0.1:' .. host:GetLocalPort(), config.Address)
        assert(config.Hostname == '127.0.0.1' and config.Protocol == 'UDP')
        assert(finder:GetGameCount() == 1)
        -- Reset: each game goes, RemoveGame
        finder:Reset()
        assert(finder.removed == 0 and finder:GetGameCount() == 0)
        -- A second host on this machine can't answer (it still hosts)
        second = NewLobby('Second') second:HostGame()
        assert(second:GetLocalPort() > 0)
    )"));
    osc::lua::set_lan_discovery("255.255.255.255", osc::sim::kLanDiscoveryPort);
}

TEST_CASE("The Steam build's calls, with no Steam; ValidateIPAddress (M218b)", "[lobby][lua]") {
    World w;
    REQUIRE(w.run(R"(
        assert(IsSignedInToSteam() == false)
        InternalStartSteamDiscoveryService()
        RefreshSteamGames(false)
        assert(type(moho.steam_discovery_service_methods) == 'table')
        local S = {} setmetatable(S, {__index = moho.steam_discovery_service_methods})
        local steam = InternalCreateSteamDiscoveryService(S)
        assert(steam:GetGameCount() == 0)
        -- The LAN screen's direct connect
        assert(ValidateIPAddress('192.168.1.20:15000') == '192.168.1.20:15000')
        assert(ValidateIPAddress('localhost:6112') == '127.0.0.1:6112')
        assert(ValidateIPAddress('010.0.0.1:1') == '10.0.0.1:1')
        assert(ValidateIPAddress('256.1.1.1:15000') == nil)
        assert(ValidateIPAddress('1.2.3:15000') == nil)
        assert(ValidateIPAddress('1.2.3.4') == nil)
        assert(ValidateIPAddress('1.2.3.4:0') == nil and ValidateIPAddress('1.2.3.4:70000') == nil)
        assert(ValidateIPAddress('1.2.3.4x:5') == nil and ValidateIPAddress('') == nil)
        assert(ValidateIPAddress(' 1.2.3.4:5') == nil and ValidateIPAddress('+1.2.3.4:5') == nil)
        assert(ValidateIPAddress('1.-2.3.4:5') == nil)
        assert(ValidateIPAddress('1.2.3.4.5:6') == nil and ValidateIPAddress('1.2.3.4.:6') == nil)
        assert(ValidateIPAddress('1..2.3:4') == nil)
        assert(ValidateIPAddress('1.2.3.99999999999:5') == nil)
    )"));
}

namespace {

/// The launch's scenarios, as doscript would read them: one for two
/// players, and one with no standard configuration.
const char* kScenarios = R"(
    Scenarios = {
        ['/maps/two/two_scenario.lua'] = {
            Configurations = {standard = {teams = {{name = 'ffa', armies = {'ARMY_1', 'ARMY_2', 'ARMY_3'}}}}},
        },
        ['/maps/bare/bare_scenario.lua'] = {Configurations = {}},
    }
    function doscript(path, env)
        if path == '/lua/dataInit.lua' then return end
        if not Scenarios[path] then error('no such file: ' .. path) end
        env.ScenarioInfo = Scenarios[path]
    end
    -- As retail's lobby.lua: the launched game takes over, the lobby goes
    TestComm.GameLaunched = function(self) self.launched = true self:Destroy() end
    function Config(scenario, players, observers)
        return {GameOptions = {ScenarioFile = scenario}, PlayerOptions = players,
                Observers = observers or {}}
    end
)";

/// Whatever a launch left in the process-wide multiplayer state goes.
struct MpGuard {
    MpGuard() { osc::lua::mp_teardown(); }
    ~MpGuard() { osc::lua::mp_teardown(); }
    MpGuard(const MpGuard&) = delete;
    MpGuard& operator=(const MpGuard&) = delete;
};

/// The lobby transport a launch left, taken out of the multiplayer state.
std::unique_ptr<osc::sim::INetTransport> take_launched() {
    auto& mp = osc::lua::mp_net_state();
    auto t = std::move(mp.lobby_transport);
    osc::lua::mp_teardown();
    return t;
}

} // namespace

TEST_CASE("LaunchGame starts the game over the lobby's connections (M218c)", "[lobby][lua]") {
    MpGuard guard;
    World w;
    REQUIRE(w.run(kScenarios));
    REQUIRE(w.run("host = NewLobby('Host') host:HostGame()"));
    REQUIRE(w.until("host.hosted"));
    REQUIRE(w.run(R"(
        port = host:GetLocalPort()
        a = NewLobby('Alice') a:JoinGame('127.0.0.1:' .. port, 'Alice', nil)
        b = NewLobby('Bob') b:JoinGame('127.0.0.1:' .. port, 'Bob', nil)
    )"));
    REQUIRE(w.until("a.me == '1' and b.me == '2'"));

    // What can't launch: the lobby stays
    REQUIRE(w.run(R"(
        two = '/maps/two/two_scenario.lua'
        local both = {{Human = true, OwnerID = '0'}, {Human = true, OwnerID = '1'}}
        a:LaunchGame(Config('/maps/none_scenario.lua', both))
        assert(a.launch_failed == '', 'no such scenario')
        a:LaunchGame(Config('/maps/bare/bare_scenario.lua', both))
        assert(a.launch_failed == 'NoConfig', a.launch_failed)
        local four = {{Human = true, OwnerID = '0'}, {Human = true, OwnerID = '1'},
                      {Human = false}, {Human = false}}
        a:LaunchGame(Config(two, four))
        assert(a.launch_failed == 'StartSpots', a.launch_failed)
        a.launch_failed = nil
        a:LaunchGame(Config(two, {{Human = true, OwnerID = '0'}}))
        assert(a.launch_failed == '', 'Alice is neither playing nor watching')
        assert(not a.launched and a:GetLocalPlayerID() == '1')
    )"));
    CHECK_FALSE(osc::lua::mp_net_state().transport_ready);

    // The game: Alice in slot 1, an AI in 2, the host in 3 (built out of
    // order), Bob watching
    REQUIRE(w.run(R"(
        function GameInfo()
            local players = {}
            players[3] = {Human = true, OwnerID = '0', PlayerName = 'Host'}
            players[2] = {Human = false, AIPersonality = 'adaptive'}
            players[1] = {Human = true, OwnerID = 1, PlayerName = 'Alice'}
            return Config(two, players, {{OwnerID = '2', PlayerName = 'Bob'}})
        end
        -- From a thread, as retail's keepalive and launch countdown call
        -- them: the networked lobby still (not the single-player loopback's)
        a.launch_failed = nil
        local co = coroutine.create(function()
            local hostPeer = a:GetPeer('0')
            assert(hostPeer and hostPeer.name == 'Host' and type(hostPeer.quiet) == 'number')
            a:LaunchGame(GameInfo())
        end)
        local ok, err = coroutine.resume(co)
        assert(ok, err)
        assert(a.launched and a.launch_failed == nil)
    )"));
    // Sources: Alice's (army 0), the host's (army 2), then Bob's (none)
    auto& mp = osc::lua::mp_net_state();
    REQUIRE(mp.transport_ready);
    REQUIRE(mp.lobby_transport);
    CHECK_FALSE(mp.lobby_game->net().hosting());
    CHECK(mp.local_source == 0);
    CHECK(mp.all_sources == std::vector<osc::u32>{0, 1, 2});
    CHECK(mp.source_armies == std::vector<osc::i32>{0, 2, -1});
    CHECK(mp.local_army() == 0);
    // Seeded by the host's time, which the welcome brought
    const auto* alice_net = static_cast<osc::sim::LobbyGameTransport*>(mp.lobby_transport.get());
    CHECK(mp.seed == alice_net->net().hosted_time());
    CHECK(mp.seed != 0);
    // The session starts as single-player's does, with the frame loop
    lua_pushstring(w.state.raw(), "__osc_launch_requested");
    lua_rawget(w.state.raw(), LUA_REGISTRYINDEX);
    CHECK(lua_toboolean(w.state.raw(), -1) != 0);
    lua_pop(w.state.raw(), 1);
    auto alice = take_launched();

    REQUIRE(w.run("b:LaunchGame(GameInfo()) assert(b.launched)"));
    CHECK(osc::lua::mp_net_state().local_source == 2);
    CHECK(osc::lua::mp_net_state().local_army() == -1); // watching
    auto bob = take_launched();

    REQUIRE(w.run("host:LaunchGame(GameInfo()) assert(host.launched)"));
    CHECK(osc::lua::mp_net_state().lobby_game->net().hosting());
    CHECK(osc::lua::mp_net_state().local_source == 1);
    CHECK(osc::lua::mp_net_state().local_army() == 2);
    CHECK(osc::lua::mp_net_state().seed == alice_net->net().hosted_time());
    auto host = take_launched();

    // Slots 2, 5 and 7 taken, the rest empty (random spawn leaves gaps):
    // the armies are the slots taken, in order, as Moho packs them, however
    // the table iterates (these keys sit in its hash part)
    {
        auto transport = std::move(host);
        REQUIRE(w.run(R"(
            gap = NewLobby('Gap') gap:HostGame()
            Scenarios['/maps/eight/eight_scenario.lua'] = {Configurations = {standard = {teams = {
                {name = 'FFA', armies = {'ARMY_1', 'ARMY_2', 'ARMY_3', 'ARMY_4', 'ARMY_5',
                                         'ARMY_6', 'ARMY_7', 'ARMY_8'}}}}}}
        )"));
        REQUIRE(w.until("gap.hosted"));
        REQUIRE(w.run(R"(
            local players = {}
            players[7] = {Human = false, AIPersonality = 'adaptive'}
            players[5] = {Human = false, AIPersonality = 'adaptive'}
            players[2] = {Human = true, OwnerID = '0', PlayerName = 'Gap'}
            local order = {}
            for slot in players do table.insert(order, slot) end
            __osc_gap_order = table.concat(order, ',')
            gap:LaunchGame(Config('/maps/eight/eight_scenario.lua', players))
            assert(gap.launched)
        )"));
        lua_getglobal(w.state.raw(), "__osc_gap_order");
        UNSCOPED_INFO("the table iterates its slots as " << lua_tostring(w.state.raw(), -1));
        lua_pop(w.state.raw(), 1);
        CHECK(osc::lua::mp_net_state().source_armies == std::vector<osc::i32>{0});
        take_launched();
        host = std::move(transport);
    }
    // The lobbies are gone (GameLaunched destroyed them); their pump is quiet
    osc::lua::pump_net_lobbies(w.state.raw(), osc::lua::net_lobby_clock_ms());

    // The connections carry the game: Alice's frame reaches both others
    alice->broadcast({1, 2, 3});
    std::vector<std::vector<osc::u8>> at_host;
    std::vector<std::vector<osc::u8>> at_bob;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && (at_host.empty() || at_bob.empty())) {
        for (auto& f : host->receive()) at_host.push_back(f);
        for (auto& f : bob->receive()) at_bob.push_back(f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(at_host == std::vector<std::vector<osc::u8>>{{1, 2, 3}});
    CHECK(at_bob == std::vector<std::vector<osc::u8>>{{1, 2, 3}});
}

namespace {

/// gamemain.lua's ReceiveChat, as the game's interface has it loaded:
/// what came, by sender.
const char* kGameMain = R"(
    __modules = __modules or {}
    got = {}
    __modules['/lua/ui/game/gamemain.lua'] = {
        ReceiveChat = function(sender, data) table.insert(got, {sender = sender, data = data}) end,
    }
)";

} // namespace

TEST_CASE("A single-player game has one client, and chat comes back (M218d)", "[lobby][lua]") {
    MpGuard guard;
    World w;
    REQUIRE(w.run(kGameMain));
    REQUIRE(w.run(R"(
        assert(SessionIsMultiplayer() == false)
        local clients = GetSessionClients()
        assert(table.getn(clients) == 1, 'one client')
        local me = clients[1]
        assert(me['local'] and me.connected and me.uid == '0', 'the local one')
        assert(me.ping == 0 and me.quiet == 0 and me.maxSP == 50)
        assert(me.authorizedCommandSources[1] == 1 and table.getn(me.ejectedBy) == 0)
        assert(type(me.name) == 'string' and me.name ~= '')
        __name = me.name

        -- To everyone (the one client), and to client 1: next frame's
        SessionSendChatMessage({Chat = true, text = 'all'})
        SessionSendChatMessage(1, {Chat = true, text = 'one'})
        SessionSendChatMessage({}, {Chat = true, text = 'nobody'})
        assert(table.getn(got) == 0, 'not at once')
        -- Moho's refusals
        local ok, err = pcall(SessionSendChatMessage, 2, {text = 'x'})
        assert(not ok and string.find(err, 'Invalid client index'), err)
        ok, err = pcall(SessionSendChatMessage, {1e20, 0/0}, {text = 'x'})
        assert(not ok and string.find(err, 'Invalid client index'), err)
        ok, err = pcall(SessionSendChatMessage, {'one'}, {text = 'x'})
        assert(not ok and string.find(err, 'Invalid value'), err)
        ok, err = pcall(SessionSendChatMessage, {text = string.rep('x', 2000)})
        assert(not ok and string.find(err, 'Message too long'), err)
    )"));
    osc::lua::pump_session_chat(w.state.raw());
    REQUIRE(w.run(R"(
        assert(table.getn(got) == 2, table.getn(got))
        assert(got[1].sender == __name and got[1].data.text == 'all')
        assert(got[2].data.text == 'one')
        -- Sent as the game ends: never delivered to the next
        SessionSendChatMessage({Chat = true, text = 'gg'})
    )"));
    osc::lua::reset_session_chat();
    osc::lua::pump_session_chat(w.state.raw());
    REQUIRE(w.run("assert(table.getn(got) == 2, 'chat from the game before came')"));
}

TEST_CASE("A lobby's game has its clients, and chat crosses to them (M218d)", "[lobby][lua]") {
    MpGuard guard;
    World w;
    REQUIRE(w.run(kScenarios));
    REQUIRE(w.run("host = NewLobby('Host') host:HostGame()"));
    REQUIRE(w.until("host.hosted"));
    REQUIRE(w.run(R"(
        a = NewLobby('Alice') a:JoinGame('127.0.0.1:' .. host:GetLocalPort(), 'Alice', nil)
    )"));
    REQUIRE(w.until("a.me == '1'"));
    REQUIRE(w.run(R"(
        function GameInfo()
            return Config('/maps/two/two_scenario.lua', {
                {Human = true, OwnerID = '0', PlayerName = 'Host'},
                {Human = true, OwnerID = '1', PlayerName = 'Alice'},
            })
        end
        a:LaunchGame(GameInfo())
    )"));
    auto alice = take_launched();
    REQUIRE(w.run("host:LaunchGame(GameInfo())"));
    REQUIRE(w.run(kGameMain));

    // The host's view: two clients, by source
    REQUIRE(w.run(R"(
        assert(SessionIsMultiplayer() == true)
        local clients = GetSessionClients()
        assert(table.getn(clients) == 2)
        assert(clients[1].name == 'Host' and clients[1].uid == '0' and clients[1]['local'])
        assert(clients[2].name == 'Alice' and clients[2].uid == '1' and not clients[2]['local'])
        assert(clients[2].connected and clients[2].authorizedCommandSources[1] == 2)
        assert(type(clients[2].ping) == 'number' and type(clients[2].quiet) == 'number')
        local names = SessionGetCommandSourceNames()
        assert(names[1] == 'Host' and names[2] == 'Alice')
        -- A network game pauses through the lockstep (M218f): with no
        -- session attached here, the ask goes nowhere
        SessionRequestPause()
        -- To Alice alone, and to everyone (the host too)
        SessionSendChatMessage({2}, {Chat = true, text = 'to alice'})
        SessionSendChatMessage({Chat = true, text = 'to all'})
    )"));
    // Alice has both, over the lobby's connections
    auto* alice_game = static_cast<osc::sim::LobbyGameTransport*>(alice.get());
    std::vector<std::string> texts;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && texts.size() < 2) {
        alice->receive();
        for (auto& e : alice_game->take_data()) {
            CHECK(e.uid == 0);
            REQUIRE(osc::lua::push_lobby_value(w.state.raw(), e.payload));
            lua_pushstring(w.state.raw(), "text");
            lua_gettable(w.state.raw(), -2);
            texts.emplace_back(lua_tostring(w.state.raw(), -1));
            lua_pop(w.state.raw(), 2);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(texts == std::vector<std::string>{"to alice", "to all"});

    // Alice's to the host: its ReceiveChat, from her
    lua_newtable(w.state.raw());
    lua_pushstring(w.state.raw(), "text");
    lua_pushstring(w.state.raw(), "hi host");
    lua_rawset(w.state.raw(), -3);
    alice_game->send_data(0, osc::lua::encode_lobby_value(w.state.raw(), -1));
    lua_pop(w.state.raw(), 1);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool heard = false;
    while (std::chrono::steady_clock::now() < until && !heard) {
        osc::lua::mp_net_state().lobby_game->receive();
        osc::lua::pump_session_chat(w.state.raw());
        REQUIRE(w.run("__heard = false for _, g in got do if g.sender == 'Alice' then __heard = "
                      "true end end"));
        lua_getglobal(w.state.raw(), "__heard");
        heard = lua_toboolean(w.state.raw(), -1) != 0;
        lua_pop(w.state.raw(), 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(heard);
    // The host's own 'to all' came back to it, and Alice's to it
    REQUIRE(w.run(R"(
        local texts = {}
        for _, g in got do texts[g.data.text] = g.sender end
        assert(texts['to all'] == 'Host', 'its own, from itself')
        assert(texts['hi host'] == 'Alice')
        assert(texts['to alice'] == nil, 'not for the host')
    )"));
}

TEST_CASE("Ejecting a client: Moho's refusals, and the lockstep drops it (M218e)", "[lobby][lua]") {
    MpGuard guard;
    World w;
    REQUIRE(w.run(kScenarios));
    // Single-player: the one client is this one
    REQUIRE(w.run(R"(
        local ok, err = pcall(EjectSessionClient, 1)
        assert(not ok and string.find(err, "Can't eject ourselves"), err)
        ok, err = pcall(EjectSessionClient, 2)
        assert(not ok and string.find(err, 'Invalid client index 2, must be >= 1 and <= 1'), err)
        ok, err = pcall(EjectSessionClient, 0/0)
        assert(not ok and string.find(err, 'Invalid client index'), err)
        -- The disconnect dialog is a network game's
        __dialog = 0
        __modules = __modules or {}
        __modules['/lua/ui/uimain.lua'] = {UpdateDisconnectDialog = function() __dialog = __dialog + 1 end}
    )"));
    osc::lua::pump_disconnect_dialog(w.state.raw());
    REQUIRE(w.run("assert(__dialog == 0, 'not in single-player')"));

    // A lobby's game: the host ejects Alice
    REQUIRE(w.run("host = NewLobby('Host') host:HostGame()"));
    REQUIRE(w.until("host.hosted"));
    REQUIRE(w.run(R"(
        a = NewLobby('Alice') a:JoinGame('127.0.0.1:' .. host:GetLocalPort(), 'Alice', nil)
    )"));
    REQUIRE(w.until("a.me == '1'"));
    REQUIRE(w.run(R"(
        function GameInfo()
            return Config('/maps/two/two_scenario.lua', {
                {Human = true, OwnerID = '0', PlayerName = 'Host'},
                {Human = true, OwnerID = '1', PlayerName = 'Alice'},
            })
        end
        a:LaunchGame(GameInfo())
    )"));
    auto alice = take_launched();
    CHECK(osc::lua::eject_session_uid(1) == osc::lua::EjectByUid::NoGame);
    REQUIRE(w.run("host:LaunchGame(GameInfo())"));
    REQUIRE(osc::lua::mp_attach_session(w.sim));
    // A matchmaking client's EjectPlayer names the client by uid (M220b):
    // none has 99; the host's own (0) is this one's
    CHECK(osc::lua::eject_session_uid(99) == osc::lua::EjectByUid::NoSuchClient);
    CHECK(osc::lua::eject_session_uid(0) == osc::lua::EjectByUid::Local);
    CHECK(osc::lua::mp_net_state().session->ejectors(1).empty());
    osc::lua::pump_disconnect_dialog(w.state.raw());
    REQUIRE(w.run(R"(
        assert(__dialog == 1, 'a network game updates it each frame')
        local ok, err = pcall(EjectSessionClient, 1)
        assert(not ok and string.find(err, "Can't eject ourselves"), err)
        local clients = GetSessionClients()
        assert(clients[2].connected and table.getn(clients[2].ejectedBy) == 0)
        EjectSessionClient(2)
        clients = GetSessionClients()
        -- Dropped (the host its only survivor), by the host
        assert(not clients[2].connected, 'Alice is gone')
        assert(table.getn(clients[2].ejectedBy) == 1 and clients[2].ejectedBy[1] == 1, 'by the host')
        assert(table.getn(clients[2].authorizedCommandSources) == 0, 'her source goes')
        assert(clients[2].quiet == -1 and clients[2].ping == 0, 'no connection, as Moho closes it')
        assert(clients[1].connected and table.getn(clients[1].ejectedBy) == 0, 'the host stays')
    )"));
    CHECK(osc::lua::mp_net_state().session->has_dropped(1));

    // The game over, the next is single-player: nothing of this one stays
    osc::lua::mp_teardown();
    osc::lua::pump_disconnect_dialog(w.state.raw());
    REQUIRE(w.run(R"(
        assert(SessionIsMultiplayer() == false, 'single-player again')
        local clients = GetSessionClients()
        assert(table.getn(clients) == 1 and clients[1]['local'], 'one client, the local one')
        local ok, err = pcall(EjectSessionClient, 2)
        assert(not ok and string.find(err, 'must be >= 1 and <= 1'), err)
        assert(__dialog == 1, "the dialog is a network game's")
    )"));
}

TEST_CASE("A lobby's game has the speed its lobby set; players change it, observers can't (M218i)",
          "[lobby][lua]") {
    MpGuard guard;
    World w;
    REQUIRE(w.run(kScenarios));
    REQUIRE(w.run("host = NewLobby('Host') host:HostGame()"));
    REQUIRE(w.until("host.hosted"));
    REQUIRE(w.run(R"(
        a = NewLobby('Alice') a:JoinGame('127.0.0.1:' .. host:GetLocalPort(), 'Alice', nil)
    )"));
    REQUIRE(w.until("a.me == '1'"));
    REQUIRE(w.run(R"(
        function GameInfo()
            return Config('/maps/two/two_scenario.lua', {
                {Human = true, OwnerID = '0', PlayerName = 'Host'},
                {Human = true, OwnerID = '1', PlayerName = 'Alice'},
            })
        end
        a:LaunchGame(GameInfo())
    )"));
    auto alice = take_launched();
    REQUIRE(w.run(R"(
        host:LaunchGame(GameInfo())
        __heard = {}
        __modules = __modules or {}
        __modules['/lua/ui/uimain.lua'] = {NoteGameSpeedChanged = function(client, speed)
            table.insert(__heard, client .. ':' .. speed)
        end}
        -- The lobby's GameSpeed, as the game's options have it
        rawset(_G, 'ScenarioInfo', {Options = {GameSpeed = 'fast'}})
    )"));

    // 'fast': fixed at +4
    REQUIRE(osc::lua::mp_attach_session(w.sim));
    REQUIRE(w.run(R"(
        assert(GetGameSpeed() == 4, GetGameSpeed())
        SetGameSpeed(7)
        assert(GetGameSpeed() == 4, 'fixed')
    )"));
    osc::lua::pump_speed_changes(w.state.raw());
    REQUIRE(w.run("assert(table.getn(__heard) == 0, 'nothing changed')"));

    // 'adjustable': a player's is taken, and heard as Moho's client
    // manager tells it
    REQUIRE(w.run("ScenarioInfo.Options.GameSpeed = 'Adjustable'"));
    REQUIRE(osc::lua::mp_attach_session(w.sim));
    REQUIRE(w.run(R"(
        assert(GetGameSpeed() == 0, GetGameSpeed())
        SetGameSpeed(3)
        assert(GetGameSpeed() == 3, GetGameSpeed())
    )"));
    osc::lua::pump_speed_changes(w.state.raw());
    REQUIRE(w.run("assert(table.getn(__heard) == 1 and __heard[1] == '1:3', __heard[1])"));

    // An observer's isn't (Moho's WLD_CanAdjustSimRate)
    osc::lua::mp_net_state().source_armies[osc::lua::mp_net_state().local_source] = -1;
    REQUIRE(w.run(R"(
        SetGameSpeed(8)
        assert(GetGameSpeed() == 3, 'an observer asked')
    )"));
}

TEST_CASE("A matchmaking client's uids are the lobby's players' (M220b)", "[lobby][lua]") {
    World w;
    REQUIRE(w.run(R"(
        function NewLobbyAs(name, uid)
            return InternalCreateLobby(TestComm, 'UDP', 0, 8, name, uid, nil)
        end
        host = NewLobbyAs('Host', '10') host:HostGame()
    )"));
    REQUIRE(w.until("host.hosted"));
    REQUIRE(w.run(R"(
        assert(host:GetLocalPlayerID() == '10', host:GetLocalPlayerID())
        port = host:GetLocalPort()
        -- JoinGame names the host (a matchmaking client knows it); the
        -- player keeps its own name and uid
        a = NewLobbyAs('Alice', '42')
        a:JoinGame('127.0.0.1:' .. port, 'Host', '10')
        -- One whose uid is taken is refused; one with none is numbered,
        -- past those taken
        c = NewLobbyAs('Carol', '42')
        c:JoinGame('127.0.0.1:' .. port, 'Host', '10')
        d = NewLobby('Dave')
        d:JoinGame('127.0.0.1:' .. port, 'Host', '10')
    )"));
    REQUIRE(w.until("a.me ~= nil and d.me ~= nil and c.ejected ~= nil"));
    REQUIRE(w.run(R"(
        assert(a.me == '42' and a.myname == 'Alice' and a.host == '10', a.me)
        assert(c.ejected == 'UidTaken', c.ejected)
        assert(d.me == '1', d.me)
        assert(host:GetPeer('42').name == 'Alice')
        -- Data finds the host by its uid, and a player by theirs
        a:SendData('10', {Type = 'ToHost'})
        host:SendData('42', {Type = 'ToAlice'})
    )"));
    REQUIRE(w.until("host.got ~= nil and a.got ~= nil"));
    REQUIRE(w.run(R"(
        assert(host.got[1].Type == 'ToHost' and host.got[1].SenderID == '42')
        assert(a.got[1].Type == 'ToAlice' and a.got[1].SenderID == '10')
        -- The client says Alice has gone: the host lets her go
        host:DisconnectFromPeer('42')
        a:DisconnectFromPeer('10') -- a client's: nothing
    )"));
    REQUIRE(w.until("a.ejected ~= nil and host.left ~= nil"));
    REQUIRE(w.run(R"(
        assert(a.ejected == 'Disconnected', a.ejected)
        assert(host.left[2] == '42' and host:GetPeer('42') == nil)
        assert(d.ejected == nil and host:GetPeer('1').name == 'Dave')
    )"));
}


TEST_CASE("The command line, as Moho's scripts read it (M220b)", "[lobby][lua]") {
    // FAF's client passes the lobby's options this way (/players, /team...)
    std::vector<std::string> argv{"/gpgnet", "127.0.0.1:1", "/players", "2", "/TEAM", "3", "/last"};
    World w;
    lua_pushstring(w.state.raw(), "__osc_cmdline_args");
    lua_pushlightuserdata(w.state.raw(), &argv);
    lua_rawset(w.state.raw(), LUA_REGISTRYINDEX);
    REQUIRE(w.run(R"(
        assert(HasCommandLineArg('/players') and HasCommandLineArg('/Players'), 'case aside')
        assert(not HasCommandLineArg('/nope'))
        local p = GetCommandLineArg('/players', 1)
        assert(type(p) == 'table' and table.getn(p) == 1 and p[1] == '2')
        assert(GetCommandLineArg('/team', 1)[1] == '3', 'case aside')
        -- Not there, or without as many after it: false
        assert(GetCommandLineArg('/last', 1) == false)
        assert(GetCommandLineArg('/nope', 0) == false)
        assert(table.getn(GetCommandLineArg('/last', 0)) == 0)
        -- What follows it, whatever it is
        local g = GetCommandLineArg('/gpgnet', 2)
        assert(g[1] == '127.0.0.1:1' and g[2] == '/players')
    )"));
}

TEST_CASE("A joiner is established with the host after what it sent on joining (M220b)",
          "[lobby][lua]") {
    // Retail's auto-lobby: the joiner sends its player on connecting, and the
    // host launches on EstablishedPeers once everyone's player is in. So the
    // host must hear the joiner established after its data, as Moho's does.
    World w;
    REQUIRE(w.run(R"(
        Order = {
            Hosting = function(self) self.hosted = true end,
            ConnectionToHostEstablished = function(self, me, name, host)
                self.me = me
                self:SendData(host, {Type = 'AddPlayer'})
            end,
            DataReceived = function(self, data)
                self.log = self.log or {}
                table.insert(self.log, 'data:' .. data.Type .. ':' .. data.SenderID)
            end,
            EstablishedPeers = function(self, uid, peers)
                self.log = self.log or {}
                table.insert(self.log, 'established:' .. uid)
            end,
        }
        setmetatable(Order, {__index = moho.lobby_methods})
        host = InternalCreateLobby(Order, 'UDP', 0, 8, 'Host', '10', nil)
        host:HostGame()
    )"));
    REQUIRE(w.until("host.hosted"));
    REQUIRE(w.run(R"(
        a = InternalCreateLobby(Order, 'UDP', 0, 8, 'Alice', '42', nil)
        a:JoinGame('127.0.0.1:' .. host:GetLocalPort(), 'Host', '10')
    )"));
    REQUIRE(w.until("host.log and table.getn(host.log) >= 3"));
    REQUIRE(w.run(R"(
        local data, last = nil, nil
        for i, entry in host.log do
            if entry == 'data:AddPlayer:42' then data = i end
            if entry == 'established:42' then last = i end
        end
        assert(data and last and data < last, table.concat(host.log, ' '))
    )"));
}

TEST_CASE("A matchmaking client's EjectPlayer ejects the client with that uid (M220b)",
          "[lobby][lua]") {
    MpGuard guard;
    World w;
    // A lobby's game of two: this client (uid 10) and one with uid 42
    osc::lua::mp_begin_lobby_game(
        std::make_unique<osc::sim::LobbyGameTransport>(
            std::make_unique<osc::sim::LobbyNet>("Host", 1), osc::lua::net_lobby_clock_ms),
        0, {0, 1}, {{10, "Host"}, {42, "Alice"}}, 1);
    REQUIRE(osc::lua::mp_attach_session(w.sim));
    CHECK(osc::lua::eject_session_uid(10) == osc::lua::EjectByUid::Local);
    CHECK(osc::lua::eject_session_uid(7) == osc::lua::EjectByUid::NoSuchClient);
    CHECK(osc::lua::eject_session_uid(42) == osc::lua::EjectByUid::Ejected);
    // This client reports Alice's source dropped, as EjectSessionClient does
    CHECK(osc::lua::mp_net_state().session->ejectors(1) == std::vector<osc::u32>{0});
}
