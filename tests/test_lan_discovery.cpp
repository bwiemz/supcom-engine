// Finding the LAN's games (M218b), over the localhost loopback: a host's
// responder answers the broadcast; the list finds, updates and forgets
// games by their place in it, as FA's lobby screen keeps them.

#include <catch2/catch_test_macros.hpp>

#include "sim/lan_discovery.hpp"
#include "sim/socket_platform.hpp"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using osc::sim::DiscoveryEvent;
using osc::sim::DiscoveryResponder;
using osc::sim::LanDiscovery;

namespace {

using Kind = DiscoveryEvent::Kind;

std::vector<osc::u8> bytes(const std::string& s) {
    return {s.begin(), s.end()};
}

/// Poll `finder` at `now` (a made-up clock) while `host` answers each
/// request once per game in `configs` (lobbies on ports 4242, 4243, ...),
/// until an event comes or a second of real time passes.
std::vector<DiscoveryEvent> ask(LanDiscovery& finder, osc::i64 now, DiscoveryResponder& host,
                                const std::vector<std::string>& configs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::vector<DiscoveryEvent> all;
    while (std::chrono::steady_clock::now() < deadline) {
        auto events = finder.poll(now);
        all.insert(all.end(), events.begin(), events.end());
        if (all.size() >= configs.size()) return all;
        for (const auto& asker : host.poll()) {
            osc::u16 port = 4242;
            for (const std::string& config : configs) host.answer(asker, 2, port++, bytes(config));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return all;
}

/// An empty datagram to the loopback's `port` (any host on a LAN can send
/// one).
void send_empty(osc::u16 port) {
    namespace net = osc::sim::net;
    net::startup();
    const net::socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(s != net::kInvalidSocket);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    REQUIRE(inet_pton(AF_INET, "127.0.0.1", &to.sin_addr) == 1);
    const char none = 0;
    CHECK(sendto(s, &none, 0, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0);
    net::close_socket(s);
}

} // namespace

TEST_CASE("A host answers the broadcast; the game is found and updated (M218b)",
          "[lobby][discovery]") {
    DiscoveryResponder host;
    REQUIRE(host.open(0));
    REQUIRE(host.port() != 0);
    LanDiscovery finder("127.0.0.1", host.port());
    REQUIRE(finder.open());

    auto events = ask(finder, 1000, host, {"first"});
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::Found);
    CHECK(events[0].index == 0);
    CHECK(events[0].game.address == "127.0.0.1:4242");
    CHECK(events[0].game.hostname == "127.0.0.1");
    CHECK(events[0].game.protocol == 2);
    CHECK(events[0].game.config == bytes("first"));
    CHECK(finder.game_count() == 1);

    // Two seconds on it asks again: the same game, updated in its place
    events = ask(finder, 3000, host, {"second"});
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == Kind::Updated);
    CHECK(events[0].index == 0);
    CHECK(events[0].game.config == bytes("second"));
    CHECK(finder.game_count() == 1);

    // Not two seconds yet: no asking
    CHECK(host.poll().empty());
    CHECK(finder.poll(3500).empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(host.poll().empty());
}

TEST_CASE("Games are listed in their order, and forgotten after five silent seconds (M218b)",
          "[lobby][discovery]") {
    DiscoveryResponder host;
    REQUIRE(host.open(0));
    LanDiscovery finder("127.0.0.1", host.port());
    REQUIRE(finder.open());
    // Two lobbies answering (two ports), each its own game
    auto events = ask(finder, 1000, host, {"a", "b"});
    REQUIRE(events.size() == 2);
    CHECK(events[1].kind == Kind::Found);
    CHECK(events[1].index == 1);
    CHECK(events[1].game.address == "127.0.0.1:4243");

    // Five seconds unheard: both go, each at index 0 as the list closes up
    events = finder.poll(1000 + LanDiscovery::kExpiryMs + 1);
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == Kind::Removed);
    CHECK(events[0].index == 0);
    CHECK(events[1].index == 0);
    CHECK(finder.game_count() == 0);
}

TEST_CASE("Reset forgets the games last first; junk is ignored (M218b)", "[lobby][discovery]") {
    DiscoveryResponder host;
    REQUIRE(host.open(0));
    // The port is taken: someone else is hosting
    DiscoveryResponder second;
    CHECK_FALSE(second.open(host.port()));

    LanDiscovery finder("127.0.0.1", host.port());
    REQUIRE(finder.open());
    ask(finder, 1000, host, {"a", "b"});
    REQUIRE(finder.game_count() == 2);
    const auto gone = finder.reset();
    REQUIRE(gone.size() == 2);
    CHECK(gone[0].index == 1);
    CHECK(gone[1].index == 0);
    CHECK(finder.game_count() == 0);

    // Not an answer (wrong type, too short): nothing found
    LanDiscovery other("127.0.0.1", host.port());
    REQUIRE(other.open());
    other.poll(1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (const auto& asker : host.poll()) {
        host.answer(asker, 2, 1, {}); // a real, empty answer
    }
    // (a stray request to the finder is ignored too: it isn't an answer)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto found = other.poll(1000);
    REQUIRE(found.size() == 1);
    CHECK(found[0].game.config.empty());
}

TEST_CASE("An empty datagram hides nothing queued behind it (M218b)", "[lobby][discovery]") {
    DiscoveryResponder host;
    REQUIRE(host.open(0));
    LanDiscovery finder("127.0.0.1", host.port());
    REQUIRE(finder.open());

    // Ahead of the request at the host's port...
    send_empty(host.port());
    finder.poll(1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto askers = host.poll();
    REQUIRE(askers.size() == 1);

    // ...and ahead of the answer at the finder's: both read in one poll
    send_empty(askers[0].port);
    host.answer(askers[0], 2, 4242, bytes("game"));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto found = finder.poll(1000);
    REQUIRE(found.size() == 1);
    CHECK(found[0].game.config == bytes("game"));
}
