// Blueprint fields a .bp leaves out read as Moho's defaults: it hands
// scripts blueprints rebuilt from its typed copies (REntityBlueprint,
// RUnitBlueprintWeapon), and retail's and FAF's scripts read them unguarded.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/blueprint_bindings.hpp"
#include "lua/lua_state.hpp"

#include <string>

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

} // namespace

TEST_CASE("A unit blueprint's omitted collision offsets read as 0", "[blueprints]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        local bare = {BlueprintId = 'bare', SizeX = 1, SizeY = 1, SizeZ = 1}
        RegisterUnitBlueprint(bare)
        assert(bare.CollisionOffsetX == 0 and bare.CollisionOffsetY == 0 and
               bare.CollisionOffsetZ == 0, 'offsets not defaulted')
        -- A .bp's own offsets stay.
        local set = {BlueprintId = 'set', CollisionOffsetY = -0.25, CollisionOffsetZ = 0.5}
        RegisterUnitBlueprint(set)
        assert(set.CollisionOffsetX == 0, 'X')
        assert(set.CollisionOffsetY == -0.25 and set.CollisionOffsetZ == 0.5, 'own offsets lost')
    )"));
}

TEST_CASE("A unit blueprint's omitted upgrade links read as Moho's: from 'none', to ''",
          "[blueprints]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        -- A T1 power generator names none; construction.lua builds a
        -- structure whose UpgradesFrom is 'none', else upgrades to it
        local bare = {BlueprintId = 'ueb1101', General = {Category = 'Economy'}}
        RegisterUnitBlueprint(bare)
        local g = bare.General
        -- (UpgradesTo is empty: FAF's unit detail view looks up a unit's
        -- UpgradesTo unless it is '')
        assert(g.UpgradesFrom == 'none' and g.UpgradesTo == '' and
               g.UpgradesFromBase == 'none', 'links not defaulted')
        assert(g.Category == 'Economy', 'General lost')
        -- Without a General at all
        local none = {BlueprintId = 'none'}
        RegisterUnitBlueprint(none)
        assert(none.General.UpgradesFrom == 'none', 'no General: not defaulted')
        -- An upgrade's own links stay
        local t2 = {BlueprintId = 'uab1201', General = {UpgradesFrom = 'uab1101',
                                                        UpgradesTo = 'uab1301'}}
        RegisterUnitBlueprint(t2)
        assert(t2.General.UpgradesFrom == 'uab1101' and t2.General.UpgradesTo == 'uab1301',
               'own links lost')
        assert(t2.General.UpgradesFromBase == 'none', 'base')
    )"));
}

TEST_CASE("A unit blueprint's omitted icon name reads as its id", "[blueprints]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        local bare = {BlueprintId = 'uab0101', Display = {Mesh = {}}}
        RegisterUnitBlueprint(bare)
        assert(bare.Display.IconName == 'uab0101', 'icon name not defaulted')
        local upper = {BlueprintId = 'UEL0001'}
        RegisterUnitBlueprint(upper)
        assert(upper.Display.IconName == 'uel0001', 'no Display, or not lowered')
        local named = {BlueprintId = 'named', Display = {IconName = 'other'}}
        RegisterUnitBlueprint(named)
        assert(named.Display.IconName == 'other', 'own icon name lost')
    )"));
}

TEST_CASE("A unit blueprint's omitted strategic icon name reads as empty", "[blueprints]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        local bot = {BlueprintId = 'ura0001'}
        RegisterUnitBlueprint(bot)
        assert(bot.StrategicIconName == '', 'strategic icon name not defaulted')
        local named = {BlueprintId = 'uel0105', StrategicIconName = 'icon_land1_engineer'}
        RegisterUnitBlueprint(named)
        assert(named.StrategicIconName == 'icon_land1_engineer', 'own name lost')
    )"));
}

TEST_CASE("A unit blueprint's omitted threat levels and regeneration read as 0", "[blueprints]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        -- The UEF T1 transport's Defense: health and armour, no threat levels,
        -- which the AI's GetThreatOfUnits adds unguarded
        local bare = {BlueprintId = 'bare', Defense = {MaxHealth = 600, Health = 600}}
        RegisterUnitBlueprint(bare)
        local d = bare.Defense
        assert(d.AirThreatLevel == 0 and d.SurfaceThreatLevel == 0 and d.SubThreatLevel == 0 and
               d.EconomyThreatLevel == 0, 'threat levels not defaulted')
        assert(d.RegenRate == 0, 'regeneration not defaulted')
        assert(d.MaxHealth == 600, 'health lost')
        -- Without a Defense at all
        local none = {BlueprintId = 'none'}
        RegisterUnitBlueprint(none)
        assert(none.Defense.SurfaceThreatLevel == 0, 'no Defense: not defaulted')
        -- A .bp's own values stay
        local set = {BlueprintId = 'set', Defense = {SurfaceThreatLevel = 3, RegenRate = 2}}
        RegisterUnitBlueprint(set)
        assert(set.Defense.SurfaceThreatLevel == 3 and set.Defense.RegenRate == 2, 'own lost')
        assert(set.Defense.AirThreatLevel == 0, 'air')
    )"));
}

