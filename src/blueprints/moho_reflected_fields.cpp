#include "blueprints/moho_reflected_fields.hpp"

#include "blueprints/flag_enums.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::blueprints {

namespace {

// The faf-re constructors that set each default.
constexpr const char* kBlueprintCtor = "RBlueprint";
constexpr const char* kEntityCtor = "REntityBlueprint";
constexpr const char* kUnitCtor = "RUnitBlueprint"; // Display, Intel, Transport, Defense
constexpr const char* kGeneralCtor = "RUnitBlueprintGeneral";
constexpr const char* kAirCtor = "RUnitBlueprintAir";
constexpr const char* kPhysicsCtor = "RUnitBlueprintPhysics";
constexpr const char* kAICtor = "RUnitBlueprintAI";
constexpr const char* kEconomyCtor = "RUnitBlueprintEconomy";
constexpr const char* kWeaponCtor = "RUnitBlueprintWeapon";
constexpr const char* kProjectileCtor = "RProjectileBlueprint"; // Display, Economy
constexpr const char* kProjectilePhysicsCtor = "RProjectileBlueprintPhysics";
constexpr const char* kPropCtor = "RPropBlueprint";
constexpr const char* kMeshCtor = "RMeshBlueprint";
constexpr const char* kMeshLodCtor = "RMeshBlueprintLOD";

constexpr ReflectedField real(const char* section, const char* name, f32 value, const char* ctor) {
    return {section, name, ReflectedType::Float, value, nullptr, ctor};
}
constexpr ReflectedField int32(const char* section, const char* name, f32 value, const char* ctor) {
    return {section, name, ReflectedType::Int32, value, nullptr, ctor};
}
constexpr ReflectedField uint32(const char* section, const char* name, f32 value,
                                const char* ctor) {
    return {section, name, ReflectedType::UInt32, value, nullptr, ctor};
}
constexpr ReflectedField uint8(const char* section, const char* name, f32 value, const char* ctor) {
    return {section, name, ReflectedType::UInt8, value, nullptr, ctor};
}
constexpr ReflectedField flag(const char* section, const char* name, bool value, const char* ctor) {
    return {section, name, ReflectedType::Bool, value ? 1.0f : 0.0f, nullptr, ctor};
}
constexpr ReflectedField text(const char* section, const char* name, const char* value,
                              const char* ctor) {
    return {section, name, ReflectedType::String, 0, value, ctor};
}
constexpr ReflectedField res_id(const char* section, const char* name, const char* value,
                                const char* ctor) {
    return {section, name, ReflectedType::ResId, 0, value, ctor};
}
constexpr ReflectedField enumeration(const char* section, const char* name, const FlagEnum& type,
                                     u32 value, const char* ctor) {
    return {section, name, ReflectedType::Enum, static_cast<f32>(value), nullptr, ctor, &type};
}
constexpr ReflectedField strings(const char* section, const char* name, const char* ctor) {
    return {section, name, ReflectedType::Strings, 0, nullptr, ctor};
}
constexpr ReflectedField floats(const char* section, const char* name, const char* ctor) {
    return {section, name, ReflectedType::Floats, 0, nullptr, ctor};
}
constexpr ReflectedField min_max(const char* section, const char* name, const char* ctor) {
    return {section, name, ReflectedType::MinMax, 0, nullptr, ctor};
}
constexpr ReflectedField structs(const char* section, const char* name,
                                 std::span<const ReflectedField> elements, const char* ctor) {
    return {section, name, ReflectedType::Structs, 0, nullptr, ctor, nullptr, elements};
}

constexpr ReflectedField kWeaponFields[] = {
    text("", "Label", "", kWeaponCtor),
    text("", "DisplayName", "", kWeaponCtor),
    enumeration("", "RangeCategory", kRangeCategoryEnum, 0, kWeaponCtor),
    flag("", "DummyWeapon", false, kWeaponCtor),
    flag("", "PrefersPrimaryWeaponTarget", false, kWeaponCtor),
    flag("", "StopOnPrimaryWeaponBusy", false, kWeaponCtor),
    flag("", "SlavedToBody", false, kWeaponCtor),
    real("", "SlavedToBodyArcRange", 1, kWeaponCtor),
    flag("", "AutoInitiateAttackCommand", false, kWeaponCtor),
    real("", "TargetCheckInterval", 3, kWeaponCtor),
    flag("", "AlwaysRecheckTarget", true, kWeaponCtor),
    real("", "MinRadius", 0, kWeaponCtor),
    real("", "MaxRadius", 0, kWeaponCtor),
    real("", "MaximumBeamLength", 0, kWeaponCtor),
    real("", "EffectiveRadius", -1, kWeaponCtor),
    real("", "MaxHeightDiff", std::numeric_limits<f32>::infinity(), kWeaponCtor),
    real("", "TrackingRadius", 1, kWeaponCtor),
    real("", "HeadingArcCenter", 0, kWeaponCtor),
    real("", "HeadingArcRange", 180, kWeaponCtor),
    real("", "FiringTolerance", 0.01f, kWeaponCtor),
    real("", "FiringRandomness", 0, kWeaponCtor),
    real("", "RequiresEnergy", 0, kWeaponCtor),
    real("", "RequiresMass", 0, kWeaponCtor),
    real("", "MuzzleVelocity", 0, kWeaponCtor),
    real("", "MuzzleVelocityRandom", 0, kWeaponCtor),
    real("", "MuzzleVelocityReduceDistance", 0, kWeaponCtor),
    flag("", "LeadTarget", true, kWeaponCtor),
    real("", "ProjectileLifetime", 0, kWeaponCtor),
    real("", "ProjectileLifetimeUsesMultiplier", 0, kWeaponCtor),
    real("", "Damage", 0, kWeaponCtor),
    real("", "DamageRadius", 0, kWeaponCtor),
    text("", "DamageType", "Normal", kWeaponCtor),
    real("", "RateOfFire", 1, kWeaponCtor),
    res_id("", "ProjectileId", "", kWeaponCtor),
    enumeration("", "BallisticArc", kBallisticArcEnum, 0, kWeaponCtor),
    text("", "TargetRestrictOnlyAllow", "", kWeaponCtor),
    text("", "TargetRestrictDisallow", "", kWeaponCtor),
    flag("", "ManualFire", false, kWeaponCtor),
    flag("", "NukeWeapon", false, kWeaponCtor),
    flag("", "OverChargeWeapon", false, kWeaponCtor),
    flag("", "NeedPrep", false, kWeaponCtor),
    flag("", "CountedProjectile", false, kWeaponCtor),
    int32("", "MaxProjectileStorage", 0, kWeaponCtor),
    flag("", "IgnoresAlly", true, kWeaponCtor),
    enumeration("", "TargetType", kTargetTypeEnum, 0, kWeaponCtor),
    int32("", "AttackGroundTries", 3, kWeaponCtor),
    flag("", "AimsStraightOnDisable", false, kWeaponCtor),
    flag("", "Turreted", false, kWeaponCtor),
    flag("", "YawOnlyOnTarget", false, kWeaponCtor),
    flag("", "AboveWaterFireOnly", false, kWeaponCtor),
    flag("", "BelowWaterFireOnly", false, kWeaponCtor),
    flag("", "AboveWaterTargetsOnly", false, kWeaponCtor),
    flag("", "BelowWaterTargetsOnly", false, kWeaponCtor),
    flag("", "ReTargetOnMiss", false, kWeaponCtor),
    flag("", "NeedToComputeBombDrop", false, kWeaponCtor),
    real("", "BombDropThreshold", 1.5f, kWeaponCtor),
    flag("", "UseFiringSolutionInsteadOfAimBone", false, kWeaponCtor),
    flag("", "IgnoreIfDisabled", false, kWeaponCtor),
    flag("", "CannotAttackGround", false, kWeaponCtor),
    text("", "UIMinRangeVisualId", "", kWeaponCtor),
    text("", "UIMaxRangeVisualId", "", kWeaponCtor),
};

constexpr ReflectedField kUnitFields[] = {
    // RBlueprint
    text("", "Description", "", kBlueprintCtor),
    // REntityBlueprint
    strings("", "Categories", kEntityCtor),
    text("", "ScriptModule", "", kEntityCtor),
    text("", "ScriptClass", "", kEntityCtor),
    enumeration("", "CollisionShape", kCollisionShapeEnum, 1, kEntityCtor),
    real("", "SizeX", 1, kEntityCtor),
    real("", "SizeY", 1, kEntityCtor),
    real("", "SizeZ", 1, kEntityCtor),
    real("", "AverageDensity", 0.49f, kEntityCtor),
    real("", "InertiaTensorX", 0, kEntityCtor),
    real("", "InertiaTensorY", 0, kEntityCtor),
    real("", "InertiaTensorZ", 0, kEntityCtor),
    real("", "CollisionOffsetX", 0, kEntityCtor),
    real("", "CollisionOffsetY", 0, kEntityCtor),
    real("", "CollisionOffsetZ", 0, kEntityCtor),
    uint8("Footprint", "SizeX", 0, kEntityCtor),
    uint8("Footprint", "SizeZ", 0, kEntityCtor),
    real("Footprint", "MaxSlope", 0, kEntityCtor),
    real("Footprint", "MinWaterDepth", 0, kEntityCtor),
    uint8("Footprint", "OccupancyCaps", 0, kEntityCtor),
    uint8("Footprint", "Flags", 0, kEntityCtor),
    uint8("AltFootprint", "SizeX", 0, kEntityCtor),
    uint8("AltFootprint", "SizeZ", 0, kEntityCtor),
    real("AltFootprint", "MaxSlope", 0, kEntityCtor),
    real("AltFootprint", "MinWaterDepth", 0, kEntityCtor),
    uint8("AltFootprint", "OccupancyCaps", 0, kEntityCtor),
    uint8("AltFootprint", "Flags", 0, kEntityCtor),
    int32("", "DesiredShooterCap", 3, kEntityCtor),
    res_id("", "StrategicIconName", "", kEntityCtor),
    flag("", "LifeBarRender", true, kUnitCtor), // the unit's constructor sets it
    real("", "LifeBarOffset", 0, kEntityCtor),
    real("", "LifeBarSize", 1, kEntityCtor),
    real("", "LifeBarHeight", 0.1f, kEntityCtor),
    real("", "SelectionSizeX", 1, kEntityCtor),
    real("", "SelectionSizeY", 1, kEntityCtor),
    real("", "SelectionSizeZ", 1, kEntityCtor),
    real("", "SelectionCenterOffsetX", 0, kEntityCtor),
    real("", "SelectionCenterOffsetY", 0, kEntityCtor),
    real("", "SelectionCenterOffsetZ", 0, kEntityCtor),
    real("", "SelectionYOffset", 0.5f, kEntityCtor),
    real("", "SelectionMeshScaleX", 1, kEntityCtor),
    real("", "SelectionMeshScaleY", 1, kEntityCtor),
    real("", "SelectionMeshScaleZ", 1, kEntityCtor),
    real("", "SelectionMeshUseTopAmount", 0, kEntityCtor),
    real("", "SelectionThickness", 0, kEntityCtor),
    real("", "UseOOBTestZoom", 0, kEntityCtor),
    uint8("", "StrategicIconSortPriority", 0, kEntityCtor),
    // RUnitBlueprintGeneral
    enumeration("General", "CommandCaps", kCommandCapsEnum, 0, kGeneralCtor),
    enumeration("General", "ToggleCaps", kToggleCapsEnum, 0, kGeneralCtor),
    res_id("General", "UpgradesTo", "", kGeneralCtor),
    res_id("General", "UpgradesFrom", "none", kGeneralCtor),
    res_id("General", "UpgradesFromBase", "none", kGeneralCtor),
    res_id("General", "SeedUnit", "", kGeneralCtor),
    int32("General", "QuickSelectPriority", 0, kGeneralCtor),
    real("General", "CapCost", 1, kGeneralCtor),
    int32("General", "SelectionPriority", 1, kGeneralCtor),
    // RUnitBlueprintDisplay
    text("Display", "DisplayName", "", kUnitCtor),
    res_id("Display", "MeshBlueprint", "", kUnitCtor),
    text("Display", "PlaceholderMeshName", "", kUnitCtor),
    real("Display", "UniformScale", 1, kUnitCtor),
    flag("Display", "SpawnRandomRotation", false, kUnitCtor),
    flag("Display", "HideLifebars", false, kUnitCtor),
    // RUnitBlueprintPhysics
    flag("Physics", "FlattenSkirt", false, kPhysicsCtor),
    real("Physics", "MaxGroundVariation", 1, kPhysicsCtor),
    enumeration("Physics", "MotionType", kMotionTypeEnum, 0, kPhysicsCtor),
    enumeration("Physics", "AltMotionType", kMotionTypeEnum, 0, kPhysicsCtor),
    flag("Physics", "StandUpright", false, kPhysicsCtor),
    flag("Physics", "SinkLower", false, kPhysicsCtor),
    flag("Physics", "RotateBodyWhileMoving", false, kPhysicsCtor),
    real("Physics", "DiveSurfaceSpeed", 1, kPhysicsCtor),
    real("Physics", "MaxSpeed", 0, kPhysicsCtor),
    real("Physics", "MaxSpeedReverse", -1, kPhysicsCtor),
    real("Physics", "MaxAcceleration", 0, kPhysicsCtor),
    real("Physics", "MaxBrake", 0, kPhysicsCtor),
    real("Physics", "MaxSteerForce", 0, kPhysicsCtor),
    real("Physics", "BankingSlope", 0, kPhysicsCtor),
    real("Physics", "RollStability", 0.2f, kPhysicsCtor),
    real("Physics", "RollDamping", 0.5f, kPhysicsCtor),
    real("Physics", "WobbleFactor", 0, kPhysicsCtor),
    real("Physics", "WobbleSpeed", 0, kPhysicsCtor),
    real("Physics", "TurnRate", 0, kPhysicsCtor),
    real("Physics", "TurnRadius", 5, kPhysicsCtor),
    real("Physics", "TurnFacingRate", 0, kPhysicsCtor),
    flag("Physics", "RotateOnSpot", false, kPhysicsCtor),
    real("Physics", "RotateOnSpotThreshold", 0.5f, kPhysicsCtor),
    real("Physics", "Elevation", 0, kPhysicsCtor),
    real("Physics", "AttackElevation", 0, kPhysicsCtor),
    real("Physics", "BackUpDistance", -1, kPhysicsCtor),
    enumeration("Physics", "BuildOnLayerCaps", kLayerEnum, occupancy::kLand, kPhysicsCtor),
    enumeration("Physics", "BuildRestriction", kBuildRestrictionEnum, 0, kPhysicsCtor),
    real("Physics", "CatchUpAcc", 0, kPhysicsCtor),
    real("Physics", "LayerChangeOffsetHeight", -0.1f, kPhysicsCtor),
    real("Physics", "LayerTransitionDuration", 0, kPhysicsCtor),
    real("Physics", "FuelUseTime", 0, kPhysicsCtor),
    real("Physics", "FuelRechargeRate", 0, kPhysicsCtor),
    real("Physics", "GroundCollisionOffset", 0, kPhysicsCtor),
    floats("Physics", "OccupyRects", kPhysicsCtor),
    floats("Physics", "RaisedPlatforms", kPhysicsCtor),
    // RUnitBlueprintAir
    flag("Air", "CanFly", false, kAirCtor),
    flag("Air", "Winged", false, kAirCtor),
    flag("Air", "FlyInWater", false, kAirCtor),
    real("Air", "AutoLandTime", 0, kAirCtor),
    real("Air", "MaxAirspeed", 0, kAirCtor),
    real("Air", "MinAirspeed", 0, kAirCtor),
    real("Air", "TurnSpeed", 1, kAirCtor),
    real("Air", "CombatTurnSpeed", 1, kAirCtor),
    real("Air", "StartTurnDistance", 0, kAirCtor),
    real("Air", "TightTurnMultiplier", 1, kAirCtor),
    real("Air", "SustainedTurnThreshold", 10, kAirCtor),
    real("Air", "LiftFactor", 5, kAirCtor),
    real("Air", "BankFactor", 0.5f, kAirCtor),
    flag("Air", "BankForward", false, kAirCtor),
    real("Air", "EngageDistance", 0, kAirCtor),
    real("Air", "BreakOffTrigger", 0, kAirCtor),
    real("Air", "BreakOffDistance", 0, kAirCtor),
    flag("Air", "BreakOffIfNearNewTarget", false, kAirCtor),
    real("Air", "KMove", 1, kAirCtor),
    real("Air", "KMoveDamping", 1, kAirCtor),
    real("Air", "KLift", 1, kAirCtor),
    real("Air", "KLiftDamping", 1, kAirCtor),
    real("Air", "KTurn", 3, kAirCtor),
    real("Air", "KTurnDamping", 3, kAirCtor),
    real("Air", "KRoll", 3, kAirCtor),
    real("Air", "KRollDamping", 3, kAirCtor),
    real("Air", "CirclingTurnMult", 3, kAirCtor),
    real("Air", "CirclingRadiusChangeMinRatio", 0.6f, kAirCtor),
    real("Air", "CirclingRadiusChangeMaxRatio", 0.9f, kAirCtor),
    real("Air", "CirclingRadiusVsAirMult", 1, kAirCtor),
    real("Air", "CirclingElevationChangeRatio", 0.25f, kAirCtor),
    real("Air", "CirclingFlightChangeFrequency", 2, kAirCtor),
    flag("Air", "CirclingDirChange", true, kAirCtor),
    flag("Air", "HoverOverAttack", false, kAirCtor),
    real("Air", "RandomBreakOffDistanceMult", 1.5f, kAirCtor),
    real("Air", "RandomMinChangeCombatStateTime", 3, kAirCtor),
    real("Air", "RandomMaxChangeCombatStateTime", 6, kAirCtor),
    real("Air", "TransportHoverHeight", 0, kAirCtor),
    real("Air", "PredictAheadForBombDrop", 0, kAirCtor),
    // RUnitBlueprintTransport
    int32("Transport", "TransportClass", 1, kUnitCtor),
    int32("Transport", "ClassGenericUpTo", 0, kUnitCtor),
    int32("Transport", "Class2AttachSize", 2, kUnitCtor),
    int32("Transport", "Class3AttachSize", 6, kUnitCtor),
    int32("Transport", "Class4AttachSize", 1, kUnitCtor),
    int32("Transport", "ClassSAttachSize", 0, kUnitCtor),
    flag("Transport", "AirClass", false, kUnitCtor),
    int32("Transport", "StorageSlots", 0, kUnitCtor),
    int32("Transport", "DockingSlots", 0, kUnitCtor),
    real("Transport", "RepairRate", 0, kUnitCtor),
    // RUnitBlueprintAI
    real("AI", "GuardScanRadius", 25, kAICtor),
    real("AI", "GuardReturnRadius", 50, kAICtor),
    real("AI", "StagingPlatformScanRadius", 300, kAICtor),
    flag("AI", "ShowAssistRangeOnSelect", false, kAICtor),
    text("AI", "GuardFormationName", "GuardFormation", kAICtor),
    flag("AI", "NeedUnpack", false, kAICtor),
    flag("AI", "InitialAutoMode", false, kAICtor),
    text("AI", "BeaconName", "", kAICtor),
    strings("AI", "TargetBones", kAICtor),
    real("AI", "RefuelingMultiplier", 1, kAICtor),
    real("AI", "RefuelingRepairAmount", 20, kAICtor),
    real("AI", "RepairConsumeEnergy", 2, kAICtor),
    real("AI", "RepairConsumeMass", 0.5f, kAICtor),
    flag("AI", "AutoSurfaceToAttack", true, kAICtor),
    real("AI", "AttackAngle", 0, kAICtor),
    // RUnitBlueprintDefense
    real("Defense", "MaxHealth", 1, kUnitCtor),
    real("Defense", "Health", 1, kUnitCtor),
    real("Defense", "RegenRate", 0, kUnitCtor),
    real("Defense", "AirThreatLevel", 0, kUnitCtor),
    real("Defense", "SurfaceThreatLevel", 0, kUnitCtor),
    real("Defense", "SubThreatLevel", 0, kUnitCtor),
    real("Defense", "EconomyThreatLevel", 0, kUnitCtor),
    text("Defense", "ArmorType", "Default", kUnitCtor),
    real("Defense.Shield", "ShieldSize", 0, kUnitCtor),
    real("Defense.Shield", "RegenAssistMult", 1, kUnitCtor),
    // RUnitBlueprintIntel
    uint32("Intel", "VisionRadius", 10, kUnitCtor),
    uint32("Intel", "WaterVisionRadius", 10, kUnitCtor),
    uint32("Intel", "RadarRadius", 0, kUnitCtor),
    uint32("Intel", "SonarRadius", 0, kUnitCtor),
    uint32("Intel", "OmniRadius", 0, kUnitCtor),
    flag("Intel", "RadarStealth", false, kUnitCtor),
    flag("Intel", "SonarStealth", false, kUnitCtor),
    flag("Intel", "Cloak", false, kUnitCtor),
    flag("Intel", "ShowIntelOnSelect", false, kUnitCtor),
    uint32("Intel", "RadarStealthFieldRadius", 0, kUnitCtor),
    uint32("Intel", "SonarStealthFieldRadius", 0, kUnitCtor),
    uint32("Intel", "CloakFieldRadius", 0, kUnitCtor),
    min_max("Intel", "JamRadius", kUnitCtor),
    min_max("Intel", "SpoofRadius", kUnitCtor),
    uint8("Intel", "JammerBlips", 0, kUnitCtor),
    // RUnitBlueprintEconomy
    real("Economy", "BuildCostEnergy", 0, kEconomyCtor),
    real("Economy", "BuildCostMass", 0, kEconomyCtor),
    real("Economy", "BuildRate", 1, kEconomyCtor),
    real("Economy", "BuildTime", 0, kEconomyCtor),
    real("Economy", "StorageEnergy", 0, kEconomyCtor),
    real("Economy", "StorageMass", 0, kEconomyCtor),
    flag("Economy", "NaturalProducer", false, kEconomyCtor),
    strings("Economy", "BuildableCategory", kEconomyCtor),
    strings("Economy", "RebuildBonusIds", kEconomyCtor),
    real("Economy", "InitialRallyX", 0, kEconomyCtor),
    real("Economy", "InitialRallyZ", 5, kEconomyCtor),
    flag("Economy", "NeedToFaceTargetToBuild", false, kEconomyCtor),
    real("Economy", "SacrificeMassMult", 0, kEconomyCtor),
    real("Economy", "SacrificeEnergyMult", 0, kEconomyCtor),
    real("Economy", "MaxBuildDistance", 5, kEconomyCtor),
    // RUnitBlueprintWeapons: the list (each entry's fields are kWeaponFields)
    structs("", "Weapon", kWeaponFields, kUnitCtor),
};


constexpr ReflectedField kEntityFields[] = {
    text("", "Description", "", kBlueprintCtor),
    strings("", "Categories", kEntityCtor),
    text("", "ScriptModule", "", kEntityCtor),
    text("", "ScriptClass", "", kEntityCtor),
    real("", "SizeX", 1, kEntityCtor),
    real("", "SizeY", 1, kEntityCtor),
    real("", "SizeZ", 1, kEntityCtor),
    real("", "AverageDensity", 0.49f, kEntityCtor),
    real("", "InertiaTensorX", 0, kEntityCtor),
    real("", "InertiaTensorY", 0, kEntityCtor),
    real("", "InertiaTensorZ", 0, kEntityCtor),
    real("", "CollisionOffsetX", 0, kEntityCtor),
    real("", "CollisionOffsetY", 0, kEntityCtor),
    real("", "CollisionOffsetZ", 0, kEntityCtor),
    uint8("Footprint", "SizeX", 0, kEntityCtor),
    uint8("Footprint", "SizeZ", 0, kEntityCtor),
    real("Footprint", "MaxSlope", 0, kEntityCtor),
    real("Footprint", "MinWaterDepth", 0, kEntityCtor),
    uint8("Footprint", "OccupancyCaps", 0, kEntityCtor),
    uint8("Footprint", "Flags", 0, kEntityCtor),
    uint8("AltFootprint", "SizeX", 0, kEntityCtor),
    uint8("AltFootprint", "SizeZ", 0, kEntityCtor),
    real("AltFootprint", "MaxSlope", 0, kEntityCtor),
    real("AltFootprint", "MinWaterDepth", 0, kEntityCtor),
    uint8("AltFootprint", "OccupancyCaps", 0, kEntityCtor),
    uint8("AltFootprint", "Flags", 0, kEntityCtor),
    int32("", "DesiredShooterCap", 3, kEntityCtor),
    res_id("", "StrategicIconName", "", kEntityCtor),
    flag("", "LifeBarRender", false, kEntityCtor),
    real("", "LifeBarOffset", 0, kEntityCtor),
    real("", "LifeBarSize", 1, kEntityCtor),
    real("", "LifeBarHeight", 0.1f, kEntityCtor),
    real("", "SelectionSizeX", 1, kEntityCtor),
    real("", "SelectionSizeY", 1, kEntityCtor),
    real("", "SelectionSizeZ", 1, kEntityCtor),
    real("", "SelectionCenterOffsetX", 0, kEntityCtor),
    real("", "SelectionCenterOffsetY", 0, kEntityCtor),
    real("", "SelectionCenterOffsetZ", 0, kEntityCtor),
    real("", "SelectionYOffset", 0.5f, kEntityCtor),
    real("", "SelectionMeshScaleX", 1, kEntityCtor),
    real("", "SelectionMeshScaleY", 1, kEntityCtor),
    real("", "SelectionMeshScaleZ", 1, kEntityCtor),
    real("", "SelectionMeshUseTopAmount", 0, kEntityCtor),
    real("", "SelectionThickness", 0, kEntityCtor),
    real("", "UseOOBTestZoom", 0, kEntityCtor),
    uint8("", "StrategicIconSortPriority", 0, kEntityCtor),
};

constexpr ReflectedField kProjectileFields[] = {
    enumeration("", "CollisionShape", kCollisionShapeEnum, 0, kProjectileCtor),
    text("", "DevStatus", "", kProjectileCtor),
    res_id("Display", "MeshBlueprint", "", kProjectileCtor),
    real("Display", "UniformScale", 0, kProjectileCtor),
    real("Display", "MeshScaleRange", 0, kProjectileCtor),
    real("Display", "MeshScaleVelocity", 0, kProjectileCtor),
    real("Display", "MeshScaleVelocityRange", 0, kProjectileCtor),
    flag("Display", "CameraFollowsProjectile", false, kProjectileCtor),
    real("Display", "CameraFollowTimeout", 1, kProjectileCtor),
    real("Display", "StrategicIconSize", 1, kProjectileCtor),
    real("Economy", "BuildCostEnergy", 0, kProjectileCtor),
    real("Economy", "BuildCostMass", 0, kProjectileCtor),
    real("Economy", "BuildTime", 10, kProjectileCtor),
    flag("Physics", "CollideSurface", true, kProjectilePhysicsCtor),
    flag("Physics", "CollideEntity", true, kProjectilePhysicsCtor),
    flag("Physics", "TrackTarget", false, kProjectilePhysicsCtor),
    flag("Physics", "VelocityAlign", true, kProjectilePhysicsCtor),
    flag("Physics", "StayUpright", false, kProjectilePhysicsCtor),
    flag("Physics", "LeadTarget", true, kProjectilePhysicsCtor),
    flag("Physics", "StayUnderwater", false, kProjectilePhysicsCtor),
    flag("Physics", "UseGravity", true, kProjectilePhysicsCtor),
    real("Physics", "DetonateAboveHeight", 0, kProjectilePhysicsCtor),
    real("Physics", "DetonateBelowHeight", 0, kProjectilePhysicsCtor),
    real("Physics", "TurnRate", 0, kProjectilePhysicsCtor),
    real("Physics", "TurnRateRange", 0, kProjectilePhysicsCtor),
    real("Physics", "Lifetime", 15, kProjectilePhysicsCtor),
    real("Physics", "LifetimeRange", 0, kProjectilePhysicsCtor),
    real("Physics", "InitialSpeed", 1, kProjectilePhysicsCtor),
    real("Physics", "InitialSpeedRange", 0, kProjectilePhysicsCtor),
    real("Physics", "MaxSpeed", 0, kProjectilePhysicsCtor),
    real("Physics", "MaxSpeedRange", 0, kProjectilePhysicsCtor),
    real("Physics", "Acceleration", 0, kProjectilePhysicsCtor),
    real("Physics", "AccelerationRange", 0, kProjectilePhysicsCtor),
    real("Physics", "PositionX", 0, kProjectilePhysicsCtor),
    real("Physics", "PositionXRange", 0, kProjectilePhysicsCtor),
    real("Physics", "PositionY", 0, kProjectilePhysicsCtor),
    real("Physics", "PositionYRange", 0, kProjectilePhysicsCtor),
    real("Physics", "PositionZ", 0, kProjectilePhysicsCtor),
    real("Physics", "PositionZRange", 0, kProjectilePhysicsCtor),
    real("Physics", "DirectionX", 0, kProjectilePhysicsCtor),
    real("Physics", "DirectionXRange", 1.5f, kProjectilePhysicsCtor),
    real("Physics", "DirectionY", 1, kProjectilePhysicsCtor),
    real("Physics", "DirectionYRange", 0, kProjectilePhysicsCtor),
    real("Physics", "DirectionZ", 0, kProjectilePhysicsCtor),
    real("Physics", "DirectionZRange", 1.5f, kProjectilePhysicsCtor),
    real("Physics", "RotationalVelocity", 0, kProjectilePhysicsCtor),
    real("Physics", "RotationalVelocityRange", 0, kProjectilePhysicsCtor),
    real("Physics", "MaxZigZag", 0, kProjectilePhysicsCtor),
    real("Physics", "ZigZagFrequency", 0, kProjectilePhysicsCtor),
    flag("Physics", "DestroyOnWater", false, kProjectilePhysicsCtor),
    int32("Physics", "MinBounceCount", 0, kProjectilePhysicsCtor),
    int32("Physics", "MaxBounceCount", 0, kProjectilePhysicsCtor),
    real("Physics", "BounceVelDamp", 0.5f, kProjectilePhysicsCtor),
    flag("Physics", "RealisticOrdinance", false, kProjectilePhysicsCtor),
    flag("Physics", "StraightDownOrdinance", false, kProjectilePhysicsCtor),
};

constexpr ReflectedField kPropFields[] = {
    enumeration("", "CollisionShape", kCollisionShapeEnum, 1, kEntityCtor),
    res_id("Display", "MeshBlueprint", "", kPropCtor),
    real("Display", "UniformScale", 1, kPropCtor),
    real("Defense", "MaxHealth", 1, kPropCtor),
    real("Defense", "Health", 1, kPropCtor),
    real("Economy", "ReclaimMassMax", 0, kPropCtor),
    real("Economy", "ReclaimEnergyMax", 0, kPropCtor),
};

constexpr ReflectedField kMeshLodFields[] = {
    text("", "MeshName", "", kMeshLodCtor),      text("", "AlbedoName", "", kMeshLodCtor),
    text("", "NormalsName", "", kMeshLodCtor),   text("", "SpecularName", "", kMeshLodCtor),
    text("", "LookupName", "", kMeshLodCtor),    text("", "SecondaryName", "", kMeshLodCtor),
    text("", "ShaderName", "", kMeshLodCtor),    real("", "LODCutoff", 1000, kMeshLodCtor),
    flag("", "Scrolling", false, kMeshLodCtor),  flag("", "Occlude", false, kMeshLodCtor),
    flag("", "Silhouette", false, kMeshLodCtor),
};

constexpr ReflectedField kMeshFields[] = {
    text("", "Description", "", kBlueprintCtor), structs("", "LODs", kMeshLodFields, kMeshCtor),
    real("", "IconFadeInZoom", 0, kMeshCtor),    real("", "SortOrder", 0, kMeshCtor),
    real("", "UniformScale", 1, kMeshCtor),      flag("", "StraddleWater", false, kMeshCtor),
};

constexpr ReflectedField kTextEntry = text("", "", "", kEntityCtor);
constexpr ReflectedField kNumberEntry = real("", "", 0, kPhysicsCtor);
constexpr ReflectedField kMinMaxFields[] = {
    uint32("", "Min", 0, kUnitCtor),
    uint32("", "Max", 0, kUnitCtor),
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

void set_string(lua_State* L, int t, const char* name, std::string_view value) {
    lua_pushstring(L, name);
    lua_pushlstring(L, value.data(), value.size());
    lua_rawset(L, t);
}

/// gpg::STR_NormalizeFilenameLowerSlash: ASCII lowercase, '\' as '/'.
std::string lower_slash(std::string id) {
    for (char& c : id) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
        if (c == '\\') {
            c = '/';
        }
    }
    return id;
}

/// What SCR_LuaBuildObject hands a value's SetLexical: a string or number as
/// lua_tostring gives it, a boolean as "1" or "0".
std::optional<std::string> lexical_at(lua_State* L, int index) {
    const int type = lua_type(L, index);
    if (type == LUA_TBOOLEAN) {
        return std::string(lua_toboolean(L, index) ? "1" : "0");
    }
    if (type != LUA_TSTRING && type != LUA_TNUMBER) {
        return std::nullopt;
    }
    lua_pushvalue(L, index);
    std::string text(lua_tostring(L, -1), lua_strlen(L, -1));
    lua_pop(L, 1);
    return text;
}

/// RIntType::SetLexical (gpg ParseLegacyIntegerLexicalBounded): an optional
/// '-', then "0x" hexadecimal, a leading 0 octal, else decimal.
std::optional<u64> parse_integer(std::string_view text) {
    const bool negative = text.starts_with('-');
    if (negative) {
        text.remove_prefix(1);
    }
    u64 base = 10;
    if (text.starts_with("0x")) {
        base = 16;
        text.remove_prefix(2);
    } else if (text.starts_with('0')) {
        base = 8;
    }
    if (text.empty()) {
        return std::nullopt;
    }
    u64 value = 0;
    for (const char c : text) {
        i64 digit = 0;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if ((c >= 'a' && c <= 'z') || c == 'A') {
            // As Moho's: a lowercase letter or, alone of the capitals, 'A' (the digit -22).
            digit = c - 'W';
        } else {
            return std::nullopt;
        }
        if (digit >= static_cast<i64>(base)) {
            return std::nullopt;
        }
        value = value * base + static_cast<u64>(digit);
    }
    return negative ? 0 - value : value;
}

bool is_space(char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/// RFloatType::SetLexical: atof as Moho's C runtime reads it, an exponent
/// after e, E, d or D, no hexadecimal, inf or nan; 0 where it reads nothing.
lua_Number parse_float(std::string_view text) {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    std::string number;
    size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        number += text[i++];
    }
    size_t digits = 0;
    for (; i < text.size() && is_digit(text[i]); ++i, ++digits) {
        number += text[i];
    }
    if (i < text.size() && text[i] == '.') {
        number += text[i++];
        for (; i < text.size() && is_digit(text[i]); ++i, ++digits) {
            number += text[i];
        }
    }
    if (digits == 0) {
        return 0;
    }
    if (i < text.size() && std::string_view("eEdD").find(text[i]) != std::string_view::npos) {
        std::string exponent = "e";
        size_t j = i + 1;
        if (j < text.size() && (text[j] == '+' || text[j] == '-')) {
            exponent += text[j++];
        }
        if (j < text.size() && is_digit(text[j])) {
            for (; j < text.size() && is_digit(text[j]); ++j) {
                exponent += text[j];
            }
            number += exponent;
        }
    }
    return std::strtod(number.c_str(), nullptr);
}

/// RBoolType::SetLexical: "0", "false", "no", "1", "true", "yes", any case.
std::optional<bool> parse_bool(std::string_view text) {
    const auto is = [text](std::string_view word) {
        return text.size() == word.size() &&
               std::equal(text.begin(), text.end(), word.begin(), [](char a, char b) {
                   return (a >= 'A' && a <= 'Z' ? static_cast<char>(a + ('a' - 'A')) : a) == b;
               });
    };
    if (is("0") || is("false") || is("no")) {
        return false;
    }
    if (is("1") || is("true") || is("yes")) {
        return true;
    }
    return std::nullopt;
}

void merge(lua_State* L, int t, std::span<const ReflectedField> fields);
void push_typed(lua_State* L, int value, const ReflectedField& f);

void push_default(lua_State* L, const ReflectedField& f) {
    switch (f.type) {
    case ReflectedType::Float:
    case ReflectedType::Int32:
    case ReflectedType::UInt32:
    case ReflectedType::UInt8: lua_pushnumber(L, static_cast<lua_Number>(f.number)); break;
    case ReflectedType::Bool: lua_pushboolean(L, f.number != 0); break;
    case ReflectedType::String:
    case ReflectedType::ResId: lua_pushstring(L, f.text); break;
    case ReflectedType::Enum: {
        const std::string name = flag_enum_lexical(*f.enumeration, static_cast<u32>(f.number));
        lua_pushlstring(L, name.data(), name.size());
        break;
    }
    case ReflectedType::Strings:
    case ReflectedType::Floats:
    case ReflectedType::Structs: lua_newtable(L); break;
    case ReflectedType::MinMax:
        lua_newtable(L);
        set_number(L, lua_gettop(L), "Min", 0);
        set_number(L, lua_gettop(L), "Max", 0);
        break;
    }
}

void merge_list(lua_State* L, int index, const ReflectedField& element) {
    const int n = luaL_getn(L, index);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, index, i);
        push_typed(L, lua_gettop(L), element);
        lua_rawseti(L, index, i);
        lua_pop(L, 1);
    }
}

