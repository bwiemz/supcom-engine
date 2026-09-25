// The lobby, LAN discovery and the world-UI provider.
// Split out of moho_bindings.cpp by class (M191 step 2); what the files
// share is declared in lua/moho_bindings_internal.hpp.

#include "lua/moho_bindings.hpp"
#include "lua/moho_bindings_internal.hpp"
#include "lua/lua_stubs.hpp"
#include "core/dmath.hpp"
#include "sim/blueprint_categories.hpp"
#include "lua/category_utils.hpp"
#include "video/video_decoder.hpp"
#include "map/scmap_parser.hpp"
#include "lua/factory_queue.hpp"
#include "lua/order_helpers.hpp"
#include "lua/lua_state.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/army_brain.hpp"
#include "sim/build_placement.hpp"
#include "sim/bone_data.hpp"
#include "sim/collision.hpp"
#include "sim/entity.hpp"
#include "sim/entity_registry.hpp"
#include "sim/ieffect.hpp"
#include "sim/manipulator.hpp"
#include "core/test_status.hpp"
#include "sim/category_expr.hpp"
#include "sim/prop.hpp"
#include "sim/prop_script.hpp"
#include "sim/sim_state.hpp"
#include "sim/collision_beam.hpp"
#include "sim/projectile_script.hpp"
#include "sim/thread_manager.hpp"
#include "map/visibility_grid.hpp"
#include "sim/unit.hpp"
#include "sim/navigator.hpp"
#include "sim/platoon.hpp"
#include "sim/projectile.hpp"
#include "sim/shield.hpp"
#include "sim/unit_command.hpp"
#include "map/pathfinder.hpp"
#include "map/pathfinding_grid.hpp"
#include "sim/weapon.hpp"
#include "blueprints/blueprint_store.hpp"
#include "audio/sound_manager.hpp"
#include "ui/ui_control.hpp"
#include "ui/world_view.hpp"
#include "ui/font_metrics_provider.hpp"
#include "ui/keymap.hpp"
#include "ui/wld_ui_provider.hpp"
#include "sim/sim_callback_queue.hpp"
#include "map/terrain.hpp"
#include "vfs/virtual_file_system.hpp"
#include "core/front_end_data.hpp"
#include "core/game_state.hpp"
#include "core/localization.hpp"
#include "core/preferences.hpp"
#include "lua/beat_system.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/sim_sync.hpp"
#include <algorithm>
#include <cctype>
#include <map>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>
#include <spdlog/spdlog.h>
#include <lua.h>
#include <lauxlib.h>

#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

// --- WldUIProvider methods (M76 → M135g) ---

// Moho's WldUIProvider class carries no engine-side CreateGameInterface /
// loading-dialog methods: those are names the engine *calls* on the Lua
// object (ui::WldUIProvider::call_method). Defining them here too would
// recurse through the engine for any provider that doesn't override one.
static int wlduiprovider_Destroy(lua_State* /*L*/) { return 0; }

// clang-format off
const MethodEntry ui_wlduiprovider_methods[] = {
    {"Destroy", wlduiprovider_Destroy},
    {nullptr, nullptr},
};
// clang-format on

// --- Discovery service methods (M76) ---

static void ensure_table_field(lua_State* L, int table_idx, const char* key) {
    if (table_idx < 0) table_idx = lua_gettop(L) + table_idx + 1;
    lua_pushstring(L, key);
    lua_rawget(L, table_idx);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, key);
        lua_pushvalue(L, -2);
        lua_rawset(L, table_idx);
    }
}

static int discovery_GetGameCount(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushnumber(L, 0);
        return 1;
    }
    ensure_table_field(L, 1, "__osc_games");
    int count = sequence_count(L, -1);
    lua_pop(L, 1);
    lua_pushnumber(L, count);
    return 1;
}

static int discovery_GetGame(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushnil(L);
        return 1;
    }
    int index = static_cast<int>(luaL_checknumber(L, 2));
    ensure_table_field(L, 1, "__osc_games");
    lua_rawgeti(L, -1, index);
    lua_remove(L, -2);
    return 1;
}

