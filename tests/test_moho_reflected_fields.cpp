// A unit or projectile blueprint reads, once registered, as Moho's write-back
// leaves it (RBlueprint::InitBlueprint, 0x0050DF10): each reflected field the
// .bp leaves out as its class constructor's default, the derived ones
// derived, resource ids lowercased; a field Moho doesn't reflect stays as the
// .bp gives it (moho_reflected_fields.hpp).

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "blueprints/moho_reflected_fields.hpp"
#include "lua/blueprint_bindings.hpp"
#include "lua/lua_state.hpp"

#include <set>
#include <string>
#include <utility>

namespace {

struct BlueprintWorld {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    BlueprintWorld() {
        state.set_blueprint_store(&store);
        osc::lua::register_blueprint_store_bindings(state);
    }
    bool check(const std::string& code) {
        auto r = state.do_string(code);
        UNSCOPED_INFO((r.ok() ? std::string() : r.error().message));
        return r.ok();
    }
};

std::string path(const char* section, const char* name) {
    return section[0] ? std::string(section) + "." + name : std::string(name);
}

} // namespace

TEST_CASE("A unit blueprint's omitted reflected fields read as Moho's constructor defaults",
          "[blueprints][moho-reflection]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        -- A bare unit: every section made, with its defaults
        local bare = {BlueprintId = 'bare'}
        RegisterUnitBlueprint(bare)
        assert(bare.Description == '', 'Description')
        assert(bare.Economy.MaxBuildDistance == 5, 'MaxBuildDistance')
        assert(bare.Economy.BuildTime == 0 and bare.Economy.BuildRate == 1, 'build time, rate')
        assert(bare.Economy.InitialRallyZ == 5, 'rally')
        assert(type(bare.Economy.RebuildBonusIds) == 'table', 'RebuildBonusIds')
        assert(bare.General.SelectionPriority == 1 and bare.General.SeedUnit == '', 'General')
        assert(bare.Display.UniformScale == 1, 'UniformScale')
        assert(bare.Physics.BuildRestriction == 'RULEUBR_None', 'BuildRestriction')
        assert(bare.Physics.LayerChangeOffsetHeight < -0.09, 'LayerChangeOffsetHeight')
        assert(bare.Air.CanFly == false and bare.Air.CirclingDirChange == true, 'Air flags')
        assert(bare.Transport.Class3AttachSize == 6, 'Transport')
        assert(bare.AI.GuardReturnRadius == 50 and bare.AI.GuardFormationName == 'GuardFormation',
               'AI')
        assert(bare.Defense.ArmorType == 'Default' and bare.Defense.Shield.RegenAssistMult == 1,
               'Defense')
        assert(bare.Intel.JamRadius.Min == 0 and bare.Intel.JamRadius.Max == 0, 'JamRadius')
        assert(bare.LifeBarRender == true and bare.SelectionYOffset == 0.5, 'entity')
        assert(type(bare.Weapon) == 'table' and table.getn(bare.Weapon) == 0, 'Weapon list')
        -- A .bp's own values stay, false and 0 included
        local own = {BlueprintId = 'own', Economy = {MaxBuildDistance = 10},
                     AI = {AutoSurfaceToAttack = false}, Intel = {VisionRadius = 0}}
        RegisterUnitBlueprint(own)
        assert(own.Economy.MaxBuildDistance == 10, 'own MaxBuildDistance')
        assert(own.AI.AutoSurfaceToAttack == false, 'own false')
        assert(own.Intel.VisionRadius == 0 and own.Intel.WaterVisionRadius == 10, 'own 0')
    )"));
}

