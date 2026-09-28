// Networked lobby objects (M218a): what FA's lobby scripts see of Moho's
// CLobby, over the localhost loopback. InternalCreateLobby with "UDP"; the
// methods; the callbacks the frame's pump brings.

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "lua/net_lobby.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"

extern "C" {
#include <lua.h>
}

#include <chrono>
#include <string>
#include <thread>

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
        -- A second Alice is made unique
        b = NewLobby('Bob')
        b:JoinGame('localhost:' .. port, 'alice', nil)
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

    // A networked game can't be launched yet (M218c): the scripts hear so
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
