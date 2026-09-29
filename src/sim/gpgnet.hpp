#pragma once

// GPGNet (M220a): the link Moho keeps to a matchmaking client -- today FAF's
// client and its ICE adapter -- over one TCP connection the game opens to
// it. Both ways it carries commands: a name, then arguments, each a 32-bit
// integer, a string or data (Moho's CGpgNetInterface, per faf-re).

#include "core/types.hpp"

#include <memory>
#include <string>
#include <vector>

namespace osc::sim {

/// One argument of a command.
struct GpgNetArg {
    enum class Type : u8 { Num = 0, String = 1, Data = 2 };
    Type type = Type::Num;
    i32 num = 0;
    std::string str; ///< a string's, or data's, bytes

    static GpgNetArg number(i32 n);
    static GpgNetArg string(std::string s);
    bool operator==(const GpgNetArg& o) const;
};

struct GpgNetCommand {
    std::string name;
    std::vector<GpgNetArg> args;
};

/// The longest name, string or data, the most arguments, and the most
/// bytes a command may have: a peer announcing more is malformed or hostile
/// (it would have us buffer gigabytes waiting for the rest).
inline constexpr u32 kMaxGpgNetField = 1u << 20;
inline constexpr u32 kMaxGpgNetArgs = 1024;
inline constexpr u32 kMaxGpgNetCommand = 4u << 20;

/// A command as the wire has it, little-endian: its name (u32 length and
/// bytes), a u32 argument count, then each argument's u8 type and body (an
/// i32; or u32 length and bytes).
std::vector<u8> encode_gpgnet(const GpgNetCommand& command);

/// Move every whole command at the front of `buf` into `out`, leaving a
/// partial one. False when the next is malformed (a length or count past
/// the limits, an unknown type): the caller closes the link.
bool extract_gpgnet_commands(std::vector<u8>& buf, std::vector<GpgNetCommand>& out);

/// The game's end of the link: a TCP client, never blocking the frame.
class GpgNetLink {
public:
    /// How long connecting may take, in ms.
    static constexpr i64 kConnectTimeoutMs = 10000;

    GpgNetLink();
    ~GpgNetLink();
    GpgNetLink(const GpgNetLink&) = delete;
    GpgNetLink& operator=(const GpgNetLink&) = delete;

    /// Start connecting to the client at `address` (dotted IPv4) and
    /// `port`; poll() sees it through. False if `address` is no address,
    /// or no socket can be made.
    bool connect(const std::string& address, u16 port);

    struct Event {
        enum class Kind : u8 {
            Connected, ///< the connection is up
            Command,   ///< the client sent `command`
            Closed,    ///< the link is down (never connected, or lost): `reason`
        };
        Kind kind{};
        GpgNetCommand command;
        std::string reason;
    };
    /// Complete the connection, read what the client sent, notice it going.
    /// `now_ms`: a monotonic clock in milliseconds.
    std::vector<Event> poll(i64 now_ms);

    /// Send one command; false (the link closed) if it can't go.
    bool send(const GpgNetCommand& command);

    /// Connected, and not closed since.
    bool connected() const;
    /// Connecting or connected.
    bool open() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace osc::sim
