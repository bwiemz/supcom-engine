#include "app/lan_game_test.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/net_lobby.hpp"
#include "sim/army_brain.hpp"
#include "sim/lockstep_session.hpp"
#include "sim/sim_state.hpp"
#include "sim/socket_platform.hpp"

extern "C" {
#include <lua.h>
}

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <utility>

namespace osc::app {

namespace {

/// Frames the flow may take in all before it counts as stuck.
constexpr u32 kMaxFrames = 30000;
/// Frames between presses of Launch, while no countdown runs.
constexpr u32 kPressInterval = 600;
/// Frames after the check that a side waits for kEndTick at most.
constexpr u32 kWindDownFrames = 600;

/// __osc_upvalue(fn, name): the upvalue `name` of the Lua function `fn`,
/// or nil. The flow reads lobby.lua's own state (its gameInfo, GUI,
/// launchThread), which the file keeps in locals.
int l_upvalue(lua_State* L) {
    if (!lua_isfunction(L, 1) || lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    const std::string want = lua_tostring(L, 2);
    for (int n = 1;; ++n) {
        const char* name = lua_getupvalue(L, 1, n);
        if (!name) break;
        if (want == name) return 1;
        lua_pop(L, 1);
    }
    lua_pushnil(L);
    return 1;
}

/// A number global of `L` (0 if none).
double number_global(lua_State* L, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const double v = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : 0.0;
    lua_pop(L, 1);
    return v;
}

/// Whether something listens at `address`:`port` (a loopback connect
/// answers at once).
bool listening(const std::string& address, u16 port) {
    namespace net = sim::net;
    net::startup();
    const net::socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == net::kInvalidSocket) return false;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    const bool ok = inet_pton(AF_INET, address.c_str(), &to.sin_addr) == 1 &&
                    connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0;
    net::close_socket(s);
    return ok;
}

/// This player's slot readied; the humans and how many are ready counted,
/// and whether retail's launch countdown runs. (Spawn and factions stay
/// retail's defaults, random.)
constexpr const char* kLobbyStep = R"(
    local lobby = import('/lua/ui/lobby/lobby.lua')
    local info = __osc_upvalue(lobby.IsLocallyOwned, 'gameInfo')
    local me = __osc_upvalue(lobby.IsLocallyOwned, 'localPlayerID')
    __osc_lan_humans, __osc_lan_ready, __osc_lan_counting = 0, 0, 0
    if not info or not me then return end
    for slot, p in info.PlayerOptions do
        if p.Human then
            __osc_lan_humans = __osc_lan_humans + 1
            if p.Ready then __osc_lan_ready = __osc_lan_ready + 1 end
            if p.OwnerID == me and not p.Ready then lobby.SetPlayerOption(slot, 'Ready', true) end
        end
    end
    if __osc_upvalue(lobby.CancelLaunch, 'launchThread') then __osc_lan_counting = 1 end
)";

/// The host's Launch button, pressed.
constexpr const char* kPressLaunch = R"(
    local gui = __osc_upvalue(import('/lua/ui/lobby/lobby.lua').CreateUI, 'GUI')
    gui.launchGameButton.OnClick(gui.launchGameButton)
)";

} // namespace

LanGameTest::LanGameTest(bool host, std::string address, u16 port)
    : host_(host), address_(std::move(address)), port_(port) {}

bool LanGameTest::start(lua::LuaState& ui) {
    spdlog::info("=== LAN game test ({}): retail's lobby to a game on port {} ===",
                 host_ ? "host" : "joiner", port_);
    // Nothing on the LAN finds this game, nor another run's
    lua::set_lan_discovery("127.0.0.1", 0);
    lua_State* L = ui.raw();
    lua_pushstring(L, "__osc_upvalue");
    lua_pushcfunction(L, l_upvalue);
    lua_rawset(L, LUA_GLOBALSINDEX);
    if (!host_) return true; // it joins once the host listens
    auto r = ui.do_string(fmt::format(R"(
        local lobby = import('/lua/ui/lobby/lobby.lua')
        lobby.CreateLobby('UDP', {}, 'LanHost', nil, nil, GetFrame(0), function() end)
        lobby.HostGame('LAN Game Test', '/maps/SCMP_009/SCMP_009_scenario.lua', false)
    )",
                                      port_));
    if (!r) {
        fail("hosting: " + r.error().message);
        return false;
    }
    return true;
}

void LanGameTest::frame(lua::LuaState& ui, const sim::SimState* sim) {
    if (done_) return;
    ++frames_;
    if (frames_ > kMaxFrames) {
        fail(fmt::format("stuck after {} frames ({}, tick {})", frames_,
                         sim ? "in the game" : "in the lobby", sim ? sim->tick_count() : 0));
        done_ = true;
        return;
    }
    if (!sim) {
        lobby_frame(ui);
        return;
    }
    if (!checked_at_ && sim->tick_count() >= kCheckTick) {
        check(ui, *sim);
        checked_at_ = frames_;
    }
    if (checked_at_ && (sim->tick_count() >= kEndTick || frames_ - *checked_at_ > kWindDownFrames))
        done_ = true;
}

