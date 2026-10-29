#include "blueprints/moho_reflected_fields.hpp"

#include "blueprints/flag_enums.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <string_view>

extern "C" {
#include <lua.h>
}

namespace osc::blueprints {

namespace {

// The faf-re constructors that set each default.
constexpr u32 kBlueprintCtor = 0x0050DD60; // RBlueprint
constexpr u32 kEntityCtor = 0x00511C30;    // REntityBlueprint
constexpr u32 kUnitCtor = 0x0051E480;      // RUnitBlueprint: Display, Intel, Transport, Defense
constexpr u32 kGeneralCtor = 0x0051EE10;   // RUnitBlueprintGeneral
constexpr u32 kAirCtor = 0x0051EF00;       // RUnitBlueprintAir
constexpr u32 kPhysicsCtor = 0x0051F090;   // RUnitBlueprintPhysics
constexpr u32 kAICtor = 0x0051F7D0;        // RUnitBlueprintAI
constexpr u32 kEconomyCtor = 0x0051F990;   // RUnitBlueprintEconomy
constexpr u32 kWeaponCtor = 0x0051F4C0;    // RUnitBlueprintWeapon
constexpr u32 kProjectilePhysicsCtor = 0x0051B650; // RProjectileBlueprintPhysics

constexpr ReflectedField num(const char* section, const char* name, f32 value, u32 address) {
    return {section, name, ReflectedType::Number, value, nullptr, address};
}
constexpr ReflectedField flag(const char* section, const char* name, bool value, u32 address) {
    return {section, name, ReflectedType::Bool, value ? 1.0f : 0.0f, nullptr, address};
}
constexpr ReflectedField text(const char* section, const char* name, const char* value,
                              u32 address) {
    return {section, name, ReflectedType::String, 0, value, address};
}
constexpr ReflectedField list(const char* section, const char* name, u32 address) {
    return {section, name, ReflectedType::List, 0, nullptr, address};
}
constexpr ReflectedField min_max(const char* section, const char* name, u32 address) {
    return {section, name, ReflectedType::MinMax, 0, nullptr, address};
}

constexpr ReflectedField kUnitFields[] = {
    // RBlueprint
    text("", "Description", "", kBlueprintCtor),
    // REntityBlueprint
    list("", "Categories", kEntityCtor),
    text("", "ScriptModule", "", kEntityCtor),
    text("", "ScriptClass", "", kEntityCtor),
    num("", "AverageDensity", 0.49f, kEntityCtor),
    num("", "InertiaTensorX", 0, kEntityCtor),
    num("", "InertiaTensorY", 0, kEntityCtor),
    num("", "InertiaTensorZ", 0, kEntityCtor),
    num("", "CollisionOffsetX", 0, kEntityCtor),
    num("", "CollisionOffsetY", 0, kEntityCtor),
    num("", "CollisionOffsetZ", 0, kEntityCtor),
    num("Footprint", "MaxSlope", 0, kEntityCtor),
    num("Footprint", "MinWaterDepth", 0, kEntityCtor),
    num("", "DesiredShooterCap", 3, kEntityCtor),
    text("", "StrategicIconName", "", kEntityCtor),
    flag("", "LifeBarRender", true, kUnitCtor), // the unit's constructor sets it
    num("", "LifeBarOffset", 0, kEntityCtor),
    num("", "LifeBarSize", 1, kEntityCtor),
    num("", "SelectionSizeY", 1, kEntityCtor),
    num("", "SelectionCenterOffsetX", 0, kEntityCtor),
    num("", "SelectionCenterOffsetY", 0, kEntityCtor),
    num("", "SelectionCenterOffsetZ", 0, kEntityCtor),
    num("", "SelectionYOffset", 0.5f, kEntityCtor),
    num("", "SelectionMeshScaleX", 1, kEntityCtor),
    num("", "SelectionMeshScaleY", 1, kEntityCtor),
    num("", "SelectionMeshScaleZ", 1, kEntityCtor),
    num("", "SelectionMeshUseTopAmount", 0, kEntityCtor),
    num("", "SelectionThickness", 0, kEntityCtor),
    num("", "UseOOBTestZoom", 0, kEntityCtor),
    num("", "StrategicIconSortPriority", 0, kEntityCtor),
    // RUnitBlueprintGeneral
    text("General", "UpgradesTo", "", kGeneralCtor),
    text("General", "UpgradesFrom", "none", kGeneralCtor),
    text("General", "UpgradesFromBase", "none", kGeneralCtor),
    text("General", "SeedUnit", "", kGeneralCtor),
    num("General", "QuickSelectPriority", 0, kGeneralCtor),
    num("General", "CapCost", 1, kGeneralCtor),
    num("General", "SelectionPriority", 1, kGeneralCtor),
    // RUnitBlueprintDisplay
    text("Display", "DisplayName", "", kUnitCtor),
    text("Display", "PlaceholderMeshName", "", kUnitCtor),
    num("Display", "UniformScale", 1, kUnitCtor),
    flag("Display", "SpawnRandomRotation", false, kUnitCtor),
    flag("Display", "HideLifebars", false, kUnitCtor),
    // RUnitBlueprintPhysics
    flag("Physics", "FlattenSkirt", false, kPhysicsCtor),
    num("Physics", "MaxGroundVariation", 1, kPhysicsCtor),
    text("Physics", "MotionType", "RULEUMT_None", kPhysicsCtor),
    text("Physics", "AltMotionType", "RULEUMT_None", kPhysicsCtor),
    flag("Physics", "StandUpright", false, kPhysicsCtor),
    flag("Physics", "SinkLower", false, kPhysicsCtor),
    flag("Physics", "RotateBodyWhileMoving", false, kPhysicsCtor),
    num("Physics", "DiveSurfaceSpeed", 1, kPhysicsCtor),
    num("Physics", "MaxSpeed", 0, kPhysicsCtor),
    num("Physics", "MaxSpeedReverse", -1, kPhysicsCtor),
    num("Physics", "MaxAcceleration", 0, kPhysicsCtor),
    num("Physics", "MaxBrake", 0, kPhysicsCtor),
    num("Physics", "MaxSteerForce", 0, kPhysicsCtor),
    num("Physics", "BankingSlope", 0, kPhysicsCtor),
    num("Physics", "RollStability", 0.2f, kPhysicsCtor),
    num("Physics", "RollDamping", 0.5f, kPhysicsCtor),
    num("Physics", "WobbleFactor", 0, kPhysicsCtor),
    num("Physics", "WobbleSpeed", 0, kPhysicsCtor),
    num("Physics", "TurnRate", 0, kPhysicsCtor),
    num("Physics", "TurnFacingRate", 0, kPhysicsCtor),
    flag("Physics", "RotateOnSpot", false, kPhysicsCtor),
    num("Physics", "RotateOnSpotThreshold", 0.5f, kPhysicsCtor),
    num("Physics", "AttackElevation", 0, kPhysicsCtor),
    text("Physics", "BuildRestriction", "RULEUBR_None", kPhysicsCtor),
    num("Physics", "CatchUpAcc", 0, kPhysicsCtor),
    num("Physics", "LayerChangeOffsetHeight", -0.1f, kPhysicsCtor),
    num("Physics", "LayerTransitionDuration", 0, kPhysicsCtor),
    num("Physics", "FuelUseTime", 0, kPhysicsCtor),
    num("Physics", "FuelRechargeRate", 0, kPhysicsCtor),
    num("Physics", "GroundCollisionOffset", 0, kPhysicsCtor),
    list("Physics", "OccupyRects", kPhysicsCtor),
    list("Physics", "RaisedPlatforms", kPhysicsCtor),
    // RUnitBlueprintAir
    flag("Air", "CanFly", false, kAirCtor),
    flag("Air", "Winged", false, kAirCtor),
    flag("Air", "FlyInWater", false, kAirCtor),
    num("Air", "AutoLandTime", 0, kAirCtor),
    num("Air", "MaxAirspeed", 0, kAirCtor),
    num("Air", "MinAirspeed", 0, kAirCtor),
    num("Air", "TurnSpeed", 1, kAirCtor),
    num("Air", "CombatTurnSpeed", 1, kAirCtor),
    num("Air", "StartTurnDistance", 0, kAirCtor),
    num("Air", "TightTurnMultiplier", 1, kAirCtor),
    num("Air", "SustainedTurnThreshold", 10, kAirCtor),
    num("Air", "LiftFactor", 5, kAirCtor),
    num("Air", "BankFactor", 0.5f, kAirCtor),
    flag("Air", "BankForward", false, kAirCtor),
    num("Air", "EngageDistance", 0, kAirCtor),
    num("Air", "BreakOffTrigger", 0, kAirCtor),
    num("Air", "BreakOffDistance", 0, kAirCtor),
    flag("Air", "BreakOffIfNearNewTarget", false, kAirCtor),
    num("Air", "KMove", 1, kAirCtor),
    num("Air", "KMoveDamping", 1, kAirCtor),
    num("Air", "KLift", 1, kAirCtor),
    num("Air", "KLiftDamping", 1, kAirCtor),
    num("Air", "KTurn", 3, kAirCtor),
    num("Air", "KTurnDamping", 3, kAirCtor),
    num("Air", "KRoll", 3, kAirCtor),
    num("Air", "KRollDamping", 3, kAirCtor),
    num("Air", "CirclingTurnMult", 3, kAirCtor),
    num("Air", "CirclingRadiusChangeMinRatio", 0.6f, kAirCtor),
    num("Air", "CirclingRadiusChangeMaxRatio", 0.9f, kAirCtor),
    num("Air", "CirclingRadiusVsAirMult", 1, kAirCtor),
    num("Air", "CirclingElevationChangeRatio", 0.25f, kAirCtor),
    num("Air", "CirclingFlightChangeFrequency", 2, kAirCtor),
    flag("Air", "CirclingDirChange", true, kAirCtor),
    flag("Air", "HoverOverAttack", false, kAirCtor),
    num("Air", "RandomBreakOffDistanceMult", 1.5f, kAirCtor),
    num("Air", "RandomMinChangeCombatStateTime", 3, kAirCtor),
    num("Air", "RandomMaxChangeCombatStateTime", 6, kAirCtor),
    num("Air", "TransportHoverHeight", 0, kAirCtor),
    num("Air", "PredictAheadForBombDrop", 0, kAirCtor),
    // RUnitBlueprintTransport
    num("Transport", "TransportClass", 1, kUnitCtor),
    num("Transport", "ClassGenericUpTo", 0, kUnitCtor),
    num("Transport", "Class2AttachSize", 2, kUnitCtor),
    num("Transport", "Class3AttachSize", 6, kUnitCtor),
    num("Transport", "Class4AttachSize", 1, kUnitCtor),
    num("Transport", "ClassSAttachSize", 0, kUnitCtor),
    flag("Transport", "AirClass", false, kUnitCtor),
    num("Transport", "StorageSlots", 0, kUnitCtor),
    num("Transport", "DockingSlots", 0, kUnitCtor),
    num("Transport", "RepairRate", 0, kUnitCtor),
    // RUnitBlueprintAI
    num("AI", "GuardReturnRadius", 50, kAICtor),
    flag("AI", "ShowAssistRangeOnSelect", false, kAICtor),
    text("AI", "GuardFormationName", "GuardFormation", kAICtor),
    flag("AI", "NeedUnpack", false, kAICtor),
    flag("AI", "InitialAutoMode", false, kAICtor),
    text("AI", "BeaconName", "", kAICtor),
    list("AI", "TargetBones", kAICtor),
    num("AI", "RefuelingMultiplier", 1, kAICtor),
    num("AI", "RefuelingRepairAmount", 20, kAICtor),
    num("AI", "RepairConsumeEnergy", 2, kAICtor),
    num("AI", "RepairConsumeMass", 0.5f, kAICtor),
    flag("AI", "AutoSurfaceToAttack", true, kAICtor),
    num("AI", "AttackAngle", 0, kAICtor),
    // RUnitBlueprintDefense
    num("Defense", "MaxHealth", 1, kUnitCtor),
    num("Defense", "Health", 1, kUnitCtor),
    num("Defense", "RegenRate", 0, kUnitCtor),
    num("Defense", "AirThreatLevel", 0, kUnitCtor),
    num("Defense", "SurfaceThreatLevel", 0, kUnitCtor),
    num("Defense", "SubThreatLevel", 0, kUnitCtor),
    num("Defense", "EconomyThreatLevel", 0, kUnitCtor),
    text("Defense", "ArmorType", "Default", kUnitCtor),
    num("Defense.Shield", "ShieldSize", 0, kUnitCtor),
    num("Defense.Shield", "RegenAssistMult", 1, kUnitCtor),
    // RUnitBlueprintIntel
    num("Intel", "VisionRadius", 10, kUnitCtor),
    num("Intel", "WaterVisionRadius", 10, kUnitCtor),
    num("Intel", "RadarRadius", 0, kUnitCtor),
    num("Intel", "SonarRadius", 0, kUnitCtor),
    num("Intel", "OmniRadius", 0, kUnitCtor),
    flag("Intel", "RadarStealth", false, kUnitCtor),
    flag("Intel", "SonarStealth", false, kUnitCtor),
    flag("Intel", "Cloak", false, kUnitCtor),
    flag("Intel", "ShowIntelOnSelect", false, kUnitCtor),
    num("Intel", "RadarStealthFieldRadius", 0, kUnitCtor),
    num("Intel", "SonarStealthFieldRadius", 0, kUnitCtor),
    num("Intel", "CloakFieldRadius", 0, kUnitCtor),
    min_max("Intel", "JamRadius", kUnitCtor),
    min_max("Intel", "SpoofRadius", kUnitCtor),
    num("Intel", "JammerBlips", 0, kUnitCtor),
    // RUnitBlueprintEconomy
    num("Economy", "BuildCostEnergy", 0, kEconomyCtor),
    num("Economy", "BuildCostMass", 0, kEconomyCtor),
    num("Economy", "BuildRate", 1, kEconomyCtor),
    num("Economy", "BuildTime", 0, kEconomyCtor),
    num("Economy", "StorageEnergy", 0, kEconomyCtor),
    num("Economy", "StorageMass", 0, kEconomyCtor),
    flag("Economy", "NaturalProducer", false, kEconomyCtor),
    list("Economy", "BuildableCategory", kEconomyCtor),
    list("Economy", "RebuildBonusIds", kEconomyCtor),
    num("Economy", "InitialRallyX", 0, kEconomyCtor),
    num("Economy", "InitialRallyZ", 5, kEconomyCtor),
    flag("Economy", "NeedToFaceTargetToBuild", false, kEconomyCtor),
    num("Economy", "SacrificeMassMult", 0, kEconomyCtor),
    num("Economy", "SacrificeEnergyMult", 0, kEconomyCtor),
    num("Economy", "MaxBuildDistance", 5, kEconomyCtor),
    // RUnitBlueprintWeapons: the list (each entry's fields are kWeaponFields)
    list("", "Weapon", kUnitCtor),
};

constexpr ReflectedField kWeaponFields[] = {
    text("", "Label", "", kWeaponCtor),
    text("", "DisplayName", "", kWeaponCtor),
    text("", "RangeCategory", "UWRC_Undefined", kWeaponCtor),
    flag("", "DummyWeapon", false, kWeaponCtor),
    flag("", "PrefersPrimaryWeaponTarget", false, kWeaponCtor),
    flag("", "StopOnPrimaryWeaponBusy", false, kWeaponCtor),
    flag("", "SlavedToBody", false, kWeaponCtor),
    num("", "SlavedToBodyArcRange", 1, kWeaponCtor),
    flag("", "AutoInitiateAttackCommand", false, kWeaponCtor),
    num("", "TargetCheckInterval", 3, kWeaponCtor),
    flag("", "AlwaysRecheckTarget", true, kWeaponCtor),
    num("", "MinRadius", 0, kWeaponCtor),
    num("", "MaxRadius", 0, kWeaponCtor),
    num("", "MaximumBeamLength", 0, kWeaponCtor),
    num("", "EffectiveRadius", -1, kWeaponCtor),
    num("", "MaxHeightDiff", std::numeric_limits<f32>::infinity(), kWeaponCtor),
    num("", "TrackingRadius", 1, kWeaponCtor),
    num("", "HeadingArcCenter", 0, kWeaponCtor),
    num("", "HeadingArcRange", 180, kWeaponCtor),
    num("", "FiringTolerance", 0.01f, kWeaponCtor),
    num("", "FiringRandomness", 0, kWeaponCtor),
    num("", "RequiresEnergy", 0, kWeaponCtor),
    num("", "RequiresMass", 0, kWeaponCtor),
    num("", "MuzzleVelocity", 0, kWeaponCtor),
    num("", "MuzzleVelocityRandom", 0, kWeaponCtor),
    num("", "MuzzleVelocityReduceDistance", 0, kWeaponCtor),
    flag("", "LeadTarget", true, kWeaponCtor),
    num("", "ProjectileLifetime", 0, kWeaponCtor),
    num("", "ProjectileLifetimeUsesMultiplier", 0, kWeaponCtor),
    num("", "Damage", 0, kWeaponCtor),
    num("", "DamageRadius", 0, kWeaponCtor),
    text("", "DamageType", "Normal", kWeaponCtor),
    num("", "RateOfFire", 1, kWeaponCtor),
    text("", "ProjectileId", "", kWeaponCtor),
    text("", "BallisticArc", "RULEUBA_None", kWeaponCtor),
    text("", "TargetRestrictOnlyAllow", "", kWeaponCtor),
    text("", "TargetRestrictDisallow", "", kWeaponCtor),
    flag("", "ManualFire", false, kWeaponCtor),
    flag("", "NukeWeapon", false, kWeaponCtor),
    flag("", "OverChargeWeapon", false, kWeaponCtor),
    flag("", "NeedPrep", false, kWeaponCtor),
    flag("", "CountedProjectile", false, kWeaponCtor),
    num("", "MaxProjectileStorage", 0, kWeaponCtor),
    flag("", "IgnoresAlly", true, kWeaponCtor),
    text("", "TargetType", "RULEWTT_Unit", kWeaponCtor),
    num("", "AttackGroundTries", 3, kWeaponCtor),
    flag("", "AimsStraightOnDisable", false, kWeaponCtor),
    flag("", "Turreted", false, kWeaponCtor),
    flag("", "YawOnlyOnTarget", false, kWeaponCtor),
    flag("", "AboveWaterFireOnly", false, kWeaponCtor),
    flag("", "BelowWaterFireOnly", false, kWeaponCtor),
    flag("", "AboveWaterTargetsOnly", false, kWeaponCtor),
    flag("", "BelowWaterTargetsOnly", false, kWeaponCtor),
    flag("", "ReTargetOnMiss", false, kWeaponCtor),
    flag("", "NeedToComputeBombDrop", false, kWeaponCtor),
    num("", "BombDropThreshold", 1.5f, kWeaponCtor),
    flag("", "UseFiringSolutionInsteadOfAimBone", false, kWeaponCtor),
    flag("", "IgnoreIfDisabled", false, kWeaponCtor),
    flag("", "CannotAttackGround", false, kWeaponCtor),
    text("", "UIMinRangeVisualId", "", kWeaponCtor),
    text("", "UIMaxRangeVisualId", "", kWeaponCtor),
};

// A projectile's spreads: FAF's cruise missiles add MaxSpeedRange to MaxSpeed
// as they are made (TIFMissileCruise01 has none). The engine spreads TurnRate,
// MaxSpeed, Acceleration and Lifetime by theirs as each projectile is made
// (Projectile::apply_blueprint_physics).
constexpr ReflectedField kProjectileFields[] = {
    num("Physics", "AccelerationRange", 0, kProjectilePhysicsCtor),
    num("Physics", "DirectionXRange", 1.5f, kProjectilePhysicsCtor),
    num("Physics", "DirectionYRange", 0, kProjectilePhysicsCtor),
    num("Physics", "DirectionZRange", 1.5f, kProjectilePhysicsCtor),
    num("Physics", "InitialSpeedRange", 0, kProjectilePhysicsCtor),
    num("Physics", "LifetimeRange", 0, kProjectilePhysicsCtor),
    num("Physics", "MaxSpeedRange", 0, kProjectilePhysicsCtor),
    num("Physics", "PositionXRange", 0, kProjectilePhysicsCtor),
    num("Physics", "PositionYRange", 0, kProjectilePhysicsCtor),
    num("Physics", "PositionZRange", 0, kProjectilePhysicsCtor),
    num("Physics", "RotationalVelocityRange", 0, kProjectilePhysicsCtor),
    num("Physics", "TurnRateRange", 0, kProjectilePhysicsCtor),
};

struct FlagField {
    const char* section;
    const char* name;
    const FlagEnum* type;
    u32 value; ///< its constructor's
};

constexpr FlagField kUnitFlagFields[] = {
    {"General", "CommandCaps", &kCommandCapsEnum, 0},
    {"General", "ToggleCaps", &kToggleCapsEnum, 0},
    {"Physics", "BuildOnLayerCaps", &kLayerEnum, occupancy::kLand},
};

constexpr const char* kSizeReason =
    "Moho's 1; every retail and FAF unit gives it, and the engine reads an absent one as no "
    "collision box (blueprint_collision_shape), which unit tests' blueprints rely on";

constexpr UnfilledField kUnitUnfilled[] = {
    {"", "CollisionShape", "an enum whose registered names the engine has not checked"},
    {"", "SizeX", kSizeReason},
    {"", "SizeY", kSizeReason},
    {"", "SizeZ", kSizeReason},
    {"Display", "MeshBlueprint",
     "Moho completes it against Source; the mesh cache resolves an absent one itself"},
    {"Physics", "TurnRadius",
     "Moho's 5; the navigator reads an absent one as 0, no turning circle, and no script "
     "reads it"},
    {"Physics", "Elevation",
     "Moho's 0; the engine flies an aircraft without one at 18, and an absent one keeps it"},
    {"Physics", "BackUpDistance",
     "derived by the drive reader (3 x SizeZ), which also honours two retail blueprints' "
     "BackupDistance spelling that a filled value would hide"},
    {"AI", "GuardScanRadius",
     "the Miscellaneous range overlay draws no ring for an absent one; the sim's reader "
     "defaults to Moho's 25"},
    {"AI", "StagingPlatformScanRadius",
     "the Miscellaneous range overlay draws no ring for an absent one; the sim's reader "
     "defaults to Moho's 300"},
    {"", "LifeBarHeight",
     "Moho's 0.1; the life bar reads an absent one as 0, and no script "
     "reads it"},
    {"", "SelectionSizeX", "the selection box sizes an absent one by the mesh"},
    {"", "SelectionSizeZ", "the selection box sizes an absent one by the mesh"},
};

/// The table t[name] (made if missing), on top of the stack.
int push_table(lua_State* L, int t, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, t);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, name);
        lua_pushvalue(L, -2);
        lua_rawset(L, t);
    }
    return lua_gettop(L);
}

