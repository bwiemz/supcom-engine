#include "sim/lan_discovery.hpp"

#include "sim/socket_platform.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace osc::sim {

namespace {

using net::kInvalidSocket;
using net::socket_t;

// Moho's discovery messages: the request, and the answer (its magic,
// lobby-protocol version and "SupCom" flag, then the lobby's protocol, port
// and the game's description).
constexpr u8 kRequest = 0x6E;
constexpr u8 kAnswer = 0x6F;
constexpr u8 kMagic = 0x0B;
constexpr u8 kVersion = 0x01;
constexpr u8 kSupCom = 0x00;
constexpr size_t kAnswerHeader = 7;
/// The largest datagram read.
constexpr size_t kMaxDatagram = 65536;
/// The most datagrams one poll reads: a LAN flooding the port can't hold
/// up the frame (the rest wait for the next).
constexpr int kMaxReadsPerPoll = 256;

socket_t open_udp() {
    net::startup();
    socket_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kInvalidSocket) return s;
    net::set_blocking(s, false); // reads until there's nothing
    net::ignore_udp_resets(s);
    return s;
}

std::string dotted(u32 ip_network_order) {
    in_addr a{};
    a.s_addr = ip_network_order;
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

/// What a read found.
enum class Read : u8 {
    Datagram, ///< one, perhaps empty: `data` and `from`
    Nothing,  ///< none waiting
    Failed,   ///< an error; later datagrams may still be waiting
};

Read receive(socket_t s, std::vector<u8>& data, sockaddr_in& from) {
    data.resize(kMaxDatagram);
    socklen_t len = sizeof(from);
    const int n = static_cast<int>(recvfrom(s, reinterpret_cast<char*>(data.data()),
                                            static_cast<int>(data.size()), 0,
                                            reinterpret_cast<sockaddr*>(&from), &len));
    if (n < 0) return net::would_block() ? Read::Nothing : Read::Failed;
    data.resize(static_cast<size_t>(n));
    return Read::Datagram;
}

/// Each datagram waiting (at most kMaxReadsPerPoll), to `take`.
template <typename Take> void read_all(socket_t s, Take&& take) {
    std::vector<u8> data;
    sockaddr_in from{};
    for (int i = 0; i < kMaxReadsPerPoll; ++i) {
        const Read r = receive(s, data, from);
        if (r == Read::Nothing) break;
        if (r == Read::Datagram) take(data, from);
    }
}

} // namespace

struct LanDiscovery::Impl {
    std::string broadcast_address;
    u16 port = 0;
    socket_t fd = kInvalidSocket;
    i64 last_request = -1;
    struct Game {
        u32 ip = 0;
        u16 game_port = 0;
        DiscoveredGame info;
    };
    std::vector<Game> games;

    ~Impl() { net::close_socket(fd); }
};

LanDiscovery::LanDiscovery(std::string broadcast_address, u16 port)
    : impl_(std::make_unique<Impl>()) {
    impl_->broadcast_address = std::move(broadcast_address);
    impl_->port = port;
}

LanDiscovery::~LanDiscovery() = default;

bool LanDiscovery::open() {
    impl_->fd = open_udp();
    if (impl_->fd == kInvalidSocket) return false;
    int one = 1;
    setsockopt(impl_->fd, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one),
               sizeof(one));
    return true;
}

