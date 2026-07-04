// Tests for the LanLobby host/client handshake driven over MuxTransport lobby
// channels on a LoopbackHub — no real sockets.

#include <catch2/catch_test_macros.hpp>

#include "lua/lan_lobby.hpp"
#include "sim/mux_transport.hpp"
#include "sim/net_transport.hpp"

#include <memory>

using namespace osc;
using osc::lua::LanLobby;
using osc::lua::LanSessionConfig;
using osc::sim::LoopbackHub;
using osc::sim::LoopbackTransport;
using osc::sim::MuxTransport;

TEST_CASE("LanLobby host/client reach launch with a synced config", "[lanlobby]") {
    LoopbackHub hub;
    int ih = hub.add_endpoint();
    int ic = hub.add_endpoint();
    MuxTransport hm(std::make_unique<LoopbackTransport>(hub, ih));
    MuxTransport cm(std::make_unique<LoopbackTransport>(hub, ic));

    LanLobby host(LanLobby::Role::Host, hm.lobby_channel());
    LanLobby client(LanLobby::Role::Client, cm.lobby_channel());
    host.set_host_config(LanSessionConfig{"/maps/x/x_scenario.lua", 0xABCDEF12u});

    for (int r = 0; r < 20 && !(host.launch_ready() && client.launch_ready()); ++r) {
        hm.pump();
        cm.pump();
        host.poll();
        client.poll();
        if (host.state() == LanLobby::State::Ready) host.request_launch();
    }

    REQUIRE(host.launch_ready());
    REQUIRE(client.launch_ready());
    CHECK(client.config().scenario == "/maps/x/x_scenario.lua");
    CHECK(client.config().seed == 0xABCDEF12u);
    // Client learned the host's config; host never sees its own READY as launch.
    CHECK(client.role() == LanLobby::Role::Client);
}

TEST_CASE("LanLobby client ignores a truncated config", "[lanlobby]") {
    LoopbackHub hub;
    int is = hub.add_endpoint();
    int ic = hub.add_endpoint();
    MuxTransport sm(std::make_unique<LoopbackTransport>(hub, is));
    MuxTransport cm(std::make_unique<LoopbackTransport>(hub, ic));
    LanLobby client(LanLobby::Role::Client, cm.lobby_channel());

    // A CONFIG message with type byte 1 but a truncated payload (only 2 bytes,
    // not the 8-byte seed + string). Sent through the lobby channel so it carries
    // the mux tag and reaches the client's lobby inbox.
    sm.lobby_channel().broadcast(std::vector<u8>{1, 0xAA, 0xBB});
    cm.pump();
    client.poll();
    CHECK(client.state() == LanLobby::State::Idle); // malformed → not applied
}
