#include "lua/mp_net_state.hpp"

#include "lua/lan_lobby.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/mux_transport.hpp"
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
    seed = 0xC0FFEE1234567890ull;
    // Destroy consumers of the transport (they hold references into the mux)
    // before the mux that owns it.
    lobby.reset();
    session.reset();
    mux.reset();
    lobby_transport.reset();
    source_armies.clear();
    host_tcp = nullptr;
    transport_ready = false;
}

osc::i32 MpNetState::army_of(osc::u32 source) const {
    if (source_armies.empty()) return static_cast<osc::i32>(source);
    return source < source_armies.size() ? source_armies[source] : -1;
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
    s.host_tcp = t.get(); // non-owning alias for poll_connections()
    s.mux = std::make_unique<osc::sim::MuxTransport>(std::move(t));
    s.lobby = std::make_unique<LanLobby>(LanLobby::Role::Host,
                                         s.mux->lobby_channel());
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
    s.mux = std::make_unique<osc::sim::MuxTransport>(std::move(t));
    s.lobby = std::make_unique<LanLobby>(LanLobby::Role::Client,
                                         s.mux->lobby_channel());
    s.transport_ready = true;
    spdlog::info("[mp] JoinGame: TCP connected to {}:{}", address, port);
    return true;
}

int mp_poll_connections() {
    auto& s = mp_net_state();
    return s.host_tcp ? s.host_tcp->poll_connections() : 0;
}

void mp_pump() {
    auto& s = mp_net_state();
    if (s.mux) s.mux->pump();
    mp_poll_connections();
}

LanLobby* mp_lobby() { return mp_net_state().lobby.get(); }

void mp_begin_lobby_game(std::unique_ptr<osc::sim::INetTransport> transport, bool host,
                         osc::u32 local_source, std::vector<osc::i32> armies, osc::u64 seed) {
    auto& s = mp_net_state();
    s.reset(); // a LAN transport set up before (LanHost/LanJoin) goes
    s.role = host ? MpNetState::Role::Host : MpNetState::Role::Join;
    s.local_source = local_source;
    s.all_sources.clear();
    for (osc::u32 source = 0; source < armies.size(); ++source) s.all_sources.push_back(source);
    s.source_armies = std::move(armies);
    s.seed = seed;
    s.lobby_transport = std::move(transport);
    s.transport_ready = true;
    spdlog::info("[mp] a lobby's game: {} command sources, this player's {} (army {}), seed "
                 "{:#018x}",
                 s.all_sources.size(), local_source, s.local_army(), seed);
}

bool mp_attach_session(osc::sim::SimState& sim) {
    auto& s = mp_net_state();
    osc::sim::INetTransport* transport = s.lobby_transport ? s.lobby_transport.get()
                                         : s.mux           ? &s.mux->game_channel()
                                                           : nullptr;
    if (!s.transport_ready || !transport) return false;
    s.session =
        std::make_unique<osc::sim::LockstepSession>(sim, *transport, s.local_source, s.all_sources);
    // Each source plays its army (as a dropped peer's defeat and GetFocusArmy
    // take it): each peer's orders move only its own army's units, and an
    // observer's none.
    for (const osc::u32 source : s.all_sources) sim.set_source_army(source, s.army_of(source));
    sim.set_source_army(s.local_source, s.local_army());
    auto* session = s.session.get();
    // Route local human orders through the session (broadcast + schedule).
    sim.set_local_command_sink(
        [session](const std::vector<osc::u32>& ids,
                  const osc::sim::UnitCommand& cmd, bool clear) {
            session->submit_local(ids, cmd, clear);
        });
    // And the UI's SimCallbacks, so every peer runs them on the same tick.
    sim.set_local_callback_sink([session](osc::sim::SimCallbackEntry cb) {
        session->submit_local_callback(std::move(cb));
    });
    // Every client seeds its sim from the host's shared seed so RNG matches.
    sim.set_seed(s.seed);
    spdlog::info("[mp] LockstepSession attached (local source {}, seed {:#018x})",
                 s.local_source, s.seed);
    return true;
}

void mp_teardown() {
    auto& s = mp_net_state();
    s.lobby.reset();     // holds a reference into the mux
    s.session.reset();   // holds a reference into the mux (or the lobby's transport)
    s.mux.reset();       // owns the transport
    s.lobby_transport.reset();
    s.source_armies.clear();
    s.host_tcp = nullptr;
    s.transport_ready = false;
    s.role = MpNetState::Role::None;
}

} // namespace osc::lua
