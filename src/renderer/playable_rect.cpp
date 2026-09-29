#include "renderer/playable_rect.hpp"

#include <algorithm>

namespace osc::renderer {

std::optional<PlayableRect> clamp_playable_rect(PlayableRect rect, i32 width, i32 height) {
    if (width <= 0 || height <= 0) return std::nullopt;
    rect.x0 = std::clamp(rect.x0, 0, width);
    rect.z0 = std::clamp(rect.z0, 0, height);
    rect.x1 = std::clamp(rect.x1, 0, width);
    rect.z1 = std::clamp(rect.z1, 0, height);
    if (rect.x1 <= rect.x0 || rect.z1 <= rect.z0) return std::nullopt;
    return rect;
}

void UserPlayableRect::apply(const sim::FrameView& view) {
    if (!pending_ || !view.cur()) return;
    const PlayableRect rect = *pending_;
    pending_.reset();
    hidden_.clear();
    for (const sim::EntityRecord& e : view.entities()) {
        // Moho truncates the position to whole units.
        const auto x = static_cast<i32>(e.position.x);
        const auto z = static_cast<i32>(e.position.z);
        if (x < rect.x0 || x >= rect.x1 || z < rect.z0 || z >= rect.z1) hidden_.insert(e.id);
    }
}

void UserPlayableRect::clear() {
    pending_.reset();
    hidden_.clear();
}

} // namespace osc::renderer
