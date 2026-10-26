// An AI brain's personality: moho.aipersonality_methods, and the object
// brain:GetPersonality() answers. As Moho's CAiPersonality (faf-re
// CAiPersonality.cpp): the AverageJoe row of /lua/aipersonality.lua's
// AIPersonalityTemplate, at difficulty 0.5.

#include "lua/moho_bindings_internal.hpp"
#include "sim/ai_personality.hpp"
#include "sim/army_brain.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

namespace {

using sim::AiPersonality;

constexpr const char* kModule = "/lua/aipersonality.lua";
constexpr const char* kRowName = "AverageJoe";
/// A row is a template only with all of its fields (FindTemplateRow).
constexpr int kRowFields = 33;
/// The registry's table of the brains' personality objects, by army from 1.
constexpr const char* kObjectsKey = "__osc_ai_personalities";

bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

/// import(path)'s module, pushed; nil when it doesn't load.
void push_module(lua_State* L, const char* path) {
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return;
    }
    lua_pushstring(L, path);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        const char* error = lua_tostring(L, -1);
        spdlog::warn("import('{}') failed: {}", path, error ? error : "");
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

/// A field of the row at `row` as a string ("" for none).
std::string string_field(lua_State* L, int row, int field) {
    lua_rawgeti(L, row, field);
    const char* s = lua_tostring(L, -1);
    std::string out = s ? s : "";
    lua_pop(L, 1);
    return out;
}

/// LoadRangeField: {min, max}; a field that isn't a table is {0, 0}.
AiPersonality::Range range_field(lua_State* L, int row, int field) {
    AiPersonality::Range r;
    lua_rawgeti(L, row, field);
    if (lua_istable(L, -1)) {
        lua_rawgeti(L, -1, 1);
        r.min = static_cast<f32>(lua_tonumber(L, -1));
        lua_rawgeti(L, -2, 2);
        r.max = static_cast<f32>(lua_tonumber(L, -1));
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
    return r;
}

/// LoadStringListField: the list's strings, in order.
std::vector<std::string> list_field(lua_State* L, int row, int field) {
    std::vector<std::string> out;
    lua_rawgeti(L, row, field);
    if (lua_istable(L, -1)) {
        const int list = lua_gettop(L);
        const int n = luaL_getn(L, list);
        for (int i = 1; i <= n; ++i) out.push_back(string_field(L, list, i));
    }
    lua_pop(L, 1);
    return out;
}

/// CAiPersonality::ReadData: the AverageJoe row of the module's
/// AIPersonalityTemplate. Without one it keeps the constructor's: named
/// "None", every range {0, 0}.
AiPersonality read_personality(lua_State* L) {
    AiPersonality p;
    const int top = lua_gettop(L);
    push_module(L, kModule);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "AIPersonalityTemplate");
        lua_gettable(L, -2);
    } else {
        lua_pushnil(L);
    }
    if (!lua_istable(L, -1)) {
        spdlog::info("Can't find AIPersonalityTemplate");
        lua_settop(L, top);
        return p;
    }
    const int templates = lua_gettop(L);
    int row = 0;
    const int rows = luaL_getn(L, templates);
    for (int i = 1; i <= rows && row == 0; ++i) {
        lua_rawgeti(L, templates, i);
        if (lua_istable(L, -1) && luaL_getn(L, -1) == kRowFields &&
            iequal(string_field(L, lua_gettop(L), 1), kRowName)) {
            row = lua_gettop(L);
        } else {
            lua_pop(L, 1);
        }
    }
    if (row == 0) {
        spdlog::info("Can't find the template for personality {}", kRowName);
        lua_settop(L, top);
        return p;
    }
    p.name = string_field(L, row, 1);
    p.chat = string_field(L, row, 2);
    for (int v = 0; v < AiPersonality::kValueCount; ++v) {
        const auto value = static_cast<AiPersonality::Value>(v);
        p.ranges[value] = range_field(L, row, AiPersonality::field_of(value));
    }
    p.favourite_structures = list_field(L, row, 18);
    p.favourite_units = list_field(L, row, 27);
    lua_settop(L, top);
    return p;
}

/// The brains' personality: every brain's is the same row at the same
/// difficulty, so the sim reads it once, as the first is asked for.
const AiPersonality& personality_of(lua_State* L) {
    static const AiPersonality kNone;
    auto* sim = get_sim(L);
    if (!sim) return kNone;
    if (!sim->ai_personality()) sim->set_ai_personality(read_personality(L));
    return *sim->ai_personality();
}

/// A personality object's class: the module's AIPersonality, else a
/// metatable over moho.aipersonality_methods (LoadAiPersonalityMetatable).
void push_personality_class(lua_State* L) {
    push_module(L, kModule);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "AIPersonality");
        lua_gettable(L, -2);
        lua_remove(L, -2);
    }
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    spdlog::info("Can't find AIPersonality, using CAiPersonality directly");
    lua_newtable(L);
    lua_pushstring(L, "__index");
    lua_pushstring(L, "moho");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "aipersonality_methods");
        lua_rawget(L, -2);
        lua_remove(L, -2);
    }
    lua_rawset(L, -3);
}

/// Push the strings as a list, as Moho's LuaObject::Insert builds it
/// (table.insert's, which records the list's length).
void push_strings(lua_State* L, const std::vector<std::string>& values) {
    lua_newtable(L);
    int i = 0;
    for (const std::string& v : values) {
        lua_pushstring(L, v.c_str());
        lua_rawseti(L, -2, ++i);
    }
    if (i > 0) luaL_setn(L, -1, i);
}

