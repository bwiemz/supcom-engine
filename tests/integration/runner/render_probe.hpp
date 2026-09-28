#pragma once

// Offscreen frames for render tests that compare pixels (M210a, M212a):
// build a scene, look at one spot, capture the middle of the frame once the
// textures it draws have loaded.

#include "core/image.hpp"
#include "core/types.hpp"
#include "renderer/renderer.hpp"
#include "sim/world_snapshot.hpp"

#include <array>
#include <string>
#include <unordered_set>
#include <vector>

namespace osc::map {
class Terrain;
}

namespace osc::test {

struct TestContext;

/// Pass/fail counts for a test mode's checks; a failure is recorded in
/// test_status.
struct Tally {
    int pass = 0;
    int fail = 0;
    void check(bool ok, const std::string& what);
};

/// A frame's middle (35%-65% each way), rgb in [0, 1] per pixel.
using Pixels = std::vector<std::array<f32, 3>>;

Pixels centre_pixels(const ImageRGBA8& image);

f32 median(std::vector<f32> values);

/// The median, over the pixels bright enough to measure in `below` (0.06 in
/// the channel), of `above`'s channel over `below`'s.
f32 median_ratio(const Pixels& above, const Pixels& below, int channel);

/// The mean absolute difference of two frames, per channel.
f32 mean_abs_diff(const Pixels& a, const Pixels& b);

/// An offscreen renderer with bloom, the fog of war and decals off, so a
/// frame is the lit scene alone, against kBackdrop.
class OffscreenShots {
public:
    /// What it clears to (the tests measure their scenery against it).
    static constexpr std::array<f32, 4> kBackdrop = {0.55f, 0.62f, 0.72f, 0.0f};
    /// The tests' pitch unless one sets another (radians, held).
    static constexpr f32 kPitch = 0.87f;

    explicit OffscreenShots(TestContext& ctx);
    ~OffscreenShots();
    OffscreenShots(const OffscreenShots&) = delete;
    OffscreenShots& operator=(const OffscreenShots&) = delete;

    /// False if there is no Vulkan device.
    bool ok() const { return ok_; }

    /// Build the scene for `terrain` (its lighting and strata as they are
    /// now, as a map load would), look at (x, z) from `distance`, and
    /// capture the frame's middle. `with_world` draws the sim's entities;
    /// without, the terrain alone.
    Pixels shoot(const map::Terrain& terrain, f32 x, f32 z, f32 distance, bool with_world = true);

    /// As shoot(), with the sim's entities, but the whole frame.
    ImageRGBA8 shoot_frame(const map::Terrain& terrain, f32 x, f32 z, f32 distance);

    renderer::Renderer& renderer() { return renderer_; }

    /// Capture the sim's world again (after the test changed it).
    void recapture();

    /// Draw the captured world again in the scene the last shot built, as a
    /// game's next frame does: what the renderer carries from frame to
    /// frame (the player's intel) carries on. `selected`: the player's
    /// selection.
    void redraw(const std::unordered_set<u32>* selected = nullptr);

    /// As redraw(), capturing the whole frame.
    ImageRGBA8 grab(const std::unordered_set<u32>* selected = nullptr);

    /// The events the next frame shows (the sim's are cleared as its tick
    /// ends, before recapture() can take them; a test adds its own).
    sim::WorldEvents& events() { return history_.events(); }

private:
    ImageRGBA8 capture(const map::Terrain& terrain, f32 x, f32 z, f32 distance, bool with_world);

    TestContext& ctx_;
    renderer::Renderer renderer_;
    sim::WorldHistory history_;
    sim::WorldHistory empty_;
    bool ok_ = false;
};

} // namespace osc::test