/// The table at `path` from the blueprint at `bp` (made where missing), on
/// top of the stack: the blueprint itself for "".
int push_section(lua_State* L, int bp, std::string_view path) {
    lua_pushvalue(L, bp);
    while (!path.empty()) {
        const size_t dot = path.find('.');
        const std::string name(path.substr(0, dot));
        const int parent = lua_gettop(L);
        push_table(L, parent, name.c_str());
        lua_remove(L, parent);
        path = dot == std::string_view::npos ? std::string_view() : path.substr(dot + 1);
    }
    return lua_gettop(L);
}

bool missing(lua_State* L, int t, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, t);
    const bool absent = lua_isnil(L, -1);
    lua_pop(L, 1);
    return absent;
}

/// t[name] if it is a number, else `fallback`.
lua_Number number_at(lua_State* L, int t, const char* name, lua_Number fallback) {
    lua_pushstring(L, name);
    lua_rawget(L, t);
    const lua_Number v = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return v;
}

bool is_number(lua_State* L, int t, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, t);
    const bool number = lua_type(L, -1) == LUA_TNUMBER;
    lua_pop(L, 1);
    return number;
}

void set_number(lua_State* L, int t, const char* name, lua_Number value) {
    lua_pushstring(L, name);
    lua_pushnumber(L, value);
    lua_rawset(L, t);
}

