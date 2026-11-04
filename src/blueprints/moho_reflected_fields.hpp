#pragma once

// The blueprint fields Moho writes back to a blueprint's Lua table as it
// registers it (RBlueprint::InitBlueprint): it builds its typed copy from the
// table (SCR_LuaBuildObject), derives what the class derives (OnInitBlueprint),
// then merges every reflected field back into the table (SCR_RObjectToLuaMerge).
// So a field a .bp leaves out reads, in every script after registration, as
// the class constructor's default.
//
// The engine writes each reflected field as Moho's typed copy holds it: the
// constructor's default for one the table leaves out, a present one read as
// SCR_LuaBuildObject reads it (the default where it can't), and the derived
// values. Lua numbers stay doubles (Moho's LuaPlus keeps floats). See
// docs/plans/2026-10-23-moho-blueprint-reflection-design.md.

#include "blueprints/footprint.hpp"
#include "core/types.hpp"

#include <functional>
#include <span>
#include <string>
#include <vector>

struct lua_State;

namespace osc::blueprints {

struct FlagEnum;

/// A reflected field's type, as SCR_LuaBuildObject reads it and
/// SCR_RObjectToLuaMerge writes it back.
enum class ReflectedType : u8 {
    Float,
    Int32,
    UInt32,
    UInt8,
    Bool,
    String,
    ResId,   ///< a resource id: lowercased, '/' slashes
    Enum,    ///< written as its registered name, or its number
    Strings, ///< a list of strings
    Floats,  ///< a list of numbers
    MinMax,  ///< {Min, Max}, unsigned
    Structs, ///< a list of tables, each with `elements`' fields
};

/// One reflected field and its constructor's default.
struct ReflectedField {
    const char* section; ///< its table's path from the blueprint, dotted; "" is the blueprint
    const char* name;
    ReflectedType type;
    f32 number;              ///< a number's default, a Bool's 0 or 1, an Enum's value
    const char* text;        ///< a String's or ResId's default
    const char* constructor; ///< the Moho class whose constructor sets it
    const FlagEnum* enumeration = nullptr;
    std::span<const ReflectedField> elements = {};
};

/// A unit blueprint's fields (REntityBlueprint, RUnitBlueprint and its
/// sections), its Weapon list's entries weapon_reflected_fields.
std::span<const ReflectedField> unit_reflected_fields();
/// Each of a unit's Weapon entries' fields (RUnitBlueprintWeapon).
std::span<const ReflectedField> weapon_reflected_fields();
/// A projectile's and a prop's REntityBlueprint fields.
std::span<const ReflectedField> entity_reflected_fields();
/// A projectile blueprint's own fields (RProjectileBlueprint).
std::span<const ReflectedField> projectile_reflected_fields();
/// A prop blueprint's own fields (RPropBlueprint).
std::span<const ReflectedField> prop_reflected_fields();
/// A mesh blueprint's fields (RMeshBlueprint), and each LOD's.
std::span<const ReflectedField> mesh_reflected_fields();
std::span<const ReflectedField> mesh_lod_reflected_fields();

/// Write the unit blueprint table at stack index `bp` back as Moho's
/// typed copy holds it, a mobile unit's footprint the nearest of `classes`.
void apply_unit_reflection(lua_State* L, int bp, const std::vector<NamedFootprint>& classes);
/// The same for a projectile blueprint table.
void apply_projectile_reflection(lua_State* L, int bp);
/// The same for a prop blueprint table.
void apply_prop_reflection(lua_State* L, int bp);
/// The same for a mesh blueprint table: an LOD's names completed against its
/// Source, or the conventional file there where `exists`.
void apply_mesh_reflection(lua_State* L, int bp,
                           const std::function<bool(const std::string&)>& exists);

} // namespace osc::blueprints
