// A brain's AI personality, as Moho's CAiPersonality (faf-re
// CAiPersonality.cpp): the AverageJoe row of /lua/aipersonality.lua's
// AIPersonalityTemplate, each range at difficulty 0.5, one object a brain
// whose class is the module's AIPersonality.

#include <catch2/catch_test_macros.hpp>

#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/sim_state.hpp"

#include <string>

extern "C" {
#include <lua.h>
}

namespace {

/// A sim of two armies whose brains Lua sees as brain1 and brain2, and
/// whose import serves `modules` (a Lua table of path -> module).
struct Game {
    osc::lua::LuaState lua;
    osc::sim::SimState sim{lua.raw(), nullptr};

    explicit Game(const std::string& modules) {
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        osc::lua::register_moho_bindings(lua, sim);
        lua_State* L = lua.raw();
        for (size_t i = 0; i < 2; ++i) {
            lua_newtable(L);
            lua_pushstring(L, "_c_object");
            lua_pushlightuserdata(L, sim.army_at(i));
            lua_rawset(L, -3);
            lua_setglobal(L, i == 0 ? "brain1" : "brain2");
        }
        const auto made = lua.do_string("local modules = " + modules + R"(
            function import(path) return modules[path] or error('no module ' .. path) end
            function personality(brain) return moho.aibrain_methods.GetPersonality(brain) end
        )");
        REQUIRE(made.ok());
    }

    /// Run `code`; its first failed assert is the test's failure.
    void check(const char* code) {
        const auto result = lua.do_string(code);
        INFO((result.ok() ? std::string() : result.error().message));
        CHECK(result.ok());
    }
};

/// A template row: every range {0, 1}, no favourites, then `overrides`.
constexpr const char* kRow = R"(
    function row(name, chat, overrides)
        local r = {name, chat}
        for i = 3, 33 do r[i] = {0.0, 1.0} end
        r[18] = {}
        r[27] = {}
        for i, v in pairs(overrides) do r[i] = v end
        return r
    end
)";

} // namespace

TEST_CASE("An AI personality is AverageJoe's row at difficulty 0.5, as Moho's",
          "[ai][personality]") {
    Game game(std::string("(function()\n") + kRow + R"(
        AIPersonality = setmetatable({}, {__index = moho.aipersonality_methods})
        AIPersonality.__index = AIPersonality
        return {['/lua/aipersonality.lua'] = {
            AIPersonality = AIPersonality,
            AIPersonalityTemplate = {
                -- Short of the 33 fields: not a template
                {'AverageJoe', 'None', {7, 7}},
                row('Rommel', 'None', {[3] = {100, 200}}),
                row('averagejoe', 'Chatty', {
                    [3] = {50, 100},                 -- Army Total Unit Size
                    [4] = {1, 2},                    -- Platoon Size Mult
                    [18] = {'T1LandFactory', 'T1EnergyProduction'},
                    [19] = {0.25, 0.75},             -- Air Units Emphasis
                    [26] = {0.5, 1.0},               -- InDirect Damage Emphasis
                    [28] = {0, 0.5},                 -- Survival Emphasis
                    [33] = {1, 3},                   -- Chat Frequency
                }),
            },
        }}
    end)()
    )");

    // One object a brain, the module's class
    game.check(R"(
        local p = personality(brain1)
        assert(p == personality(brain1), 'a brain answers the same personality')
        assert(p ~= personality(brain2), 'each brain has its own')
        assert(getmetatable(p) == AIPersonality, 'its class is AIPersonality')
    )");
    // The row named AverageJoe without regard to case, each range's middle
    game.check(R"(
        local p = personality(brain1)
        assert(p:GetPersonalityName() == 'averagejoe', p:GetPersonalityName())
        assert(p:GetChatPersonality() == 'Chatty')
        assert(p:GetDifficulty() == 0.5)
        assert(p:GetArmySize() == 75, p:GetArmySize())
        assert(p:GetPlatoonSize() == 1.5, p:GetPlatoonSize())
        assert(p:GetAirUnitsEmphasis() == 0.5, p:GetAirUnitsEmphasis())
        assert(p:GetTankUnitsEmphasis() == 0.5)
        assert(p:GetInDirectDamageEmphasis() == 0.75, p:GetInDirectDamageEmphasis())
        assert(p:GetSurvivalEmphasis() == 0.25, p:GetSurvivalEmphasis())
        assert(p:GetChatFrequency() == 2, p:GetChatFrequency())
        local favourites = p:GetFavouriteStructures()
        assert(table.getn(favourites) == 2 and favourites[1] == 'T1LandFactory'
               and favourites[2] == 'T1EnergyProduction')
        assert(table.getn(p:GetFavouriteUnits()) == 0)
    )");
    // AdjustDelay: the delay and half of delay x spread, rounded to even
    game.check(R"(
        local p = personality(brain1)
        assert(p:AdjustDelay(100, 2) == 200, p:AdjustDelay(100, 2))
        assert(p:AdjustDelay(20, 4) == 60, p:AdjustDelay(20, 4))
        assert(p:AdjustDelay(3, 1) == 5, p:AdjustDelay(3, 1))
        assert(p:AdjustDelay(5, 1) == 7, p:AdjustDelay(5, 1))
        local ok, message = pcall(p.AdjustDelay, p, 'soon', 1)
        assert(not ok and string.find(message, 'integer expected'), message)
    )");
}

TEST_CASE("Without its template an AI personality keeps the constructor's", "[ai][personality]") {
    // Neither AIPersonality nor AIPersonalityTemplate: the object is over
    // moho.aipersonality_methods, named None, every range {0, 0}
    Game game("{['/lua/aipersonality.lua'] = {}}");
    game.check(R"(
        local p = personality(brain1)
        assert(getmetatable(p).__index == moho.aipersonality_methods)
        assert(p:GetPersonalityName() == 'None')
        assert(p:GetArmySize() == 0)
        assert(p:GetDifficulty() == 0.5)
        assert(p:AdjustDelay(100, 2) == 200)
    )");
}
