// Reliable, ordered byte streams over UDP (M220c): over a network that
// loses, duplicates and reorders datagrams (seeded, so each run is the
// same), and over the loopback's real UDP sockets.

#include <catch2/catch_test_macros.hpp>

#include "sim/reliable_udp.hpp"

#include <chrono>
#include <map>
#include <random>
#include <thread>
#include <utility>
#include <vector>

using osc::i64;
using osc::u32;
using osc::u8;
using osc::sim::DatagramPort;
using osc::sim::ReliableUdp;
using osc::sim::UdpAddress;
using State = ReliableUdp::State;

namespace {

/// An in-memory network: each datagram may be lost, sent twice, or held up
/// to `delay` ms (so they come out of order).
struct Network {
    // A fixed seed on purpose: every run loses and reorders the same datagrams
    // NOLINTNEXTLINE(bugprone-random-generator-seed)
    std::mt19937 rng{2026};
    double loss = 0.0;
    double twice = 0.0;
    i64 delay = 0;
    i64 now = 0;
    struct Flying {
        i64 at;
        UdpAddress from, to;
        std::vector<u8> d;
    };
    std::vector<Flying> flying;
    std::map<UdpAddress, std::vector<std::pair<UdpAddress, std::vector<u8>>>> inbox;
    size_t sent = 0;
    /// Data datagrams sent, and those sent again (by sender and segment)
    size_t data = 0;
    size_t resent = 0;
    std::map<std::pair<UdpAddress, u32>, int> segments;

    void send(const UdpAddress& from, const UdpAddress& to, const std::vector<u8>& d) {
        ++sent;
        if (d.size() >= 14 && d[1] == 3) { // a data segment: u32 seq at 6
            ++data;
            const u32 seq = d[6] | (d[7] << 8) | (d[8] << 16) | (static_cast<u32>(d[9]) << 24);
            if (segments[{from, seq}]++ > 0) ++resent;
        }
        std::uniform_real_distribution<double> p(0.0, 1.0);
        std::uniform_int_distribution<i64> wait(0, delay);
        if (p(rng) < loss) return;
        flying.push_back({now + wait(rng), from, to, d});
        if (p(rng) < twice) flying.push_back({now + wait(rng), from, to, d});
    }
    void deliver() {
        std::vector<Flying> later;
        for (auto& f : flying) {
            if (f.at <= now) inbox[f.to].emplace_back(f.from, std::move(f.d));
            else later.push_back(std::move(f));
        }
        flying = std::move(later);
    }
};

struct Port : DatagramPort {
    Network& net;
    UdpAddress me;
    Port(Network& n, UdpAddress a) : net(n), me(a) {}
    void send_to(const UdpAddress& to, const std::vector<u8>& d) override { net.send(me, to, d); }
    std::vector<std::pair<UdpAddress, std::vector<u8>>> receive() override {
        net.deliver();
        return std::exchange(net.inbox[me], {});
    }
};

const UdpAddress kHost{0x7f000001, 6112};
const UdpAddress kJoiner{0x7f000001, 6113};

/// Two sides on one network: a host listening, a joiner connecting to it.
struct Pair {
    Network net;
    Port host_port{net, kHost}, joiner_port{net, kJoiner};
    ReliableUdp host{host_port}, joiner{joiner_port};
    u32 to_host = 0;   ///< the joiner's stream
    u32 to_joiner = 0; ///< the host's
    Pair() { host.listen(true); }
    /// Pump both, 10 ms a step, `steps` times or until `done`.
    template <typename Done> bool run(int steps, Done done) {
        for (int i = 0; i < steps; ++i) {
            net.now += 10;
            host.pump(net.now);
            joiner.pump(net.now);
            for (u32 id : host.take_accepted()) to_joiner = id;
            if (done()) return true;
        }
        return false;
    }
    bool open() {
        to_host = joiner.connect(kHost, net.now);
        return run(2000, [&] {
            return joiner.state(to_host) == State::Open && to_joiner != 0 &&
                   host.state(to_joiner) == State::Open;
        });
    }
};

std::vector<u8> bytes(size_t n, u32 seed) {
    std::mt19937 rng(seed);
    std::vector<u8> out(n);
    for (u8& b : out) b = static_cast<u8>(rng());
    return out;
}

/// Send `a` one way and `b` the other, in pieces, and see each arrive whole.
void exchange(Pair& p, const std::vector<u8>& a, const std::vector<u8>& b) {
    for (size_t at = 0; at < a.size(); at += 3000)
        REQUIRE(p.joiner.send(p.to_host, a.data() + at, std::min<size_t>(3000, a.size() - at)));
    for (size_t at = 0; at < b.size(); at += 700)
        REQUIRE(p.host.send(p.to_joiner, b.data() + at, std::min<size_t>(700, b.size() - at)));
    std::vector<u8> got_a, got_b;
    REQUIRE(p.run(20000, [&] {
        const auto x = p.host.take_received(p.to_joiner);
        got_a.insert(got_a.end(), x.begin(), x.end());
        const auto y = p.joiner.take_received(p.to_host);
        got_b.insert(got_b.end(), y.begin(), y.end());
        return got_a.size() >= a.size() && got_b.size() >= b.size();
    }));
    CHECK(got_a == a);
    CHECK(got_b == b);
}

} // namespace

