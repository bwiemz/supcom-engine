#include "lua/mp_net_state.hpp"

#include "sim/lockstep_session.hpp"
#include "sim/net_transport.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit_command.hpp"

#include <spdlog/spdlog.h>

namespace osc::lua {

void MpNetState::reset() {
    role = Role::None;
    join_address = "127.0.0.1";
    port = 47624;
    local_source = 0;
    all_sources = {0, 1};
    session.reset();   // destroy session before its transport
    transport.reset();
    host_tcp = nullptr;
    transport_ready = false;
}

MpNetState& mp_net_state() {
    static MpNetState s;
    return s;
}

bool mp_begin_host(osc::u16 port) {
    auto& s = mp_net_state();
    auto t = osc::sim::TcpTransport::host(port);
    if (!t || !t->ok()) {
        spdlog::error("[mp] HostGame: TCP bind failed on port {}", port);
        return false;
    }
    s.role = MpNetState::Role::Host;
    s.port = t->port();
    s.local_source = 0;
    s.host_tcp = t.get();          // non-owning alias for poll_connections()
    s.transport = std::move(t);
    s.transport_ready = true;
    spdlog::info("[mp] HostGame: TCP host listening on port {}", s.port);
    return true;
}

bool mp_begin_join(const std::string& address, osc::u16 port) {
    auto& s = mp_net_state();
    auto t = osc::sim::TcpTransport::join(address, port);
    if (!t || !t->ok()) {
        spdlog::error("[mp] JoinGame: TCP connect to {}:{} failed", address, port);
        return false;
    }
    s.role = MpNetState::Role::Join;
    s.join_address = address;
    s.port = port;
    s.local_source = 1;
    s.transport = std::move(t);
    s.transport_ready = true;
    spdlog::info("[mp] JoinGame: TCP connected to {}:{}", address, port);
    return true;
}

int mp_poll_connections() {
    auto& s = mp_net_state();
    return s.host_tcp ? s.host_tcp->poll_connections() : 0;
}

bool mp_attach_session(osc::sim::SimState& sim) {
    auto& s = mp_net_state();
    if (!s.transport_ready || !s.transport) return false;
    s.session = std::make_unique<osc::sim::LockstepSession>(
        sim, *s.transport, s.local_source, s.all_sources);
    auto* session = s.session.get();
    // Route local human orders through the session (broadcast + schedule).
    sim.set_local_command_sink(
        [session](const std::vector<osc::u32>& ids,
                  const osc::sim::UnitCommand& cmd, bool clear) {
            session->submit_local(ids, cmd, clear);
        });
    spdlog::info("[mp] LockstepSession attached (local source {})",
                 s.local_source);
    return true;
}

void mp_teardown() {
    auto& s = mp_net_state();
    s.session.reset();   // session refs the transport → destroy it first
    s.transport.reset();
    s.host_tcp = nullptr; // non-owning alias into the just-destroyed transport
    s.transport_ready = false;
    s.role = MpNetState::Role::None;
}

} // namespace osc::lua