/// AdjustDelay(delay, spread): two integers, the delay lengthened by the
/// difficulty's share of delay x spread. Retail's AI waits AdjustDelay(100, 2)
/// ticks between plan evaluations and AdjustDelay(20, 4) between executions.
int personality_AdjustDelay(lua_State* L) {
    if (lua_type(L, 2) != LUA_TNUMBER) return luaL_typerror(L, 2, "integer");
    const int delay = static_cast<int>(lua_tonumber(L, 2));
    if (lua_type(L, 3) != LUA_TNUMBER) return luaL_typerror(L, 3, "integer");
    const int spread = static_cast<int>(lua_tonumber(L, 3));
    lua_pushnumber(L, static_cast<f32>(personality_of(L).adjust_delay(delay, spread)));
    return 1;
}

int personality_GetPersonalityName(lua_State* L) {
    lua_pushstring(L, personality_of(L).name.c_str());
    return 1;
}

int personality_GetChatPersonality(lua_State* L) {
    lua_pushstring(L, personality_of(L).chat.c_str());
    return 1;
}

int personality_GetDifficulty(lua_State* L) {
    lua_pushnumber(L, personality_of(L).difficulty);
    return 1;
}

int personality_GetFavouriteStructures(lua_State* L) {
    push_strings(L, personality_of(L).favourite_structures);
    return 1;
}

int personality_GetFavouriteUnits(lua_State* L) {
    push_strings(L, personality_of(L).favourite_units);
    return 1;
}

/// A range's value at the difficulty (PushDifficultyScaledPersonalityRange).
template <AiPersonality::Value V> int personality_value(lua_State* L) {
    lua_pushnumber(L, personality_of(L).value(V));
    return 1;
}

} // namespace

void push_brain_personality(lua_State* L, const sim::ArmyBrain& brain) {
    lua_pushstring(L, kObjectsKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, kObjectsKey);
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    const int objects = lua_gettop(L);
    lua_rawgeti(L, objects, brain.index() + 1);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        // CAiBrain's constructor makes its personality, which reads its data
        (void)personality_of(L);
        lua_newtable(L);
        push_personality_class(L);
        lua_setmetatable(L, -2);
        lua_pushvalue(L, -1);
        lua_rawseti(L, objects, brain.index() + 1);
    }
    lua_remove(L, objects);
}

// clang-format off
const MethodEntry aipersonality_methods[] = {
    {"AdjustDelay",                personality_AdjustDelay},
    {"GetPersonalityName",         personality_GetPersonalityName},
    {"GetChatPersonality",         personality_GetChatPersonality},
    {"GetDifficulty",              personality_GetDifficulty},
    {"GetArmySize",                personality_value<AiPersonality::ArmySize>},
    {"GetPlatoonSize",             personality_value<AiPersonality::PlatoonSize>},
    {"GetAttackFrequency",         personality_value<AiPersonality::AttackFrequency>},
    {"GetRepeatAttackFrequency",   personality_value<AiPersonality::RepeatAttackFrequency>},
    {"GetCounterForces",           personality_value<AiPersonality::CounterForces>},
    {"GetIntelGathering",          personality_value<AiPersonality::IntelGathering>},
    {"GetCoordinatedAttacks",      personality_value<AiPersonality::CoordinatedAttacks>},
    {"GetExpansionDriven",         personality_value<AiPersonality::ExpansionDriven>},
    {"GetTechAdvancement",         personality_value<AiPersonality::TechAdvancement>},
    {"GetUpgradesDriven",          personality_value<AiPersonality::UpgradesDriven>},
    {"GetDefenseDriven",           personality_value<AiPersonality::DefenseDriven>},
    {"GetEconomyDriven",           personality_value<AiPersonality::EconomyDriven>},
    {"GetFactoryTycoon",           personality_value<AiPersonality::FactoryTycoon>},
    {"GetIntelBuildingTycoon",     personality_value<AiPersonality::IntelBuildingTycoon>},
    {"GetSuperWeaponTendency",     personality_value<AiPersonality::SuperWeaponTendency>},
    {"GetFavouriteStructures",     personality_GetFavouriteStructures},
    {"GetAirUnitsEmphasis",        personality_value<AiPersonality::AirUnitsEmphasis>},
    {"GetTankUnitsEmphasis",       personality_value<AiPersonality::TankUnitsEmphasis>},
    {"GetBotUnitsEmphasis",        personality_value<AiPersonality::BotUnitsEmphasis>},
    {"GetSeaUnitsEmphasis",        personality_value<AiPersonality::SeaUnitsEmphasis>},
    {"GetSpecialtyForcesEmphasis", personality_value<AiPersonality::SpecialtyForcesEmphasis>},
    {"GetSupportUnitsEmphasis",    personality_value<AiPersonality::SupportUnitsEmphasis>},
    {"GetDirectDamageEmphasis",    personality_value<AiPersonality::DirectDamageEmphasis>},
    {"GetInDirectDamageEmphasis",  personality_value<AiPersonality::IndirectDamageEmphasis>},
    {"GetFavouriteUnits",          personality_GetFavouriteUnits},
    {"GetSurvivalEmphasis",        personality_value<AiPersonality::SurvivalEmphasis>},
    {"GetTeamSupport",             personality_value<AiPersonality::TeamSupport>},
    {"GetFormationUse",            personality_value<AiPersonality::FormationUse>},
    {"GetTargetSpread",            personality_value<AiPersonality::TargetSpread>},
    {"GetQuittingTendency",        personality_value<AiPersonality::QuittingTendency>},
    {"GetChatFrequency",           personality_value<AiPersonality::ChatFrequency>},
    {nullptr, nullptr},
};
// clang-format on

} // namespace osc::lua
