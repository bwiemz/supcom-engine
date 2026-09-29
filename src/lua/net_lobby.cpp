#include "lua/net_lobby.hpp"

#include "lua/lobby_wire.hpp"
#include "lua/mp_net_state.hpp"
#include "sim/lan_discovery.hpp"
#include "sim/lobby_net.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace osc::lua {

struct NetLobby {
    /// Its UI state: the state's registry, which its threads share (a
    /// method may come from a thread, as retail's launch countdown calls
    /// LaunchGame)
    const void* state = nullptr;
    /// The thread to call back on: the pump's, or a method's caller while
    /// the method runs
    lua_State* L = nullptr;
    int self_ref = LUA_NOREF; ///< the scripts' object, for its callbacks
    u16 port = 0;
    u32 max_connections = 8;
    std::string player_name;
    u8 protocol = 2; ///< Moho's: 1 TCP, 2 UDP (what discovery tells)
    std::unique_ptr<sim::LobbyNet> net;
    /// A host's: answers the LAN's discovery (M218b)
    std::unique_ptr<sim::DiscoveryResponder> responder;
    bool hosting_pending = false; ///< Hosting() at the next pump
    bool join_failed = false;     ///< ConnectionFailed at the next pump
};

struct NetDiscovery {
    const void* state = nullptr; ///< as NetLobby's
    lua_State* L = nullptr;      ///< as NetLobby's
    int self_ref = LUA_NOREF;
    sim::LanDiscovery finder;
    NetDiscovery(const std::string& address, u16 port) : finder(address, port) {}
};

namespace {

/// Moho caps a game's name at 32 characters and a player's at 24.
constexpr size_t kMaxPlayerName = 24;

/// The UI state `L` (or one of its threads) belongs to.
const void* state_of(lua_State* L) {
    return lua_topointer(L, LUA_REGISTRYINDEX);
}

/// The object at 1 no longer a networked one (its C++ side is gone).
void forget_object(lua_State* L) {
    if (!lua_istable(L, 1)) return;
    lua_pushstring(L, "_c_object");
    lua_pushnil(L);
    lua_rawset(L, 1);
}

std::vector<std::unique_ptr<NetLobby>>& lobbies() {
    static std::vector<std::unique_ptr<NetLobby>> all;
    return all;
}

std::vector<std::unique_ptr<NetDiscovery>>& discoveries() {
    static std::vector<std::unique_ptr<NetDiscovery>> all;
    return all;
}

/// Where discovery asks and hosts listen (set_lan_discovery).
std::string& discovery_address() {
    static std::string address = "255.255.255.255";
    return address;
}
u16& discovery_port() {
    static u16 port = sim::kLanDiscoveryPort;
    return port;
}

bool alive(const NetDiscovery* d) {
    return std::any_of(discoveries().begin(), discoveries().end(),
                       [&](const auto& x) { return x.get() == d; });
}

bool alive(const NetLobby* lobby) {
    return std::any_of(lobbies().begin(), lobbies().end(),
                       [&](const auto& l) { return l.get() == lobby; });
}

std::string uid_string(u32 uid) {
    return std::to_string(uid);
}

/// A uid the scripts pass: a string ("2") or a number.
bool parse_uid(lua_State* L, int idx, u32& out) {
    if (lua_type(L, idx) == LUA_TNUMBER) {
        const double d = lua_tonumber(L, idx);
        if (!(d >= 0 && d <= 4294967295.0)) return false;
        out = static_cast<u32>(d);
        return true;
    }
    if (lua_type(L, idx) != LUA_TSTRING) return false;
    const std::string s = lua_tostring(L, idx);
    // Digits, and few enough for a u32: std::stoul would throw past its
    // range, through Lua's C frames (a peer's data can carry any string)
    if (s.empty() || s.size() > 10 || s.find_first_not_of("0123456789") != std::string::npos)
        return false;
    const unsigned long long v = std::strtoull(s.c_str(), nullptr, 10);
    if (v > 0xFFFFFFFFull) return false;
    out = static_cast<u32>(v);
    return true;
}

/// Call the object's `method`, its arguments pushed by `push_args` (which
/// returns how many); a script error is logged.
template <typename PushArgs>
void callback(NetLobby& lobby, const char* method, const PushArgs& push_args) {
    lua_State* L = lobby.L;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, lobby.self_ref);
    lua_pushstring(L, method);
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return;
    }
    lua_insert(L, -2); // the method, then the object: self
    const int nargs = push_args(L);
    if (lua_pcall(L, 1 + nargs, 0, 0) != 0) {
        spdlog::warn("lobby {}: {}", method, lua_tostring(L, -1));
    }
    lua_settop(L, top);
}