TEST_CASE("A weapon's omitted reflected fields read as RUnitBlueprintWeapon's defaults",
          "[blueprints][moho-reflection]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        -- FAF's unit view formats MinRadius as a build button is hovered;
        -- most weapons leave it out
        local bp = {BlueprintId = 'gunner', Weapon = {
            {Label = 'Gun', MaxRadius = 26},
            {Label = 'Own', MinRadius = 5, MaxHeightDiff = 8, TargetCheckInterval = 0.5},
        }}
        RegisterUnitBlueprint(bp)
        local gun, own = bp.Weapon[1], bp.Weapon[2]
        assert(gun.MinRadius == 0 and gun.MaxRadius == 26, 'radii')
        assert(gun.EffectiveRadius == -1 and gun.HeadingArcRange == 180, 'arcs')
        assert(gun.MaxHeightDiff > 1e30, 'MaxHeightDiff is unbounded')
        assert(gun.TargetCheckInterval == 3 and gun.AlwaysRecheckTarget == true, 'targeting')
        assert(gun.DamageType == 'Normal' and gun.TargetType == 'RULEWTT_Unit', 'enums')
        assert(gun.BallisticArc == 'RULEUBA_None' and gun.RangeCategory == 'UWRC_Undefined',
               'enum names')
        assert(gun.ProjectileId == '' and gun.IgnoresAlly == true, 'strings, flags')
        assert(own.MinRadius == 5 and own.MaxHeightDiff == 8 and own.TargetCheckInterval == 0.5,
               'own values lost')
    )"));
}

TEST_CASE("A weapon's rack timings and a unit's movement effects aren't reflected",
          "[blueprints][moho-reflection]") {
    // RUnitBlueprintWeapon has no rack fields and RUnitBlueprintDisplay no
    // MovementEffects: Moho leaves them as the .bp gives them, nil included.
    BlueprintWorld w;
    CHECK(w.check(R"(
        local bp = {BlueprintId = 'deathweapon', Weapon = {{Label = 'DeathWeapon'},
                                                           {Label = 'Gun', RackReloadTimeout = 2}}}
        RegisterUnitBlueprint(bp)
        local death = bp.Weapon[1]
        assert(death.RackReloadTimeout == nil and death.RackSalvoReloadTime == nil and
               death.RackSalvoChargeTime == nil and death.RackRecoilDistance == nil and
               death.WeaponRepackTimeout == nil, 'a rack time filled')
        assert(bp.Weapon[2].RackReloadTimeout == 2, 'own rack time lost')
        assert(bp.Display.MovementEffects == nil, 'MovementEffects filled')
    )"));
}

TEST_CASE("A unit blueprint's derived fields read as Moho's OnInitBlueprint derives them",
          "[blueprints][moho-reflection]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        -- An aircraft that names only its motion type and top speed
        local plane = {BlueprintId = 'plane', SizeX = 1, SizeY = 0.5, SizeZ = 2,
                       Physics = {MotionType = 'RULEUMT_Air', MaxSpeed = 10, Elevation = 18,
                                  MaxAcceleration = 4, MaxBrake = 6}}
        RegisterUnitBlueprint(plane)
        local air, p = plane.Air, plane.Physics
        assert(air.CanFly == true, 'CanFly from the motion type')
        assert(air.MaxAirspeed == 10 and air.MinAirspeed == 10, 'airspeeds from MaxSpeed')
        assert(air.StartTurnDistance == 6, 'StartTurnDistance is 3 lengths')
        assert(p.AttackElevation == 18, 'AttackElevation from Elevation')
        assert(p.CatchUpAcc == 6, 'CatchUpAcc is the larger of acceleration and brake')
        assert(p.MaxSpeedReverse == 10, 'reverse speed')
        -- A solid box's inertia, as floats
        assert(math.abs(plane.InertiaTensorX - (0.25 + 4) / 12) < 1e-5, 'InertiaTensorX')
        assert(math.abs(plane.InertiaTensorY - (1 + 4) / 12) < 1e-5, 'InertiaTensorY')
        assert(math.abs(plane.InertiaTensorZ - (1 + 0.25) / 12) < 1e-5, 'InertiaTensorZ')
        -- Its footprint its size, rounded up, at least 1; a skirt the footprint
        assert(plane.Footprint.SizeX == 1 and plane.Footprint.SizeZ == 2, 'footprint')
        assert(p.SkirtSizeX == 1 and p.SkirtOffsetZ == 0, 'skirt')
        -- What the .bp gives stays
        local own = {BlueprintId = 'own', SizeZ = 2, Air = {StartTurnDistance = 1,
                     MinAirspeed = 4, MaxAirspeed = 12, CanFly = true},
                     InertiaTensorX = 1, InertiaTensorY = 2, InertiaTensorZ = 3,
                     Physics = {MotionType = 'RULEUMT_Air', CatchUpAcc = 2, AttackElevation = 5,
                                Elevation = 9}}
        RegisterUnitBlueprint(own)
        assert(own.Air.StartTurnDistance == 1 and own.Air.MinAirspeed == 4, 'own air')
        assert(own.InertiaTensorY == 2, 'own inertia')
        assert(own.Physics.CatchUpAcc == 2 and own.Physics.AttackElevation == 5, 'own physics')
        -- A unit that doesn't fly keeps no airspeed; without an Elevation,
        -- its attack elevation stays 0
        local tank = {BlueprintId = 'tank', Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 3}}
        RegisterUnitBlueprint(tank)
        assert(tank.Air.CanFly == false and tank.Air.MaxAirspeed == 0, 'tank airspeed')
        assert(tank.Physics.AttackElevation == 0, 'tank attack elevation')
    )"));
}