/// t[name] as Moho holds a float: what a derived value is computed in.
f32 float_at(lua_State* L, int t, const char* name, f32 fallback) {
    return static_cast<f32>(number_at(L, t, name, fallback));
}

std::string string_at(lua_State* L, int t, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, t);
    std::string s = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return s;
}

void push_default(lua_State* L, const ReflectedField& f) {
    switch (f.type) {
    case ReflectedType::Number: lua_pushnumber(L, static_cast<lua_Number>(f.number)); break;
    case ReflectedType::Bool: lua_pushboolean(L, f.number != 0); break;
    case ReflectedType::String: lua_pushstring(L, f.text); break;
    case ReflectedType::List: lua_newtable(L); break;
    case ReflectedType::MinMax:
        lua_newtable(L);
        set_number(L, lua_gettop(L), "Min", 0);
        set_number(L, lua_gettop(L), "Max", 0);
        break;
    }
}

/// Each field of `fields` the table at `t` (a blueprint or a weapon entry)
/// leaves out, as its default.
void fill(lua_State* L, int t, std::span<const ReflectedField> fields) {
    for (const ReflectedField& f : fields) {
        const int section = push_section(L, t, f.section);
        if (missing(L, section, f.name)) {
            lua_pushstring(L, f.name);
            push_default(L, f);
            lua_rawset(L, section);
        }
        lua_pop(L, 1);
    }
}