TEST_CASE("A reliable UDP stream carries bytes both ways, whole and in order (M220c)", "[udp]") {
    Pair p;
    REQUIRE(p.open());
    CHECK(p.host.address(p.to_joiner) == kJoiner);
    CHECK(p.joiner.address(p.to_host) == kHost);
    exchange(p, bytes(200000, 1), bytes(50000, 2));
}

TEST_CASE("A reliable UDP stream's acknowledgements ride on its data: nothing is resent on a "
          "clean network (M220c)",
          "[udp]") {
    // A lockstep's traffic: a small message each way every pump, for two
    // seconds. Each side's segments are acknowledged by the other's data,
    // before any waits long enough to go again.
    Pair p;
    REQUIRE(p.open());
    const std::vector<u8> frame(40, 7);
    size_t got_host = 0, got_joiner = 0;
    REQUIRE_FALSE(p.run(200, [&] {
        REQUIRE(p.joiner.send(p.to_host, frame.data(), frame.size()));
        REQUIRE(p.host.send(p.to_joiner, frame.data(), frame.size()));
        got_host += p.host.take_received(p.to_joiner).size();
        got_joiner += p.joiner.take_received(p.to_host).size();
        return false;
    }));
    CHECK(p.net.data >= 400);
    CHECK(p.net.resent == 0);
    CHECK(got_host >= 190 * frame.size());
    CHECK(got_joiner >= 190 * frame.size());
}

TEST_CASE("A reliable UDP stream sends at once, as a socket would (M220c)", "[udp]") {
    Pair p;
    REQUIRE(p.open());
    const size_t before = p.net.data;
    const std::vector<u8> one{1, 2, 3};
    REQUIRE(p.joiner.send(p.to_host, one.data(), one.size()));
    CHECK(p.net.data == before + 1); // no pump yet: it went
}

TEST_CASE("A reliable UDP stream survives loss, duplicates and reordering (M220c)", "[udp]") {
    Pair p;
    p.net.loss = 0.3;
    p.net.twice = 0.1;
    p.net.delay = 60;
    REQUIRE(p.open());
    exchange(p, bytes(300000, 3), bytes(120000, 4));
    // It took more datagrams than a clean network would: they were resent
    CHECK(p.net.sent > (300000 + 120000) / ReliableUdp::kSegmentBytes);
}

TEST_CASE("Closing a reliable UDP stream delivers what was sent, then says goodbye (M220c)",
          "[udp]") {
    Pair p;
    p.net.loss = 0.2;
    p.net.delay = 30;
    REQUIRE(p.open());
    const auto last = bytes(20000, 5);
    REQUIRE(p.host.send(p.to_joiner, last.data(), last.size()));
    p.host.close(p.to_joiner);
    CHECK(p.host.state(p.to_joiner) == State::Closing);
    CHECK_FALSE(p.host.send(p.to_joiner, last.data(), 1)); // closed here
    std::vector<u8> got;
    REQUIRE(p.run(20000, [&] {
        const auto x = p.joiner.take_received(p.to_host);
        got.insert(got.end(), x.begin(), x.end());
        return p.joiner.state(p.to_host) == State::Closed;
    }));
    const auto rest = p.joiner.take_received(p.to_host);
    got.insert(got.end(), rest.begin(), rest.end());
    CHECK(got == last);
    REQUIRE(p.run(2000, [&] { return p.host.state(p.to_joiner) == State::Closed; }));
}

