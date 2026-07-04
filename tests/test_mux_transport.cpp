// Tests for MuxTransport — two logical channels over one transport.

#include <catch2/catch_test_macros.hpp>

#include "sim/mux_transport.hpp"
#include "sim/net_transport.hpp"

#include <memory>
#include <vector>

using namespace osc;
using osc::sim::LoopbackHub;
using osc::sim::LoopbackTransport;
using osc::sim::MuxTransport;

TEST_CASE("MuxTransport routes messages to the correct channel", "[mux]") {
    LoopbackHub hub;
    int ida = hub.add_endpoint();
    int idb = hub.add_endpoint();
    MuxTransport a(std::make_unique<LoopbackTransport>(hub, ida));
    MuxTransport b(std::make_unique<LoopbackTransport>(hub, idb));

    a.lobby_channel().broadcast(std::vector<u8>{1, 2, 3});
    a.game_channel().broadcast(std::vector<u8>{9, 9});
    b.pump();

    auto lob = b.lobby_channel().receive();
    auto gam = b.game_channel().receive();
    REQUIRE(lob.size() == 1);
    CHECK(lob[0] == std::vector<u8>{1, 2, 3});
    REQUIRE(gam.size() == 1);
    CHECK(gam[0] == std::vector<u8>{9, 9});

    // Inboxes are drained by receive().
    CHECK(b.lobby_channel().receive().empty());
    CHECK(b.game_channel().receive().empty());
}

TEST_CASE("MuxTransport keeps channels isolated", "[mux]") {
    LoopbackHub hub;
    int ida = hub.add_endpoint();
    int idb = hub.add_endpoint();
    MuxTransport a(std::make_unique<LoopbackTransport>(hub, ida));
    MuxTransport b(std::make_unique<LoopbackTransport>(hub, idb));

    a.lobby_channel().broadcast(std::vector<u8>{42});
    b.pump();
    CHECK(b.game_channel().receive().empty()); // lobby msg not seen on game
    auto lob = b.lobby_channel().receive();
    REQUIRE(lob.size() == 1);
    CHECK(lob[0] == std::vector<u8>{42});
}
