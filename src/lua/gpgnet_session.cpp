#include "lua/gpgnet_session.hpp"

#include "lua/lua_state.hpp"
#include "lua/net_lobby.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace osc::lua {

namespace {

using sim::GpgNetArg;
using sim::GpgNetCommand;

/// Where the client's lobby lives: a field of its UI state's registry, so a
/// new UI state (a game's) has none.
constexpr const char* kLobbyKey = "__osc_gpgnet_lobby";

struct GpgNet {
    std::unique_ptr<sim::GpgNetLink> link;
    GpgNetState state = GpgNetState::None;
};

GpgNet& gpgnet() {
    static GpgNet g;
    return g;
}

/// A command the game can't carry out, and why (logged: the link stays up,
/// as Moho's).
struct Refused : std::runtime_error {
    using std::runtime_error::runtime_error;
};

i32 number_arg(const std::vector<GpgNetArg>& args, size_t i) {
    if (args[i].type != GpgNetArg::Type::Num)
        throw Refused("argument " + std::to_string(i + 1) + " should be an integer");
    return args[i].num;
}

const std::string& string_arg(const std::vector<GpgNetArg>& args, size_t i) {
    if (args[i].type != GpgNetArg::Type::String)
        throw Refused("argument " + std::to_string(i + 1) + " should be a string");
    return args[i].str;
}

void expect_args(const GpgNetCommand& c, size_t n) {
    if (c.args.size() != n)
        throw Refused("Wrong number of arguments to " + c.name + " command, expected " +
                      std::to_string(n));
}

/// Push the client's lobby (the module onlineprovider.lua made it with);
/// false (nothing pushed) if this UI state has none.
bool push_lobby(lua_State* L) {
    lua_pushstring(L, kLobbyKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}

/// Call the lobby's `method` with the `nargs` values on the stack (plain,
/// as Moho calls the module's functions), which it pops.
void call_lobby(lua_State* L, const char* method, int nargs) {
    const int base = lua_gettop(L) - nargs;
    if (!push_lobby(L)) {
        lua_settop(L, base);
        throw Refused("No lobby.");
    }
    lua_pushstring(L, method);
    lua_rawget(L, -2);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, base);
        throw Refused(std::string("Lobby method \"") + method + "\" is unavailable.");
    }
    lua_insert(L, base + 1); // the function below its arguments
    if (lua_pcall(L, nargs, 0, 0) != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
        lua_settop(L, base);
        throw Refused(err);
    }
    lua_settop(L, base);
}

std::string uid_text(i32 uid) {
    return std::to_string(uid);
}

