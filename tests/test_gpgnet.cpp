// GPGNet (M220a): the link Moho keeps to a matchmaking client. Its commands'
// wire form, and the game's end of the link over the localhost loopback,
// against a stand-in for the client.

#include <catch2/catch_test_macros.hpp>

#include "lua/gpgnet_session.hpp"
#include "lua/lua_state.hpp"
#include "sim/gpgnet.hpp"
#include "sim/socket_platform.hpp"

extern "C" {
#include <lua.h>
}

#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using osc::sim::extract_gpgnet_commands;
using osc::sim::GpgNetArg;
using osc::sim::GpgNetCommand;
using osc::sim::GpgNetLink;
namespace net = osc::sim::net;

namespace {

GpgNetCommand create_lobby() {
    return {"CreateLobby",
            {GpgNetArg::number(0), GpgNetArg::number(6112), GpgNetArg::string("Tester"),
             GpgNetArg::number(-42), GpgNetArg::number(1)}};
}

bool same(const GpgNetCommand& a, const GpgNetCommand& b) {
    return a.name == b.name && a.args == b.args;
}

osc::i64 now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

/// A stand-in for the client: it listens on the loopback, and takes the
/// game's connection.
struct FakeClient {
    net::socket_t listener = net::kInvalidSocket;
    net::socket_t conn = net::kInvalidSocket;
    osc::u16 port = 0;

