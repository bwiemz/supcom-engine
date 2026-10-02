#pragma once

#include "core/types.hpp"
#include "map/terrain.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace osc::sim {
struct WorldSnapshot;
}

namespace osc::renderer {

/// A texture a runtime decal names, as Moho's CDecalManager resolves it
/// (ResolveDecalTexturePath): a name starting with '/' or '\' is a path;
/// any other is a file in /env/common/splats/ (a splat's) or
/// /env/common/decals/ with ".dds" added. An empty name is none.
std::string resolve_decal_texture(const std::string& name, bool splat);

/// A decal type by its name (CWldTerrainDecal::LookupDecalType over
/// sTypeDesc: "Albedo", "Alpha Normals", ...), or none for an unknown name
/// or "Undefined".
std::optional<map::DecalType> decal_type_named(std::string_view name);

/// The runtime decals and splats as the player sees them (Moho's user-side
/// CDecalManager, M212c), fed each sim tick as CWldSession::DoBeat feeds it:
/// the decals the focus army has come to see are added, those it has lost
/// (gone from the sim, or no longer seen) removed, and removals fade.
class RuntimeDecals {
public:
    struct Decal {
        u32 id = 0; ///< its effect's
        bool splat = false;
        map::DecalInfo info; ///< its type, resolved textures, placement and cutoff
        f32 alpha = 1.0f;    ///< mCurrentAlpha: 1 until it fades
        u32 remove_tick = 0; ///< fades once the tick passes it; 0: never
        bool live = true;    ///< still in the sim and seen (not removed)
        u32 fidelity = 1;    ///< its fidelity: 0 draws at graphics fidelity 0 too (M212h)
    };

    /// A splat's fade a beat, and a decal's (CDecalManager::ProcessRemovals).
    static constexpr f32 kSplatFade = 0.03f;
    static constexpr f32 kDecalFade = 0.2f;

    /// Take `snap`'s tick for `focus_army` (-1: an observer, who sees them
    /// all). Once a tick: its adds and removals, then a fade for each tick
    /// since the last. An earlier tick than the last (a new game) starts
    /// again.
    void update(const sim::WorldSnapshot& snap, i32 focus_army);
    void clear();

    /// The decals (not splats), in the order they came.
    const std::vector<Decal>& decals() const { return decals_; }
    /// The splats, in the order they came.
    const std::vector<Decal>& splats() const { return splats_; }
    /// Changes whenever a decal (not a splat) comes or goes, so the
    /// renderer knows to gather their triangles again.
    u32 generation() const { return generation_; }

private:
    void add(const sim::WorldSnapshot& snap, size_t effect);
    void fade(u32 tick);

    std::vector<Decal> decals_;
    std::vector<Decal> splats_;
    /// The effects taken (added, or dropped for an unknown type) and live.
    std::unordered_set<u32> taken_;
    std::optional<u32> last_tick_;
    i32 last_focus_ = -1;
    u32 generation_ = 0;
};

} // namespace osc::renderer