void callback0(NetLobby& lobby, const char* method) {
    callback(lobby, method, [](lua_State*) { return 0; });
}

/// Everyone `uid` reaches: every player the lobby knows but `uid` (the
/// host relays, so each reaches all).
void push_established(lua_State* L, const NetLobby& lobby, u32 uid) {
    lua_newtable(L);
    int n = 0;
    const auto add = [&](u32 other) {
        if (other == uid) return;
        lua_pushstring(L, uid_string(other).c_str());
        lua_rawseti(L, -2, ++n);
    };
    add(lobby.net->local_uid());
    for (const sim::LobbyPeer& p : lobby.net->peers()) add(p.uid);
}

/// A peer as Moho's SPeer::ToLua gives it.
void push_peer(lua_State* L, const NetLobby& lobby, const sim::LobbyPeer& p, i64 now) {
    lua_newtable(L);
    lua_pushstring(L, "name");
    lua_pushstring(L, p.name.c_str());
    lua_rawset(L, -3);
    lua_pushstring(L, "id");
    lua_pushstring(L, uid_string(p.uid).c_str());
    lua_rawset(L, -3);
    lua_pushstring(L, "status");
    lua_pushstring(L, "Established");
    lua_rawset(L, -3);
    lua_pushstring(L, "ping");
    lua_pushnumber(L, p.ping_ms);
    lua_rawset(L, -3);
    lua_pushstring(L, "quiet");
    lua_pushnumber(L, p.last_heard_ms < 0 ? -1.0 : static_cast<double>(now - p.last_heard_ms));
    lua_rawset(L, -3);
    lua_pushstring(L, "establishedPeers");
    push_established(L, lobby, p.uid);
    lua_rawset(L, -3);
}

/// EstablishedPeers for every peer: who each now reaches.
void announce_established(NetLobby& lobby) {
    std::vector<u32> uids;
    for (const sim::LobbyPeer& p : lobby.net->peers()) uids.push_back(p.uid);
    for (const u32 uid : uids) {
        if (!alive(&lobby) || !lobby.net) return;
        callback(lobby, "EstablishedPeers", [&](lua_State* L) {
            lua_pushstring(L, uid_string(uid).c_str());
            push_established(L, lobby, uid);
            return 2;
        });
    }
}

void dispatch(NetLobby& lobby, const sim::LobbyEvent& e) {
    using Kind = sim::LobbyEvent::Kind;
    switch (e.kind) {
    case Kind::ConnectedToHost: {
        const std::string me = uid_string(lobby.net->local_uid());
        const std::string name = lobby.net->local_name();
        const std::string host = uid_string(lobby.net->host_uid());
        callback(lobby, "ConnectionToHostEstablished", [&](lua_State* L) {
            lua_pushstring(L, me.c_str());
            lua_pushstring(L, name.c_str());
            lua_pushstring(L, host.c_str());
            return 3;
        });
        if (alive(&lobby) && lobby.net) announce_established(lobby);
        return;
    }
    case Kind::PeerJoined: announce_established(lobby); return;
    case Kind::PeerLeft:
        // (name, id): the scripts' order, where faf-re's recovery has the other
        callback(lobby, "PeerDisconnected", [&](lua_State* L) {
            lua_pushstring(L, e.name.c_str());
            lua_pushstring(L, uid_string(e.uid).c_str());
            return 2;
        });
        if (alive(&lobby) && lobby.net) announce_established(lobby);
        return;
    case Kind::Data:
        callback(lobby, "DataReceived", [&](lua_State* L) {
            if (!push_lobby_value(L, e.payload) || !lua_istable(L, -1)) {
                spdlog::warn("lobby: from {} came data that isn't a table", e.uid);
                lua_pop(L, 1);
                lua_newtable(L);
            }
            lua_pushstring(L, "SenderID");
            lua_pushstring(L, uid_string(e.uid).c_str());
            lua_rawset(L, -3);
            lua_pushstring(L, "SenderName");
            lua_pushstring(L, e.name.c_str());
            lua_rawset(L, -3);
            return 1;
        });
        return;
    case Kind::Ejected:
        callback(lobby, "Ejected", [&](lua_State* L) {
            lua_pushstring(L, e.reason.c_str());
            return 1;
        });
        return;
    case Kind::ConnectionFailed:
        callback(lobby, "ConnectionFailed", [&](lua_State* L) {
            lua_pushstring(L, e.reason.c_str());
            return 1;
        });
        return;
    }
}

