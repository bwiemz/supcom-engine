#pragma once

#include "core/types.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>

struct lua_State;

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::renderer {

/// A `TrailEmitterBlueprint { ... }` (M214b), with Moho's defaults
/// (RTrailBlueprint) for what it leaves out. Moho reflects no UShift/VShift
/// for trails, so those some retail files name are ignored.
struct TrailBlueprintData {
    f32 lifetime = 0.0f;     ///< ticks the emitter lives (negative: until its entity goes)
    f32 trail_length = 0.0f; ///< ticks a segment lasts
    f32 size = 0.0f;         ///< half the ribbon's width (Moho's StartSize)
    f32 sort_order = 0.0f;   ///< below 0, drawn under the water
    i32 blendmode = 0;       ///< particle.fx's TPolyTrail suffix (0: ALPHABLEND)
    f32 lod_cutoff = 100.0f; ///< emits within this of the camera (0: anywhere)
    bool emit_if_visible = true;
    f32 texture_repeat_rate = 0.0f; ///< repeats a unit of distance travelled
    std::string repeat_texture;     ///< sampled (across, distance), wrapping
    std::string ramp_texture;       ///< sampled (age, across), clamped
};

/// Trail blueprints by VFS path, read as BeamBlueprintCache reads beams.
class TrailBlueprintCache {
public:
    void set_vfs(const vfs::VirtualFileSystem* vfs) { vfs_ = vfs; }

    /// The trail blueprint at `path`, loaded on first use; null if it isn't
    /// one or can't be read. A failed path is remembered.
    const TrailBlueprintData* get(const std::string& path, lua_State* L);

    void clear() {
        cache_.clear();
        failed_.clear();
    }

private:
    const vfs::VirtualFileSystem* vfs_ = nullptr;
    std::unordered_map<std::string, TrailBlueprintData> cache_;
    std::unordered_set<std::string> failed_;
};

} // namespace osc::renderer
