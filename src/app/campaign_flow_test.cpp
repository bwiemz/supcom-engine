#include "app/campaign_flow_test.hpp"

#include "app/mods_flow_test.hpp"
#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"
#include "ui/lazyvar.hpp"
#include "ui/ui_control.hpp"
#include "ui/ui_dispatch.hpp"

#include <GLFW/glfw3.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cstring>
#include <optional>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::app {

namespace {

/// Frames a step may take before the flow counts as stuck (the game's
/// load and its intro's NIS are the longest), and how often a step looks.
constexpr u32 kStepFrames = 12000;
constexpr u32 kStepEvery = 10;
/// The timeline movie runs about 5,400 frames: a click skips it well
/// within this.
constexpr u32 kSkipFrames = 1500;

/// The UEF's button in X1CA_001's faction dialog (factionselect.lua's,
/// made with UIUtil.CreateButton, which keeps its textures, each a
/// SkinnableFile function giving the path)
constexpr const char* kUefButton =
    "local up = c.mNormal if type(up) == 'function' then up = up() end "
    "return type(up) == 'string' and string.find(up, 'logo-uef_btn_up', 1, true) ~= nil";

/// Run `code` in `L`: its string result, or "error: ..." (none: nil).
std::optional<std::string> evaluate(lua_State* L, const char* code) {
    const int top = lua_gettop(L);
    std::optional<std::string> result;
    if (luaL_loadbuffer(L, code, std::strlen(code), "=campaign-flow") != 0 ||
        lua_pcall(L, 0, 1, 0) != 0) {
        result = std::string("error: ") + (lua_isstring(L, -1) ? lua_tostring(L, -1) : "?");
    } else if (lua_type(L, -1) == LUA_TSTRING) {
        result = lua_tostring(L, -1);
    }
    lua_settop(L, top);
    return result;
}

/// The game's interface while the intro's NIS plays, when the faction
/// dialog shows: in campaign mode (the sim's first sync, before the
/// interface is built), and hidden (gamemain.HideGameUI, which toggles: a
/// first sync after the interface left it shown).
constexpr const char* kIntroInterface = R"(
    local campaign = import('/lua/ui/campaign/campaignmanager.lua').campaignMode
    local hidden = import('/lua/ui/game/gamemain.lua').gameUIHidden
    if campaign and hidden then return 'ok' end
    return 'campaignMode ' .. tostring(campaign) .. ', gameUIHidden ' .. tostring(hidden)
)";

/// The operation's flow, as the launch gave the sim (ScenarioInfo.campaignInfo),
/// checked once the faction pick has named the campaign; then the operation
/// won, as its scripts win it.
constexpr const char* kEndOperation = R"(
    local info = ScenarioInfo.campaignInfo
    if not info then return 'no ScenarioInfo.campaignInfo' end
    if info.campaignID ~= 'uef' then return 'wait' end
    if info.opKey ~= 'X1CA_001' or info.difficulty ~= 2 then
        return 'ScenarioInfo.campaignInfo is ' .. repr(info)
    end
    import('/lua/ScenarioFramework.lua').EndOperation(true, true, false)
    return 'ended'
)";

/// Once the faction is picked and the UI holds objectives: the game saved
/// as a campaign save in the current profile, listed, then loaded as the
/// Load dialog loads it. The UI's objective count, or "wait", or an error.
constexpr const char* kSaveAndLoad = R"(
    local held = 0
    for _ in import('/lua/ui/game/objectives2.lua').GetCurrentObjectiveTable() or {} do
        held = held + 1
    end
    if held == 0 then return 'wait' end
    local profile = import('/lua/user/prefs.lua').GetCurrentProfile()
    profile = profile and profile.Name or 'Player'
    local data = GetSpecialFiles('CampaignSave')
    local file = data.directory .. profile .. '/flow.' .. data.extension
    local result
    InternalSaveGame(file, 'flow', function(worked, errmsg)
        result = {worked = worked, errmsg = errmsg}
    end)
    if not (result and result.worked) then
        return 'error: InternalSaveGame: ' .. tostring(result and result.errmsg)
    end
    local listed = false
    for _, name in GetSpecialFiles('CampaignSave').files[profile] or {} do
        if name == 'flow' then listed = true end
    end
    if not listed then return 'error: the campaign save is not listed' end
    rawset(_G, '__osc_flow_before_load', true)
    local worked, err, detail = LoadSavedGame(file)
    if not worked then
        return 'error: LoadSavedGame: ' .. tostring(err) .. ' ' .. tostring(detail)
    end
    return tostring(held)
)";

