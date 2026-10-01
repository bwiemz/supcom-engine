#include "app/mods_flow_test.hpp"

#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"
#include "ui/ui_control.hpp"
#include "ui/ui_dispatch.hpp"

#include <GLFW/glfw3.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cstring>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::app {

namespace {

/// Frames to reach the report before the run counts as stuck.
constexpr u32 kMaxFrames = 20000;
/// Frames a lobby step may wait for its control, and between its tries
/// (a dialog builds over a few frames).
constexpr u32 kStepFrames = 3000;
constexpr u32 kStepEvery = 10;

/// Retail's Resource Rich (gamedata/mods.scd), which the lobby run picks.
constexpr const char* kResourceRich = "74A9EAB2-E851-11DB-A1F1-F2C755D89593";

/// The front end's skirmish (as --auto-skirmish's), with the game mods
/// retail's lobby would launch: Mods.GetGameMods() of the player's
/// selection. The AI plays UEF too: the test is about the mods.
constexpr const char* kLaunch = R"(
    local scenario = '/maps/SCMP_009/SCMP_009_scenario.lua'
    LaunchSinglePlayerSession({
        GameOptions = { ScenarioFile = scenario },
        PlayerOptions = {
            [1] = { Human = true, PlayerName = 'Player', Faction = 1, Team = 1, StartSpot = 1 },
            [2] = { Human = false, PlayerName = 'AI', AIPersonality = 'adaptive',
                    Faction = 1, Team = 2, StartSpot = 2 },
        },
        GameMods = import('/lua/mods.lua').GetGameMods(),
    })
)";

/// A UEF AI in the lobby's second slot, as its slot menu adds one (the
/// single-player lobby's host is "0").
constexpr const char* kAddAi = R"(
    import('/lua/ui/lobby/lobby.lua').HostTryAddPlayer('0', 2, 'AI', false, 'adaptive', nil, 1)
)";

/// What a state's mods made of it, on one line.
constexpr const char* kReport = R"(
    local uids = {}
    for i, m in ipairs(__active_mods) do table.insert(uids, tostring(m.uid)) end
    local bps = rawget(_G, '__blueprints')
    local acu = bps and bps.uel0001
    local economy = acu and acu.Economy or {}
    local general = acu and acu.General or {}
    local selected = {}
    local prefs = rawget(_G, 'GetPreference') and GetPreference('active_mods') or {}
    for uid, on in prefs do
        if on then table.insert(selected, uid) end
    end
    table.sort(selected)
    return 'mods=' .. table.concat(uids, ',')
        .. ' mass=' .. tostring(economy.ProductionPerSecondMass)
        .. ' energy=' .. tostring(economy.ProductionPerSecondEnergy)
        .. ' name=' .. tostring(general.UnitName)
        .. ' probe=' .. tostring(rawget(_G, 'OscModsFlowProbe'))
        .. ' selected=' .. table.concat(selected, ',')
)";

/// The mod manager's entry for `uid` (`and_also`: more it must be).
std::string mod_entry(const char* uid, const char* and_also = "true") {
    return fmt::format("return c.modInfo and c.modInfo.uid == '{}' and {}", uid, and_also);
}

/// Neither the control nor any of its parents hidden or destroyed.
bool shown(const ui::UIControl* c) {
    for (; c; c = c->parent())
        if (c->destroyed() || c->hidden()) return false;
    return true;
}

/// Whether a click at the found control's centre reaches it: it is what
/// the mouse hits there, from the top input capture or the root frame as
/// the dispatch hit-tests. A menu still animating in, or a dialog over it,
/// takes the click instead; a player would wait.
bool takes_click(lua_State* L, ui::UIDispatch& input, ui::UIControlRegistry& controls,
                 const FoundControl& found) {
    ui::UIControl* hit_root = controls.input_capture();
    if (!hit_root) {
        lua_pushstring(L, "__osc_root_frame");
        lua_rawget(L, LUA_REGISTRYINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "_c_object");
            lua_rawget(L, -2);
            hit_root = static_cast<ui::UIControl*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    return hit_root && input.hit_test(L, hit_root, found.x, found.y) == found.control;
}

} // namespace

std::string labelled(const char* loc) {
    return fmt::format("return c.label and c.label.GetText and c.label:GetText() == LOC('{}')",
                       loc);
}

bool click(lua_State* L, ui::UIDispatch& input, ui::UIControlRegistry& controls,
           const std::optional<FoundControl>& found) {
    if (!found || !takes_click(L, input, controls, *found)) return false;
    input.on_cursor_pos(found->x, found->y);
    input.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
    input.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
    return true;
}

