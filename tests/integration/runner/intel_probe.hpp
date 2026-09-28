#pragma once

// What a render test reads back from a frame (the render-state dump), and
// the scene helpers the intel and icon tests share (M215).

#include "core/types.hpp"
#include "renderer/recon_view.hpp"
#include "sim/entity.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace osc::sim {
class SimState;
}
namespace osc::renderer {
class Renderer;
}

namespace osc::test {

struct TestContext;

/// A mesh the last frame drew: where (its model's translation), what
/// texture, and its pose's digest (empty without one).
struct Drawn {
    std::string texture;
    f32 x = 0, z = 0;
    std::string pose;
};

/// A 2D quad the last frame drew: its middle on screen, size, colour, and
/// (an icon's) texture.
struct Quad {
    f32 x = 0, y = 0, w = 0, h = 0;
    f32 r = 0, g = 0, b = 0;
    std::string texture;
};

struct Frame {
    std::vector<Drawn> meshes;
    std::vector<Quad> icons;   ///< strategic icons, in draw order
    std::vector<Quad> overlay; ///< health bars and the like
    std::vector<Quad> minimap; ///< the C++ HUD's minimap
};

/// The last frame's meshes, strategic icons, overlays and minimap, from its
/// render-state dump.
Frame drawn(renderer::Renderer& r);

/// The mesh drawn at (x, z).
const Drawn* mesh_at(const Frame& frame, f32 x, f32 z);

/// How many meshes of texture `texture` were drawn within `radius` of x.
int meshes_near(const Frame& frame, const std::string& texture, f32 x, f32 radius);

struct Spot {
    f32 x = 0, z = 0;
};

/// The spot, on a 16-unit lattice, farthest from every unit whose box
/// [x - 30, x + 50] x [z - 40, z + 70] is dry land; none within 60 of a
/// unit will do.
std::optional<Spot> quiet_spot(sim::SimState& sim);

const char* name(renderer::Sight s);

/// Run `code` in the sim state, logging a failure.
void run_lua(TestContext& ctx, const std::string& code);

/// Make a `bp` of `army`'s on the ground at `at` (or `lift` above it), as
/// Lua global `global`; its entity id.
u32 spawn_unit(TestContext& ctx, const char* global, const char* bp, const char* army, Spot at,
               f32 lift = 0.0f);

/// Where `p` is on `r`'s screen, as its icons and overlays project it.
std::optional<std::array<f32, 2>> screen_of(renderer::Renderer& r, const sim::Vector3& p);

/// The w x h quad centred at (x, y), within a pixel, or null.
const Quad* quad_at(const std::vector<Quad>& quads, f32 x, f32 y, f32 w, f32 h);

bool same_colour(const Quad& q, f32 r, f32 g, f32 b);

/// The C++ HUD minimap's dot for a unit at world `p`, or null (the map
/// `map_w` x `map_h`, drawn in `r`'s corner).
const Quad* minimap_dot(const Frame& frame, renderer::Renderer& r, f32 map_w, f32 map_h,
                        const sim::Vector3& p);

} // namespace osc::test