void LanGameTest::lobby_frame(lua::LuaState& ui) {
    if (!host_ && !joined_) {
        if (frames_ % 30 != 0 || !listening(address_, port_)) return;
        auto r = ui.do_string(fmt::format(R"(
            local lobby = import('/lua/ui/lobby/lobby.lua')
            lobby.CreateLobby('UDP', 0, 'LanJoiner', nil, nil, GetFrame(0), function() end)
            lobby.JoinGame('{}:{}', false)
        )",
                                          address_, port_));
        joined_ = true;
        if (!r) {
            fail("joining: " + r.error().message);
            done_ = true;
        }
        return;
    }
    auto r = ui.do_string(kLobbyStep);
    if (!r) {
        fail("in the lobby: " + r.error().message);
        done_ = true;
        return;
    }
    if (!host_) return;
    lua_State* L = ui.raw();
    const bool everyone_ready =
        number_global(L, "__osc_lan_humans") == 2 && number_global(L, "__osc_lan_ready") == 2;
    const bool counting = number_global(L, "__osc_lan_counting") != 0;
    if (everyone_ready && !counting &&
        (last_press_ == 0 || frames_ - last_press_ > kPressInterval)) {
        last_press_ = frames_;
        spdlog::info("[lan-game] everyone is ready: Launch");
        if (auto p = ui.do_string(kPressLaunch); !p) {
            fail("pressing Launch: " + p.error().message);
            done_ = true;
        }
    }
}

void LanGameTest::check(lua::LuaState& ui, const sim::SimState& sim) {
    const auto& mp = lua::mp_net_state();
    if (!mp.session) {
        fail("the game isn't a lockstep game");
        return;
    }
    if (mp.session->desynced()) {
        std::string domains;
        for (const std::string& d : mp.session->desync_domains()) domains += " " + d;
        fail(fmt::format("desynced at tick {} (domains:{})", mp.session->desync_tick(), domains));
    }
    // Random spawn: the two slots taken, in order, are armies 0 and 1 (each
    // side's by where retail's shuffle put it), both human, each starting at
    // the marker of its slot's army, apart
    const i32 want = mp.local_army();
    if (want != 0 && want != 1) fail(fmt::format("plays army {}, not 0 or 1", want));
    const auto human = [&](size_t i) {
        const sim::ArmyBrain* brain = sim.army_at(i);
        return brain && brain->is_human();
    };
    if (sim.army_count() != 2 || !human(0) || !human(1)) {
        fail(fmt::format("{} armies, not the lobby's two humans", sim.army_count()));
    } else {
        const sim::ArmyBrain& a = *sim.army_at(0);
        const sim::ArmyBrain& b = *sim.army_at(1);
        if (a.name() == b.name() || a.name().rfind("ARMY_", 0) != 0 ||
            b.name().rfind("ARMY_", 0) != 0)
            fail(fmt::format("the armies are {} and {}", a.name(), b.name()));
        if (a.start_position().x == b.start_position().x &&
            a.start_position().z == b.start_position().z)
            fail(fmt::format("{} and {} start at the same place", a.name(), b.name()));
    }
    // The UI watches its own army (1-based)
    if (auto r = ui.do_string("__osc_lan_focus = GetFocusArmy()"); !r)
        fail("GetFocusArmy: " + r.error().message);
    else if (number_global(ui.raw(), "__osc_lan_focus") != want + 1)
        fail(fmt::format("the UI's focus army is {}, not {}",
                         number_global(ui.raw(), "__osc_lan_focus"), want + 1));
    spdlog::info("[lan-game] tick {}: frame {} of the lockstep, army {}", sim.tick_count(),
                 mp.session->current_frame(), mp.local_army());
}

void LanGameTest::fail(std::string why) {
    if (failure_.empty()) failure_ = std::move(why);
}

void LanGameTest::finish(const sim::SimState* sim) const {
    if (!failure_.empty()) {
        test_status::fail("[FAIL] lan-game ({}): {}", host_ ? "host" : "joiner", failure_);
    } else if (!checked_at_) {
        test_status::fail("[FAIL] lan-game ({}): the window closed at tick {}, before the check",
                          host_ ? "host" : "joiner", sim ? sim->tick_count() : 0);
    } else if (test_status::failure_count() == 0) {
        spdlog::info("[PASS] lan-game ({}): retail's lobby launched a game played in lockstep to "
                     "tick {}, as army {}",
                     host_ ? "host" : "joiner", sim ? sim->tick_count() : 0, host_ ? 1 : 2);
    }
}

} // namespace osc::app
