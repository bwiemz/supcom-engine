#pragma once

#include "core/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace osc::ui {

/// Moho's ScriptedDecal (Lua's UserDecal): a splat the UI lays on the
/// terrain, such as the cursor's target area.
class UserDecals {
public:
    struct Decal {
        std::string texture;
        std::array<f32, 3> corner{};
        std::array<f32, 3> scale{1.0f, 1.0f, 1.0f};
        u32 added = 0;
    };

    u32 create() {
        const u32 id = next_id_++;
        decals_.emplace(id, Decal{});
        return id;
    }

    void destroy(u32 id) { decals_.erase(id); }

    void set_texture(u32 id, std::string path) {
        if (Decal* d = find(id)) {
            d->texture = std::move(path);
            d->added = next_added_++;
        }
    }

    void set_position(u32 id, f32 x, f32 y, f32 z) {
        Decal* d = find(id);
        if (!d || std::isnan(x) || std::isnan(y) || std::isnan(z)) {
            return;
        }
        d->corner = {x - d->scale[0] * 0.5f, y, z - d->scale[2] * 0.5f};
    }

    /// Moho's SetScale places it again from its corner, not its centre.
    void set_scale(u32 id, f32 x, f32 y, f32 z) {
        Decal* d = find(id);
        if (!d) {
            return;
        }
        d->scale = {x, y, z};
        set_position(id, d->corner[0], d->corner[1], d->corner[2]);
    }

    const Decal* get(u32 id) const {
        const auto it = decals_.find(id);
        return it == decals_.end() ? nullptr : &it->second;
    }

    /// Those with a texture, in the order they got it.
    std::vector<const Decal*> splats() const {
        std::vector<const Decal*> out;
        for (const auto& [id, d] : decals_) {
            if (!d.texture.empty()) {
                out.push_back(&d);
            }
        }
        std::sort(out.begin(), out.end(),
                  [](const Decal* a, const Decal* b) { return a->added < b->added; });
        return out;
    }

    void clear() { decals_.clear(); }

private:
    Decal* find(u32 id) {
        const auto it = decals_.find(id);
        return it == decals_.end() ? nullptr : &it->second;
    }

    std::map<u32, Decal> decals_;
    u32 next_id_ = 1;
    u32 next_added_ = 0;
};

} // namespace osc::ui