/// The value at stack index `value` as Moho's typed copy of `f` holds it
/// (SCR_LuaBuildObject), pushed.
void push_typed(lua_State* L, int value, const ReflectedField& f) {
    const std::optional<std::string> text = lexical_at(L, value);
    switch (f.type) {
    case ReflectedType::Float:
        if (lua_type(L, value) == LUA_TNUMBER) {
            lua_pushvalue(L, value);
        } else if (text) {
            lua_pushnumber(L, parse_float(*text));
        } else {
            push_default(L, f);
        }
        return;
    case ReflectedType::Int32:
    case ReflectedType::UInt32:
    case ReflectedType::UInt8:
        if (const std::optional<u64> n = text ? parse_integer(*text) : std::nullopt) {
            if (f.type == ReflectedType::Int32) {
                lua_pushnumber(L, static_cast<i32>(static_cast<u32>(*n)));
            } else if (f.type == ReflectedType::UInt32) {
                lua_pushnumber(L, static_cast<u32>(*n));
            } else {
                lua_pushnumber(L, static_cast<u8>(*n));
            }
        } else {
            push_default(L, f);
        }
        return;
    case ReflectedType::Bool:
        if (const std::optional<bool> b = text ? parse_bool(*text) : std::nullopt) {
            lua_pushboolean(L, *b ? 1 : 0);
        } else {
            push_default(L, f);
        }
        return;
    case ReflectedType::String:
        if (text) {
            lua_pushlstring(L, text->data(), text->size());
        } else {
            push_default(L, f);
        }
        return;
    case ReflectedType::ResId: {
        const std::string id = lower_slash(text ? *text : f.text);
        lua_pushlstring(L, id.data(), id.size());
        return;
    }
    case ReflectedType::Enum: {
        const u32 bits =
            read_flag_enum(L, value, *f.enumeration).value_or(static_cast<u32>(f.number));
        const std::string name = flag_enum_lexical(*f.enumeration, bits);
        lua_pushlstring(L, name.data(), name.size());
        return;
    }
    case ReflectedType::Strings:
    case ReflectedType::Floats:
        if (lua_istable(L, value)) {
            merge_list(L, value, f.type == ReflectedType::Strings ? kTextEntry : kNumberEntry);
            lua_pushvalue(L, value);
        } else {
            push_default(L, f);
        }
        return;
    case ReflectedType::MinMax:
        if (lua_istable(L, value)) {
            merge(L, value, kMinMaxFields);
            lua_pushvalue(L, value);
        } else {
            push_default(L, f);
        }
        return;
    case ReflectedType::Structs:
        if (lua_istable(L, value)) {
            const int n = luaL_getn(L, value);
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(L, value, i);
                if (!lua_istable(L, -1)) {
                    lua_pop(L, 1);
                    lua_newtable(L);
                    lua_pushvalue(L, -1);
                    lua_rawseti(L, value, i);
                }
                merge(L, lua_gettop(L), f.elements);
                lua_pop(L, 1);
            }
            lua_pushvalue(L, value);
        } else {
            push_default(L, f);
        }
        return;
    }
}

