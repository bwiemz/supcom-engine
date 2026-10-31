#pragma once

// The blueprint fields Moho writes back to a blueprint's Lua table as it
// registers it (RBlueprint::InitBlueprint, 0x0050DF10): it builds its typed
// copy from the table (SCR_LuaBuildObject), derives what the class derives
// (OnInitBlueprint), then merges every reflected field back into the table
// (SCR_RObjectToLuaMerge, 0x004CF0B0). So a field a .bp leaves out reads, in
// every script after registration, as the class constructor's default.
//
// The engine fills only what the table leaves out, with those defaults and
// the derived values, normalizes resource ids as Moho's RResId does, and
// writes the flag enums as their lexical form. See
// docs/plans/2026-10-23-moho-blueprint-reflection-design.md.

#include "blueprints/footprint.hpp"
#include "core/types.hpp"

#include <span>
#include <vector>

struct lua_State;

namespace osc::blueprints {

/// How a reflected field reads back: a number, a boolean, a string (an enum
/// field as its registered name, e.g. 'RULEUMT_None'), an empty list (a
/// vector), or an {Min = 0, Max = 0} pair (SMinMax).
enum class ReflectedType : u8 { Number, Bool, String, List, MinMax };

/// One reflected field and its constructor's default.
struct ReflectedField {
    const char* section; ///< its table's path from the blueprint, dotted; "" is the blueprint
    const char* name;
    ReflectedType type;
    f32 number;       ///< a Number's default, as Moho holds it; a Bool's, 0 or 1
    const char* text; ///< a String's default
    u32 address;      ///< the Moho function that sets it (faf-re)
};

/// A unit blueprint's fields, but for its weapons' (REntityBlueprint,
/// RUnitBlueprint and its sections).
std::span<const ReflectedField> unit_reflected_fields();
/// Each of a unit's Weapon entries' fields (RUnitBlueprintWeapon).
std::span<const ReflectedField> weapon_reflected_fields();
/// A projectile blueprint's fields the engine fills (RProjectileBlueprintPhysics).
std::span<const ReflectedField> projectile_reflected_fields();

/// Fill the unit blueprint table at stack index `bp` as Moho's write-back
/// leaves it: defaults for what it leaves out, derived values (a mobile
/// unit's footprint the nearest of `classes`), resource ids lowercased.
void apply_unit_reflection(lua_State* L, int bp, const std::vector<NamedFootprint>& classes);
/// The same for a projectile blueprint table.
void apply_projectile_reflection(lua_State* L, int bp);

} // namespace osc::blueprints
