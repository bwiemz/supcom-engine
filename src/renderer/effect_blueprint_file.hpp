#pragma once

#include "core/types.hpp"

#include <functional>
#include <string>

struct lua_State;

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::renderer {

/// Run the effect blueprint file at `path` (`EmitterBlueprint { … }`,
/// `BeamBlueprint { … }` or `TrailEmitterBlueprint { … }`) in `L`, with the
/// global `kind` capturing its table and the other kinds doing nothing, then
/// hand the table to `parse(L, index)`. False when the file can't be read or
/// run, or holds no blueprint of that kind. `L`'s globals are as they were
/// afterwards.
bool run_effect_blueprint(const vfs::VirtualFileSystem* vfs, const std::string& path, lua_State* L,
                          const char* kind, const std::function<void(lua_State*, int)>& parse);

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

} // namespace osc::renderer