TEST_CASE("A weapon's omitted RateOfFire reads as 1, its other numbers as 0", "[blueprints]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        -- The UEF T1 transport's guidance system: no RateOfFire, which FAF's
        -- projectile weapons divide by as they are made.
        local bp = {BlueprintId = 'transport', Weapon = {
            {Label = 'GuidanceSystem'},
            {Label = 'Gun', RateOfFire = 0.5, DamageRadius = 2},
        }}
        RegisterUnitBlueprint(bp)
        local guidance, gun = bp.Weapon[1], bp.Weapon[2]
        assert(guidance.RateOfFire == 1, 'RateOfFire ' .. tostring(guidance.RateOfFire))
        assert(guidance.Label == 'GuidanceSystem' and gun.Label == 'Gun', 'labels changed')
        assert(guidance.Damage == 0 and guidance.DamageRadius == 0, 'numbers not 0')
        assert(gun.RateOfFire == 0.5 and gun.DamageRadius == 2, 'own values lost')
    )"));
}

TEST_CASE("A weapon without a Label reads as the empty string", "[blueprints]") {
    // The UEF T1 mobile AA's first weapon has none; FAF's weapons copy it to
    // self.Label and its unit indexes WeaponInstances by it, which a nil key
    // breaks ("table index is nil" in Unit.OnCreate).
    BlueprintWorld w;
    CHECK(w.check(R"(
        local bp = {BlueprintId = 'aa', Weapon = {{MaxRadius = 28}, {Label = 'AAGun'}}}
        RegisterUnitBlueprint(bp)
        assert(bp.Weapon[1].Label == '', 'Label ' .. tostring(bp.Weapon[1].Label))
        assert(bp.Weapon[2].Label == 'AAGun', 'own label lost')
        local instances = {}
        instances[bp.Weapon[1].Label] = true -- a key, as FAF's unit makes it
    )"));
}

TEST_CASE("A unit blueprint's omitted intel, cap cost and drive read as Moho's defaults",
          "[blueprints]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        -- A T1 tank gives no WaterVisionRadius, a power generator no Physics
        -- speeds: FAF's unit detail view formats all of them as a build
        -- button is hovered
        local tank = {BlueprintId = 'tank', Intel = {VisionRadius = 20},
                      Physics = {MotionType = 'RULEUMT_Land', MaxSpeed = 3}}
        RegisterUnitBlueprint(tank)
        assert(tank.Intel.VisionRadius == 20 and tank.Intel.WaterVisionRadius == 10, 'vision')
        assert(tank.Intel.RadarRadius == 0 and tank.Intel.OmniRadius == 0, 'other intel')
        assert(tank.General.CapCost == 1, 'cap cost')
        local p = tank.Physics
        assert(p.MotionType == 'RULEUMT_Land' and p.AltMotionType == 'RULEUMT_None', 'motion')
        assert(p.MaxSpeedReverse == 3, 'reverse speed is the top speed')
        assert(p.MaxAcceleration == 0 and p.MaxBrake == 0 and p.TurnRate == 0, 'drive')
        local pgen = {BlueprintId = 'pgen'}
        RegisterUnitBlueprint(pgen)
        assert(pgen.Intel.VisionRadius == 10, 'default vision')
        assert(pgen.Physics.MotionType == 'RULEUMT_None', 'no motion')
        assert(pgen.Physics.MaxSpeed == 0 and pgen.Physics.MaxSpeedReverse == -1, 'immobile')
        local own = {BlueprintId = 'own', General = {CapCost = 2},
                     Physics = {MaxSpeed = 4, MaxSpeedReverse = 1}}
        RegisterUnitBlueprint(own)
        assert(own.General.CapCost == 2 and own.Physics.MaxSpeedReverse == 1, 'own lost')
    )"));
}

TEST_CASE("Scripts iterate a blueprint's tables in the order of Moho's copy of it",
          "[blueprints]") {
    BlueprintWorld w;
    CHECK(w.check(R"(
        RegisterUnitBlueprint({BlueprintId = 'uel0001', Enhancements = {Slots = {
            Back = {name = '<LOC _Back>', x = 38, y = -10},
            LCH = {name = '<LOC _LCH>', x = 105, y = 30},
            RCH = {name = '<LOC _RCH>', x = -10, y = 30},
        }}})
    )"));
    w.store.copy_lua_tables(w.state.raw());
    w.store.expose_to_lua(w.state.raw());
    CHECK(w.check(R"(
        local order = ''
        for slot in pairs(__blueprints.uel0001.Enhancements.Slots) do
            order = order .. slot .. ' '
        end
        assert(order == 'RCH Back LCH ', order)
    )"));
}