TEST_CASE("A blueprint's resource ids read lowercased, with forward slashes",
          "[blueprints][moho-reflection]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        local bp = {BlueprintId = 'UEB1101', StrategicIconName = 'Icon_Structure1_Energy',
                    General = {UpgradesTo = 'UEB1201', UpgradesFrom = ''},
                    Weapon = {{Label = 'Gun',
                               ProjectileId = '/Projectiles\\TDFGauss01/TDFGauss01_proj.bp'}}}
        RegisterUnitBlueprint(bp)
        assert(bp.General.UpgradesTo == 'ueb1201', 'UpgradesTo ' .. bp.General.UpgradesTo)
        assert(bp.General.UpgradesFrom == 'none', 'an empty UpgradesFrom reads none')
        assert(bp.Display.IconName == 'ueb1101', 'IconName from the id')
        assert(bp.StrategicIconName == 'icon_structure1_energy', 'StrategicIconName')
        assert(bp.Weapon[1].ProjectileId == '/projectiles/tdfgauss01/tdfgauss01_proj.bp',
               'ProjectileId ' .. bp.Weapon[1].ProjectileId)
        -- Not a resource id: as given
        assert(bp.Weapon[1].Label == 'Gun', 'Label changed')
    )"));
}

TEST_CASE("A projectile blueprint's omitted spreads read as Moho's",
          "[blueprints][moho-reflection]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        local bp = {BlueprintId = 'shell', Physics = {MaxSpeed = 40, TurnRateRange = 2}}
        RegisterProjectileBlueprint(bp)
        local p = bp.Physics
        assert(p.DirectionXRange == 1.5 and p.DirectionZRange == 1.5, 'direction spread')
        assert(p.MaxSpeedRange == 0 and p.LifetimeRange == 0, 'other spreads')
        assert(p.TurnRateRange == 2 and p.MaxSpeed == 40, 'own values lost')
        -- The rest of its physics isn't filled: the engine's projectile
        -- reads an absent one as its own preset
        assert(p.Acceleration == nil, 'Acceleration filled')
    )"));
}

TEST_CASE("The fields left unfilled stay as the .bp gives them, and aren't also filled",
          "[blueprints][moho-reflection]") {
    BlueprintWorld w;
    CHECK(w.check("bare = {BlueprintId = 'bare'} RegisterUnitBlueprint(bare)"));
    std::set<std::string> filled;
    for (const auto& f : osc::blueprints::unit_reflected_fields()) {
        CHECK(f.address != 0);
        CHECK(filled.insert(path(f.section, f.name)).second);
    }
    for (const auto& f : osc::blueprints::unit_unfilled_fields()) {
        const std::string name = path(f.section, f.name);
        INFO(name);
        CHECK(f.reason[0] != '\0');
        CHECK(filled.count(name) == 0);
        CHECK(w.check("assert(bare." + name + " == nil)"));
    }
    std::set<std::string> weapon;
    for (const auto& f : osc::blueprints::weapon_reflected_fields()) {
        CHECK(f.address != 0);
        CHECK(weapon.insert(f.name).second);
    }
}

TEST_CASE("Both Lua states read a registered blueprint's reflected fields",
          "[blueprints][moho-reflection]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        RegisterUnitBlueprint({BlueprintId = 'uaa0310', Weapon = {{Label = 'Gun'}}})
    )"));
    w.store.copy_lua_tables(w.state.raw());
    w.store.expose_to_lua(w.state.raw());
    CHECK(w.check(R"(
        local bp = __blueprints.uaa0310
        assert(bp.Economy.MaxBuildDistance == 5, 'MaxBuildDistance')
        assert(bp.Weapon[1].MinRadius == 0, 'MinRadius')
        assert(bp.Description == '', 'Description')
    )"));
}