/// CreateLobby(init mode, port, player name, uid, NAT port): the lobby
/// onlineprovider.lua makes -- retail's auto-lobby for init mode 1 (the
/// matchmaker's), else its lobby -- and the game is in the Lobby state.
void create_lobby(lua_State* L, const GpgNetCommand& c) {
    expect_args(c, 5);
    const bool autolaunch = number_arg(c.args, 0) != 0;
    const i32 port = number_arg(c.args, 1);
    const std::string& name = string_arg(c.args, 2);
    const i32 uid = number_arg(c.args, 3);
    (void)number_arg(c.args, 4); // the NAT port: M220c
    if (push_lobby(L)) {
        lua_pop(L, 1);
        throw Refused("Lobby already exists.");
    }
    const int top = lua_gettop(L);
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    lua_pushstring(L, "/lua/multiplayer/onlineprovider.lua");
    if (!lua_isfunction(L, -2) || lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
        lua_settop(L, top);
        throw Refused("Failed to load \"/lua/multiplayer/onlineprovider.lua\".");
    }
    lua_pushstring(L, "CreateLobby");
    lua_rawget(L, -2);
    // Retail's: CreateLobby(autolaunch, protocol, port, playerName, uid,
    // natTraversalProvider, hasSupcom), the protocol GPGNet's, UDP
    lua_pushboolean(L, autolaunch ? 1 : 0);
    lua_pushstring(L, "UDP");
    lua_pushnumber(L, static_cast<lua_Number>(port));
    lua_pushstring(L, name.c_str());
    lua_pushstring(L, uid_text(uid).c_str());
    lua_pushnil(L);       // no NAT traversal yet (M220c)
    lua_pushnumber(L, 1); // this is Forged Alliance
    if (lua_pcall(L, 7, 1, 0) != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(error)";
        lua_settop(L, top);
        throw Refused(err);
    }
    lua_pushstring(L, kLobbyKey);
    lua_insert(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_settop(L, top);
    spdlog::info("GPGNET: entering lobby state.");
    gpgnet_send({"GameState", {GpgNetArg::string("Lobby")}});
}

/// Carry out one of the client's commands, as Moho's CGpgNetInterface does.
void carry_out(lua_State* L, const GpgNetCommand& c) {
    if (c.name == "Test") {
        spdlog::info("GPGNET: test message, {} args", c.args.size());
        return;
    }
    if (c.name == "CreateLobby") {
        create_lobby(L, c);
        return;
    }
    if (c.name == "HostGame") {
        // HostGame([map]): the map's scenario (retail's lobbies take the game's
        // name, then the scenario)
        if (c.args.size() > 1)
            throw Refused("Wrong number of arguments to HostGame command, expected 0 or 1");
        const std::string map = c.args.empty() ? std::string() : string_arg(c.args, 0);
        const std::string scenario =
            map.empty() ? "" : "/maps/" + map + "/" + map + "_scenario.lua";
        lua_pushstring(L, map.c_str());
        lua_pushstring(L, scenario.c_str());
        lua_pushboolean(L, 0);
        call_lobby(L, "HostGame", 3);
        return;
    }
    if (c.name == "JoinGame") {
        // JoinGame(address, the host's name, the host's uid)
        expect_args(c, 3);
        const std::string& address = string_arg(c.args, 0);
        const std::string& name = string_arg(c.args, 1);
        const i32 uid = number_arg(c.args, 2);
        lua_pushstring(L, address.c_str());
        lua_pushboolean(L, 0); // not as an observer
        lua_pushstring(L, name.c_str());
        lua_pushstring(L, uid_text(uid).c_str());
        call_lobby(L, "JoinGame", 4);
        return;
    }
    if (c.name == "ConnectToPeer") {
        expect_args(c, 3);
        const std::string& address = string_arg(c.args, 0);
        const std::string& name = string_arg(c.args, 1);
        const i32 uid = number_arg(c.args, 2);
        lua_pushstring(L, address.c_str());
        lua_pushstring(L, name.c_str());
        lua_pushstring(L, uid_text(uid).c_str());
        call_lobby(L, "ConnectToPeer", 3);
        return;
    }
    if (c.name == "DisconnectFromPeer") {
        expect_args(c, 1);
        lua_pushstring(L, uid_text(number_arg(c.args, 0)).c_str());
        call_lobby(L, "DisconnectFromPeer", 1);
        return;
    }
    if (c.name == "HasSupcom" || c.name == "HasForgedAlliance") {
        expect_args(c, 1);
        lua_pushboolean(L, number_arg(c.args, 0) != 0);
        call_lobby(L, c.name == "HasSupcom" ? "SetHasSupcom" : "SetHasForgedAlliance", 1);
        return;
    }
    if (c.name == "EjectPlayer" || c.name == "SendNatPacket") {
        spdlog::warn("GPGNET: {} isn't supported yet", c.name); // M220b, M220c
        return;
    }
    spdlog::warn("GPGNET: unknown command \"{}\"", c.name);
}

// GpgNetActive() -> whether the game has a link to a client
int l_GpgNetActive(lua_State* L) {
    lua_pushboolean(L, gpgnet_active() ? 1 : 0);
    return 1;
}

// GpgNetSend(cmd, args...): to the client, numbers as integers (booleans 1
// or 0), strings as strings, as Moho's; nothing without a link
int l_GpgNetSend(lua_State* L) {
    const int n = lua_gettop(L);
    GpgNetCommand c;
    c.name = luaL_checkstring(L, 1);
    for (int i = 2; i <= n; ++i) {
        const int t = lua_type(L, i);
        if (t == LUA_TNUMBER) {
            c.args.push_back(GpgNetArg::number(static_cast<i32>(lua_tonumber(L, i))));
        } else if (t == LUA_TBOOLEAN) {
            c.args.push_back(GpgNetArg::number(lua_toboolean(L, i) ? 1 : 0));
        } else if (t == LUA_TSTRING) {
            c.args.push_back(GpgNetArg::string(lua_tostring(L, i)));
        } else {
            return luaL_error(
                L,
                "invalid kind of argument to GpgNetSend(): can only deal with ints and strings.");
        }
    }
    gpgnet_send(c);
    return 0;
}

} // namespace

bool gpgnet_attach(const std::string& endpoint) {
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) return false;
    const std::string host = endpoint.substr(0, colon);
    const std::string port = endpoint.substr(colon + 1);
    if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return false;
    const int p = std::stoi(port);
    if (p < 1 || p > 65535) return false;
    auto link = std::make_unique<sim::GpgNetLink>();
    if (!link->connect(host == "localhost" ? "127.0.0.1" : host, static_cast<u16>(p))) return false;
    gpgnet().link = std::move(link);
    gpgnet().state = GpgNetState::Linked;
    spdlog::info("GPGNET: connecting to the client at {}", endpoint);
    return true;
}

bool gpgnet_active() {
    return gpgnet().state == GpgNetState::Linked;
}

GpgNetState gpgnet_state() {
    return gpgnet().state;
}

void pump_gpgnet(lua_State* L) {
    auto& g = gpgnet();
    if (!g.link) return;
    for (sim::GpgNetLink::Event& e : g.link->poll(net_lobby_clock_ms())) {
        switch (e.kind) {
        case sim::GpgNetLink::Event::Kind::Connected:
            // Moho's Connected: the game says it is there, idle
            spdlog::info("GPGNET: connected to the client");
            gpgnet_send({"GameState", {GpgNetArg::string("Idle")}});
            break;
        case sim::GpgNetLink::Event::Kind::Command: try { carry_out(L, e.command);
            } catch (const Refused& r) {
                spdlog::warn("GPGNET: command processing failed: {}: {}", e.command.name, r.what());
            }
            break;
        case sim::GpgNetLink::Event::Kind::Closed:
            spdlog::warn("GPGNET: the link to the client is down: {}", e.reason);
            g.state = GpgNetState::Closed;
            break;
        }
    }
}

void gpgnet_send(const sim::GpgNetCommand& command) {
    auto& g = gpgnet();
    if (g.link && g.link->connected()) g.link->send(command);
}

void register_gpgnet_bindings(LuaState& state) {
    state.register_function("GpgNetActive", l_GpgNetActive);
    state.register_function("GpgNetSend", l_GpgNetSend);
}

void gpgnet_detach() {
    auto& g = gpgnet();
    g.link.reset();
    g.state = GpgNetState::None;
}

} // namespace osc::lua