/// SCR_RObjectToLuaMerge of `fields` into the table at `t`.
void merge(lua_State* L, int t, std::span<const ReflectedField> fields) {
    lua_checkstack(L, 8);
    for (const ReflectedField& f : fields) {
        const int section = push_section(L, t, f.section);
        lua_pushstring(L, f.name);
        lua_rawget(L, section);
        lua_pushstring(L, f.name);
        push_typed(L, lua_gettop(L) - 1, f);
        lua_rawset(L, section);
        lua_pop(L, 2);
    }
}

/// FILE_CollapsePath: "." and ".." resolved.
std::string collapse_path(std::string_view path) {
    std::vector<std::string_view> parts;
    while (!path.empty()) {
        const size_t slash = path.find('/');
        const std::string_view part = path.substr(0, slash);
        if (part == "..") {
            if (!parts.empty()) {
                parts.pop_back();
            }
        } else if (!part.empty() && part != ".") {
            parts.push_back(part);
        }
        path = slash == std::string_view::npos ? std::string_view() : path.substr(slash + 1);
    }
    std::string out;
    for (const std::string_view part : parts) {
        out += '/';
        out += part;
    }
    return out;
}

/// FILE_DirPrefix: the path up to its last '/' when a '.' follows that '/',
/// else the path itself.
std::string dir_prefix(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos) {
        return {};
    }
    const size_t dot = path.rfind('.');
    return dot == std::string::npos || dot < slash ? path : path.substr(0, slash);
}