/// In the loaded game's UI state (a new one: the old one's marker is gone),
/// once the post-load has run: campaign mode, and the objectives' count.
constexpr const char* kLoadedInterface = R"(
    if rawget(_G, '__osc_flow_before_load') then return 'wait' end
    local held = 0
    for _ in import('/lua/ui/game/objectives2.lua').GetCurrentObjectiveTable() or {} do
        held = held + 1
    end
    if held == 0 or not import('/lua/ui/campaign/campaignmanager.lua').campaignMode then
        return 'wait'
    end
    return tostring(held)
)";

/// The tutorial's opening done (its intro VO, then the zoom mission): the
/// zoom objective given, no army with a commander yet -- Moho spawns no
/// unit for an operation; the tutorial's player warps in later -- and none
/// defeated for it (the launch names no victory condition).
constexpr const char* kTutorialOpening = R"(
    if not ScenarioInfo.TUTZoom then return 'wait' end
    for _, brain in ArmyBrains do
        local n = table.getn(brain:GetListOfUnits(categories.COMMAND, false) or {})
        if n > 0 then return brain.Name .. ' has ' .. n .. ' commanders' end
        if brain:IsDefeated() then return brain.Name .. ' is defeated' end
    end
    return 'ok'
)";

/// The zoom objective's marker (its Objectives.Camera position): the UI's
/// world camera on it, as a player brings it on screen.
constexpr const char* kTutorialCamera =
    "GetCamera('WorldCamera'):MoveTo({150, 50, 360}, nil, 60, 0)";

/// The move mission, once its camera move is over (its objective takes a
/// result callback then): the commander ordered into its area.
constexpr const char* kTutorialMove = R"(
    local objective = ScenarioInfo.TUTMoveACU
    if ScenarioInfo.MissionNumber ~= 1 or not objective or not ScenarioInfo.PlayerCDR or
       table.getn(objective.ResultCallbacks) == 0 then
        return 'wait'
    end
    local rect = import('/lua/sim/ScenarioUtilities.lua').AreaToRect('TUT1_MoveACU')
    local x, z = (rect.x0 + rect.x1) / 2, (rect.y0 + rect.y1) / 2
    IssueMove({ScenarioInfo.PlayerCDR}, {x, GetTerrainHeight(x, z), z})
    return 'ordered'
)";

/// The attack mission, once its camera move is over: the commander sent at
/// each of the fodder.
constexpr const char* kTutorialAttack = R"(
    local objective = ScenarioInfo.TUTACUAttack
    if ScenarioInfo.MissionNumber ~= 2 or not objective or
       table.getn(objective.ResultCallbacks) == 0 then
        return 'wait'
    end
    for _, unit in ScenarioInfo.FodderUnits do
        if not unit:IsDead() then IssueAttack({ScenarioInfo.PlayerCDR}, unit) end
    end
    return 'ordered'
)";

/// The operation whose briefing shows (operationbriefing.CreateUI records it)
constexpr const char* kBriefingShown = R"(
    local shown = import('/lua/user/prefs.lua').GetFromCurrentProfile('Last_Op_Selected')
    return shown and shown.id or 'no briefing'
)";

/// The front end after the operation: the next one's briefing, and the
/// profile's campaign progress
constexpr const char* kProgress = R"(
    local Prefs = import('/lua/user/prefs.lua')
    local campaign = import('/lua/ui/campaign/campaignmanager.lua')
    local briefing = Prefs.GetFromCurrentProfile('Last_Op_Selected')
    if not briefing or briefing.id ~= 'X1CA_002' then
        return 'the briefing shown is ' .. repr(briefing)
    end
    if not campaign.IsOperationFinished('uef', 'X1CA_001', 2) then
        return 'X1CA_001 is not finished: ' .. repr(Prefs.GetFromCurrentProfile('campaign'))
    end
    if not campaign.IsOperationSelectable('uef', 'X1CA_002') then
        return 'X1CA_002 is not unlocked'
    end
    return 'ok'
)";

/// The control the mouse is captured by, at its centre, when it plays
/// `movie` (the timeline movie captures it).
std::optional<FoundControl> capturing_movie(lua_State* L, ui::UIControlRegistry& controls,
                                            const char* movie) {
    const ui::UIControl* capture = controls.input_capture();
    if (!capture || capture->lua_table_ref() < 0 ||
        capture->movie_filename().find(movie) == std::string::npos)
        return std::nullopt;
    lua_rawgeti(L, LUA_REGISTRYINDEX, capture->lua_table_ref());
    const int t = lua_gettop(L);
    const f64 x = ui::read_lazyvar(L, t, "Left") + ui::read_lazyvar(L, t, "Width") / 2;
    const f64 y = ui::read_lazyvar(L, t, "Top") + ui::read_lazyvar(L, t, "Height") / 2;
    lua_pop(L, 1);
    return FoundControl{capture, x, y};
}

} // namespace

