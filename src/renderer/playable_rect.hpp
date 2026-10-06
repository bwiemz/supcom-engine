#pragma once

#include "core/types.hpp"
#include "sim/world_snapshot.hpp"

#include <optional>
#include <unordered_set>

namespace osc::renderer {

/// A playable rect in whole map units: x0 <= x < x1, z0 <= z < z1
/// (Moho's gpg::Rect2i).
struct PlayableRect {
    i32 x0 = 0;
    i32 z0 = 0;
    i32 x1 = 0;
    i32 z1 = 0;
};

/// `rect` kept to a `width` x `height` map, as STIMap::SetPlayableMapRect
/// keeps it: each edge within [0, size]. Nothing when no area is left.
std::optional<PlayableRect> clamp_playable_rect(PlayableRect rect, i32 width, i32 height);

/// The user side's playable rect (Moho's CWldSession::SyncPlayableRect,
/// which the sim's scripts reach through Sync.CameraRequests). A sync hides
/// the meshes of the entities outside the rect then, until the next sync:
/// one that moves in stays hidden, one that appears later is drawn.
class UserPlayableRect {
public:
    /// A sync from the scripts, applied to the next frame's world.
    void sync(const PlayableRect& rect) { pending_ = rect; }

    /// Apply a pending sync to `view`'s entities, as they are at its newest
    /// tick.
    void apply(const sim::FrameView& view);

    /// Whether the last sync hid `id`'s mesh.
    bool hides(u32 id) const { return hidden_.count(id) != 0; }

    const std::optional<PlayableRect>& rect() const { return rect_; }

    /// A new world: nothing hidden, nothing pending.
    void clear();

private:
    std::optional<PlayableRect> pending_;
    std::optional<PlayableRect> rect_;
    std::unordered_set<u32> hidden_;
};

} // namespace osc::renderer