/// A resource id as Moho's RResId holds it (RResIdType::SetLexical,
/// 0x004A9430; gpg::STR_NormalizeFilenameLowerSlash): ASCII lowercase, '\'
/// as '/'.
void normalize_res_id(lua_State* L, int t, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, t);
    if (lua_type(L, -1) != LUA_TSTRING) {
        lua_pop(L, 1);
        return;
    }
    std::string id(lua_tostring(L, -1), lua_strlen(L, -1));
    lua_pop(L, 1);
    std::string normal = id;
    for (char& c : normal) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (c == '\\') c = '/';
    }
    if (normal == id) return;
    lua_pushstring(L, name);
    lua_pushlstring(L, normal.data(), normal.size());
    lua_rawset(L, t);
}

/// The id an absent IconName takes: the blueprint's, as a resource id.
void default_icon_name(lua_State* L, int bp, int display) {
    if (!missing(L, display, "IconName")) return;
    lua_pushstring(L, "BlueprintId");
    lua_rawget(L, bp);
    if (lua_type(L, -1) == LUA_TSTRING) {
        lua_pushstring(L, "IconName");
        lua_pushvalue(L, -2);
        lua_rawset(L, display);
    }
    lua_pop(L, 1);
}

/// REntityBlueprint::OnInitBlueprint (0x00512060): an unsized footprint
/// axis is the unit's size rounded up (at least 1; FAF's loader rounds to
/// nearest, which differs on 13 retail axes, e.g. a T2 tank's 1.2 length),
/// and a zero inertia tensor is a solid box's of its size.
void derive_entity(lua_State* L, int bp) {
    for (const char* section : {"Footprint", "AltFootprint"}) {
        const int footprint = push_section(L, bp, section);
        for (const char* axis : {"SizeX", "SizeZ"}) {
            if (number_at(L, footprint, axis, 0) != 0) {
                continue;
            }
            const lua_Number size = number_at(L, bp, axis, 1);
            set_number(L, footprint, axis, std::max(1.0, std::ceil(size)));
        }
        lua_pop(L, 1);
    }

    const f32 tx = float_at(L, bp, "InertiaTensorX", 0);
    const f32 ty = float_at(L, bp, "InertiaTensorY", 0);
    const f32 tz = float_at(L, bp, "InertiaTensorZ", 0);
    if (tx * ty * tz == 0.0f) {
        const f32 sx = float_at(L, bp, "SizeX", 1);
        const f32 sy = float_at(L, bp, "SizeY", 1);
        const f32 sz = float_at(L, bp, "SizeZ", 1);
        constexpr f32 kOneTwelfth = 1.0f / 12.0f;
        set_number(L, bp, "InertiaTensorX", (sy * sy + sz * sz) * kOneTwelfth);
        set_number(L, bp, "InertiaTensorY", (sx * sx + sz * sz) * kOneTwelfth);
        set_number(L, bp, "InertiaTensorZ", (sx * sx + sy * sy) * kOneTwelfth);
    }
}