void CampaignFlowTest::next(Step step) {
    spdlog::info("campaign-flow: step {} after {} frames", static_cast<int>(step), step_frames_);
    step_ = step;
    step_frames_ = 0;
    ordered_ = false;
}

void CampaignFlowTest::frame(lua::LuaState& ui, lua::LuaState* sim_lua, const sim::SimState* sim,
                             ui::UIDispatch& input, ui::UIControlRegistry& controls) {
    if (done_) return;
    ++frames_;
    if (++step_frames_ > kStepFrames) {
        fail(fmt::format("stuck at step {}", static_cast<int>(step_)));
        return;
    }
    if (step_frames_ % kStepEvery != 0) return;
    lua_State* L = ui.raw();
    const auto press = [&](const std::string& match) {
        return click(L, input, controls, find_control(L, controls, match));
    };
    switch (step_) {
    case Step::Campaign:
        if (press(labelled("<LOC _Campaign>"))) next(Step::Timeline);
        return;
    case Step::Timeline:
        // The first visit's movie (selectcampaign.TimelineFMV): a click,
        // once it has loaded, skips to X1CA_001's briefing (which records
        // its operation); watched to its end, it leads to operation select
        if (find_control(L, controls, labelled("<LOC opbrief_0003>Launch"))) {
            const auto shown = evaluate(L, kBriefingShown);
            if (!shown || *shown != "X1CA_001") {
                fail("the timeline movie led to " + shown.value_or("nothing"));
                return;
            }
            next(Step::FirstBriefing);
            return;
        }
        if (step_frames_ > kSkipFrames) {
            fail("the timeline movie took no click to skip it");
            return;
        }
        if (step_frames_ % 60 == 0)
            (void)click(L, input, controls, capturing_movie(L, controls, "timeline"));
        return;
    case Step::FirstBriefing:
        if (press(labelled("<LOC opbrief_0002>Back")))
            next(tutorial_ ? Step::TutorialEntry : Step::Select);
        return;
    case Step::Select:
        // Operation select, its last operation (X1CA_001) chosen
        if (press(labelled("<LOC sel_campaign_0013>Select"))) next(Step::Launch);
        return;
    case Step::Launch:
        if (press(labelled("<LOC opbrief_0003>Launch"))) next(Step::Faction);
        return;
    case Step::Faction: {
        if (!sim || !find_control(L, controls, kUefButton)) return;
        if (const auto result = evaluate(L, kIntroInterface); !result || *result != "ok") {
            fail("during the intro: " + result.value_or("nothing"));
            return;
        }
        if (press(kUefButton)) next(Step::Save);
        return;
    }
    case Step::Save: {
        if (!sim_lua) return;
        const auto picked = evaluate(sim_lua->raw(), R"(
            local info = ScenarioInfo.campaignInfo
            return info and info.campaignID == 'uef' and 'yes' or 'wait')");
        if (!picked || *picked != "yes") return;
        const auto result = evaluate(L, kSaveAndLoad);
        if (result && *result == "wait") return;
        if (!result || result->rfind("error: ", 0) == 0) {
            fail("saving and loading X1CA_001: " + result.value_or("nothing"));
            return;
        }
        objectives_ = static_cast<u32>(std::strtoul(result->c_str(), nullptr, 10));
        next(Step::Loaded);
        return;
    }
    case Step::Loaded: {
        if (!sim) return;
        const auto result = evaluate(L, kLoadedInterface);
        if (result && *result == "wait") return;
        if (!result || result->rfind("error: ", 0) == 0) {
            fail("after loading X1CA_001: " + result.value_or("nothing"));
            return;
        }
        const auto held = static_cast<u32>(std::strtoul(result->c_str(), nullptr, 10));
        if (held != objectives_) {
            fail(fmt::format("after loading X1CA_001: the UI holds {} objectives, {} before", held,
                             objectives_));
            return;
        }
        spdlog::info("campaign-flow: loaded X1CA_001 from its campaign save, in campaign mode "
                     "with its {} objectives",
                     held);
        next(Step::End);
        return;
    }
    case Step::End: {
        if (!sim_lua) return;
        const auto result = evaluate(sim_lua->raw(), kEndOperation);
        if (result && *result == "wait") return;
        if (!result || *result != "ended") {
            fail("ending X1CA_001: " + result.value_or("nothing"));
            return;
        }
        next(Step::Ok);
        return;
    }
    case Step::Ok:
        if (press(labelled("<LOC _Ok>"))) next(Step::Continue);
        return;
    case Step::Continue:
        if (press(labelled("<LOC _Continue>"))) next(Step::NextBriefing);
        return;
    case Step::NextBriefing: {
        if (sim || !find_control(L, controls, labelled("<LOC opbrief_0003>Launch"))) return;
        const auto result = evaluate(L, kProgress);
        if (!result || *result != "ok") {
            fail("after X1CA_001: " + result.value_or("nothing"));
            return;
        }
        passed_ = true;
        done_ = true;
        next(Step::Done);
        return;
    }
    case Step::TutorialEntry: {
        // Operation select's entries are bitmaps with a title over them: a
        // click on the title picks the entry, and the select button then
        // reads Launch Tutorial
        if (find_control(L, controls, labelled("<LOC sel_campaign_0017>Launch Tutorial"))) {
            next(Step::TutorialLaunch);
            return;
        }
        const auto title = find_control(
            L, controls,
            "return c.GetText and c:GetText() == LOC('<LOC sel_campaign_0000>Tutorial')");
        if (title) {
            input.on_cursor_pos(title->x, title->y);
            input.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
            input.on_mouse_button(GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
        }
        return;
    }
    case Step::TutorialLaunch:
        if (press(labelled("<LOC sel_campaign_0017>Launch Tutorial"))) next(Step::TutorialOpening);
        return;
    case Step::TutorialOpening: {
        if (!sim_lua) return;
        const auto result = evaluate(sim_lua->raw(), kTutorialOpening);
        if (result && *result == "wait") return;
        if (!result || *result != "ok") {
            fail("the tutorial's opening: " + result.value_or("nothing"));
            return;
        }
        next(Step::TutorialZoom);
        return;
    }
    case Step::TutorialZoom: {
        if (!sim_lua) return;
        if (!ordered_) {
            if (auto r = ui.do_string(kTutorialCamera); !r) {
                fail("moving the camera: " + r.error().message);
                return;
            }
            ordered_ = true;
        }
        const auto moving =
            evaluate(sim_lua->raw(), "return ScenarioInfo.MissionNumber == 1 and 'yes' or 'wait'");
        if (moving && *moving == "yes") next(Step::TutorialMove);
        return;
    }
    case Step::TutorialMove: {
        if (!sim_lua) return;
        if (!ordered_) {
            const auto result = evaluate(sim_lua->raw(), kTutorialMove);
            if (result && *result == "wait") return;
            if (!result || *result != "ordered") {
                fail("the move mission: " + result.value_or("nothing"));
                return;
            }
            ordered_ = true;
        }
        const auto attacking =
            evaluate(sim_lua->raw(), "return ScenarioInfo.MissionNumber == 2 and 'yes' or 'wait'");
        if (attacking && *attacking == "yes") next(Step::TutorialAttack);
        return;
    }
    case Step::TutorialAttack: {
        if (!sim_lua) return;
        if (!ordered_) {
            const auto result = evaluate(sim_lua->raw(), kTutorialAttack);
            if (result && *result == "wait") return;
            if (!result || *result != "ordered") {
                fail("the attack mission: " + result.value_or("nothing"));
                return;
            }
            ordered_ = true;
        }
        const auto building =
            evaluate(sim_lua->raw(), "return ScenarioInfo.MissionNumber == 3 and 'yes' or 'wait'");
        if (building && *building == "yes") {
            passed_ = true;
            done_ = true;
            next(Step::Done);
        }
        return;
    }
    case Step::Done: return;
    }
}

void CampaignFlowTest::fail(const std::string& why) {
    test_status::fail("[FAIL] campaign-flow: {}", why);
    done_ = true;
}

void CampaignFlowTest::finish() const {
    if (passed_ && tutorial_) {
        spdlog::info("[PASS] tutorial-flow: from operation select through the tutorial's zoom, "
                     "move and attack missions to its build-mass mission, in {} frames",
                     frames_);
    } else if (passed_) {
        spdlog::info("[PASS] campaign-flow: from the main menu through X1CA_001 to X1CA_002's "
                     "briefing, in {} frames",
                     frames_);
    } else if (test_status::failure_count() == 0) {
        test_status::fail("[FAIL] campaign-flow: stopped at step {} ({} frames)",
                          static_cast<int>(step_), frames_);
    }
}

} // namespace osc::app
