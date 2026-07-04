#include "lua/lan_lobby.hpp"

#include "sim/net_transport.hpp"

#include <vector>

namespace osc::lua {

namespace {

enum MsgType : osc::u8 { MSG_CONFIG = 1, MSG_READY = 2, MSG_LAUNCH = 3 };

void put_u8(std::vector<osc::u8>& b, osc::u8 v) { b.push_back(v); }
void put_u32(std::vector<osc::u8>& b, osc::u32 v) {
    b.push_back(static_cast<osc::u8>(v & 0xFF));
    b.push_back(static_cast<osc::u8>((v >> 8) & 0xFF));
    b.push_back(static_cast<osc::u8>((v >> 16) & 0xFF));
    b.push_back(static_cast<osc::u8>((v >> 24) & 0xFF));
}
void put_u64(std::vector<osc::u8>& b, osc::u64 v) {
    put_u32(b, static_cast<osc::u32>(v & 0xFFFFFFFFull));
    put_u32(b, static_cast<osc::u32>((v >> 32) & 0xFFFFFFFFull));
}
void put_str(std::vector<osc::u8>& b, const std::string& s) {
    put_u32(b, static_cast<osc::u32>(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}

struct Reader {
    const std::vector<osc::u8>& b;
    size_t pos = 0;
    bool ok = true;
    bool need(size_t n) {
        if (pos + n > b.size()) ok = false;
        return ok;
    }
    osc::u8 u8v() { return need(1) ? b[pos++] : 0; }
    osc::u32 u32v() {
        if (!need(4)) return 0;
        osc::u32 v = static_cast<osc::u32>(b[pos]) |
                     (static_cast<osc::u32>(b[pos + 1]) << 8) |
                     (static_cast<osc::u32>(b[pos + 2]) << 16) |
                     (static_cast<osc::u32>(b[pos + 3]) << 24);
        pos += 4;
        return v;
    }
    osc::u64 u64v() {
        osc::u64 lo = u32v();
        osc::u64 hi = u32v();
        return lo | (hi << 32);
    }
    std::string strv() {
        osc::u32 len = u32v();
        if (!need(len)) return {};
        std::string s(b.begin() + static_cast<long>(pos),
                      b.begin() + static_cast<long>(pos + len));
        pos += len;
        return s;
    }
};

} // namespace

LanLobby::LanLobby(Role role, osc::sim::INetTransport& channel)
    : role_(role), channel_(channel) {}

void LanLobby::set_host_config(const LanSessionConfig& cfg) {
    if (role_ != Role::Host) return;
    config_ = cfg;
    have_config_ = true;
}

void LanLobby::send_config() {
    std::vector<osc::u8> m;
    put_u8(m, MSG_CONFIG);
    put_u64(m, config_.seed);
    put_str(m, config_.scenario);
    channel_.broadcast(m);
}

void LanLobby::send_ready() {
    std::vector<osc::u8> m;
    put_u8(m, MSG_READY);
    channel_.broadcast(m);
}

void LanLobby::send_launch() {
    std::vector<osc::u8> m;
    put_u8(m, MSG_LAUNCH);
    channel_.broadcast(m);
}

void LanLobby::request_launch() {
    if (role_ == Role::Host && state_ == State::Ready) {
        send_launch();
        state_ = State::LaunchReady;
    }
}

void LanLobby::poll() {
    for (const auto& raw : channel_.receive()) {
        Reader r{raw};
        osc::u8 type = r.u8v();
        if (role_ == Role::Host) {
            if (type == MSG_READY && state_ == State::Idle) state_ = State::Ready;
        } else { // Client
            if (type == MSG_CONFIG) {
                osc::u64 seed = r.u64v();
                std::string scenario = r.strv();
                if (r.ok) {
                    config_.seed = seed;
                    config_.scenario = scenario;
                    have_config_ = true;
                    send_ready();
                    if (state_ == State::Idle) state_ = State::Configured;
                }
            } else if (type == MSG_LAUNCH) {
                state_ = State::LaunchReady;
            }
        }
    }
    // Host keeps advertising until a client readies (covers a client that
    // connected after the first CONFIG went out).
    if (role_ == Role::Host && state_ == State::Idle && have_config_) {
        send_config();
    }
}

} // namespace osc::lua