/// The object's `method`, called with no arguments; its result left on the
/// stack (nil if it has none, or fails).
void call_for_value(lua_State* L, int self_ref, const char* method) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, self_ref);
    lua_pushstring(L, method);
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        lua_pushnil(L);
        return;
    }
    lua_insert(L, -2);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        spdlog::warn("lobby {}: {}", method, lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

/// A host answering the LAN's discovery: its scripts' description of the
/// game (GameConfigRequested), its protocol and port.
void answer_discovery(NetLobby& lobby) {
    const std::vector<sim::DiscoveryAsker> askers = lobby.responder->poll();
    if (askers.empty()) return;
    lua_State* L = lobby.L;
    const int top = lua_gettop(L);
    call_for_value(L, lobby.self_ref, "GameConfigRequested");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    const std::vector<u8> config = encode_lobby_value(L, -1);
    lua_settop(L, top);
    if (!alive(&lobby) || !lobby.responder || !lobby.net) return;
    for (const sim::DiscoveryAsker& asker : askers)
        lobby.responder->answer(asker, lobby.protocol, lobby.net->port(), config);
}

/// A discovery service's callbacks: GameFound(index, config),
/// GameUpdated(index, config), RemoveGame(index); the config carries the
/// game's Address, Hostname and Protocol.
void discovery_dispatch(NetDiscovery& d, const sim::DiscoveryEvent& e) {
    lua_State* L = d.L;
    const int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, d.self_ref);
    const char* method = e.kind == sim::DiscoveryEvent::Kind::Found     ? "GameFound"
                         : e.kind == sim::DiscoveryEvent::Kind::Updated ? "GameUpdated"
                                                                        : "RemoveGame";
    lua_pushstring(L, method);
    lua_gettable(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return;
    }
    lua_insert(L, -2);
    lua_pushnumber(L, e.index);
    int nargs = 2;
    if (e.kind != sim::DiscoveryEvent::Kind::Removed) {
        if (!push_lobby_value(L, e.game.config) || !lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
        }
        lua_pushstring(L, "Address");
        lua_pushstring(L, e.game.address.c_str());
        lua_rawset(L, -3);
        lua_pushstring(L, "Hostname");
        lua_pushstring(L, e.game.hostname.c_str());
        lua_rawset(L, -3);
        lua_pushstring(L, "Protocol");
        lua_pushstring(L, e.game.protocol == 1 ? "TCP" : "UDP");
        lua_rawset(L, -3);
        ++nargs;
    }
    if (lua_pcall(L, nargs, 0, 0) != 0)
        spdlog::warn("discovery {}: {}", method, lua_tostring(L, -1));
    lua_settop(L, top);
}

} // namespace

NetLobby* net_lobby_of(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* p = static_cast<NetLobby*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    for (const auto& l : lobbies())
        if (l.get() == p && l->state == state_of(L)) {
            p->L = L; // callbacks during the method: on its caller's thread
            return p;
        }
    return nullptr;
}

