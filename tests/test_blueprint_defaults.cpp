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