/// RES_CompletePath: `name` against `source`'s directory, with
/// '\' as '/'.
std::string complete_path(std::string name, std::string source) {
    std::replace(name.begin(), name.end(), '\\', '/');
    std::replace(source.begin(), source.end(), '\\', '/');
    if (name.starts_with('/')) {
        return collapse_path(name);
    }
    return collapse_path(dir_prefix(source) + "/" + name);
}

/// The OnInitBlueprint of a unit, prop or projectile: Display's MeshBlueprint
/// completed against Source, as a resource id.
void complete_mesh_blueprint(lua_State* L, int bp) {
    const std::string source = string_at(L, bp, "Source");
    const int display = push_section(L, bp, "Display");
    const std::string mesh = string_at(L, display, "MeshBlueprint");
    if (!mesh.empty() && !source.empty()) {
        set_string(L, display, "MeshBlueprint", lower_slash(complete_path(mesh, source)));
    }
    lua_pop(L, 1);
}

/// RMeshBlueprintLOD::Init: each LOD's names completed against
/// the mesh's Source; where one gives none, the file named after the source
/// that `exists`, else none.
void init_mesh_lods(lua_State* L, int bp, const std::function<bool(const std::string&)>& exists) {
    const std::string source = string_at(L, bp, "Source");
    if (source.empty()) {
        return;
    }
    const std::string prefix = source.substr(0, source.rfind('_'));
    lua_pushstring(L, "LODs");
    lua_rawget(L, bp);
    const int lods = lua_gettop(L);
    const int n = luaL_getn(L, lods);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, lods, i);
        const int lod = lua_gettop(L);
        const auto complete = [&](const char* name, const std::string& fallback) {
            const std::string given = string_at(L, lod, name);
            if (!given.empty()) {
                set_string(L, lod, name, complete_path(given, source));
            } else if (exists && exists(fallback)) {
                set_string(L, lod, name, fallback);
            }
        };
        complete("MeshName", prefix + "_lod" + std::to_string(i - 1) + ".scm");
        complete("AlbedoName", prefix + "_albedo.dds");
        complete("NormalsName", prefix + "_normalsTS.dds");
        complete("SpecularName", prefix + "_SpecTeam.dds");
        complete("LookupName", prefix + "_lookup.dds");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

