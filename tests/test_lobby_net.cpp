// The lobby's network (M218a), over the localhost loopback: FA's CLobby's
// joins, uids, names, relayed data, ejection, departures and keepalive.

#include <catch2/catch_test_macros.hpp>

#include "sim/lobby_net.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using osc::sim::LobbyEvent;
using osc::sim::LobbyNet;

namespace {

using Kind = LobbyEvent::Kind;

/// A lobby and everything it has reported.
struct Side {
    LobbyNet net;
    std::vector<LobbyEvent> events;
    Side(const std::string& name, osc::u32 max = 8) : net(name, max) {}
    bool saw(Kind kind, osc::u32 uid) const {
        for (const LobbyEvent& e : events)
            if (e.kind == kind && e.uid == uid) return true;
        return false;
    }
    const LobbyEvent* last(Kind kind) const {
        for (auto it = events.rbegin(); it != events.rend(); ++it)
            if (it->kind == kind) return &*it;
        return nullptr;
    }
};

osc::i64 now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/// Poll every side until `done` holds, or `wait_ms` pass (loopback is
/// quick, but a loaded machine schedules late).
bool pump(const std::vector<Side*>& sides, const std::function<bool()>& done,
          osc::i64 wait_ms = 2000) {
    const osc::i64 deadline = now_ms() + wait_ms;
    while (now_ms() < deadline) {
        for (Side* s : sides) {
            auto events = s->net.poll(now_ms());
            s->events.insert(s->events.end(), events.begin(), events.end());
        }
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

std::vector<osc::u8> bytes(const std::string& s) {
    return {s.begin(), s.end()};
}

} // namespace

TEST_CASE("Players join a hosted lobby: uids, names and the peer lists (M218a)", "[lobby]") {
    Side host("Host");
    REQUIRE(host.net.host(0, 1234567));
    REQUIRE(host.net.hosting());
    REQUIRE(host.net.port() != 0);
    CHECK(host.net.local_uid() == 0);

    Side a("Alice");
    REQUIRE(a.net.join("127.0.0.1", host.net.port()));
    REQUIRE(pump({&host, &a}, [&] { return a.net.joined() && host.saw(Kind::PeerJoined, 1); }));
    // The first to join is uid 1; the welcome carries the host's time
    CHECK(a.net.local_uid() == 1);
    CHECK(a.net.host_uid() == 0);
    CHECK(a.net.hosted_time() == 1234567);
    CHECK(a.saw(Kind::ConnectedToHost, 1));
    REQUIRE(a.net.peers().size() == 1);
    CHECK(a.net.peers()[0].name == "Host");

    // A second, with the same name case aside: made unique
    Side b("alice");
    REQUIRE(b.net.join("127.0.0.1", host.net.port()));
    REQUIRE(pump({&host, &a, &b}, [&] { return b.net.joined() && a.saw(Kind::PeerJoined, 2); }));
    CHECK(b.net.local_uid() == 2);
    CHECK(b.net.local_name() == "alice1");
    // Everyone knows everyone else, never themselves
    CHECK(host.net.peers().size() == 2);
    CHECK(a.net.peers().size() == 2);
    CHECK(b.net.peers().size() == 2);
    CHECK(b.net.peer(1) != nullptr);
    CHECK(b.net.peer(1)->name == "Alice");
    CHECK(b.net.peer(2) == nullptr);
}

TEST_CASE("Lobby data goes to one player or everyone, through the host (M218a)", "[lobby]") {
    Side host("Host");
    REQUIRE(host.net.host(0, 1));
    Side a("A");
    Side b("B");
    REQUIRE(a.net.join("127.0.0.1", host.net.port()));
    REQUIRE(b.net.join("127.0.0.1", host.net.port()));
    REQUIRE(pump({&host, &a, &b}, [&] {
        return a.net.joined() && b.net.joined() && a.net.peers().size() == 2 &&
               b.net.peers().size() == 2;
    }));
    const osc::u32 ua = a.net.local_uid();
    const osc::u32 ub = b.net.local_uid();

    // A player to another: relayed, the host not told
    a.net.send(ub, bytes("psst"));
    REQUIRE(pump({&host, &a, &b}, [&] { return b.saw(Kind::Data, ua); }));
    CHECK(b.last(Kind::Data)->payload == bytes("psst"));
    CHECK(b.last(Kind::Data)->name == a.net.local_name());
    CHECK_FALSE(host.saw(Kind::Data, ua));

    // To everyone: the host and the others, not the sender
    a.net.broadcast(bytes("hello all"));
    REQUIRE(pump({&host, &a, &b}, [&] {
        return host.saw(Kind::Data, ua) && b.last(Kind::Data)->payload == bytes("hello all");
    }));
    CHECK_FALSE(a.saw(Kind::Data, ua));

    // The host to one player
    host.net.send(ub, bytes("from host"));
    REQUIRE(pump({&host, &a, &b}, [&] { return b.saw(Kind::Data, 0); }));
    CHECK(b.last(Kind::Data)->payload == bytes("from host"));
    CHECK_FALSE(a.saw(Kind::Data, 0));
}

TEST_CASE("A full lobby refuses; an ejected player and the others hear of it (M218a)", "[lobby]") {
    Side host("Host", 1);
    REQUIRE(host.net.host(0, 1));
    Side a("A");
    REQUIRE(a.net.join("127.0.0.1", host.net.port()));
    REQUIRE(pump({&host, &a}, [&] { return a.net.joined(); }));

    // One player allowed: the next is refused, LobbyFull
    Side late("Late");
    REQUIRE(late.net.join("127.0.0.1", host.net.port()));
    REQUIRE(pump({&host, &a, &late}, [&] { return late.last(Kind::Ejected) != nullptr; }));
    CHECK(late.last(Kind::Ejected)->reason == "LobbyFull");
    CHECK_FALSE(late.net.joined());
    CHECK(host.net.peers().size() == 1);

    // Ejected: told why; the host's list loses them
    const osc::u32 ua = a.net.local_uid();
    REQUIRE(host.net.eject(ua, "KickedByHost"));
    CHECK_FALSE(host.net.eject(99, "nobody"));
    REQUIRE(pump({&host, &a}, [&] { return a.last(Kind::Ejected) != nullptr; }));
    CHECK(a.last(Kind::Ejected)->reason == "KickedByHost");
    CHECK(host.saw(Kind::PeerLeft, ua));
    CHECK(host.net.peers().empty());
    CHECK_FALSE(a.net.joined());
}

TEST_CASE("A player leaving, and the host leaving (M218a)", "[lobby]") {
    auto host = std::make_unique<Side>("Host");
    REQUIRE(host->net.host(0, 1));
    Side a("A");
    auto b = std::make_unique<Side>("B");
    REQUIRE(a.net.join("127.0.0.1", host->net.port()));
    REQUIRE(b->net.join("127.0.0.1", host->net.port()));
    REQUIRE(pump({host.get(), &a, b.get()},
                 [&] { return a.net.peers().size() == 2 && b->net.peers().size() == 2; }));
    const osc::u32 ub = b->net.local_uid();

    // B goes: the host and A hear of it
    b.reset();
    REQUIRE(pump({host.get(), &a},
                 [&] { return host->saw(Kind::PeerLeft, ub) && a.saw(Kind::PeerLeft, ub); }));
    CHECK(a.last(Kind::PeerLeft)->name == "B");
    CHECK(a.net.peers().size() == 1);

    // The host goes: A's connection fails, HostLeft
    host.reset();
    REQUIRE(pump({&a}, [&] { return a.last(Kind::ConnectionFailed) != nullptr; }));
    CHECK(a.last(Kind::ConnectionFailed)->reason == "HostLeft");
    CHECK_FALSE(a.net.joined());
    CHECK(a.net.peers().empty());
}

TEST_CASE("Joining nowhere fails; pings measure the host (M218a)", "[lobby]") {
    // No address at all: refused at once
    Side nowhere("Nowhere");
    REQUIRE_FALSE(nowhere.net.join("not an address", 1));

    // A port nothing listens on any more: the connection fails, HostLeft
    auto dead = std::make_unique<LobbyNet>("Dead", 8);
    REQUIRE(dead->host(0, 1));
    const osc::u16 dead_port = dead->port();
    dead.reset();
    Side lost("Lost");
    REQUIRE(lost.net.join("127.0.0.1", dead_port));
    // Windows retries a refused connect (its SYN retransmits, about two
    // seconds) before it says so; the join's own limit is the most it takes
    REQUIRE(pump(
        {&lost}, [&] { return lost.last(Kind::ConnectionFailed) != nullptr; },
        LobbyNet::kJoinTimeoutMs + 2000));
    CHECK(lost.last(Kind::ConnectionFailed)->reason == "HostLeft");

    // A joined player hears from the host at least once a second, and the
    // host from it
    Side host("Host");
    REQUIRE(host.net.host(0, 1));
    Side a("A");
    REQUIRE(a.net.join("127.0.0.1", host.net.port()));
    REQUIRE(pump({&host, &a}, [&] { return a.net.joined(); }));
    const osc::i64 start = now_ms();
    REQUIRE(pump({&host, &a}, [&] {
        const auto* h = a.net.peer(0);
        const auto* p = host.net.peer(a.net.local_uid());
        return h && p && h->last_heard_ms > start && p->last_heard_ms > start;
    }));
}