void make_net_lobby(lua_State* L, int idx, const std::string& protocol, u16 port,
                    u32 max_connections, const std::string& player_name) {
    if (idx < 0) idx = lua_gettop(L) + idx + 1;
    auto lobby = std::make_unique<NetLobby>();
    lobby->state = state_of(L);
    lobby->L = L;
    lobby->protocol = protocol == "TCP" ? 1 : 2;
    lobby->port = port;
    lobby->max_connections = max_connections;
    lobby->player_name = player_name.substr(0, kMaxPlayerName);
    if (lobby->player_name.empty()) lobby->player_name = "Player";
    lua_pushvalue(L, idx);
    lobby->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, lobby.get());
    lua_rawset(L, idx);
    lobbies().push_back(std::move(lobby));
}

int net_lobby_HostGame(lua_State* L, NetLobby& lobby) {
    if (lobby.net) return 0; // already hosting or joined
    const auto hosted_time =
        static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count());
    auto net = std::make_unique<sim::LobbyNet>(lobby.player_name, lobby.max_connections);
    if (!net->host(lobby.port, hosted_time))
        return luaL_error(L, "HostGame: can't listen on port %d", static_cast<int>(lobby.port));
    lobby.net = std::move(net);
    lobby.player_name = lobby.net->local_name();
    // The LAN's discovery finds it (M218b); a second host on one machine
    // can't listen, as Moho's couldn't
    lobby.responder = std::make_unique<sim::DiscoveryResponder>();
    if (!lobby.responder->open(discovery_port())) {
        spdlog::warn("lobby: can't answer the LAN's discovery on port {}: someone else must be "
                     "hosting a game on this machine",
                     discovery_port());
        lobby.responder.reset();
    }
    // Hosting() comes at the next pump, once the scripts' own setup is done
    lobby.hosting_pending = true;
    spdlog::info("lobby: hosting on port {}", lobby.net->port());
    return 0;
}

int net_lobby_JoinGame(lua_State* L, NetLobby& lobby) {
    if (lobby.net) return 0;
    const std::string address = lua_type(L, 2) == LUA_TSTRING ? lua_tostring(L, 2) : "";
    if (lua_type(L, 3) == LUA_TSTRING) {
        lobby.player_name = std::string(lua_tostring(L, 3)).substr(0, kMaxPlayerName);
    }
    // "a.b.c.d:port"
    const size_t colon = address.rfind(':');
    std::string host = colon == std::string::npos ? address : address.substr(0, colon);
    if (host == "localhost") host = "127.0.0.1";
    const std::string port_text = colon == std::string::npos ? "" : address.substr(colon + 1);
    const bool port_ok = !port_text.empty() &&
                         port_text.find_first_not_of("0123456789") == std::string::npos &&
                         port_text.size() <= 5 && std::stoul(port_text) <= 65535;
    auto net = std::make_unique<sim::LobbyNet>(lobby.player_name, lobby.max_connections);
    if (!port_ok || !net->join(host, static_cast<u16>(std::stoul(port_ok ? port_text : "0")))) {
        spdlog::warn("lobby: can't join '{}'", address);
        lobby.join_failed = true; // ConnectionFailed at the next pump
        return 0;
    }
    lobby.net = std::move(net);
    spdlog::info("lobby: joining {}", address);
    return 0;
}

int net_lobby_SendData(lua_State* L, NetLobby& lobby) {
    u32 to = 0;
    if (!lobby.net || !parse_uid(L, 2, to)) return 0;
    lobby.net->send(to, encode_lobby_value(L, 3));
    return 0;
}

int net_lobby_BroadcastData(lua_State* L, NetLobby& lobby) {
    if (lobby.net) lobby.net->broadcast(encode_lobby_value(L, 2));
    return 0;
}

