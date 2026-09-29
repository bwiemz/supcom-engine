#include "lua/mp_net_state.hpp"

#include "lua/session_clients.hpp"
#include "sim/lobby_net.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/net_transport.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <spdlog/spdlog.h>

namespace osc::lua {

void MpNetState::reset() {
    local_source = 0;
    all_sources.clear();
    seed = 0xC0FFEE1234567890ull;
    // The session holds a reference into the lobby's connections: it goes first
    session.reset();
    lobby_game = nullptr;
    lobby_transport.reset();
    source_armies.clear();
    clients.clear();
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

void mp_begin_lobby_game(std::unique_ptr<osc::sim::LobbyGameTransport> transport,
                         osc::u32 local_source, std::vector<osc::i32> armies,
                         std::vector<SessionClient> clients, osc::u64 seed) {
    auto& s = mp_net_state();
    s.reset(); // any game set up before goes
    s.local_source = local_source;
    s.all_sources.clear();
    for (osc::u32 source = 0; source < armies.size(); ++source) s.all_sources.push_back(source);
    s.source_armies = std::move(armies);
    s.seed = seed;
    s.clients = std::move(clients);
    s.lobby_game = transport.get();
    s.lobby_transport = std::move(transport);
    s.transport_ready = true;
    spdlog::info("[mp] a lobby's game: {} command sources, this player's {} (army {}), seed "
                 "{:#018x}",
                 s.all_sources.size(), local_source, s.local_army(), seed);
}

namespace {

/// The game's Timeouts option (ScenarioInfo.Options.Timeouts, a string in
/// the lobby's): -1 (unlimited) without it.
osc::i32 game_timeouts(osc::sim::SimState& sim) {
    lua_State* L = sim.lua_state();
    if (!L) return -1;
    const int top = lua_gettop(L);
    osc::i32 out = -1;
    lua_pushstring(L, "ScenarioInfo"); // raw: the sim's globals are strict
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "Options");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "Timeouts");
            lua_rawget(L, -2);
            if (lua_isnumber(L, -1)) { // a number, or a string of one
                const double t = lua_tonumber(L, -1);
                out = t < 0 ? -1 : t > 1000 ? 1000 : static_cast<osc::i32>(t);
            }
        }
    }
    lua_settop(L, top);
    return out;
}

} // namespace

bool mp_attach_session(osc::sim::SimState& sim) {
    auto& s = mp_net_state();
    if (!s.transport_ready || !s.lobby_transport) return false;
    s.session = std::make_unique<osc::sim::LockstepSession>(sim, *s.lobby_transport, s.local_source,
                                                            s.all_sources);
    // Each source plays its army (as a dropped peer's defeat and GetFocusArmy
    // take it): each peer's orders move only its own army's units, and an
    // observer's none.
    for (const osc::u32 source : s.all_sources) sim.set_source_army(source, s.army_of(source));
    // Each source's pause timeouts, as Moho's: a player the game's Timeouts
    // ('0', '3', '-1': unlimited), an observer none
    const osc::i32 timeouts = game_timeouts(sim);
    for (const osc::u32 source : s.all_sources)
        sim.set_pause_timeouts(source, s.army_of(source) >= 0 ? timeouts : 0);
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

void mp_disconnect_source(osc::u32 source) {
    auto& s = mp_net_state();
    if (s.lobby_game && source < s.clients.size()) s.lobby_game->disconnect(s.clients[source].uid);
}

void mp_teardown() {
    reset_session_chat(); // the game's chat not yet delivered goes with it
    mp_net_state().reset();
}

} // namespace osc::lua