/// Each flag enum as its lexical form; Moho keeps the constructor's value
/// for one it can't read.
void write_flag_enums(lua_State* L, int bp) {
    for (const FlagField& f : kUnitFlagFields) {
        const int section = push_section(L, bp, f.section);
        lua_pushstring(L, f.name);
        lua_rawget(L, section);
        const u32 value = read_flag_enum(L, -1, *f.type).value_or(f.value);
        lua_pop(L, 1);
        const std::string lexical = flag_enum_lexical(*f.type, value);
        lua_pushstring(L, f.name);
        lua_pushlstring(L, lexical.data(), lexical.size());
        lua_rawset(L, section);
        lua_pop(L, 1);
    }
}

Footprint footprint_at(lua_State* L, int t) {
    const auto byte = [](lua_Number v) {
        return static_cast<u8>(std::clamp(std::lround(v), 0L, 255L));
    };
    Footprint fp;
    fp.size_x = byte(number_at(L, t, "SizeX", 0));
    fp.size_z = byte(number_at(L, t, "SizeZ", 0));
    fp.caps = byte(number_at(L, t, "OccupancyCaps", 0));
    fp.flags = byte(number_at(L, t, "Flags", 0));
    fp.max_slope = float_at(L, t, "MaxSlope", 0);
    fp.min_water_depth = float_at(L, t, "MinWaterDepth", 0);
    return fp;
}

