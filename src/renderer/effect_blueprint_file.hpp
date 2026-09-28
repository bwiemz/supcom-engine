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

} // namespace osc::renderer