std::vector<DiscoveryEvent> LanDiscovery::poll(i64 now_ms) {
    std::vector<DiscoveryEvent> events;
    if (impl_->fd == kInvalidSocket) return events;
    // Ask, every two seconds
    if (impl_->last_request < 0 || now_ms - impl_->last_request >= kRequestIntervalMs) {
        impl_->last_request = now_ms;
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(impl_->port);
        if (inet_pton(AF_INET, impl_->broadcast_address.c_str(), &to.sin_addr) == 1) {
            const u8 request[] = {kRequest, kMagic, kVersion};
            sendto(impl_->fd, reinterpret_cast<const char*>(request), sizeof(request), 0,
                   reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        }
    }
    // The answers
    read_all(impl_->fd, [&](const std::vector<u8>& data, const sockaddr_in& from) {
        if (data.size() < kAnswerHeader || data[0] != kAnswer || data[1] != kMagic ||
            data[2] != kVersion || data[3] != kSupCom)
            return;
        const u8 protocol = data[4];
        const u16 game_port = static_cast<u16>(data[5] | (data[6] << 8));
        const u32 ip = from.sin_addr.s_addr;
        DiscoveredGame info;
        info.hostname = dotted(ip);
        info.address = info.hostname + ":" + std::to_string(game_port);
        info.protocol = protocol;
        info.config.assign(data.begin() + static_cast<long>(kAnswerHeader), data.end());
        info.last_heard_ms = now_ms;
        const auto it =
            std::find_if(impl_->games.begin(), impl_->games.end(), [&](const Impl::Game& g) {
                return g.ip == ip && g.game_port == game_port;
            });
        if (it == impl_->games.end()) {
            impl_->games.push_back({ip, game_port, info});
            events.push_back(
                {DiscoveryEvent::Kind::Found, static_cast<u32>(impl_->games.size() - 1), info});
        } else {
            it->info = info;
            events.push_back(
                {DiscoveryEvent::Kind::Updated, static_cast<u32>(it - impl_->games.begin()), info});
        }
    });
    // Those silent five seconds are gone; the later ones move up
    for (size_t i = 0; i < impl_->games.size();) {
        if (now_ms - impl_->games[i].info.last_heard_ms > kExpiryMs) {
            impl_->games.erase(impl_->games.begin() + static_cast<long>(i));
            events.push_back({DiscoveryEvent::Kind::Removed, static_cast<u32>(i), {}});
        } else {
            ++i;
        }
    }
    return events;
}

std::vector<DiscoveryEvent> LanDiscovery::reset() {
    std::vector<DiscoveryEvent> events;
    for (size_t i = impl_->games.size(); i-- > 0;)
        events.push_back({DiscoveryEvent::Kind::Removed, static_cast<u32>(i), {}});
    impl_->games.clear();
    return events;
}

size_t LanDiscovery::game_count() const {
    return impl_->games.size();
}

struct DiscoveryResponder::Impl {
    socket_t fd = kInvalidSocket;
    u16 port = 0;
    ~Impl() { net::close_socket(fd); }
};

DiscoveryResponder::DiscoveryResponder() : impl_(std::make_unique<Impl>()) {}
DiscoveryResponder::~DiscoveryResponder() = default;

bool DiscoveryResponder::open(u16 port) {
    socket_t s = open_udp();
    if (s == kInvalidSocket) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        net::close_socket(s);
        return false;
    }
    socklen_t alen = sizeof(addr);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&addr), &alen) == 0)
        impl_->port = ntohs(addr.sin_port);
    impl_->fd = s;
    return true;
}

u16 DiscoveryResponder::port() const {
    return impl_->port;
}

std::vector<DiscoveryAsker> DiscoveryResponder::poll() {
    std::vector<DiscoveryAsker> askers;
    if (impl_->fd == kInvalidSocket) return askers;
    read_all(impl_->fd, [&](const std::vector<u8>& data, const sockaddr_in& from) {
        if (data.size() >= 3 && data[0] == kRequest && data[1] == kMagic && data[2] == kVersion)
            askers.push_back({from.sin_addr.s_addr, ntohs(from.sin_port)});
    });
    return askers;
}

void DiscoveryResponder::answer(const DiscoveryAsker& asker, u8 protocol, u16 game_port,
                                const std::vector<u8>& config) {
    if (impl_->fd == kInvalidSocket) return;
    std::vector<u8> msg = {kAnswer,
                           kMagic,
                           kVersion,
                           kSupCom,
                           protocol,
                           static_cast<u8>(game_port & 0xFF),
                           static_cast<u8>(game_port >> 8)};
    if (config.size() + msg.size() > kMaxDatagram - 64) {
        spdlog::warn("[discovery] a game's description of {} bytes is too long to send",
                     config.size());
        return;
    }
    msg.insert(msg.end(), config.begin(), config.end());
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = asker.ip;
    to.sin_port = htons(asker.port);
    sendto(impl_->fd, reinterpret_cast<const char*>(msg.data()), static_cast<int>(msg.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

} // namespace osc::sim