void set_footprint(lua_State* L, int t, const Footprint& fp, bool whole) {
    if (whole) {
        set_number(L, t, "SizeX", fp.size_x);
        set_number(L, t, "SizeZ", fp.size_z);
        set_number(L, t, "MaxSlope", fp.max_slope);
        set_number(L, t, "MinWaterDepth", fp.min_water_depth);
    }
    set_number(L, t, "OccupancyCaps", fp.caps);
    set_number(L, t, "Flags", fp.flags);
}

/// The footprints ComputeDerivedQuantities leaves (resolve_unit_footprints).
void derive_footprints(lua_State* L, int physics, int footprint, int alt,
                       const std::vector<NamedFootprint>& classes) {
    lua_pushstring(L, "BuildOnLayerCaps");
    lua_rawget(L, physics);
    const u32 layers = read_flag_enum(L, -1, kLayerEnum).value_or(occupancy::kLand);
    lua_pop(L, 1);
    const UnitFootprints resolved =
        resolve_unit_footprints(classes, footprint_at(L, footprint), footprint_at(L, alt),
                                string_at(L, physics, "MotionType"),
                                string_at(L, physics, "AltMotionType"), static_cast<u8>(layers));
    set_footprint(L, footprint, resolved.main, resolved.main_class >= 0);
    set_footprint(L, alt, resolved.alt, true);
}

