#include "lua/net_lobby.hpp"

#include "lua/lobby_wire.hpp"
#include "sim/lobby_net.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace osc::lua {

struct NetLobby {
    lua_State* L = nullptr;
    int self_ref = LUA_NOREF; ///< the scripts' object, for its callbacks
    u16 port = 0;
    u32 max_connections = 8;
    std::string player_name;
    std::unique_ptr<sim::LobbyNet> net;
    bool hosting_pending = false; ///< Hosting() at the next pump
    bool join_failed = false;     ///< ConnectionFailed at the next pump
};

namespace {

/// Moho caps a game's name at 32 characters and a player's at 24.
constexpr size_t kMaxPlayerName = 24;

std::vector<std::unique_ptr<NetLobby>>& lobbies() {
    static std::vector<std::unique_ptr<NetLobby>> all;
    return all;
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

} // namespace

NetLobby* net_lobby_of(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* p = static_cast<NetLobby*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    for (const auto& l : lobbies())
        if (l.get() == p && l->L == L) return p;
    return nullptr;
}

void make_net_lobby(lua_State* L, int idx, u16 port, u32 max_connections,
                    const std::string& player_name) {
    if (idx < 0) idx = lua_gettop(L) + idx + 1;
    auto lobby = std::make_unique<NetLobby>();
    lobby->L = L;
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

int net_lobby_LaunchGame(lua_State* L, NetLobby& lobby) {
    (void)L;
    // Starting a networked game is M218c's; until then the scripts hear
    // that it failed rather than each player starting a game of their own.
    spdlog::warn("lobby: a networked game can't be launched yet");
    callback(lobby, "LaunchFailed", [](lua_State* s) {
        lua_pushstring(s, "");
        return 1;
    });
    return 0;
}

int net_lobby_Destroy(lua_State* L, NetLobby& lobby) {
    auto& all = lobbies();
    const auto it =
        std::find_if(all.begin(), all.end(), [&](const auto& l) { return l.get() == &lobby; });
    if (it == all.end()) return 0;
    luaL_unref(L, LUA_REGISTRYINDEX, lobby.self_ref);
    all.erase(it); // its sockets close
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
        if (l->L == L) mine.push_back(l.get());
    for (NetLobby* lobby : mine) {
        // A callback may destroy any lobby: check each still is
        if (!alive(lobby)) continue;
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
    }
}

void close_net_lobbies(lua_State* L) {
    auto& all = lobbies();
    for (auto it = all.begin(); it != all.end();) {
        if ((*it)->L == L) {
            luaL_unref(L, LUA_REGISTRYINDEX, (*it)->self_ref);
            it = all.erase(it);
        } else {
            ++it;
        }
    }
}

i64 net_lobby_clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace osc::lua
