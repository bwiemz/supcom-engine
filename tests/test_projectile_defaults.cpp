// A projectile blueprint's Physics spreads a .bp leaves out read as Moho's
// RProjectileBlueprintPhysics defaults (faf-re): FAF's cruise missiles add
// MaxSpeedRange to MaxSpeed as they are made.

#include <catch2/catch_test_macros.hpp>

#include "blueprints/blueprint_store.hpp"
#include "lua/blueprint_bindings.hpp"
#include "lua/lua_state.hpp"

#include <string>

TEST_CASE("A projectile's omitted Physics spreads read as Moho's defaults", "[blueprints]") {
    osc::lua::LuaState state;
    osc::blueprints::BlueprintStore store{state.raw()};
    state.set_blueprint_store(&store);
    osc::lua::register_blueprint_store_bindings(state);
    auto r = state.do_string(R"(
        -- TIFMissileCruise01: a MaxSpeed, no MaxSpeedRange.
        local cruise = {BlueprintId = 'cruise', Physics = {MaxSpeed = 12, TurnRateRange = 4}}
        RegisterProjectileBlueprint(cruise)
        local p = cruise.Physics
        assert(p.MaxSpeedRange == 0, 'MaxSpeedRange ' .. tostring(p.MaxSpeedRange))
        assert(p.MaxSpeed + p.MaxSpeedRange == 12, 'as FAF adds them')
        assert(p.DirectionXRange == 1.5 and p.DirectionZRange == 1.5 and p.DirectionYRange == 0,
               'direction spreads')
        assert(p.TurnRateRange == 4, 'its own spread lost')
        -- Only the spreads: the engine's flight reads the rest as before.
        assert(p.Acceleration == nil and p.Lifetime == nil, 'filled more than the spreads')
        -- One with no Physics at all gets them too.
        local bare = {BlueprintId = 'bare'}
        RegisterProjectileBlueprint(bare)
        assert(bare.Physics and bare.Physics.LifetimeRange == 0, 'no Physics')
    )");
    INFO((r.ok() ? std::string() : r.error().message));
    CHECK(r.ok());
}
