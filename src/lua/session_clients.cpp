#include "lua/session_clients.hpp"

#include "lua/lobby_wire.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/net_lobby.hpp"
#include "sim/army_brain.hpp"
#include "sim/lobby_net.hpp"
#include "sim/sim_state.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace osc::lua {

namespace {

/// Moho's limit on a chat message, serialized.
constexpr size_t kMaxChatBytes = 1024;
/// Moho's clients report 50 as their sim rate unless told otherwise.
constexpr int kMaxSimRate = 50;

/// A chat message for the local client: who sent it, and its bytes.
struct Chat {
    std::string sender;
    std::vector<u8> bytes;
};

std::vector<Chat>& local_chat() {
    static std::vector<Chat> queue;
    return queue;
}

sim::SimState* sim_of(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return sim;
}

/// A single-player game's one client's name: its player's army's.
std::string single_player_name(lua_State* L) {
    lua_pushstring(L, "__osc_focus_army");
    lua_rawget(L, LUA_REGISTRYINDEX);
    const int focus = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);
    const sim::SimState* sim = sim_of(L);
    const sim::ArmyBrain* brain =
        sim && focus >= 0 ? sim->army_at(static_cast<size_t>(focus)) : nullptr;
    return brain && !brain->nickname().empty() ? brain->nickname() : "Player";
}

/// The name of client `index` (0-based).
std::string client_name(lua_State* L, size_t index) {
    const auto& mp = mp_net_state();
    if (index < mp.clients.size()) return mp.clients[index].name;
    if (mp.active()) return "Player " + std::to_string(index + 1);
    return single_player_name(L);
}

bool is_local(size_t index) {
    const auto& mp = mp_net_state();
    return !session_is_multiplayer() || index == mp.local_source;
}

void set_number(lua_State* L, const char* key, double v) {
    lua_pushstring(L, key);
    lua_pushnumber(L, v);
    lua_rawset(L, -3);
}

void set_bool(lua_State* L, const char* key, bool v) {
    lua_pushstring(L, key);
    lua_pushboolean(L, v ? 1 : 0);
    lua_rawset(L, -3);
}

/// The 0-based clients `SessionSendChatMessage`'s selector at `idx` names
/// (1-based indices, one or a table): Lua errors as Moho's for others.
std::vector<size_t> chosen_clients(lua_State* L, int idx, size_t count) {
    std::vector<size_t> out;
    const auto add = [&](int value_idx) {
        if (!lua_isnumber(L, value_idx))
            luaL_error(L, "Invalid value for client-or-clients: must be an integer or a table of "
                          "integers");
        const double k = lua_tonumber(L, value_idx);
        if (!(k >= 1 && k <= static_cast<double>(count))) {
            // (shown clamped: casting a huge or NaN index to int is undefined)
            const int shown = !(k == k)  ? 0
                              : k > 1e9  ? 1000000000
                              : k < -1e9 ? -1000000000
                                         : static_cast<int>(k);
            luaL_error(L, "Invalid client index. Must be between 1 and %d inclusive, not %d.",
                       static_cast<int>(count), shown);
        }
        const auto index = static_cast<size_t>(k) - 1;
        if (std::find(out.begin(), out.end(), index) == out.end()) out.push_back(index);
    };
    if (lua_istable(L, idx)) {
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            add(lua_gettop(L));
            lua_pop(L, 1);
        }
    } else {
        add(idx);
    }
    return out;
}

} // namespace

bool session_is_multiplayer() {
    const auto& mp = mp_net_state();
    return mp.active() || mp.lobby_transport != nullptr;
}

size_t session_client_count(lua_State* L) {
    const auto& mp = mp_net_state();
    if (!mp.clients.empty()) return mp.clients.size();
    if (mp.active()) return mp.all_sources.size();
    return sim_of(L) ? 1 : 0;
}

