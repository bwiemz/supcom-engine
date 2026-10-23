// --unitview-sweep-test: every unit's build button hovered.
//
// A build button's MouseEnter runs the construction panel's rollover, which
// shows the unit view's detail for that blueprint (unitviewDetail.Show).
// MAUI's Button sets its hover latch only after the rollover returns, so a
// script error there loses the button's click. FAF's detail, with the
// armament option on, formats each weapon's MinRadius and the builder's
// MaxBuildDistance unguarded: it relies on Moho's blueprint write-back to
// fill them (docs/plans/2026-10-23-moho-blueprint-reflection-design.md).

#include "unitview_sweep_test.hpp"

#include "blueprints/blueprint_store.hpp"
#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "render_probe.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <charconv>
#include <string>
#include <system_error>
#include <vector>

namespace osc::test {

namespace {

/// Runs `body` as a function in the UI state; what it returns, as a string
/// (a script error's message, prefixed, when it fails).
std::string ui_answer(TestContext& ui, const std::string& body) {
    auto r =
        ui.lua_state.do_string("rawset(_G, '__osc_uvs_answer', (function() " + body + " end)())");
    if (!r) return "script error: " + r.error().message;
    lua_State* L = ui.L;
    lua_pushstring(L, "__osc_uvs_answer");
    lua_rawget(L, LUA_GLOBALSINDEX);
    std::string answer = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return answer;
}

/// The sweep's answer begins "<swept> <failed>": false when it doesn't.
bool read_counts(const std::string& answer, unsigned& swept, unsigned& failed) {
    const char* const end = answer.data() + answer.size();
    const auto first = std::from_chars(answer.data(), end, swept);
    if (first.ec != std::errc{} || first.ptr == end || *first.ptr != ' ') return false;
    return std::from_chars(first.ptr + 1, end, failed).ec == std::errc{};
}

/// The option on, the commander selected: 'ready', or why not.
constexpr const char* kSelect = R"(
    local options = import('/lua/user/prefs.lua').GetFromCurrentProfile('options')
    if not (options and options.gui_render_armament_detail == 1) then
        return 'the profile leaves gui_render_armament_detail off '
            .. '(run with --prefs tests/integration/fixtures/armament_detail.prefs)'
    end
    local acu = GetArmyAvatars()[1]
    if not acu then return 'army 1 has no commander' end
    SelectUnits({acu})
    return 'ready'
)";

/// Each id of __osc_uvs_ids hovered: Show with the commander building, then
/// the rollover's enter and exit. "<swept> <failed>\n", then each error
/// with how many failed on it and the first of them.
constexpr const char* kSweep = R"(
    local detail = import('/lua/ui/game/unitviewdetail.lua')
    local construction = import('/lua/ui/game/construction.lua')
    local acu = GetArmyAvatars()[1]
    local glow = {SetNeedsFrameUpdate = function() end, SetAlpha = function() end}
    local swept, failed, errors, order = 0, 0, {}, {}
    for _, id in rawget(_G, '__osc_uvs_ids') do
        local button = {Data = {type = 'item', id = id}, Glow = glow}
        local ok, err = pcall(detail.Show, __blueprints[id], acu, id)
        if ok then ok, err = pcall(construction.OnRolloverHandler, button, 'enter') end
        local left, why = pcall(construction.OnRolloverHandler, button, 'exit')
        if ok and not left then ok, err = false, why end
        swept = swept + 1
        if not ok then
            failed = failed + 1
            err = tostring(err)
            if not errors[err] then
                errors[err] = {n = 0, first = id}
                table.insert(order, err)
            end
            errors[err].n = errors[err].n + 1
        end
    end
    detail.Hide()
    local lines = swept .. ' ' .. failed .. '\n'
    for _, err in order do
        lines = lines .. errors[err].n .. ' (' .. errors[err].first .. ' first): ' .. err .. '\n'
    end
    return lines
)";

} // namespace

void test_unitview_sweep(TestContext& ui, const std::function<void(int)>& pump,
                         const std::function<void(int)>& play) {
    spdlog::info("=== Unit view sweep: every unit's build button hovered ===");
    Tally t;
    play(2);
    const std::string selected = ui_answer(ui, kSelect);
    if (selected != "ready") {
        t.check(false, "Test 1: the commander selected, the armament detail on: " + selected);
        return;
    }
    pump(6); // the selection reaches the construction panel

    // Every unit blueprint, by id, in order: the UI state's are the sim's
    std::vector<std::string> ids;
    for (const auto* entry : ui.store.get_all(blueprints::BlueprintType::Unit))
        ids.push_back(entry->id);
    std::sort(ids.begin(), ids.end());
    lua_State* L = ui.L;
    lua_pushstring(L, "__osc_uvs_ids");
    lua_newtable(L);
    for (size_t i = 0; i < ids.size(); ++i) {
        lua_pushstring(L, ids[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_rawset(L, LUA_GLOBALSINDEX);

    const std::string answer = ui_answer(ui, kSweep);
    unsigned swept = 0;
    unsigned failed = 0;
    const bool counted = read_counts(answer, swept, failed);
    const size_t eol = answer.find('\n');
    const std::string errors = eol == std::string::npos ? answer : answer.substr(eol + 1);
    if (!errors.empty()) spdlog::info("unit view sweep, its failures by error:\n{}", errors);
    t.check(counted && swept == ids.size() && swept >= 100 && failed == 0,
            fmt::format("Test 2: {} of {} unit blueprints hovered, {} failed{}", swept, ids.size(),
                        failed, counted ? "" : " (" + answer + ")"));
    lua_pushstring(L, "__osc_uvs_ids");
    lua_pushnil(L);
    lua_rawset(L, LUA_GLOBALSINDEX);
    pump(1);
}

} // namespace osc::test
