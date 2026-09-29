#pragma once

// Reliable, ordered byte streams over UDP (M220c): the lobby's connections
// where only UDP goes through -- FAF's ICE adapter relays UDP only -- and
// Moho's own lobby protocol for "UDP". Each stream is one peer address's,
// all over one datagram port; what a side sends arrives whole and in order,
// or the stream closes.

#include "core/types.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace osc::sim {

/// An IPv4 address and port, in host byte order.
struct UdpAddress {
    u32 ip = 0;
    u16 port = 0;
    bool operator==(const UdpAddress& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const UdpAddress& o) const { return !(*this == o); }
    bool operator<(const UdpAddress& o) const { return ip != o.ip ? ip < o.ip : port < o.port; }
    /// "a.b.c.d:port"
    std::string text() const;
    /// A dotted IPv4 address (or "localhost") and a port; nothing if not one.
    static std::optional<UdpAddress> parse(const std::string& host, u16 port);
};

/// Where ReliableUdp's datagrams go and come from: a UDP socket, or (tests) a
/// network that loses, reorders and duplicates them.
class DatagramPort {
public:
    virtual ~DatagramPort() = default;
    virtual void send_to(const UdpAddress& to, const std::vector<u8>& datagram) = 0;
    /// The datagrams that came since the last call, each with its sender.
    virtual std::vector<std::pair<UdpAddress, std::vector<u8>>> receive() = 0;
};

/// A UDP socket, bound to a port on every address, as a DatagramPort.
class UdpSocketPort : public DatagramPort {
public:
    UdpSocketPort();
    ~UdpSocketPort() override;
    UdpSocketPort(const UdpSocketPort&) = delete;
    UdpSocketPort& operator=(const UdpSocketPort&) = delete;

    /// Bind `port` (0: any free one). False if it can't be had.
    bool open(u16 port);
    /// The port bound.
    u16 port() const;

    void send_to(const UdpAddress& to, const std::vector<u8>& datagram) override;
    std::vector<std::pair<UdpAddress, std::vector<u8>>> receive() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ReliableUdp {
public:
    /// The most bytes a datagram carries; how many may be unacknowledged.
    static constexpr size_t kSegmentBytes = 1024;
    static constexpr u32 kWindow = 256;
    /// Resending: the first wait, doubling to the most; a joiner's hello.
    static constexpr i64 kFirstResendMs = 200;
    static constexpr i64 kMostResendMs = 2000;
    static constexpr i64 kHelloEveryMs = 250;
    /// A stream with no word from its peer this long is lost; a hello
    /// unanswered this long fails.
    static constexpr i64 kLostMs = 10000;
    static constexpr i64 kConnectMs = 10000;

    enum class State : u8 {
        Connecting, ///< a joiner's, saying hello
        Open,
        Closing, ///< closed here: what was sent goes, then goodbye
        Closed,  ///< gone: closed, lost, refused
    };

    explicit ReliableUdp(DatagramPort& port);
    /// Goodbye to every open stream (the pumping thread stopped first).
    ~ReliableUdp();

    /// Pump on a thread of its own too, every few ms on the steady clock's
    /// milliseconds, as Moho's network runs apart from its frame: a side
    /// whose frame is held up (loading a map) still answers and keeps its
    /// streams alive. Every call is then safe from either thread.
    void run_in_background();
    ReliableUdp(const ReliableUdp&) = delete;
    ReliableUdp& operator=(const ReliableUdp&) = delete;

    /// Whether a hello from an address makes a stream (a host's, until its
    /// game starts).
    void listen(bool on);
    /// A stream to `to`: hello until it answers. Its id.
    u32 connect(const UdpAddress& to, i64 now);
    /// The streams hellos made since the last call.
    std::vector<u32> take_accepted();

    /// Read the port; answer, acknowledge, send, resend and time out.
    /// `now`: a monotonic clock in milliseconds.
    void pump(i64 now);

    /// Send bytes on an open stream (at once, as far as its window allows),
    /// or queue them on one still connecting. False if neither.
    bool send(u32 id, const u8* data, size_t len);
    /// The bytes that came in order since the last call.
    std::vector<u8> take_received(u32 id);
    State state(u32 id) const;
    UdpAddress address(u32 id) const;
    /// Close a stream: what was sent still goes, then goodbye.
    void close(u32 id);

private:
    struct Stream;
    mutable std::mutex mu_;
    std::thread pumper_;
    std::atomic<bool> stop_{false};
    DatagramPort& port_;
    bool listening_ = false;
    u32 next_id_ = 1;
    i64 now_ = 0;
    std::map<u32, std::unique_ptr<Stream>> streams_;
    std::vector<u32> accepted_;

    Stream* find(u32 id) const;
    Stream* by_address(const UdpAddress& a) const;
    void pump_locked(i64 now);
    void on_datagram(const UdpAddress& from, const std::vector<u8>& d);
    void flush(Stream& s);
    void send_control(const Stream& s, u8 type) const;
};

} // namespace osc::sim