static int discovery_AddGame(lua_State* L) {
    if (!lua_istable(L, 1) || !lua_istable(L, 2)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    ensure_table_field(L, 1, "__osc_games");
    int next = sequence_count(L, -1) + 1;
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, next);
    lua_pop(L, 1);
    lua_pushboolean(L, 1);
    return 1;
}

static int discovery_Reset(lua_State* L) {
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "__osc_games");
        lua_newtable(L);
        lua_rawset(L, 1);
    }
    return 0;
}

static int discovery_Destroy(lua_State* L) {
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "__osc_destroyed");
        lua_pushboolean(L, 1);
        lua_rawset(L, 1);
    }
    return 0;
}

// clang-format off
const MethodEntry ui_discovery_methods[] = {
    {"AddGame",      discovery_AddGame},
    {"GetGame",      discovery_GetGame},
    {"GetGameCount", discovery_GetGameCount},
    {"Reset",        discovery_Reset},
    {"Destroy",      discovery_Destroy},
    {nullptr, nullptr},
};
// clang-format on

// --- Lobby methods (M76) ---

static int lobby_SendData(lua_State* L);
static int lobby_BroadcastData(lua_State* L) { return lobby_SendData(L); }

static const char* lobby_arg_string(lua_State* L, int idx, const char* fallback) {
    return lua_type(L, idx) == LUA_TSTRING ? lua_tostring(L, idx) : fallback;
}

static std::string lobby_arg_id(lua_State* L, int idx, const char* fallback) {
    if (lua_type(L, idx) == LUA_TSTRING) return lua_tostring(L, idx);
    if (lua_type(L, idx) == LUA_TNUMBER) {
        std::ostringstream ss;
        ss << static_cast<int>(lua_tonumber(L, idx));
        return ss.str();
    }
    return fallback;
}

static int lua_abs_index(lua_State* L, int idx) {
    return idx > 0 ? idx : lua_gettop(L) + idx + 1;
}

static void lobby_default_number_field(lua_State* L, int table_idx,
                                       const char* key, double value) {
    table_idx = lua_abs_index(L, table_idx);
    lua_pushstring(L, key);
    lua_rawget(L, table_idx);
    const bool missing = lua_isnil(L, -1);
    lua_pop(L, 1);
    if (missing) {
        lua_pushstring(L, key);
        lua_pushnumber(L, value);
        lua_rawset(L, table_idx);
    }
}

static void lobby_default_string_field(lua_State* L, int table_idx,
                                       const char* key, const char* value) {
    table_idx = lua_abs_index(L, table_idx);
    lua_pushstring(L, key);
    lua_rawget(L, table_idx);
    const bool missing = lua_isnil(L, -1);
    lua_pop(L, 1);
    if (missing) {
        lua_pushstring(L, key);
        lua_pushstring(L, value);
        lua_rawset(L, table_idx);
    }
}

static void lobby_sanitize_peer_table(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return;
    lobby_default_number_field(L, idx, "quiet", 0);
    lobby_default_number_field(L, idx, "ping", 0);
    lobby_default_string_field(L, idx, "status", "Established");
}

static void lobby_push_peers(lua_State* L, int lobby_idx) {
    ensure_table_field(L, lobby_idx, "__osc_peers");
}

