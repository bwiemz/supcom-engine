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

#include <algorithm>
#include <utility>

namespace osc::app {

namespace {

/// Frames the flow may take in all before it counts as stuck.
constexpr u32 kMaxFrames = 30000;
/// Frames the joiner leaves the host's pause before resuming it.
constexpr u32 kResumeAfterFrames = 60;
/// Frames the joiner waits for its host to listen.
constexpr u32 kMaxHostWaitFrames = 6000;
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

LanGameTest::LanGameTest(bool host, std::string address, u16 port, u32 quit_at)
    : host_(host), address_(std::move(address)), port_(port), quit_at_(quit_at) {}

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
    if (!listening_ && sim->tick_count() >= 1) listen_for_chat(ui);
    // --lan-game-quit-at: the joiner leaves here; the host plays on
    if (!host_ && quit_at_ > 0 && checked_at_ && sim->tick_count() >= quit_at_) {
        spdlog::info("[lan-game] the joiner leaves at tick {}", sim->tick_count());
        check_chat(ui);
        check_pause(ui);
        left_ = true;
        done_ = true;
        return;
    }
    if (host_ && quit_at_ > 0 && checked_at_ && disconnect_dialog_open(ui)) dialog_seen_ = true;
    if (checked_at_) pause_frame(ui, *sim);
    if (!checked_at_ && sim->tick_count() >= kCheckTick) {
        check(ui, *sim);
        checked_at_ = frames_;
    }
    const bool over =
        checked_at_ && (sim->tick_count() >= kEndTick || frames_ - *checked_at_ > kWindDownFrames);
    if (over && !over_at_) over_at_ = frames_;
    // A frame or two more, so the frame's pumps (the disconnect dialog's
    // update) see what the lockstep did this frame
    if (over && frames_ >= *over_at_ + 2) {
        // Both play to the end together: the lockstep lets neither stop
        // short (a player who paused alone would stall the other, and be
        // dropped)
        if (sim->tick_count() < kEndTick)
            fail(fmt::format("stalled at tick {}, short of {}", sim->tick_count(), kEndTick));
        // No one dropped; or, the joiner having left, it alone: its army
        // defeated, and retail's disconnect dialog shown and closed
        const auto& mp = lua::mp_net_state();
        // The joiner's source: by slot, whichever it took
        const auto joiner = std::find_if(mp.clients.begin(), mp.clients.end(),
                                         [](const lua::SessionClient& c) { return c.uid == 1; });
        const auto joiner_source = static_cast<u32>(joiner - mp.clients.begin());
        for (const u32 source : mp.all_sources) {
            const bool dropped = mp.session && mp.session->has_dropped(source);
            const bool left = quit_at_ > 0 && source == joiner_source;
            if (dropped != left)
                fail(dropped ? fmt::format("the lockstep dropped source {}", source)
                             : fmt::format("source {}, which left, wasn't dropped", source));
        }
        if (host_ && quit_at_ > 0) {
            const i32 army = mp.army_of(joiner_source);
            const sim::ArmyBrain* gone =
                army >= 0 ? sim->army_at(static_cast<size_t>(army)) : nullptr;
            if (!gone || !gone->is_defeated())
                fail(fmt::format("the joiner's army {} wasn't defeated", army));
            if (!dialog_seen_) fail("retail's disconnect dialog never opened");
            if (disconnect_dialog_open(ui)) fail("retail's disconnect dialog stayed open");
        }
        check_chat(ui);
        check_pause(ui);
        done_ = true;
    }
}

void LanGameTest::check_pause(lua::LuaState& ui) {
    lua_State* L = ui.raw();
    lua_pushstring(L, "__osc_lan_paused");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const std::string heard = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    // The host's source (its client's place, by slot) with 2 of the lobby's
    // 3 timeouts left
    const std::string want = fmt::format("{}/2", number_global(L, "__osc_lan_host_source"));
    if (!saw_pause_) fail("the host's pause never held here");
    if (heard != want)
        fail(fmt::format("OnPause heard '{}', not '{}'", heard.empty() ? "nothing" : heard, want));
    if (number_global(L, "__osc_lan_resumed") < 1) fail("OnResume never came");
}

void LanGameTest::pause_frame(lua::LuaState& ui, const sim::SimState& sim) {
    // The host paused at the check: while it holds, no tick runs here; the
    // joiner resumes it after a while (any player may)
    auto r = ui.do_string("__osc_lan_is_paused = SessionIsPaused() and 1 or 0");
    const bool paused = r && number_global(ui.raw(), "__osc_lan_is_paused") != 0;
    if (!paused) {
        paused_tick_.reset();
        return;
    }
    if (!paused_tick_) {
        paused_tick_ = sim.tick_count();
        saw_pause_ = true;
    } else if (sim.tick_count() != *paused_tick_) {
        fail(fmt::format("ticked from {} to {} while paused", *paused_tick_, sim.tick_count()));
    }
    if (!host_ && ++paused_frames_ == kResumeAfterFrames) {
        if (auto s = ui.do_string("SessionResume()"); !s) fail("resuming: " + s.error().message);
    }
}

bool LanGameTest::disconnect_dialog_open(lua::LuaState& ui) {
    auto r = ui.do_string(R"(
        local module = __modules and __modules['/lua/ui/dialogs/disconnect.lua']
        __osc_lan_dialog = module and __osc_upvalue(module.Update, 'parent') and 1 or 0
    )");
    return r && number_global(ui.raw(), "__osc_lan_dialog") != 0;
}

void LanGameTest::listen_for_chat(lua::LuaState& ui) {
    listening_ = true;
    // The game's UI state is a fresh one: the upvalue reader again
    lua_State* L = ui.raw();
    lua_pushstring(L, "__osc_upvalue");
    lua_pushcfunction(L, l_upvalue);
    lua_rawset(L, LUA_GLOBALSINDEX);
    // The chat, and what the UI hears of pauses (M218f)
    auto r = ui.do_string(R"(
        local gamemain = import('/lua/ui/game/gamemain.lua')
        gamemain.RegisterChatFunc(function(sender, data)
            __osc_lan_chat = sender .. ': ' .. tostring(data.text)
        end, 'LanGameTest')
        __osc_lan_resumed = 0 -- (the UI's globals are strict: set before read)
        local onPause, onResume = gamemain.OnPause, gamemain.OnResume
        gamemain.OnPause = function(pausedBy, timeouts)
            __osc_lan_paused = tostring(pausedBy) .. '/' .. tostring(timeouts)
            return onPause(pausedBy, timeouts)
        end
        gamemain.OnResume = function()
            __osc_lan_resumed = __osc_lan_resumed + 1
            return onResume()
        end
    )");
    if (!r) fail("hearing chat and pauses: " + r.error().message);
}

void LanGameTest::check_chat(lua::LuaState& ui) {
    lua_State* L = ui.raw();
    lua_pushstring(L, "__osc_lan_chat");
    lua_rawget(L, LUA_GLOBALSINDEX);
    const std::string heard = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    if (heard != "LanHost: glhf")
        fail(fmt::format("the host's chat to everyone came as '{}'",
                         heard.empty() ? "nothing" : heard));
}

void LanGameTest::lobby_frame(lua::LuaState& ui) {
    if (!host_ && !joined_) {
        if (frames_ > kMaxHostWaitFrames) {
            fail(fmt::format("no host listened on port {}", port_));
            done_ = true;
            return;
        }
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
    // The session (M218d): its two clients, by source -- in slot order,
    // whoever hosted, since spawn is random -- and their sources; the host
    // pauses (M218f: the joiner resumes it) and chats to everyone
    lua_State* L = ui.raw();
    lua_pushstring(L, "__osc_lan_me");
    lua_pushstring(L, host_ ? "LanHost" : "LanJoiner");
    lua_rawset(L, LUA_GLOBALSINDEX);
    auto session = ui.do_string(R"(
        assert(SessionIsMultiplayer(), 'not a network game')
        local clients = GetSessionClients()
        assert(table.getn(clients) == 2, 'clients: ' .. table.getn(clients))
        local uids, mine = {LanHost = '0', LanJoiner = '1'}, nil
        for i, c in clients do
            assert(uids[c.name] == c.uid, 'client ' .. i .. ': ' .. tostring(c.name) .. ', ' .. tostring(c.uid))
            assert(c.connected, 'client ' .. i .. ' connected')
            if c['local'] then
                assert(not mine, 'one local client')
                mine = i
            end
        end
        assert(clients[1].name ~= clients[2].name, 'two players')
        __osc_lan_host_source = clients[1].name == 'LanHost' and 1 or 2
        assert(mine and clients[mine].name == __osc_lan_me, 'the local client')
        local names = SessionGetCommandSourceNames()
        assert(names[1] == clients[1].name and names[2] == clients[2].name, 'source names')
        assert(SessionGetLocalCommandSource() == mine, 'the local source')
        local armies = GetArmiesTable().armiesTable
        local a, b = armies[1].authorizedCommandSources[1], armies[2].authorizedCommandSources[1]
        assert(a and b and a ~= b and a >= 1 and a <= 2 and b >= 1 and b <= 2, 'the armies\' sources')
        assert(armies[GetFocusArmy()].authorizedCommandSources[1] == mine, 'this player\'s army')
        if __osc_lan_me == 'LanHost' then
            SessionRequestPause()
            SessionSendChatMessage({LanGameTest = true, text = 'glhf'})
        end
    )");
    if (!session) fail("the session: " + session.error().message);

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
    } else if (test_status::failure_count() == 0 && left_) {
        spdlog::info("[PASS] lan-game (joiner): played in lockstep, then left at tick {}",
                     sim ? sim->tick_count() : 0);
    } else if (test_status::failure_count() == 0) {
        spdlog::info("[PASS] lan-game ({}): retail's lobby launched a game played in lockstep to "
                     "tick {}, as army {}{}",
                     host_ ? "host" : "joiner", sim ? sim->tick_count() : 0,
                     lua::mp_net_state().local_army() + 1,
                     quit_at_ > 0 ? ", the joiner dropped when it left" : "");
    }
}

} // namespace osc::app