/// A resource id as Moho's RResId holds it (RResIdType::SetLexical).
void normalize_res_id(lua_State* L, int t, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, t);
    if (lua_type(L, -1) != LUA_TSTRING) {
        lua_pop(L, 1);
        return;
    }
    std::string id(lua_tostring(L, -1), lua_strlen(L, -1));
    lua_pop(L, 1);
    set_string(L, t, name, lower_slash(std::move(id)));
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

/// REntityBlueprint::OnInitBlueprint: an unsized footprint
/// axis is the entity's size rounded up (FAF's loader rounds to nearest,
/// which differs on 13 retail axes, e.g. a T2 tank's 1.2 length), and a zero
/// inertia tensor is a solid box's of its size.
void derive_entity(lua_State* L, int bp) {
    for (const char* section : {"Footprint", "AltFootprint"}) {
        const int footprint = push_section(L, bp, section);
        for (const char* axis : {"SizeX", "SizeZ"}) {
            if (number_at(L, footprint, axis, 0) != 0) {
                continue;
            }
            const f32 size = float_at(L, bp, axis, 1);
            set_number(L, footprint, axis, static_cast<u8>(static_cast<int>(std::ceil(size))));
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

/// RUnitBlueprintPhysics::ComputeDerivedQuantities, as far as the table
/// shows it: no motion type for a unit with no top speed, a moving unit's
/// reverse speed, the attack elevation, the catch-up acceleration, the
/// footprint, a skirt at least that footprint, never offset outward, and an
/// unset BackUpDistance three lengths.
void derive_physics(lua_State* L, int bp, const std::vector<NamedFootprint>& classes) {
    const int physics = push_section(L, bp, "Physics");
    const f32 max_speed = float_at(L, physics, "MaxSpeed", 0);
    if (max_speed == 0.0f) {
        set_string(L, physics, "MotionType", "RULEUMT_None");
    } else if (float_at(L, physics, "MaxSpeedReverse", -1) < 0.0f) {
        set_number(L, physics, "MaxSpeedReverse", max_speed);
    }
    if (float_at(L, physics, "AttackElevation", 0) == 0.0f && is_number(L, physics, "Elevation"))
        set_number(L, physics, "AttackElevation", float_at(L, physics, "Elevation", 0));
    if (float_at(L, physics, "CatchUpAcc", 0) == 0.0f) {
        set_number(L, physics, "CatchUpAcc",
                   std::max(float_at(L, physics, "MaxAcceleration", 0),
                            float_at(L, physics, "MaxBrake", 0)));
    }
    if (float_at(L, physics, "BackUpDistance", -1) < 0.0f) {
        set_number(L, physics, "BackUpDistance", float_at(L, bp, "SizeZ", 1) * 3.0f);
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

/// RUnitBlueprint::OnInitBlueprint: an aircraft can fly, its airspeeds default
/// to its top speed and its turn-in distance to three lengths; an unnamed icon
/// is the blueprint's id.
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
std::span<const ReflectedField> entity_reflected_fields() {
    return kEntityFields;
}
std::span<const ReflectedField> projectile_reflected_fields() {
    return kProjectileFields;
}
std::span<const ReflectedField> prop_reflected_fields() {
    return kPropFields;
}
std::span<const ReflectedField> mesh_reflected_fields() {
    return kMeshFields;
}
std::span<const ReflectedField> mesh_lod_reflected_fields() {
    return kMeshLodFields;
}

void apply_unit_reflection(lua_State* L, int bp, const std::vector<NamedFootprint>& classes) {
    lua_checkstack(L, 8);
    default_upgrade_links(L, bp);
    merge(L, bp, kUnitFields);
    derive_entity(L, bp);
    derive_physics(L, bp, classes);
    derive_unit(L, bp);
    complete_mesh_blueprint(L, bp);
}

void apply_projectile_reflection(lua_State* L, int bp) {
    lua_checkstack(L, 8);
    merge(L, bp, kEntityFields);
    merge(L, bp, kProjectileFields);
    derive_entity(L, bp);
    complete_mesh_blueprint(L, bp);
}

void apply_prop_reflection(lua_State* L, int bp) {
    lua_checkstack(L, 8);
    merge(L, bp, kEntityFields);
    merge(L, bp, kPropFields);
    derive_entity(L, bp);
    complete_mesh_blueprint(L, bp);
}

void apply_mesh_reflection(lua_State* L, int bp,
                           const std::function<bool(const std::string&)>& exists) {
    lua_checkstack(L, 8);
    merge(L, bp, kMeshFields);
    init_mesh_lods(L, bp, exists);
}

} // namespace osc::blueprints
