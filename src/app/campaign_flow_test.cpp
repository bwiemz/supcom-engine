#include "app/campaign_flow_test.hpp"

#include "app/mods_flow_test.hpp"
#include "core/test_status.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"
#include "ui/lazyvar.hpp"
#include "ui/ui_control.hpp"
#include "ui/ui_dispatch.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

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
        if (press(labelled("<LOC opbrief_0002>Back"))) next(Step::Select);
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
        if (press(kUefButton)) next(Step::End);
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
    case Step::Done: return;
    }
}

void CampaignFlowTest::fail(const std::string& why) {
    test_status::fail("[FAIL] campaign-flow: {}", why);
    done_ = true;
}

void CampaignFlowTest::finish() const {
    if (passed_) {
        spdlog::info("[PASS] campaign-flow: from the main menu through X1CA_001 to X1CA_002's "
                     "briefing, in {} frames",
                     frames_);
    } else if (test_status::failure_count() == 0) {
        test_status::fail("[FAIL] campaign-flow: stopped at step {} ({} frames)",
                          static_cast<int>(step_), frames_);
    }
}

} // namespace osc::app