    FakeClient() {
        net::startup();
        listener = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        REQUIRE(bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        REQUIRE(listen(listener, 1) == 0);
        socklen_t len = sizeof(addr);
        getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
        port = ntohs(addr.sin_port);
    }
    ~FakeClient() {
        net::close_socket(conn);
        net::close_socket(listener);
    }
    void accept_game() {
        conn = accept(listener, nullptr, nullptr);
        REQUIRE(conn != net::kInvalidSocket);
    }
    void send(const std::vector<osc::u8>& bytes) {
        REQUIRE(net::send_all(conn, bytes.data(), bytes.size()));
    }
    /// Whether the game has sent something not yet read.
    bool has_data() const {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(conn, &fds);
        timeval tv{0, 0};
        return select(static_cast<int>(conn) + 1, &fds, nullptr, nullptr, &tv) > 0;
    }
    /// The commands the game sent, until `count` have come (or 2 s pass).
    std::vector<GpgNetCommand> read(size_t count) {
        std::vector<osc::u8> buf;
        std::vector<GpgNetCommand> out;
        const osc::i64 deadline = now_ms() + 2000;
        while (out.size() < count && now_ms() < deadline) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(conn, &fds);
            timeval tv{0, 10000};
            if (select(static_cast<int>(conn) + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            osc::u8 tmp[1024];
            const int n =
                static_cast<int>(recv(conn, reinterpret_cast<char*>(tmp), sizeof(tmp), 0));
            if (n <= 0) break;
            buf.insert(buf.end(), tmp, tmp + n);
            REQUIRE(extract_gpgnet_commands(buf, out));
        }
        return out;
    }
};

/// Poll `link` until `done` holds for what it reported, or 3 s pass.
bool pump(GpgNetLink& link, std::vector<GpgNetLink::Event>& seen,
          const std::function<bool()>& done) {
    const osc::i64 deadline = now_ms() + 3000;
    while (now_ms() < deadline) {
        for (auto& e : link.poll(now_ms())) seen.push_back(std::move(e));
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

size_t count_of(const std::vector<GpgNetLink::Event>& seen, GpgNetLink::Event::Kind kind) {
    size_t n = 0;
    for (const auto& e : seen) n += e.kind == kind ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("GPGNet commands keep every argument's type and value (M220a)", "[gpgnet]") {
    GpgNetCommand data{"ProcessNatPacket", {GpgNetArg::string("1.2.3.4:5")}};
    GpgNetArg bytes;
    bytes.type = GpgNetArg::Type::Data;
    bytes.str = std::string("\x00\xff\x10", 3);
    data.args.push_back(bytes);
    const GpgNetCommand none{"Connected", {}};

    std::vector<osc::u8> wire;
    for (const GpgNetCommand& c : std::vector<GpgNetCommand>{data, none}) {
        const auto one = osc::sim::encode_gpgnet(c);
        wire.insert(wire.end(), one.begin(), one.end());
    }
    const auto lobby = osc::sim::encode_gpgnet(create_lobby());
    wire.insert(wire.end(), lobby.begin(), lobby.end());

    // The wire's form: the name's length, little-endian, then its bytes
    CHECK(wire[0] == 16);
    CHECK(wire[1] == 0);
    CHECK(std::string(wire.begin() + 4, wire.begin() + 20) == "ProcessNatPacket");

    // Byte by byte: each command only once it is whole
    std::vector<osc::u8> buf;
    std::vector<GpgNetCommand> out;
    size_t whole = 0;
    for (const osc::u8 b : wire) {
        buf.push_back(b);
        REQUIRE(extract_gpgnet_commands(buf, out));
        CHECK(out.size() >= whole);
        whole = out.size();
    }
    REQUIRE(out.size() == 3);
    CHECK(same(out[0], data));
    CHECK(same(out[1], none));
    CHECK(same(out[2], create_lobby()));
    CHECK(out[2].args[3].num == -42);
    CHECK(buf.empty());
}

TEST_CASE("A malformed GPGNet command is refused (M220a)", "[gpgnet]") {
    std::vector<GpgNetCommand> out;
    // A name announcing more than the limit: refused before its bytes
    std::vector<osc::u8> big{0x00, 0x00, 0x00, 0x7f, 'x'};
    CHECK_FALSE(extract_gpgnet_commands(big, out));
    // An argument of no known type
    auto bad = osc::sim::encode_gpgnet({"Test", {GpgNetArg::number(1)}});
    bad[4 + 4 + 4] = 7; // the argument's type
    CHECK_FALSE(extract_gpgnet_commands(bad, out));
    // Too many arguments
    std::vector<osc::u8> many = osc::sim::encode_gpgnet({"Test", {}});
    many[8] = 0xff;
    many[9] = 0xff;
    CHECK_FALSE(extract_gpgnet_commands(many, out));
    // Each argument within its limit, but the command past its own: refused
    // before the whole of it has come
    GpgNetCommand huge{"Test", {}};
    for (int i = 0; i < 5; ++i) huge.args.push_back(GpgNetArg::string(std::string(1u << 20, 'x')));
    auto wire = osc::sim::encode_gpgnet(huge);
    wire.resize(wire.size() - 1); // not yet whole
    CHECK_FALSE(extract_gpgnet_commands(wire, out));
    CHECK(out.empty());
}

TEST_CASE("The game's GPGNet link talks to the client both ways (M220a)", "[gpgnet]") {
    FakeClient client;
    GpgNetLink link;
    REQUIRE(link.connect("127.0.0.1", client.port));
    CHECK(link.open());
    CHECK_FALSE(link.connected());
    client.accept_game();
    std::vector<GpgNetLink::Event> seen;
    REQUIRE(pump(link, seen, [&] { return count_of(seen, GpgNetLink::Event::Kind::Connected); }));
    CHECK(link.connected());

    // The game says it is there; the client hears it
    REQUIRE(link.send({"GameState", {GpgNetArg::string("Idle")}}));
    const auto heard = client.read(1);
    REQUIRE(heard.size() == 1);
    CHECK(same(heard[0], GpgNetCommand{"GameState", {GpgNetArg::string("Idle")}}));

    // Two commands in one write, the second split from its end: in order
    auto both = osc::sim::encode_gpgnet(create_lobby());
    const auto host = osc::sim::encode_gpgnet({"HostGame", {GpgNetArg::string("SCMP_009")}});
    both.insert(both.end(), host.begin(), host.end() - 3);
    client.send(both);
    REQUIRE(
        pump(link, seen, [&] { return count_of(seen, GpgNetLink::Event::Kind::Command) == 1; }));
    client.send({host.end() - 3, host.end()});
    REQUIRE(
        pump(link, seen, [&] { return count_of(seen, GpgNetLink::Event::Kind::Command) == 2; }));
    CHECK(same(seen[1].command, create_lobby()));
    CHECK(seen[2].command.name == "HostGame");

    // The client goes: the link says so, and is closed
    net::close_socket(client.conn);
    client.conn = net::kInvalidSocket;
    REQUIRE(pump(link, seen, [&] { return count_of(seen, GpgNetLink::Event::Kind::Closed); }));
    CHECK_FALSE(link.open());
    CHECK_FALSE(link.send({"GameState", {GpgNetArg::string("Ended")}}));
}

TEST_CASE("A GPGNet link to no client closes, saying why (M220a)", "[gpgnet]") {
    GpgNetLink link;
    CHECK_FALSE(link.connect("not an address", 1));
    osc::u16 port = 0;
    {
        FakeClient gone; // a port no one listens on once it closes
        port = gone.port;
    }
    REQUIRE(link.connect("127.0.0.1", port));
    std::vector<GpgNetLink::Event> seen;
    // A refused connect takes Windows about two seconds
    const osc::i64 deadline = now_ms() + 8000;
    while (!count_of(seen, GpgNetLink::Event::Kind::Closed) && now_ms() < deadline) {
        for (auto& e : link.poll(now_ms())) seen.push_back(std::move(e));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(count_of(seen, GpgNetLink::Event::Kind::Closed) == 1);
    CHECK(count_of(seen, GpgNetLink::Event::Kind::Connected) == 0);
    CHECK_FALSE(seen.back().reason.empty());
    CHECK_FALSE(link.open());
}

namespace {

/// The UI state's side: an onlineprovider.lua that records what the game
/// asks of it, and a lobby module recording its calls.
const char* kProvider = R"(
    calls = {}
    local function record(...)
        local parts = {}
        for i = 1, arg.n do
            -- a number marked, so a uid sent as one shows
            local v = arg[i]
            table.insert(parts, type(v) == 'number' and '#' .. v or tostring(v))
        end
        table.insert(calls, table.concat(parts, ','))
    end
    local lobby = {
        HostGame = function(...) record('HostGame', unpack(arg)) end,
        JoinGame = function(...) record('JoinGame', unpack(arg)) end,
        ConnectToPeer = function(...) record('ConnectToPeer', unpack(arg)) end,
        DisconnectFromPeer = function(...) record('DisconnectFromPeer', unpack(arg)) end,
    }
    __modules = {['/lua/multiplayer/onlineprovider.lua'] = {
        CreateLobby = function(...)
            record('CreateLobby', unpack(arg))
            return lobby
        end,
    }}
    function import(path) return __modules[path] end
)";

/// The recorded calls, joined by ';'.
std::string calls(osc::lua::LuaState& state) {
    state.do_string("__calls = table.concat(calls, ';')");
    lua_getglobal(state.raw(), "__calls");
    std::string out = lua_tostring(state.raw(), -1) ? lua_tostring(state.raw(), -1) : "";
    lua_pop(state.raw(), 1);
    return out;
}

/// Pump the game's link until `done`, or 3 s pass.
bool pump_game(osc::lua::LuaState& state, const std::function<bool()>& done) {
    const osc::i64 deadline = now_ms() + 3000;
    while (now_ms() < deadline) {
        osc::lua::pump_gpgnet(state.raw());
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

struct Detach {
    ~Detach() { osc::lua::gpgnet_detach(); }
};

} // namespace

TEST_CASE("The client's GPGNet commands reach the lobby as Moho's do (M220a)", "[gpgnet][lua]") {
    Detach detach;
    osc::lua::LuaState state;
    osc::lua::register_gpgnet_bindings(state);
    REQUIRE(state.do_string(kProvider));
    FakeClient client;

    CHECK_FALSE(osc::lua::gpgnet_attach("nowhere"));
    CHECK_FALSE(osc::lua::gpgnet_attach("127.0.0.1:0"));
    CHECK_FALSE(osc::lua::gpgnet_attach("127.0.0.1:99999"));
    REQUIRE(osc::lua::gpgnet_attach("127.0.0.1:" + std::to_string(client.port)));
    CHECK(osc::lua::gpgnet_active());
    client.accept_game();

    // Connected: the game says it is Idle
    REQUIRE(pump_game(state, [&] { return client.has_data(); }));
    auto heard = client.read(1);
    REQUIRE(heard.size() == 1);
    CHECK(same(heard[0], GpgNetCommand{"GameState", {GpgNetArg::string("Idle")}}));

    // CreateLobby: retail's onlineprovider.lua, the auto-lobby (init mode 1),
    // over UDP, the uid a string; then the Lobby state
    client.send(osc::sim::encode_gpgnet(
        {"CreateLobby",
         {GpgNetArg::number(1), GpgNetArg::number(6112), GpgNetArg::string("Tester"),
          GpgNetArg::number(42), GpgNetArg::number(0)}}));
    REQUIRE(pump_game(state, [&] { return !calls(state).empty(); }));
    CHECK(calls(state) == "CreateLobby,true,UDP,#6112,Tester,42,nil,#1");
    heard = client.read(1);
    REQUIRE(heard.size() == 1);
    CHECK(same(heard[0], GpgNetCommand{"GameState", {GpgNetArg::string("Lobby")}}));

    // The lobby's own: HostGame names the map's scenario; JoinGame the host;
    // a second CreateLobby, a wrong type and an unknown command are refused,
    // and the link stays up
    for (const GpgNetCommand& c : std::vector<GpgNetCommand>{
             {"CreateLobby",
              {GpgNetArg::number(0), GpgNetArg::number(1), GpgNetArg::string("Again"),
               GpgNetArg::number(1), GpgNetArg::number(0)}},
             {"HostGame", {GpgNetArg::string("SCMP_009")}},
             {"JoinGame",
              {GpgNetArg::string("127.0.0.1:6113"), GpgNetArg::string("Host"),
               GpgNetArg::number(7)}},
             {"ConnectToPeer",
              {GpgNetArg::string("127.0.0.1:6114"), GpgNetArg::string("Peer"),
               GpgNetArg::number(8)}},
             {"ConnectToPeer",
              {GpgNetArg::number(1), GpgNetArg::string("Bad"), GpgNetArg::number(9)}},
             {"NoSuchCommand", {}},
             {"DisconnectFromPeer", {GpgNetArg::number(8)}},
         })
        client.send(osc::sim::encode_gpgnet(c));
    REQUIRE(pump_game(
        state, [&] { return calls(state).find("DisconnectFromPeer") != std::string::npos; }));
    CHECK(calls(state) == "CreateLobby,true,UDP,#6112,Tester,42,nil,#1;"
                          "HostGame,SCMP_009,/maps/SCMP_009/SCMP_009_scenario.lua,false;"
                          "JoinGame,127.0.0.1:6113,false,Host,7;"
                          "ConnectToPeer,127.0.0.1:6114,Peer,8;"
                          "DisconnectFromPeer,8");
    CHECK(osc::lua::gpgnet_active());

    // The scripts' GpgNetSend: numbers as integers, booleans 1 or 0
    REQUIRE(
        state.do_string("assert(GpgNetActive()) GpgNetSend('GameState', 'Launching', 3, true)"));
    heard = client.read(1);
    REQUIRE(heard.size() == 1);
    CHECK(same(heard[0], GpgNetCommand{"GameState",
                                       {GpgNetArg::string("Launching"), GpgNetArg::number(3),
                                        GpgNetArg::number(1)}}));
    CHECK_FALSE(state.do_string("GpgNetSend('GameState', {})"));

    // The client goes: the link is down
    net::close_socket(client.conn);
    client.conn = net::kInvalidSocket;
    REQUIRE(pump_game(state, [] { return !osc::lua::gpgnet_active(); }));
    CHECK(osc::lua::gpgnet_state() == osc::lua::GpgNetState::Closed);
    REQUIRE(state.do_string("assert(not GpgNetActive()) GpgNetSend('GameState', 'Ended')"));
}