/// RUnitBlueprintPhysics::ComputeDerivedQuantities (0x0051F260), as far as
/// the table shows it: a moving unit's reverse speed, the attack elevation,
/// the catch-up acceleration, the footprint, and a skirt at least that
/// footprint, never offset outward. Moho's MotionType None for a unit with
/// no MaxSpeed is left to the readers.
void derive_physics(lua_State* L, int bp, const std::vector<NamedFootprint>& classes) {
    const int physics = push_section(L, bp, "Physics");
    const f32 max_speed = float_at(L, physics, "MaxSpeed", 0);
    if (max_speed != 0.0f && float_at(L, physics, "MaxSpeedReverse", -1) < 0.0f)
        set_number(L, physics, "MaxSpeedReverse", max_speed);
    if (float_at(L, physics, "AttackElevation", 0) == 0.0f && is_number(L, physics, "Elevation"))
        set_number(L, physics, "AttackElevation", float_at(L, physics, "Elevation", 0));
    if (float_at(L, physics, "CatchUpAcc", 0) == 0.0f) {
        set_number(L, physics, "CatchUpAcc",
                   std::max(float_at(L, physics, "MaxAcceleration", 0),
                            float_at(L, physics, "MaxBrake", 0)));
    }

    const int footprint = push_section(L, bp, "Footprint");
    const int alt = push_section(L, bp, "AltFootprint");
    derive_footprints(L, physics, footprint, alt, classes);
    for (const auto& [skirt, offset, axis] :
         {std::array<const char*, 3>{"SkirtSizeX", "SkirtOffsetX", "SizeX"},
          std::array<const char*, 3>{"SkirtSizeZ", "SkirtOffsetZ", "SizeZ"}}) {
        const lua_Number size = std::max(1.0, number_at(L, footprint, axis, 1));
        if (number_at(L, physics, skirt, 0) < size) {
            set_number(L, physics, skirt, size);
        }
        if (!is_number(L, physics, offset) || number_at(L, physics, offset, 0) > 0) {
            set_number(L, physics, offset, 0);
        }
    }
    lua_pop(L, 3); // AltFootprint, Footprint, Physics
}