std::optional<FoundControl> find_control(lua_State* L, ui::UIControlRegistry& controls,
                                         const std::string& match) {
    const std::string source = "return function(c) local function picked() " + match +
                               " end if picked() then return c.Left() + c.Width() / 2, "
                               "c.Top() + c.Height() / 2 end end";
    const int top = lua_gettop(L);
    if (luaL_loadbuffer(L, source.data(), source.size(), "=find_control") != 0 ||
        lua_pcall(L, 0, 1, 0) != 0) {
        spdlog::warn("find_control: {}", lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_settop(L, top);
        return std::nullopt;
    }
    const int picker = lua_gettop(L);
    const auto& all = controls.all();
    for (auto it = all.rbegin(); it != all.rend(); ++it) { // the newest first
        const ui::UIControl* c = it->get();
        if (!c || c->lua_table_ref() < 0 || !shown(c)) continue;
        lua_pushvalue(L, picker);
        lua_rawgeti(L, LUA_REGISTRYINDEX, c->lua_table_ref());
        // (A control the match can't read, it doesn't pick)
        if (lua_pcall(L, 1, 2, 0) == 0 && lua_type(L, -2) == LUA_TNUMBER &&
            lua_type(L, -1) == LUA_TNUMBER) {
            const FoundControl found{c, lua_tonumber(L, -2), lua_tonumber(L, -1)};
            lua_settop(L, top);
            return found;
        }
        lua_settop(L, picker);
    }
    lua_settop(L, top);
    return std::nullopt;
}

bool ModsFlowTest::start(lua::LuaState& ui) {
    const char* what = launch_ == Launch::Direct ? "a skirmish with the player's mods"
                       : launch_ == Launch::Lobby
                           ? "retail's lobby, its mod manager, and a skirmish"
                           : "a recorded game's mods";
    spdlog::info("=== mods flow test: {} ===", what);
    if (launch_ != Launch::Direct) return true; // the lobby's clicks, or the replay's
    if (auto r = ui.do_string(kLaunch); !r) {
        fail("launching: " + r.error().message);
        return false;
    }
    return true;
}

void ModsFlowTest::frame(lua::LuaState& ui, lua::LuaState* sim_lua, const sim::SimState* sim,
                         ui::UIDispatch& input, ui::UIControlRegistry& controls) {
    if (done_) return;
    if (++frames_ > kMaxFrames) {
        done_ = true; // stuck: reported by finish
        return;
    }
    if (launch_ == Launch::Lobby && step_ != Step::Launched) {
        lobby_frame(ui, input, controls);
        return;
    }
    if (!sim || !sim_lua || sim->tick_count() < kReportTick) return;
    spdlog::info("mods-flow: sim: {}", report(sim_lua->raw()));
    spdlog::info("mods-flow: ui: {}", report(ui.raw()));
    reported_ = true;
    done_ = true;
}

void ModsFlowTest::lobby_frame(lua::LuaState& ui, ui::UIDispatch& input,
                               ui::UIControlRegistry& controls) {
    if (++step_frames_ > kStepFrames) {
        fail(fmt::format("the lobby never got past step {}", static_cast<int>(step_)));
        return;
    }
    if (step_frames_ % kStepEvery != 0) return;
    lua_State* L = ui.raw();
    const auto next = [&](Step step) {
        step_ = step;
        step_frames_ = 0;
    };
    switch (step_) {
    case Step::Skirmish: // the main menu's
        if (click(L, input, controls, find_control(L, controls, labelled("<LOC _Skirmish>"))))
            next(Step::Tutorial);
        return;
    case Step::Tutorial:
        // A first run asks about the tutorial first: no.
        if (click(L, input, controls, find_control(L, controls, labelled("<LOC _No>")))) {
            next(Step::AddAi);
        } else if (find_control(L, controls, labelled("<LOC lobui_0212>Launch"))) {
            next(Step::AddAi);
        }
        return;
    case Step::AddAi:
        // The lobby is up once its Launch button shows.
        if (!find_control(L, controls, labelled("<LOC lobui_0212>Launch"))) return;
        if (auto r = ui.do_string(kAddAi); !r) {
            fail("adding the AI: " + r.error().message);
            return;
        }
        next(Step::Options);
        return;
    case Step::Options:
        if (click(L, input, controls,
                  find_control(L, controls, labelled("<LOC map_sel_0000>Game Options"))))
            next(Step::ModsButton);
        return;
    case Step::ModsButton: // the map dialog's
        if (click(L, input, controls, find_control(L, controls, labelled("<LOC tooltipui0145>"))))
            next(Step::Mod);
        return;
    case Step::Mod:
        if (click(L, input, controls, find_control(L, controls, mod_entry(kResourceRich))))
            next(Step::Close);
        return;
    case Step::Close:
        // Picked, the mod manager closes on Enter (UIUtil.MakeInputModal's)
        if (!find_control(L, controls, mod_entry(kResourceRich, "c.active"))) return;
        input.on_key(GLFW_KEY_ENTER, GLFW_PRESS, 0);
        input.on_key(GLFW_KEY_ENTER, GLFW_RELEASE, 0);
        next(Step::MapOk);
        return;
    case Step::MapOk:
        if (find_control(L, controls, "return c.modInfo ~= nil")) return; // still open
        if (click(L, input, controls, find_control(L, controls, labelled("<LOC _OK>"))))
            next(Step::Launch);
        return;
    case Step::Launch:
        if (find_control(L, controls, labelled("<LOC tooltipui0145>"))) return; // map dialog open
        if (click(L, input, controls,
                  find_control(L, controls, labelled("<LOC lobui_0212>Launch"))))
            next(Step::Launched);
        return;
    case Step::Launched: return;
    }
}

void ModsFlowTest::fail(const std::string& why) {
    test_status::fail("[FAIL] mods-flow: {}", why);
    done_ = true;
}

void ModsFlowTest::finish() const {
    if (reported_) {
        spdlog::info("[PASS] mods-flow: reported at tick {}", kReportTick);
    } else if (test_status::failure_count() == 0) {
        test_status::fail("[FAIL] mods-flow: the game never reached tick {} ({} frames)",
                          kReportTick, frames_);
    }
}

std::string ModsFlowTest::report(lua_State* L) {
    const int top = lua_gettop(L);
    std::string line;
    if (luaL_loadbuffer(L, kReport, std::strlen(kReport), "=mods-flow") != 0 ||
        lua_pcall(L, 0, 1, 0) != 0) {
        line = std::string("error: ") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
        test_status::fail("[FAIL] mods-flow: the report: {}", line);
    } else if (lua_type(L, -1) == LUA_TSTRING) {
        line = lua_tostring(L, -1);
    }
    lua_settop(L, top);
    return line;
}

} // namespace osc::app
