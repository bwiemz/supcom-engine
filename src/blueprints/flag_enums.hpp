#pragma once

// A unit blueprint's flag enums as Moho reflects them (gpg::REnumType): read
// by SCR_LuaBuildObject, written back by SCR_RObjectToLuaMerge as a string.

#include "core/types.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace osc::blueprints {

struct EnumEntry {
    const char* token;
    u32 value;
};

struct FlagEnum {
    std::span<const EnumEntry> entries;
    std::string_view prefix; ///< dropped from the names (ELayer's LAYER_)
};

extern const FlagEnum kCommandCapsEnum; ///< ERuleBPUnitCommandCaps (General.CommandCaps)
extern const FlagEnum kToggleCapsEnum;  ///< ERuleBPUnitToggleCaps (General.ToggleCaps)
extern const FlagEnum kLayerEnum;       ///< ELayer (Physics.BuildOnLayerCaps)

/// The value at `index` as SCR_LuaBuildObject reads it (REnumType::SetLexical);
/// nothing for nil or what Moho can't read.
std::optional<u32> read_flag_enum(lua_State* L, int index, const FlagEnum& e);

/// REnumType::GetLexical
std::string flag_enum_lexical(const FlagEnum& e, u32 value);

/// The token of each one-bit entry `value` holds.
std::vector<const char*> flag_enum_tokens(const FlagEnum& e, u32 value);

} // namespace osc::blueprints
