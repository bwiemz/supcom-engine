#pragma once

#include "blueprints/blueprint_store.hpp"
#include "core/types.hpp"

#include <functional>
#include <string>

struct lua_State;

namespace osc::renderer {

/// Hand the effect blueprint of `type` registered under `name`
/// (BlueprintStore::find_effect) to `parse(L, index)`: its table, read in
/// `L`, which must be the store's Lua state. False when none is (or `L`
/// isn't). Moho's effects keep the blueprint the rules registered
/// (CEffectManagerImpl looks it up with GetEmitterBlueprint,
/// GetBeamBlueprint or GetTrailBlueprint), so a mod's change to one -- a
/// blueprint of the same id, a Merge, a ModBlueprints edit -- is what
/// draws; and Moho never reads the file again, so an effect whose name
/// isn't registered draws nothing. `L`'s stack is as it was afterwards.
bool read_effect_blueprint(const blueprints::BlueprintStore* store, lua_State* L,
                           const std::string& name, blueprints::BlueprintType type,
                           const std::function<void(lua_State*, int)>& parse);

/// t[key] of the blueprint table at absolute index `idx` as a number, or
/// `fallback` when it isn't one.
f32 blueprint_number(lua_State* L, int idx, const char* key, f32 fallback);

/// t[key] as a lower-cased path (VFS paths are), or "" when it isn't a
/// string.
std::string blueprint_path(lua_State* L, int idx, const char* key);

/// The fidelities an effect blueprint is made at (REffectBlueprint's
/// LowFidelity, MedFidelity and HighFidelity, each true unless the file says
/// false): bit n allows graphics_Fidelity n (0 low, 1 medium, 2 high).
u8 blueprint_fidelity(lua_State* L, int idx);

/// Whether an effect of `fidelity` is made at `graphics_fidelity`, as
/// CEffectManagerImpl's IsBlueprintEnabledForCurrentFidelity asks: Moho
/// makes each effect, then destroys at once one its blueprint leaves out.
constexpr bool fidelity_allows(u8 fidelity, int graphics_fidelity) {
    return graphics_fidelity >= 0 && graphics_fidelity < 8 &&
           (fidelity & (1u << static_cast<u32>(graphics_fidelity))) != 0;
}

/// Moho's efx_ParticleWaterSurface and efx_WaterOffset: retail's emitters under the water
/// have SortOrder -102 and below, its water-surface wakes -101 and above.
constexpr f32 kParticleWaterSurface = -101.0f;
constexpr f32 kParticleWaterOffset = 0.01f;

constexpr bool draws_under_water(f32 sort_order) {
    return sort_order < kParticleWaterSurface;
}

} // namespace osc::renderer