TEST_CASE("A reliable UDP stream is lost when its peer goes quiet (M220c)", "[udp]") {
    Pair p;
    REQUIRE(p.open());
    // The joiner stops (a crash): no goodbye
    i64 quiet_from = p.net.now;
    while (p.host.state(p.to_joiner) == State::Open && p.net.now - quiet_from < 20000) {
        p.net.now += 100;
        p.host.pump(p.net.now);
    }
    CHECK(p.host.state(p.to_joiner) == State::Closed);
    CHECK(p.net.now - quiet_from > ReliableUdp::kLostMs);
    CHECK(p.net.now - quiet_from < ReliableUdp::kLostMs + 500);
}

TEST_CASE("A hello no one answers fails; one to a side not listening too (M220c)", "[udp]") {
    Pair p;
    p.host.listen(false);
    const u32 id = p.joiner.connect(kHost, p.net.now);
    REQUIRE(p.run(2000, [&] { return p.joiner.state(id) == State::Closed; }));
    CHECK(p.to_joiner == 0);
    CHECK(p.net.now > ReliableUdp::kConnectMs);
    const u32 nobody = p.joiner.connect(UdpAddress{0x7f000001, 1}, p.net.now);
    REQUIRE(p.run(2000, [&] { return p.joiner.state(nobody) == State::Closed; }));
}

TEST_CASE("A peer that starts over is a new stream; the old one's datagrams are ignored (M220c)",
          "[udp]") {
    Pair p;
    REQUIRE(p.open());
    const u32 first = p.to_joiner;
    // The joiner's process restarts at the same address: a new stream
    Port again_port(p.net, kJoiner);
    auto again = std::make_unique<ReliableUdp>(again_port);
    const u32 id = again->connect(kHost, p.net.now);
    p.to_joiner = 0;
    bool open = false;
    for (int i = 0; i < 2000 && !open; ++i) {
        p.net.now += 10;
        p.host.pump(p.net.now);
        again->pump(p.net.now);
        for (u32 a : p.host.take_accepted()) p.to_joiner = a;
        open = again->state(id) == State::Open && p.to_joiner != 0;
    }
    REQUIRE(open);
    CHECK(p.to_joiner != first);
    CHECK(p.host.state(first) == State::Closed);
    // The old stream's side sends on: the host takes none of it
    const std::vector<u8> stale{1, 2, 3};
    REQUIRE(p.joiner.send(p.to_host, stale.data(), stale.size()));
    const std::vector<u8> fresh{9, 8, 7};
    REQUIRE(again->send(id, fresh.data(), fresh.size()));
    std::vector<u8> got;
    for (int i = 0; i < 200; ++i) {
        p.net.now += 10;
        p.joiner.pump(p.net.now);
        again->pump(p.net.now);
        p.host.pump(p.net.now);
        const auto x = p.host.take_received(p.to_joiner);
        got.insert(got.end(), x.begin(), x.end());
    }
    CHECK(got == fresh);
}

TEST_CASE("Reliable UDP over the loopback's real sockets (M220c)", "[udp]") {
    osc::sim::UdpSocketPort a, b;
    REQUIRE(a.open(0));
    REQUIRE(b.open(0));
    REQUIRE(a.port() != 0);
    ReliableUdp host(a), joiner(b);
    host.listen(true);
    const auto to = UdpAddress::parse("127.0.0.1", a.port());
    REQUIRE(to);
    CHECK(to->text() == "127.0.0.1:" + std::to_string(a.port()));
    CHECK_FALSE(UdpAddress::parse("not an address", 1));
    using clock = std::chrono::steady_clock;
    const auto ms = [] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   clock::now().time_since_epoch())
            .count();
    };
    const u32 id = joiner.connect(*to, ms());
    u32 accepted = 0;
    const auto payload = bytes(100000, 6);
    REQUIRE(joiner.send(id, payload.data(), payload.size())); // queued while connecting
    std::vector<u8> got;
    const i64 deadline = ms() + 5000;
    while (got.size() < payload.size() && ms() < deadline) {
        host.pump(ms());
        joiner.pump(ms());
        for (u32 x : host.take_accepted()) accepted = x;
        if (accepted) {
            const auto r = host.take_received(accepted);
            got.insert(got.end(), r.begin(), r.end());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(got == payload);
    CHECK(host.address(accepted).port == b.port());
}