int net_lobby_GetPeers(lua_State* L, NetLobby& lobby) {
    lua_newtable(L);
    if (!lobby.net) return 1;
    const i64 now = net_lobby_clock_ms();
    int n = 0;
    for (const sim::LobbyPeer& p : lobby.net->peers()) {
        push_peer(L, lobby, p, now);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

int net_lobby_GetPeer(lua_State* L, NetLobby& lobby) {
    u32 uid = 0;
    const sim::LobbyPeer* p = lobby.net && parse_uid(L, 2, uid) ? lobby.net->peer(uid) : nullptr;
    if (!p) {
        lua_pushnil(L);
        return 1;
    }
    push_peer(L, lobby, *p, net_lobby_clock_ms());
    return 1;
}

int net_lobby_GetLocalPlayerID(lua_State* L, NetLobby& lobby) {
    const bool known = lobby.net && (lobby.net->hosting() || lobby.net->joined());
    lua_pushstring(L, known ? uid_string(lobby.net->local_uid()).c_str() : "-1");
    return 1;
}

int net_lobby_GetLocalPlayerName(lua_State* L, NetLobby& lobby) {
    lua_pushstring(L, (lobby.net ? lobby.net->local_name() : lobby.player_name).c_str());
    return 1;
}

int net_lobby_GetLocalPort(lua_State* L, NetLobby& lobby) {
    if (lobby.net) lua_pushnumber(L, lobby.net->port());
    else lua_pushnil(L);
    return 1;
}

int net_lobby_IsHost(lua_State* L, NetLobby& lobby) {
    // Moho's: no connection to a host
    lua_pushboolean(L, !lobby.net || lobby.net->hosting());
    return 1;
}

int net_lobby_EjectPeer(lua_State* L, NetLobby& lobby) {
    u32 uid = 0;
    if (!lobby.net || !lobby.net->hosting() || !parse_uid(L, 2, uid)) return 0;
    const std::string reason = lua_type(L, 3) == LUA_TSTRING ? lua_tostring(L, 3) : "KickedByHost";
    lobby.net->eject(uid, reason);
    return 0;
}

int net_lobby_MakeValidPlayerName(lua_State* L, NetLobby& lobby) {
    u32 uid = 0;
    const bool has_uid = parse_uid(L, 2, uid);
    const std::string name = lua_type(L, 3) == LUA_TSTRING ? lua_tostring(L, 3) : "";
    if (lobby.net)
        lua_pushstring(
            L, lobby.net->valid_player_name(has_uid ? uid : lobby.net->local_uid(), name).c_str());
    else
        lua_pushstring(
            L, (name.empty() ? std::string("Player") : name.substr(0, kMaxPlayerName)).c_str());
    return 1;
}

namespace {

/// The scenario at `path`, as the lobby's LaunchGame reads it (Moho's
/// WLD_LoadScenarioInfo; retail's MapUtil.LoadScenario does the same): its
/// ScenarioInfo left on the stack, or nil.
void push_scenario_info(lua_State* L, const std::string& path) {
    // (A function of the path: Lua 5.0's main chunks take no `...`)
    static const char* const kLoad = R"(
        return function(path)
            local env = {}
            doscript('/lua/dataInit.lua', env)
            doscript(path, env)
            return env.ScenarioInfo
        end
    )";
    if (luaL_loadbuffer(L, kLoad, std::strlen(kLoad), "=LaunchGame") != 0 ||
        lua_pcall(L, 0, 1, 0) != 0) {
        spdlog::error("lobby LaunchGame: the scenario reader: {}", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_pushnil(L);
        return;
    }
    lua_pushstring(L, path.c_str());
    if (lua_pcall(L, 1, 1, 0) != 0) {
        spdlog::warn("lobby LaunchGame: can't read {}: {}", path, lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

bool is_ffa(const char* name) {
    return name && std::strlen(name) == 3 &&
           std::toupper(static_cast<unsigned char>(name[0])) == 'F' &&
           std::toupper(static_cast<unsigned char>(name[1])) == 'F' &&
           std::toupper(static_cast<unsigned char>(name[2])) == 'A';
}

/// How many armies the scenario's FFA team has (ScenarioInfo at `info`);
/// none without an FFA team, nullopt without its standard configuration's
/// teams (Moho's "NoConfig").
std::optional<size_t> ffa_army_count(lua_State* L, int info) {
    const int top = lua_gettop(L);
    lua_pushvalue(L, info);
    for (const char* key : {"Configurations", "standard", "teams"}) {
        if (!lua_istable(L, -1)) {
            lua_settop(L, top);
            return std::nullopt;
        }
        lua_pushstring(L, key);
        lua_gettable(L, -2);
    }
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        return std::nullopt;
    }
    const int teams = lua_gettop(L);
    size_t count = 0;
    lua_pushnil(L);
    while (lua_next(L, teams) != 0) {
        if (lua_istable(L, -1)) {
            const int team = lua_gettop(L);
            lua_pushstring(L, "name");
            lua_gettable(L, team);
            const bool ffa = lua_type(L, -1) == LUA_TSTRING && is_ffa(lua_tostring(L, -1));
            lua_pop(L, 1);
            if (ffa) {
                lua_pushstring(L, "armies");
                lua_gettable(L, team);
                if (lua_istable(L, -1)) {
                    const int armies = lua_gettop(L);
                    lua_pushnil(L);
                    while (lua_next(L, armies) != 0) {
                        if (lua_type(L, -1) == LUA_TSTRING) ++count;
                        lua_pop(L, 1);
                    }
                }
                break; // the first FFA team
            }
        }
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    return count;
}

/// A launch's command sources, as Moho's LaunchGame assigns them: one per
/// owner, the humans' by slot and then the observers'.
struct LaunchSources {
    std::vector<u32> owners;        ///< source s's owner (a lobby uid)
    std::vector<i32> armies;        ///< source s's army (-1: an observer's)
    std::vector<std::string> names; ///< source s's PlayerName in the config
    size_t players = 0;             ///< PlayerOptions' entries, AIs too
};

LaunchSources read_launch_sources(lua_State* L, int cfg) {
    struct Entry {
        double key = 0; ///< the slot (the observer's place)
        bool human = true;
        bool owned = false;
        u32 owner = 0;
        std::string name;
    };
    // Each entry of the table at cfg[field] that is a table: its key, and
    // Human and OwnerID
    const auto read = [&](const char* field) {
        std::vector<Entry> entries;
        const int top = lua_gettop(L);
        lua_pushstring(L, field);
        lua_gettable(L, cfg);
        if (lua_istable(L, -1)) {
            const int t = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, t) != 0) {
                if (lua_istable(L, -1)) {
                    Entry e;
                    e.key = lua_type(L, -2) == LUA_TNUMBER
                                ? lua_tonumber(L, -2)
                                : std::numeric_limits<double>::infinity();
                    lua_pushstring(L, "Human");
                    lua_gettable(L, -2);
                    e.human = lua_toboolean(L, -1) != 0;
                    lua_pop(L, 1);
                    lua_pushstring(L, "OwnerID");
                    lua_gettable(L, -2);
                    e.owned = parse_uid(L, lua_gettop(L), e.owner);
                    lua_pop(L, 1);
                    lua_pushstring(L, "PlayerName");
                    lua_gettable(L, -2);
                    if (lua_type(L, -1) == LUA_TSTRING) e.name = lua_tostring(L, -1);
                    lua_pop(L, 1);
                    entries.push_back(e);
                }
                lua_pop(L, 1);
            }
        }
        lua_settop(L, top);
        // In their places: every peer orders them alike, however its copy
        // of the table was built
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            return a.key < b.key || (a.key == b.key && a.owner < b.owner);
        });
        return entries;
    };
    LaunchSources out;
    const auto add = [&](u32 owner, i32 army, const std::string& name) {
        if (std::find(out.owners.begin(), out.owners.end(), owner) != out.owners.end()) return;
        out.owners.push_back(owner);
        out.armies.push_back(army);
        out.names.push_back(name);
    };
    const std::vector<Entry> players = read("PlayerOptions");
    out.players = players.size();
    // The armies are the slots taken, in order (Moho's: players in slots 1
    // and 5 play armies 0 and 1); a slot that is no number plays none
    i32 army = 0;
    for (const Entry& e : players) {
        const bool slot = e.key != std::numeric_limits<double>::infinity();
        if (e.human && e.owned) add(e.owner, slot ? army : -1, e.name);
        if (slot) ++army;
    }
    for (const Entry& e : read("Observers"))
        if (e.owned) add(e.owner, -1, e.name);
    return out;
}

} // namespace

int net_lobby_LaunchGame(lua_State* L, NetLobby& lobby) {
    // What's wrong, as LaunchFailed(reason) tells it
    const auto fail = [&](const char* reason) {
        callback(lobby, "LaunchFailed", [&](lua_State* s) {
            lua_pushstring(s, reason);
            return 1;
        });
        return 0;
    };
    if (!lobby.net || !(lobby.net->hosting() || lobby.net->joined()) || !lua_istable(L, 2)) {
        spdlog::warn("lobby LaunchGame: not in a lobby, or no config");
        return fail("");
    }
    // The scenario: GameOptions.ScenarioFile
    std::string scenario;
    lua_pushstring(L, "GameOptions");
    lua_gettable(L, 2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "ScenarioFile");
        lua_gettable(L, -2);
        if (lua_type(L, -1) == LUA_TSTRING) scenario = lua_tostring(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (scenario.empty()) {
        spdlog::warn("lobby LaunchGame: the config names no GameOptions.ScenarioFile");
        return fail("");
    }
    push_scenario_info(L, scenario);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        return fail("");
    }
    const std::optional<size_t> spots = ffa_army_count(L, lua_gettop(L));
    lua_pop(L, 1);
    if (!spots) return fail("NoConfig");
    const LaunchSources sources = read_launch_sources(L, 2);
    if (sources.players > *spots) return fail("StartSpots");
    const auto mine =
        std::find(sources.owners.begin(), sources.owners.end(), lobby.net->local_uid());
    if (mine == sources.owners.end()) {
        spdlog::warn("lobby LaunchGame: this player (uid {}) has no slot and isn't observing",
                     lobby.net->local_uid());
        return fail("");
    }
    lua_pushstring(L, "LaunchSinglePlayerSession");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        spdlog::warn("lobby LaunchGame: no session to launch (LaunchSinglePlayerSession)");
        return fail("");
    }

    // The lobby's connections become the game's, seeded by the host's
    // time: every player has it from their welcome
    if (lobby.net->hosting()) lobby.net->stop_joining();
    lobby.responder.reset(); // no longer a game to find
    const u64 seed = lobby.net->hosted_time();
    const auto local = static_cast<u32>(mine - sources.owners.begin());
    // Each client by its lobby name, as Moho's are (the config's, for one
    // the lobby no longer has)
    std::vector<SessionClient> clients;
    for (size_t s = 0; s < sources.owners.size(); ++s) {
        const u32 uid = sources.owners[s];
        const sim::LobbyPeer* peer = lobby.net->peer(uid);
        std::string name = uid == lobby.net->local_uid() ? lobby.net->local_name()
                           : peer                        ? peer->name
                                                         : sources.names[s];
        clients.push_back({uid, name.empty() ? "Player" : name});
    }
    mp_begin_lobby_game(
        std::make_unique<sim::LobbyGameTransport>(std::move(lobby.net), net_lobby_clock_ms), local,
        sources.armies, std::move(clients), seed);
    // The session, as single-player's starts: the frame loop loads it
    lua_pushvalue(L, 2);
    if (lua_pcall(L, 1, 0, 0) != 0) {
        spdlog::warn("lobby LaunchGame: {}", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    // Last: the scripts tear their lobby down here (retail's destroys this
    // object), so nothing after may touch it
    callback0(lobby, "GameLaunched");
    return 0;
}

int net_lobby_Destroy(lua_State* L, NetLobby& lobby) {
    auto& all = lobbies();
    const auto it =
        std::find_if(all.begin(), all.end(), [&](const auto& l) { return l.get() == &lobby; });
    if (it == all.end()) return 0;
    luaL_unref(L, LUA_REGISTRYINDEX, lobby.self_ref);
    all.erase(it); // its sockets close
    forget_object(L);
    return 0;
}

int net_lobby_DebugDump(lua_State* L, NetLobby& lobby) {
    (void)L;
    if (!lobby.net) {
        spdlog::info("lobby: not hosting or joined");
        return 0;
    }
    spdlog::info("lobby: {} (uid {}), {} peers", lobby.net->local_name(), lobby.net->local_uid(),
                 lobby.net->peers().size());
    for (const sim::LobbyPeer& p : lobby.net->peers())
        spdlog::info("  {} (uid {}), ping {} ms", p.name, p.uid, p.ping_ms);
    return 0;
}

void pump_net_lobbies(lua_State* L, i64 now_ms) {
    std::vector<NetLobby*> mine;
    for (const auto& l : lobbies())
        if (l->state == state_of(L)) mine.push_back(l.get());
    for (NetLobby* lobby : mine) {
        // A callback may destroy any lobby: check each still is
        if (!alive(lobby)) continue;
        lobby->L = L;
        if (lobby->hosting_pending) {
            lobby->hosting_pending = false;
            callback0(*lobby, "Hosting");
            if (!alive(lobby)) continue;
        }
        if (lobby->join_failed) {
            lobby->join_failed = false;
            callback(*lobby, "ConnectionFailed", [](lua_State* s) {
                lua_pushstring(s, "HostLeft");
                return 1;
            });
            continue;
        }
        if (!lobby->net) continue;
        const std::vector<sim::LobbyEvent> events = lobby->net->poll(now_ms);
        for (const sim::LobbyEvent& e : events) {
            if (!alive(lobby) || !lobby->net) break;
            dispatch(*lobby, e);
        }
        if (alive(lobby) && lobby->responder && lobby->net) answer_discovery(*lobby);
    }
    std::vector<NetDiscovery*> finders;
    for (const auto& d : discoveries())
        if (d->state == state_of(L)) finders.push_back(d.get());
    for (NetDiscovery* d : finders) {
        if (!alive(d)) continue;
        d->L = L;
        const std::vector<sim::DiscoveryEvent> events = d->finder.poll(now_ms);
        for (const sim::DiscoveryEvent& e : events) {
            if (!alive(d)) break;
            discovery_dispatch(*d, e);
        }
    }
}

void close_net_lobbies(lua_State* L) {
    auto& all = lobbies();
    for (auto it = all.begin(); it != all.end();) {
        if ((*it)->state == state_of(L)) {
            luaL_unref(L, LUA_REGISTRYINDEX, (*it)->self_ref);
            it = all.erase(it);
        } else {
            ++it;
        }
    }
    auto& finders = discoveries();
    for (auto it = finders.begin(); it != finders.end();) {
        if ((*it)->state == state_of(L)) {
            luaL_unref(L, LUA_REGISTRYINDEX, (*it)->self_ref);
            it = finders.erase(it);
        } else {
            ++it;
        }
    }
}

NetDiscovery* net_discovery_of(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* p = static_cast<NetDiscovery*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    for (const auto& d : discoveries())
        if (d.get() == p && d->state == state_of(L)) {
            p->L = L;
            return p;
        }
    return nullptr;
}

void make_net_discovery(lua_State* L, int idx) {
    if (idx < 0) idx = lua_gettop(L) + idx + 1;
    auto d = std::make_unique<NetDiscovery>(discovery_address(), discovery_port());
    if (!d->finder.open()) spdlog::warn("discovery: no socket to ask the LAN with");
    d->state = state_of(L);
    d->L = L;
    lua_pushvalue(L, idx);
    d->self_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, d.get());
    lua_rawset(L, idx);
    discoveries().push_back(std::move(d));
}

int net_discovery_GetGameCount(lua_State* L, NetDiscovery& d) {
    lua_pushnumber(L, static_cast<double>(d.finder.game_count()));
    return 1;
}

int net_discovery_Reset(lua_State* L, NetDiscovery& d) {
    (void)L;
    for (const sim::DiscoveryEvent& e : d.finder.reset()) {
        if (!alive(&d)) break;
        discovery_dispatch(d, e);
    }
    return 0;
}

int net_discovery_Destroy(lua_State* L, NetDiscovery& d) {
    auto& all = discoveries();
    const auto it =
        std::find_if(all.begin(), all.end(), [&](const auto& x) { return x.get() == &d; });
    if (it == all.end()) return 0;
    luaL_unref(L, LUA_REGISTRYINDEX, d.self_ref);
    all.erase(it);
    forget_object(L);
    return 0;
}

void set_lan_discovery(const std::string& broadcast_address, u16 port) {
    discovery_address() = broadcast_address;
    discovery_port() = port;
}

i64 net_lobby_clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace osc::lua