static void lobby_store_peer(lua_State* L, int lobby_idx, const char* id,
                             const char* name, double port) {
    lobby_push_peers(L, lobby_idx);
    lua_newtable(L);
    lua_pushstring(L, "ID"); lua_pushstring(L, id); lua_rawset(L, -3);
    lua_pushstring(L, "id"); lua_pushstring(L, id); lua_rawset(L, -3);
    lua_pushstring(L, "Name"); lua_pushstring(L, name); lua_rawset(L, -3);
    lua_pushstring(L, "name"); lua_pushstring(L, name); lua_rawset(L, -3);
    lua_pushstring(L, "Port"); lua_pushnumber(L, port); lua_rawset(L, -3);
    lua_pushstring(L, "port"); lua_pushnumber(L, port); lua_rawset(L, -3);
    lua_pushstring(L, "Connected"); lua_pushboolean(L, 1); lua_rawset(L, -3);
    lua_pushstring(L, "quiet"); lua_pushnumber(L, 0); lua_rawset(L, -3);
    lua_pushstring(L, "ping"); lua_pushnumber(L, 0); lua_rawset(L, -3);
    lua_pushstring(L, "status"); lua_pushstring(L, "Established"); lua_rawset(L, -3);
    lua_pushstring(L, "establishedPeers");
    lua_newtable(L);
    lua_pushnumber(L, 1);
    lua_pushstring(L, "1");
    lua_rawset(L, -3);
    lua_rawset(L, -3);
    lua_pushstring(L, id);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4);
    lua_pop(L, 2);
}

static int lobby_ConnectToPeer(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    std::string id_storage = lobby_arg_id(L, 2, "1");
    const char* id = id_storage.c_str();
    const char* name = lobby_arg_string(L, 3, id);
    double port = lua_type(L, 4) == LUA_TNUMBER ? lua_tonumber(L, 4) : 0.0;

    lobby_store_peer(L, 1, id, name, port);

    lua_pushboolean(L, 1);
    return 1;
}

static int lobby_DebugDump(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lobby_push_peers(L, 1);
    spdlog::debug("Lobby peers: {}", luaL_getn(L, -1));
    lua_pop(L, 1);
    return 0;
}

static int lobby_Destroy(lua_State* L) {
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "__osc_destroyed");
        lua_pushboolean(L, 1);
        lua_rawset(L, 1);
        lua_pushstring(L, "__osc_peers");
        lua_newtable(L);
        lua_rawset(L, 1);
    }
    return 0;
}

static int lobby_DisconnectFromPeer(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    std::string id_storage = lobby_arg_id(L, 2, "");
    const char* id = id_storage.c_str();
    lobby_push_peers(L, 1);
    lua_pushstring(L, id);
    lua_rawget(L, -2);
    bool existed = !lua_isnil(L, -1);
    lua_pop(L, 1);
    lua_pushstring(L, id);
    lua_pushnil(L);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    lua_pushboolean(L, existed ? 1 : 0);
    return 1;
}

static int lobby_EjectPeer(lua_State* L) {
    return lobby_DisconnectFromPeer(L);
}

static int lobby_GetLocalPlayerID(lua_State* L) {
    lua_pushstring(L, "0");
    return 1;
}

static int lobby_GetLocalPlayerName(lua_State* L) {
    // Try to read player name from preferences
    lua_pushstring(L, "GetPreference");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, "profile.playerName");
        lua_pushstring(L, "Player");
        if (lua_pcall(L, 2, 1, 0) == 0) {
            return 1; // leave result on stack
        }
        lua_pop(L, 1); // pop error
    } else {
        lua_pop(L, 1);
    }
    lua_pushstring(L, "Player");
    return 1;
}

static int lobby_GetLocalPort(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

static int lobby_GetPeer(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushnil(L);
        return 1;
    }
    std::string id_storage = lobby_arg_id(L, 2, "");
    const char* id = id_storage.c_str();
    lobby_push_peers(L, 1);
    lua_pushstring(L, id);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1) && std::string(id) == "1") {
        lua_pop(L, 1);
        lua_pop(L, 1);
        lobby_store_peer(L, 1, "1", "Player", 0.0);
        lobby_push_peers(L, 1);
        lua_pushstring(L, id);
        lua_rawget(L, -2);
    }
    lobby_sanitize_peer_table(L, -1);
    lua_remove(L, -2);
    return 1;
}

static int lobby_GetPeers(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_newtable(L);
        return 1;
    }
    lobby_push_peers(L, 1);
    return 1;
}