/// RUnitBlueprint::OnInitBlueprint (0x0051EA40): an aircraft can fly, its
/// airspeeds default to its top speed and its turn-in distance to three
/// lengths; an unnamed icon is the blueprint's id.
void derive_unit(lua_State* L, int bp) {
    const int air = push_section(L, bp, "Air");
    const int physics = push_section(L, bp, "Physics");
    lua_pushstring(L, "MotionType");
    lua_rawget(L, physics);
    const bool flies =
        lua_type(L, -1) == LUA_TSTRING && std::string_view(lua_tostring(L, -1)) == "RULEUMT_Air";
    lua_pop(L, 1);
    lua_pushstring(L, "CanFly");
    lua_rawget(L, air);
    bool can_fly = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    if (!can_fly && flies) {
        can_fly = true;
        lua_pushstring(L, "CanFly");
        lua_pushboolean(L, 1);
        lua_rawset(L, air);
    }
    if (float_at(L, air, "MaxAirspeed", 0) == 0.0f && can_fly)
        set_number(L, air, "MaxAirspeed", float_at(L, physics, "MaxSpeed", 0));
    if (float_at(L, air, "MinAirspeed", 0) == 0.0f)
        set_number(L, air, "MinAirspeed", float_at(L, air, "MaxAirspeed", 0));
    if (float_at(L, air, "StartTurnDistance", 0) == 0.0f)
        set_number(L, air, "StartTurnDistance", float_at(L, bp, "SizeZ", 1) * 3.0f);
    lua_pop(L, 2); // Physics, Air

    const int display = push_section(L, bp, "Display");
    default_icon_name(L, bp, display);
    normalize_res_id(L, display, "IconName");
    lua_pop(L, 1);
}

/// The upgrade links: an empty UpgradesFrom or UpgradesFromBase reads as
/// 'none' (construction.lua's OnClickHandler takes a structure whose
/// UpgradesFrom isn't 'none' for an upgrade of the selected unit).
void default_upgrade_links(lua_State* L, int bp) {
    const int general = push_section(L, bp, "General");
    for (const char* link : {"UpgradesTo", "UpgradesFrom", "UpgradesFromBase", "SeedUnit"})
        normalize_res_id(L, general, link);
    for (const char* link : {"UpgradesFrom", "UpgradesFromBase"}) {
        lua_pushstring(L, link);
        lua_rawget(L, general);
        const bool empty = lua_type(L, -1) == LUA_TSTRING && lua_strlen(L, -1) == 0;
        lua_pop(L, 1);
        if (!empty) continue;
        lua_pushstring(L, link);
        lua_pushstring(L, "none");
        lua_rawset(L, general);
    }
    lua_pop(L, 1);
}

} // namespace

std::span<const ReflectedField> unit_reflected_fields() {
    return kUnitFields;
}
std::span<const ReflectedField> weapon_reflected_fields() {
    return kWeaponFields;
}
std::span<const ReflectedField> projectile_reflected_fields() {
    return kProjectileFields;
}
std::span<const UnfilledField> unit_unfilled_fields() {
    return kUnitUnfilled;
}

void apply_unit_reflection(lua_State* L, int bp, const std::vector<NamedFootprint>& classes) {
    lua_checkstack(L, 8);
    default_upgrade_links(L, bp);
    fill(L, bp, kUnitFields);
    write_flag_enums(L, bp);
    normalize_res_id(L, bp, "StrategicIconName");

    lua_pushstring(L, "Weapon");
    lua_rawget(L, bp);
    if (lua_istable(L, -1)) {
        const int weapons = lua_gettop(L);
        for (int i = 1;; ++i) {
            lua_rawgeti(L, weapons, i);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            if (lua_istable(L, -1)) {
                const int weapon = lua_gettop(L);
                fill(L, weapon, kWeaponFields);
                normalize_res_id(L, weapon, "ProjectileId");
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1); // Weapon

    derive_entity(L, bp);
    derive_physics(L, bp, classes);
    derive_unit(L, bp);
}

void apply_projectile_reflection(lua_State* L, int bp) {
    lua_checkstack(L, 8);
    fill(L, bp, kProjectileFields);
}

} // namespace osc::blueprints
