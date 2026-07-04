#pragma once

#include "core/types.hpp"

#include <string>

namespace osc::sim {
class INetTransport;
}

namespace osc::lua {

// The minimal per-game configuration a host must convey to a client so both
// build an identical sim: which scenario to load and the shared RNG seed. Slots
// are constructed identically on both sides for the 1v1 case, so they are not
// (yet) part of the wire config — extend this when real slot customization lands.
struct LanSessionConfig {
    std::string scenario;
    osc::u64 seed = 0;
};

// Host-authoritative LAN lobby handshake over one INetTransport channel (the
// lobby channel of a MuxTransport). The host advertises a config; the client
// applies it and replies READY; the host fires a LAUNCH barrier. Both sides then
// launch the same session with the same config + seed.
class LanLobby {
public:
    enum class Role { Host, Client };
    enum class State { Idle, Configured, Ready, LaunchReady };

    LanLobby(Role role, osc::sim::INetTransport& channel);

    // Host: set the config to advertise (sent on subsequent poll() calls until a
    // client READY arrives). No effect for a client.
    void set_host_config(const LanSessionConfig& cfg);

    // Host: send the launch signal. Only fires once state()==Ready.
    void request_launch();

    // Process inbound lobby messages and advance the state machine. The host
    // keeps advertising CONFIG while Idle so a late-connecting client still gets
    // it.
    void poll();

    Role role() const { return role_; }
    State state() const { return state_; }
    bool launch_ready() const { return state_ == State::LaunchReady; }
    const LanSessionConfig& config() const { return config_; }

private:
    void send_config();
    void send_ready();
    void send_launch();

    Role role_;
    osc::sim::INetTransport& channel_;
    State state_ = State::Idle;
    LanSessionConfig config_;
    bool have_config_ = false; // host: config set; client: config received
};

} // namespace osc::lua