int push_session_clients(lua_State* L) {
    const size_t count = session_client_count(L);
    if (count == 0) {
        lua_pushnil(L);
        return 1;
    }
    const auto& mp = mp_net_state();
    const i64 now = net_lobby_clock_ms();
    lua_newtable(L);
    for (size_t i = 0; i < count; ++i) {
        const bool local = is_local(i);
        // A player over the lobby's connections: its peer, while it has one
        const sim::LobbyPeer* peer = nullptr;
        u32 uid = static_cast<u32>(i);
        if (i < mp.clients.size()) {
            uid = mp.clients[i].uid;
            if (!local && mp.lobby_game) peer = mp.lobby_game->net().peer(uid);
        }
        const bool connected = local || peer || mp.clients.empty();
        lua_newtable(L);
        lua_pushstring(L, "name");
        lua_pushstring(L, client_name(L, i).c_str());
        lua_rawset(L, -3);
        lua_pushstring(L, "uid");
        lua_pushstring(L, std::to_string(uid).c_str());
        lua_rawset(L, -3);
        set_bool(L, "connected", connected);
        set_bool(L, "local", local);
        set_number(L, "ping", peer ? static_cast<double>(peer->ping_ms) : 0.0);
        // ms since anything came (-1: nothing yet); the local client's 0
        set_number(L, "quiet",
                   local ? 0.0
                   : peer && peer->last_heard_ms >= 0
                       ? static_cast<double>(now - peer->last_heard_ms)
                       : -1.0);
        set_number(L, "maxSP", kMaxSimRate);
        lua_pushstring(L, "authorizedCommandSources");
        lua_newtable(L);
        if (connected) {
            lua_pushnumber(L, static_cast<double>(i + 1));
            lua_rawseti(L, -2, 1);
        }
        lua_rawset(L, -3);
        lua_pushstring(L, "ejectedBy");
        lua_newtable(L);
        lua_rawset(L, -3);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

bool push_network_source_names(lua_State* L) {
    const auto& mp = mp_net_state();
    if (mp.clients.empty()) return false;
    lua_newtable(L);
    for (size_t i = 0; i < mp.clients.size(); ++i) {
        lua_pushstring(L, mp.clients[i].name.c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return true;
}

void send_session_chat(lua_State* L) {
    const int args = lua_gettop(L);
    const size_t count = session_client_count(L);
    if (args < 1 || count == 0) return;
    const int msg = args >= 2 ? 2 : 1;
    std::vector<size_t> to;
    if (args < 2 || lua_isnil(L, 1)) {
        for (size_t i = 0; i < count; ++i) to.push_back(i);
    } else {
        to = chosen_clients(L, 1, count);
    }
    const std::vector<u8> bytes = encode_lobby_value(L, msg);
    if (bytes.size() > kMaxChatBytes) luaL_error(L, "Message too long.");
    const auto& mp = mp_net_state();
    for (const size_t i : to) {
        if (is_local(i)) {
            local_chat().push_back({client_name(L, i), bytes});
        } else if (i < mp.clients.size() && mp.lobby_game) {
            mp.lobby_game->send_data(mp.clients[i].uid, bytes);
        }
    }
}

void reset_session_chat() {
    local_chat().clear();
}

void pump_session_chat(lua_State* L) {
    // gamemain.lua's ReceiveChat, once the game's interface has loaded it
    const int top = lua_gettop(L);
    lua_pushstring(L, "__modules");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "/lua/ui/game/gamemain.lua");
        lua_rawget(L, -2);
    }
    if (!lua_istable(L, -1)) {
        lua_settop(L, top);
        return;
    }
    const int gamemain = lua_gettop(L);

    std::vector<Chat> arrived;
    std::swap(arrived, local_chat());
    auto& mp = mp_net_state();
    if (mp.lobby_game) {
        for (sim::LobbyEvent& e : mp.lobby_game->take_data()) {
            // Its client's name, as Moho's sender is
            const auto client =
                std::find_if(mp.clients.begin(), mp.clients.end(),
                             [&](const SessionClient& c) { return c.uid == e.uid; });
            arrived.push_back(
                {client != mp.clients.end() ? client->name : e.name, std::move(e.payload)});
        }
    }
    for (const Chat& chat : arrived) {
        lua_pushstring(L, "ReceiveChat");
        lua_gettable(L, gamemain);
        if (!lua_isfunction(L, -1)) {
            lua_settop(L, top);
            return;
        }
        lua_pushstring(L, chat.sender.c_str());
        if (!push_lobby_value(L, chat.bytes)) {
            spdlog::warn("chat from {}: not a message", chat.sender);
            lua_pop(L, 3);
            continue;
        }
        if (lua_pcall(L, 2, 0, 0) != 0) {
            spdlog::warn("ReceiveChat: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_settop(L, top);
}

} // namespace osc::lua