/// lobby:HostGame() — for single-player, defer ConnectionToHostEstablished
/// to the next frame via ForkThread. FA's engine fires this asynchronously;
/// we simulate with a 1-tick delay so InitLobbyComm fully completes first.
static int lobby_HostGame(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;

    lua_pushstring(L, "__osc_lobby_host_game_called");
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_GLOBALSINDEX);
    spdlog::info("lobby:HostGame invoked");

    // Multiplayer: if a LAN host port was requested, stand up the real TCP
    // transport now so peers can connect while the lobby is open. Absent → the
    // single-player loopback path below runs unchanged.
    {
        lua_pushstring(L, "__osc_mp_host_port");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_type(L, -1) == LUA_TNUMBER && !mp_net_state().transport_ready) {
            mp_begin_host(static_cast<u16>(lua_tonumber(L, -1)));
        }
        lua_pop(L, 1);
    }

    // Store lobbyComm in a well-known global for the deferred thread to read.
    // Using rawset to bypass config.lua's global lock.
    lua_pushstring(L, "__osc_pending_host_comm");
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_GLOBALSINDEX);

    // Get player name
    std::string player_name = "Player";
    lua_pushstring(L, "GetLocalPlayerName");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 1, 0) == 0 && lua_type(L, -1) == LUA_TSTRING)
            player_name = lua_tostring(L, -1);
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }
    lua_pushstring(L, "__osc_pending_host_name");
    lua_pushstring(L, player_name.c_str());
    lua_rawset(L, LUA_GLOBALSINDEX);
    lobby_store_peer(L, 1, "1", player_name.c_str(), 0.0);

    // Defer the callback by 1 tick via ForkThread + WaitTicks(1).
    // Also patch HostUtils.RefreshButtonEnabledness if nil — the HostUtils table
    // in FAF's lobby.lua is so large that some fields may be missing due to Lua 5.0
    // table constructor limitations.
    // Lua 5.0: use luaL_loadbuffer + lua_pcall (no luaL_dostring).
    {
        const char* code =
            "ForkThread(function()\n"
            "  WaitTicks(1)\n"
            "  local comm = rawget(_G, '__osc_pending_host_comm')\n"
            "  local name = rawget(_G, '__osc_pending_host_name') or 'Player'\n"
            "  local LobbyComm = import('/lua/ui/lobby/lobbyComm.lua')\n"
            "  if LobbyComm and LobbyComm.quietTimeout == nil then LobbyComm.quietTimeout = 30000 end\n"
            "  rawset(_G, '__osc_pending_host_comm', nil)\n"
            "  rawset(_G, '__osc_pending_host_name', nil)\n"
            "  -- Hosting() is the host's only callback: it takes slot 1 and\n"
            "  -- builds the lobby UI (retail and FAF alike). ConnectionToHost-\n"
            "  -- Established is for joining clients; firing it on the host made\n"
            "  -- the lobby add the host again as a remote player.\n"
            "  if comm and comm.Hosting then\n"
            "    comm:Hosting()\n"
            "    rawset(_G, '__osc_lobby_hosting_callback_fired', true)\n"
            "  end\n"
            "end)\n";
        if (luaL_loadbuffer(L, code, std::strlen(code), "=HostGame") == 0) {
            if (lua_pcall(L, 0, 0, 0) != 0) {
                spdlog::warn("lobby HostGame deferred error: {}", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            spdlog::warn("lobby HostGame loadbuffer error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }

    return 0;
}

static int lobby_IsHost(lua_State* L) {
    lua_pushboolean(L, 1); // always host in our engine
    return 1;
}

/// lobby:JoinGame(...) — for a LAN client, connect to the host over TCP if a
/// join address has been configured (single-player never sets one, so this is
/// a no-op there, matching the old loopback behavior).
static int lobby_JoinGame(lua_State* L) {
    lua_pushstring(L, "__osc_mp_join_address");
    lua_rawget(L, LUA_GLOBALSINDEX);
    std::string addr =
        lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : std::string();
    lua_pop(L, 1);
    if (!addr.empty() && !mp_net_state().transport_ready) {
        lua_pushstring(L, "__osc_mp_join_port");
        lua_rawget(L, LUA_GLOBALSINDEX);
        u16 port = lua_type(L, -1) == LUA_TNUMBER
                       ? static_cast<u16>(lua_tonumber(L, -1))
                       : static_cast<u16>(47624);
        lua_pop(L, 1);
        mp_begin_join(addr, port);
    }
    return 0;
}
/// lobby:LaunchGame(config) — delegates to LaunchSinglePlayerSession
static int lobby_LaunchGame(lua_State* L) {
    lua_pushstring(L, "LaunchSinglePlayerSession");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 2); // push config arg
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("lobby LaunchGame error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    return 0;
}

static int lobby_MakeValidGameName(lua_State* L) {
    // Return the original name
    if (lua_type(L, 2) == LUA_TSTRING)
        lua_pushvalue(L, 2);
    else
        lua_pushstring(L, "Game");
    return 1;
}

static int lobby_MakeValidPlayerName(lua_State* L) {
    // Return the original name (arg 3)
    if (lua_type(L, 3) == LUA_TSTRING)
        lua_pushvalue(L, 3);
    else
        lua_pushstring(L, "Player");
    return 1;
}

static int lobby_SendData(lua_State* L) {
    // Single-player loopback: call lobbyComm:DataReceived(data) directly
    // Args: self (lobbyComm), targetID, data
    if (!lua_istable(L, 1)) return 0;
    int data_idx = lua_istable(L, 3) ? 3 : (lua_istable(L, 2) ? 2 : 0);
    if (data_idx == 0) return 0;

    // Inject SenderID and SenderName
    lua_pushstring(L, "SenderID");
    lua_pushstring(L, "1");
    lua_rawset(L, data_idx);

    lua_pushstring(L, "SenderName");
    lua_pushstring(L, "GetLocalPlayerName");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 1, 0) == 0 && lua_type(L, -1) == LUA_TSTRING) {
            lua_rawset(L, data_idx);
        } else {
            lua_pop(L, 1);
            lua_pushstring(L, "Player");
            lua_rawset(L, data_idx);
        }
    } else {
        lua_pop(L, 1);
        lua_pushstring(L, "Player");
        lua_rawset(L, data_idx);
    }

    // Call self:DataReceived(data) for loopback
    lua_pushstring(L, "DataReceived");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        lua_pushvalue(L, data_idx);
        if (lua_pcall(L, 2, 0, 0) != 0) {
            spdlog::warn("SendData loopback error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    return 0;
}

// Steam-build lobby methods (retail FA 3599 on Steam). UpdateSteamLobby
// publishes lobby metadata to Steam matchmaking: offline, nothing to publish.
// JoinSteamGame joins through a Steam lobby id; with no Steam backend it is
// the regular JoinGame.
static int lobby_UpdateSteamLobby(lua_State* L) { // stub: no Steam backend
    (void)L;
    return 0;
}

// clang-format off
const MethodEntry ui_lobby_methods[] = {
    {"BroadcastData",       lobby_BroadcastData},
    {"ConnectToPeer",       lobby_ConnectToPeer},
    {"DebugDump",           lobby_DebugDump},
    {"Destroy",             lobby_Destroy},
    {"DisconnectFromPeer",  lobby_DisconnectFromPeer},
    {"EjectPeer",           lobby_EjectPeer},
    {"GetLocalPlayerID",    lobby_GetLocalPlayerID},
    {"GetLocalPlayerName",  lobby_GetLocalPlayerName},
    {"GetLocalPort",        lobby_GetLocalPort},
    {"GetPeer",             lobby_GetPeer},
    {"GetPeers",            lobby_GetPeers},
    {"HostGame",            lobby_HostGame},
    {"IsHost",              lobby_IsHost},
    {"JoinGame",            lobby_JoinGame},
    {"LaunchGame",          lobby_LaunchGame},
    {"MakeValidGameName",   lobby_MakeValidGameName},
    {"MakeValidPlayerName", lobby_MakeValidPlayerName},
    {"SendData",            lobby_SendData},
    {"UpdateSteamLobby",    lobby_UpdateSteamLobby},
    {"JoinSteamGame",       lobby_JoinGame},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
