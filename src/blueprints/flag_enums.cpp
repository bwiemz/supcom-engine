#include "blueprints/flag_enums.hpp"

#include <charconv>
#include <cstddef>

extern "C" {
#include <lua.h>
}

namespace osc::blueprints {

namespace {

// faf-re RUnitBlueprintEnumTypeInfo.cpp, ELayerTypeInfo.cpp
constexpr EnumEntry kCommandCaps[] = {
    {"RULEUCC_Move", 0x1},
    {"RULEUCC_Stop", 0x2},
    {"RULEUCC_Attack", 0x4},
    {"RULEUCC_Guard", 0x8},
    {"RULEUCC_Patrol", 0x10},
    {"RULEUCC_RetaliateToggle", 0x20},
    {"RULEUCC_Repair", 0x40},
    {"RULEUCC_Capture", 0x80},
    {"RULEUCC_Transport", 0x100},
    {"RULEUCC_CallTransport", 0x200},
    {"RULEUCC_Nuke", 0x400},
    {"RULEUCC_Tactical", 0x800},
    {"RULEUCC_Teleport", 0x1000},
    {"RULEUCC_Ferry", 0x2000},
    {"RULEUCC_SiloBuildTactical", 0x4000},
    {"RULEUCC_SiloBuildNuke", 0x8000},
    {"RULEUCC_Sacrifice", 0x10000},
    {"RULEUCC_Pause", 0x20000},
    {"RULEUCC_Overcharge", 0x40000},
    {"RULEUCC_Dive", 0x80000},
    {"RULEUCC_Reclaim", 0x100000},
    {"RULEUCC_SpecialAction", 0x200000},
    {"RULEUCC_Dock", 0x400000},
    {"RULEUCC_Script", 0x800000},
    {"RULEUCC_Invalid", 0x1000000},
};

constexpr EnumEntry kToggleCaps[] = {
    {"RULEUTC_ShieldToggle", 0x1},      {"RULEUTC_WeaponToggle", 0x2},
    {"RULEUTC_JammingToggle", 0x4},     {"RULEUTC_IntelToggle", 0x8},
    {"RULEUTC_ProductionToggle", 0x10}, {"RULEUTC_StealthToggle", 0x20},
    {"RULEUTC_GenericToggle", 0x40},    {"RULEUTC_SpecialToggle", 0x80},
    {"RULEUTC_CloakToggle", 0x100},
};

constexpr EnumEntry kLayers[] = {
    {"LAYER_None", 0},    {"LAYER_Land", 0x1}, {"LAYER_Seabed", 0x2}, {"LAYER_Sub", 0x4},
    {"LAYER_Water", 0x8}, {"LAYER_Air", 0x10}, {"LAYER_Orbit", 0x20}, {"LAYER_All", 0x7F},
};

std::string_view name_of(const FlagEnum& e, const EnumEntry& entry) {
    std::string_view name = entry.token;
    if (name.starts_with(e.prefix)) {
        name.remove_prefix(e.prefix.size());
    }
    return name;
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_no_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

/// gpg::ParseNum
std::optional<u32> parse_number(std::string_view token) {
    const bool negative = token.starts_with('-');
    if (negative) {
        token.remove_prefix(1);
    }
    int base = 10;
    if (token.starts_with("0x")) {
        base = 16;
        token.remove_prefix(2);
    } else if (token.size() > 1 && token.starts_with('0')) {
        base = 8;
    }
    u32 value = 0;
    const auto [end, ec] = std::from_chars(token.data(), token.data() + token.size(), value, base);
    if (token.empty() || ec != std::errc{} || end != token.data() + token.size()) {
        return std::nullopt;
    }
    return negative ? 0u - value : value;
}

std::optional<u32> token_value(const FlagEnum& e, std::string_view token) {
    if (token.starts_with(e.prefix)) {
        token.remove_prefix(e.prefix.size());
    }
    for (const EnumEntry& entry : e.entries) {
        if (equals_no_case(name_of(e, entry), token)) {
            return entry.value;
        }
    }
    return parse_number(token);
}

std::optional<u32> parse_lexical(const FlagEnum& e, std::string_view text) {
    u32 value = 0;
    for (;;) {
        const size_t bar = text.find('|');
        const std::optional<u32> token = token_value(e, text.substr(0, bar));
        if (!token) {
            return std::nullopt;
        }
        value |= *token;
        if (bar == std::string_view::npos) {
            return value;
        }
        text.remove_prefix(bar + 1);
    }
}

std::optional<std::string> text_at(lua_State* L, int index) {
    const int type = lua_type(L, index);
    if (type != LUA_TSTRING && type != LUA_TNUMBER) {
        return std::nullopt;
    }
    lua_pushvalue(L, index);
    std::string text(lua_tostring(L, -1), lua_strlen(L, -1));
    lua_pop(L, 1);
    return text;
}

} // namespace

const FlagEnum kCommandCapsEnum{kCommandCaps, ""};
const FlagEnum kToggleCapsEnum{kToggleCaps, ""};
const FlagEnum kLayerEnum{kLayers, "LAYER_"};

std::optional<u32> read_flag_enum(lua_State* L, int index, const FlagEnum& e) {
    if (index < 0 && index > LUA_REGISTRYINDEX) {
        index = lua_gettop(L) + index + 1;
    }
    if (lua_type(L, index) == LUA_TBOOLEAN) {
        return lua_toboolean(L, index) ? 1u : 0u;
    }
    if (const std::optional<std::string> text = text_at(L, index)) {
        return parse_lexical(e, *text);
    }
    if (!lua_istable(L, index)) {
        return std::nullopt;
    }
    std::optional<u32> value = 0;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        const std::optional<std::string> key = text_at(L, -2);
        if (!key) {
            value.reset();
        } else if (value && lua_toboolean(L, -1)) {
            const std::optional<u32> bits = parse_lexical(e, *key);
            value = bits ? std::optional<u32>(*value | *bits) : std::nullopt;
        }
        lua_pop(L, 1);
    }
    return value;
}

std::string flag_enum_lexical(const FlagEnum& e, u32 value) {
    for (const EnumEntry& entry : e.entries) {
        if (entry.value == value) {
            return std::string(name_of(e, entry));
        }
    }
    return std::to_string(static_cast<i32>(value));
}

std::vector<const char*> flag_enum_tokens(const FlagEnum& e, u32 value) {
    std::vector<const char*> tokens;
    for (const EnumEntry& entry : e.entries) {
        const bool one_bit = entry.value != 0 && (entry.value & (entry.value - 1)) == 0;
        if (one_bit && (value & entry.value) != 0) {
            tokens.push_back(entry.token);
        }
    }
    return tokens;
}

} // namespace osc::blueprints
