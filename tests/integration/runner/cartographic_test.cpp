// --cartographic-test: the cartographic frame (the minimap's map), drawn on
// the GPU, against a CPU reference of cartographic.fx's two passes fed
// from the same lookup data (renderer/cartographic_math):
// - Terrain_Stage0, every pixel: the colour ramp's texel at the point's
//   elevation, the contour ramp's in alpha; black past the map;
// - Terrain_Stage1, every pixel, from the GPU's own Stage 0: the colour
//   less saturate(16 (c - mean of the eight neighbours' alpha)), alpha 0;
// - its structure: contour lines on land, none in open water, the shore
//   lined.
// On SCMP_009, square and letterboxed.
//
// --cartographic-minimap-test: a cartographic minimap view (retail's
// minimap.lua sets SetCartographic(true)) draws that frame, pixel for
// pixel, over the whole pixels the UI clips it to, in both frames in
// flight and after a resize; without the flag, the terrain texture again.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "core/image.hpp"
#include "core/ui_registry_keys.hpp"
#include "lua/lua_state.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/cartographic_math.hpp"
#include "renderer/cartographic_renderer.hpp"
#include "renderer/minimap_renderer.hpp"
#include "renderer/range_overlays.hpp"
#include "renderer/renderer.hpp"
#include "renderer/resource_icon_renderer.hpp"
#include "renderer/texture_cache.hpp"
#include "renderer/ui_renderer.hpp"
#include "sim/category_expr.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "ui/ui_control.hpp"

#include <fmt/format.h>
#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace osc::test {

namespace {

using renderer::CartographicParams;
using renderer::CartographicRenderer;
using Images = renderer::Renderer::CartographicImages;

f32 half_to_float(u16 half) {
    const u32 h = half;
    const u32 exponent = (h >> 10u) & 0x1Fu;
    const u32 mantissa = h & 0x3FFu;
    f32 value = 0.0f;
    if (exponent == 0) value = std::ldexp(static_cast<f32>(mantissa), -24);
    else if (exponent == 31) value = mantissa ? NAN : INFINITY;
    else value = std::ldexp(static_cast<f32>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    return (h & 0x8000u) ? -value : value;
}

/// cartographic.fx's Terrain_Stage0 at the centres of a view's pixels.
struct Reference {
    CartographicParams params;
    std::vector<f32> elevation; ///< the texels, widened
    std::array<u32, renderer::kCartographicRampTexels> hypsometric{};
    std::array<u8, renderer::kCartographicRampTexels> topographic{};
    f32 span_x = 0, span_z = 0;

    explicit Reference(const map::Terrain& terrain)
        : params(renderer::cartographic_params(terrain)),
          hypsometric(renderer::hypsometric_ramp(params)),
          topographic(renderer::topographic_ramp(params)) {
        for (u16 h : renderer::cartographic_elevation(terrain, params))
            elevation.push_back(half_to_float(h));
        span_x = static_cast<f32>(terrain.heightmap().grid_width() - 1);
        span_z = static_cast<f32>(terrain.heightmap().grid_height() - 1);
    }

    f32 tap(i32 x, i32 z) const {
        x = std::clamp(x, 0, params.grid_width - 1);
        z = std::clamp(z, 0, params.grid_height - 1);
        return elevation[static_cast<size_t>(z) * static_cast<size_t>(params.grid_width) +
                         static_cast<size_t>(x)];
    }

    /// The elevation TerrainPS0 indexes the ramps with at world (x, z).
    f32 height(f32 x, f32 z) const {
        const f32 vx = params.terrain_size_coeff[0] * x;
        const f32 vz = params.terrain_size_coeff[1] * z;
        const f32 fx = std::floor(vx);
        const f32 fz = std::floor(vz);
        const f32 tx = vx - fx;
        const f32 tz = vz - fz;
        const auto bx = static_cast<i32>(fx);
        const auto bz = static_cast<i32>(fz);
        const auto lerp = [](f32 a, f32 b, f32 t) { return a + t * (b - a); };
        const f32 h = lerp(lerp(tap(bx, bz), tap(bx + 1, bz), tx),
                           lerp(tap(bx, bz + 1), tap(bx + 1, bz + 1), tx), tz);
        return std::clamp(h - 0.0001f, 0.0f, 1.0f);
    }

    static int index(f32 h) { return std::min(static_cast<int>(std::floor(h * 256.0f)), 255); }

    /// The pixel's RGBA as the ramps give it at elevation index `i`.
    std::array<u8, 4> rgba(int i) const {
        const u32 c = hypsometric[static_cast<size_t>(i)];
        return {static_cast<u8>(c >> 16u), static_cast<u8>(c >> 8u), static_cast<u8>(c),
                topographic[static_cast<size_t>(i)]};
    }
};

struct Compared {
    size_t pixels = 0, matched = 0, outside_black = 0, outside = 0;
};

/// Stage 0, every pixel more than a pixel from the map's edge. A point's
/// ramp texel may fall one over where its elevation is within 1e-4 of a
/// texel's boundary (the GPU's interpolation of the point is not the CPU's).
Compared compare_stage0(const Reference& ref, const Images& img,
                        const CartographicRenderer::View& v) {
    Compared c;
    const f32 dx = (v.x1 - v.x0) / static_cast<f32>(v.width);
    const f32 dz = (v.z1 - v.z0) / static_cast<f32>(v.height);
    for (u32 py = 0; py < img.height; ++py) {
        for (u32 px = 0; px < img.width; ++px) {
            const f32 x = v.x0 + (static_cast<f32>(px) + 0.5f) * dx;
            const f32 z = v.z0 + (static_cast<f32>(py) + 0.5f) * dz;
            const u8* got = &img.frame[(static_cast<size_t>(py) * img.width + px) * 4];
            const bool inside = x >= 0 && z >= 0 && x <= ref.span_x && z <= ref.span_z;
            const bool near_edge = std::abs(x) < 2 * dx || std::abs(z) < 2 * dz ||
                                   std::abs(x - ref.span_x) < 2 * dx ||
                                   std::abs(z - ref.span_z) < 2 * dz;
            if (near_edge) continue;
            if (!inside) {
                ++c.outside;
                if (got[0] == 0 && got[1] == 0 && got[2] == 0 && got[3] == 255) ++c.outside_black;
                continue;
            }
            ++c.pixels;
            const f32 h = ref.height(x, z);
            for (f32 off : {0.0f, -1e-4f, 1e-4f}) {
                const auto want = ref.rgba(Reference::index(std::clamp(h + off, 0.0f, 1.0f)));
                if (std::equal(want.begin(), want.end(), got)) {
                    ++c.matched;
                    break;
                }
            }
        }
    }
    return c;
}

struct Edges {
    size_t pixels = 0, exact = 0, within_one = 0, alpha_zero = 0, lined = 0;
    size_t open_water = 0, open_water_unlined = 0;
};

/// Stage 1, every pixel, from the GPU's own Stage 0 (8-bit, as Moho's
/// target is): the output may round a unit apart.
Edges compare_stage1(const Images& img) {
    Edges e;
    const auto at = [&](i32 x, i32 y) {
        x = std::clamp(x, 0, static_cast<i32>(img.width) - 1);
        y = std::clamp(y, 0, static_cast<i32>(img.height) - 1);
        return &img.frame[(static_cast<size_t>(y) * img.width + static_cast<size_t>(x)) * 4];
    };
    constexpr std::array<std::array<i32, 2>, 8> kOrder = {
        {{-1, -1}, {-1, 0}, {-1, 1}, {0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}}};
    for (u32 py = 0; py < img.height; ++py) {
        for (u32 px = 0; px < img.width; ++px) {
            const auto x = static_cast<i32>(px);
            const auto y = static_cast<i32>(py);
            const u8* c = at(x, y);
            f32 sum = 0.0f;
            bool any_land = false;
            for (const auto& o : kOrder) {
                const u8 a = at(x + o[0], y + o[1])[3];
                sum += static_cast<f32>(a) / 255.0f;
                any_land = any_land || a != 0;
            }
            const f32 topo =
                std::clamp(16.0f * (static_cast<f32>(c[3]) / 255.0f - 0.125f * sum), 0.0f, 1.0f);
            const u8* got = &img.output[(static_cast<size_t>(py) * img.width + px) * 4];
            bool exact = true;
            bool close = true;
            for (int ch = 0; ch < 3; ++ch) {
                const f32 v = std::clamp(static_cast<f32>(c[ch]) / 255.0f - topo, 0.0f, 1.0f);
                const auto want = static_cast<int>(std::lround(v * 255.0f));
                exact = exact && got[ch] == want;
                close = close && std::abs(got[ch] - want) <= 1;
            }
            ++e.pixels;
            e.exact += exact ? 1 : 0;
            e.within_one += close ? 1 : 0;
            e.alpha_zero += got[3] == 0 ? 1 : 0;
            e.lined += topo > 0.0f ? 1 : 0;
            // Open water: the pixel and its neighbours all under the water
            if (c[3] == 0 && !any_land) {
                ++e.open_water;
                if (got[0] == c[0] && got[1] == c[1] && got[2] == c[2]) ++e.open_water_unlined;
            }
        }
    }
    return e;
}

/// A minimap frame's pixels against the cartographic output drawn for its
/// view: those not under the minimap's later quads (icons, the camera's
/// outline), RGB.
struct Copied {
    size_t pixels = 0, matched = 0;
};

Copied compare_copy(const ImageRGBA8& frame, const renderer::MapArea& rect, const Images& drawn,
                    const std::vector<renderer::UIQuad>& painted) {
    Copied c;
    const auto x0 = static_cast<u32>(rect.x);
    const auto y0 = static_cast<u32>(rect.y);
    for (u32 j = 0; j < drawn.height; ++j) {
        for (u32 i = 0; i < drawn.width; ++i) {
            const u32 x = x0 + i;
            const u32 y = y0 + j;
            if (x >= frame.width || y >= frame.height) continue;
            const f32 cx = static_cast<f32>(x) + 0.5f;
            const f32 cy = static_cast<f32>(y) + 0.5f;
            bool covered = false;
            for (size_t q = 1; q < painted.size() && !covered; ++q) {
                const auto& r = painted[q].inst.rect;
                covered = cx >= r[0] && cx < r[0] + r[2] && cy >= r[1] && cy < r[1] + r[3];
            }
            if (covered) continue;
            ++c.pixels;
            const size_t f = (static_cast<size_t>(y) * frame.width + x) * 4;
            const size_t d = (static_cast<size_t>(j) * drawn.width + i) * 4;
            if (frame.pixels[f] == drawn.output[d] && frame.pixels[f + 1] == drawn.output[d + 1] &&
                frame.pixels[f + 2] == drawn.output[d + 2])
                ++c.matched;
        }
    }
    return c;
}

/// Lay the test's minimap view at (x, y, w, h).
std::string place_view(f32 x, f32 y, f32 w, f32 h) {
    return fmt::format("do local v = rawget(_G, '__osc_carto_view')\n"
                       "v.Left:Set(function() return {} end)\n"
                       "v.Top:Set(function() return {} end)\n"
                       "v.Width:Set(function() return {} end)\n"
                       "v.Height:Set(function() return {} end)\n"
                       "end\n",
                       x, y, w, h);
}

/// The frame's pixels against a capture without overlays, by where each
/// pixel's centre lies from (cx, cz): in a band of its distance, of those
/// whose colour is `want(base)` (each channel within `slack`) and whose
/// alpha is the base's or `alpha`.
struct Band {
    size_t pixels = 0, matched = 0;
};
template <class Want>
Band band(const Images& base, const Images& img, const CartographicRenderer::View& v, f32 cx,
          f32 cz, f32 from, f32 to, Want&& want, int slack, int alpha = -1) {
    Band b;
    for (u32 y = 0; y < v.height; ++y) {
        for (u32 x = 0; x < v.width; ++x) {
            const f32 wx =
                v.x0 + (static_cast<f32>(x) + 0.5f) / static_cast<f32>(v.width) * (v.x1 - v.x0);
            const f32 wz =
                v.z0 + (static_cast<f32>(y) + 0.5f) / static_cast<f32>(v.height) * (v.z1 - v.z0);
            const f32 d = std::hypot(wx - cx, wz - cz);
            if (d < from || d >= to) continue;
            const size_t i = (static_cast<size_t>(y) * v.width + x) * 4;
            const std::array<int, 3> expect =
                want(base.frame[i], base.frame[i + 1], base.frame[i + 2]);
            bool ok = true;
            for (size_t c = 0; c < 3; ++c)
                ok = ok && std::abs(static_cast<int>(img.frame[i + c]) - expect[c]) <= slack;
            const int a = img.frame[i + 3];
            ok = ok && (alpha < 0 ? a == base.frame[i + 3] : std::abs(a - alpha) <= 1);
            ++b.pixels;
            b.matched += ok ? 1 : 0;
        }
    }
    return b;
}

/// Moho's Cartographic::Render draws the range rings and then the fog into
/// the frame over the terrain, so the contours run through both: the fog
/// darkens what no vision circle holds to 67% (frame.fx Vision), the rings
/// burn their colour and glow (RangeBurn), each a stencil volume counted
/// against the flat quad's depth.
void check_overlays(Tally& t, renderer::Renderer& r, const map::Terrain& terrain,
                    const CartographicRenderer::View& v) {
    using Input = renderer::Renderer::CartographicOverlayInput;
    using renderer::RangeRing;
    const auto base = r.capture_cartographic(terrain, v);
    if (!base) {
        t.check(false, "overlays: the plain frame is drawn");
        return;
    }
    const f32 cx = (v.x0 + v.x1) * 0.5f;
    const f32 cz = (v.z0 + v.z1) * 0.5f;
    const f32 radius = (v.x1 - v.x0) * 0.15f;
    const f32 pixel = (v.x1 - v.x0) / static_cast<f32>(v.width);
    const auto same = [](u8 r_, u8 g, u8 b) { return std::array<int, 3>{r_, g, b}; };
    // VisionPS(0.33) blended SrcAlpha, InvSrcAlpha over the colour
    const auto fogged = [](u8 r_, u8 g, u8 b) {
        const auto dim = [](u8 c) { return static_cast<int>(std::lround(c * 0.67f)); };
        return std::array<int, 3>{dim(r_), dim(g), dim(b)};
    };
    const f32 all = (v.x1 - v.x0) * 4.0f;

    {
        Input in;
        in.fog = std::vector<RangeRing>{{cx, cz, 0.0f, radius}};
        const auto img = r.capture_cartographic(terrain, v, &in);
        const Band inside =
            img ? band(*base, *img, v, cx, cz, 0.0f, radius - 2.0f * pixel, same, 0) : Band{};
        const Band outside =
            img ? band(*base, *img, v, cx, cz, radius + 2.0f * pixel, all, fogged, 1) : Band{};
        t.check(inside.pixels > 0 && inside.matched == inside.pixels && outside.pixels > 0 &&
                    outside.matched == outside.pixels,
                fmt::format("overlays: in the vision circle {} of {} pixels as before; outside "
                            "it {} of {} fogged to 67%, their alpha as before",
                            inside.matched, inside.pixels, outside.matched, outside.pixels));
    }
    {
        // No circle: the focus army sees nothing, all fogged
        Input in;
        in.fog = std::vector<RangeRing>{};
        const auto img = r.capture_cartographic(terrain, v, &in);
        const Band b = img ? band(*base, *img, v, cx, cz, 0.0f, all, fogged, 1) : Band{};
        t.check(b.pixels > 0 && b.matched == b.pixels,
                fmt::format("overlays: no vision, {} of {} pixels fogged", b.matched, b.pixels));
    }
    {
        // The counts wrap (vision.fx's incr and decr): 255 circles over a
        // point still hold it, 256 count it back to none
        for (const u32 n : {255u, 256u}) {
            Input in;
            in.fog = std::vector<RangeRing>(n, RangeRing{cx, cz, 0.0f, radius});
            const auto img = r.capture_cartographic(terrain, v, &in);
            const bool clear = n == 255;
            const Band b =
                img ? (clear ? band(*base, *img, v, cx, cz, 0.0f, radius - 2.0f * pixel, same, 0)
                             : band(*base, *img, v, cx, cz, 0.0f, radius - 2.0f * pixel, fogged, 1))
                    : Band{};
            t.check(b.pixels > 0 && b.matched == b.pixels,
                    fmt::format("overlays: {} circles on one spot, {} of {} pixels inside {}", n,
                                b.matched, b.pixels, clear ? "clear" : "fogged (wrapped)"));
        }
    }
    {
        // A ring: its outer line 8 pixels thick at the minimap's zoom (ratio
        // 1), the fill inside it untinted (range_Fill off)
        const f32 span = 1024.0f;
        const f32 coeff = r.range_overlays().settings().outer_thickness_coeff;
        const f32 thickness = 8.0f * pixel;
        renderer::RangeBatch batch;
        batch.color = {0.0f, 1.0f, 0.0f, 0.5f};
        batch.outer = {0.0f, thickness / (coeff * span)};
        batch.rings = {RangeRing{cx, cz, 0.0f, radius}};
        Input in;
        in.rings = {batch};
        in.ring_span = span;
        const auto img = r.capture_cartographic(terrain, v, &in);
        const auto green = [](u8, u8, u8) { return std::array<int, 3>{0, 255, 0}; };
        const Band line = img ? band(*base, *img, v, cx, cz, radius - thickness + 2.0f * pixel,
                                     radius - 2.0f * pixel, green, 0, 128)
                              : Band{};
        const Band fill = img ? band(*base, *img, v, cx, cz, 2.0f * pixel,
                                     radius - thickness - 2.0f * pixel, same, 0)
                              : Band{};
        const Band beyond =
            img ? band(*base, *img, v, cx, cz, radius + 2.0f * pixel, all, same, 0) : Band{};
        t.check(line.pixels > 0 && line.matched == line.pixels,
                fmt::format("overlays: the ring's line, {} of {} pixels its colour and glow",
                            line.matched, line.pixels));
        t.check(fill.pixels > 0 && fill.matched == fill.pixels && beyond.pixels > 0 &&
                    beyond.matched == beyond.pixels,
                fmt::format("overlays: inside the ring {} of {} pixels as before, beyond it {} "
                            "of {}",
                            fill.matched, fill.pixels, beyond.matched, beyond.pixels));
        // The rings first, then the fog over them: a line no vision holds
        // is fogged with the ground
        in.fog = std::vector<RangeRing>{};
        const auto dim = r.capture_cartographic(terrain, v, &in);
        const auto dim_green = [](u8, u8, u8) {
            return std::array<int, 3>{0, static_cast<int>(std::lround(255 * 0.67f)), 0};
        };
        const Band dimmed = dim ? band(*base, *dim, v, cx, cz, radius - thickness + 2.0f * pixel,
                                       radius - 2.0f * pixel, dim_green, 1, 128)
                                : Band{};
        t.check(dimmed.pixels > 0 && dimmed.matched == dimmed.pixels,
                fmt::format("overlays: fogged, the ring's line is dimmed with the ground ({} of "
                            "{} pixels)",
                            dimmed.matched, dimmed.pixels));
    }
}

} // namespace

void test_cartographic(TestContext& ctx) {
    spdlog::info("=== Cartographic frame test ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    if (!terrain) {
        t.check(false, "a map is loaded");
        return;
    }
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.init_ui_caches(&ctx.vfs); // the texture cache, which a game has up by now
    const u32 errors_before = renderer::Renderer::validation_error_count();
    const Reference ref(*terrain);
    const f32 w = ref.span_x;
    const f32 h = ref.span_z;

    // The map fills a square frame; then a wide one letterboxes it, black
    // past its sides, as the minimap's window may
    const f32 wide = h * 640.0f / 384.0f;
    const std::array<CartographicRenderer::View, 2> views = {{
        {512, 512, 0.0f, 0.0f, w, h},
        {640, 384, w * 0.5f - wide * 0.5f, 0.0f, w * 0.5f + wide * 0.5f, h},
    }};
    for (const CartographicRenderer::View& v : views) {
        const std::string name = fmt::format("{}x{}", v.width, v.height);
        const auto img = r.capture_cartographic(*terrain, v);
        if (!img || img->frame.size() != static_cast<size_t>(v.width) * v.height * 4) {
            t.check(false, name + ": the frame is drawn and read back");
            continue;
        }
        // OSC_CARTOGRAPHIC_TEST_PNG=<prefix>: both stages as PNGs, to look at
        if (const char* dump = std::getenv("OSC_CARTOGRAPHIC_TEST_PNG")) {
            for (const auto& [stage, bytes] :
                 {std::pair{"frame", &img->frame}, std::pair{"output", &img->output}}) {
                ImageRGBA8 png;
                png.width = img->width;
                png.height = img->height;
                png.pixels = *bytes;
                for (size_t i = 3; i < png.pixels.size(); i += 4) png.pixels[i] = 255;
                write_png(fmt::format("{}{}-{}.png", dump, name, stage), png);
            }
        }
        const Compared s0 = compare_stage0(ref, *img, v);
        t.check(s0.pixels > 0 && s0.matched * 1000 >= s0.pixels * 999,
                fmt::format("{}: Stage 0, {} of {} pixels the ramps' texels at their elevation",
                            name, s0.matched, s0.pixels));
        if (s0.outside > 0)
            t.check(s0.outside_black == s0.outside,
                    fmt::format("{}: past the map, {} of {} pixels the clear's opaque black", name,
                                s0.outside_black, s0.outside));
        const Edges e = compare_stage1(*img);
        t.check(e.within_one == e.pixels && e.exact * 100 >= e.pixels * 99,
                fmt::format("{}: Stage 1, {} of {} pixels exact, {} within a unit", name, e.exact,
                            e.pixels, e.within_one));
        t.check(e.alpha_zero == e.pixels,
                fmt::format("{}: Stage 1 writes alpha 0 ({} of {})", name, e.alpha_zero, e.pixels));
        t.check(e.lined > e.pixels / 200 && e.open_water > 0 &&
                    e.open_water_unlined == e.open_water,
                fmt::format("{}: {} pixels lined; open water ({} pixels) unlined in {}", name,
                            e.lined, e.open_water, e.open_water_unlined));
    }
    // A map flooded over its highest ground, and one whose water lies under
    // its lowest: the quad, at the surface, is drawn all the same
    for (const auto& [label, surface] :
         {std::pair{"flooded", 50.0f}, std::pair{"sunken", -10.0f}}) {
        std::vector<u16> heights;
        for (u32 z = 0; z <= 64; ++z)
            for (u32 x = 0; x <= 64; ++x) heights.push_back(static_cast<u16>((x + z) * 8));
        map::Terrain odd(map::Heightmap(64, 64, 1.0f / 128.0f, std::move(heights)), surface, true);
        odd.set_water_deep_elevation(surface - 5.0f);
        const Reference odd_ref(odd);
        const CartographicRenderer::View v{128, 128, 0.0f, 0.0f, 64.0f, 64.0f};
        const auto img = r.capture_cartographic(odd, v);
        const Compared c = img ? compare_stage0(odd_ref, *img, v) : Compared{};
        t.check(c.pixels > 0 && c.matched == c.pixels,
                fmt::format("a {} map (surface {}, ground 0 to 8): Stage 0, {} of {} pixels", label,
                            surface, c.matched, c.pixels));
    }

    check_overlays(t, r, *terrain, views[0]);

    t.check(renderer::Renderer::validation_error_count() == errors_before,
            "no Vulkan validation errors drawing it");
    spdlog::info("Cartographic frame test: {} passed, {} failed", t.pass, t.fail);
}

void test_cartographic_minimap(TestContext& ctx) {
    spdlog::info("=== Cartographic minimap test ===");
    Tally t;
    const map::Terrain* terrain = ctx.sim.terrain();
    lua_State* L = ctx.L;
    lua_pushstring(L, "osc_ui_registry");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* registry = static_cast<ui::UIControlRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!terrain || !registry) {
        t.check(false, "a map is loaded and the UI's controls are up");
        return;
    }
    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.init_ui_caches(&ctx.vfs);
    const u32 errors_before = renderer::Renderer::validation_error_count();
    const auto map_w = static_cast<f32>(terrain->map_width());
    const auto map_h = static_cast<f32>(terrain->map_height());

    // A minimap view as retail's minimap.lua makes one, its rect off the
    // pixel grid; FA's game interface is up, so the engine's HUD is not
    const auto ran = ctx.lua_state.do_string(
        "do local v = {}\n"
        "setmetatable(v, {__index = moho.UIWorldView})\n"
        "moho.UIWorldView.__init(v, GetFrame(0), 'MiniMap', 2, true, 'WorldCamera')\n"
        "v.Depth:Set(function() return 50 end)\n"
        "moho.UIWorldView.SetCartographic(v, true)\n"
        "rawset(_G, '__osc_carto_view', v)\n"
        "end\n" +
        place_view(40.4f, 30.6f, 241.3f, 180.8f));
    t.check(static_cast<bool>(ran), "a cartographic minimap view is made");
    lua_pushstring(L, core::kWorldUiActiveKey);
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    (void)shots.shoot_frame(*terrain, map_w * 0.5f, map_h * 0.5f, 200.0f); // the scene
    shots.set_ui(registry);

    const auto is_frame = [&](VkDescriptorSet ds) {
        return ds &&
               (ds == r.cartographic().ui_descriptor(0) || ds == r.cartographic().ui_descriptor(1));
    };
    // Two frames, one in each slot; each view's pixels against the frame
    // drawn for that view
    const auto check_view = [&](f32 x, f32 y, f32 w, f32 h, const std::string& name) {
        const auto m = renderer::cartographic_minimap(x, y, w, h, map_w, map_h);
        if (!m) {
            t.check(false, name + ": the view has a frame");
            return;
        }
        std::array<ImageRGBA8, 2> frames;
        std::array<std::vector<renderer::UIQuad>, 2> painted;
        for (size_t f = 0; f < frames.size(); ++f) {
            frames[f] = shots.grab();
            painted[f] = r.painted_minimap();
        }
        const auto drawn = r.capture_cartographic(*terrain, m->frame);
        for (size_t f = 0; f < frames.size(); ++f) {
            const std::string at = fmt::format("{} (frame {})", name, f + 1);
            const auto& ps = painted[f];
            const bool first_is_map =
                !ps.empty() && is_frame(ps[0].texture_ds) && ps[0].inst.rect[0] == m->rect.x &&
                ps[0].inst.rect[1] == m->rect.y && ps[0].inst.rect[2] == m->rect.w &&
                ps[0].inst.rect[3] == m->rect.h;
            t.check(first_is_map,
                    fmt::format("{}: the view's first quad is the cartographic frame over its "
                                "rect snapped to {} {} {}x{}",
                                at, m->rect.x, m->rect.y, m->rect.w, m->rect.h));
            const bool outlined = std::any_of(ps.begin(), ps.end(), [](const renderer::UIQuad& q) {
                return q.inst.color[0] == 1.0f && q.inst.color[1] == 1.0f &&
                       q.inst.color[2] == 0.0f;
            });
            t.check(outlined, fmt::format("{}: the camera's outline is drawn over it ({} quads)",
                                          at, ps.size()));
            if (!drawn || frames[f].pixels.empty()) {
                t.check(false, at + ": the frames are captured");
                continue;
            }
            const Copied c = compare_copy(frames[f], m->rect, *drawn, ps);
            t.check(c.pixels > 0 && c.matched == c.pixels,
                    fmt::format("{}: {} of {} pixels the frame's, copied pixel for pixel", at,
                                c.matched, c.pixels));
        }
        t.check(r.cartographic().width(0) == m->frame.width &&
                    r.cartographic().width(1) == m->frame.width &&
                    r.cartographic().height(0) == m->frame.height &&
                    r.cartographic().height(1) == m->frame.height,
                fmt::format("{}: both frames' targets are {}x{}", name, m->frame.width,
                            m->frame.height));
    };
    check_view(40.4f, 30.6f, 241.3f, 180.8f, "a wide view");

    // A click at the view's middle is the map's middle
    {
        const auto m = renderer::cartographic_minimap(40.4f, 30.6f, 241.3f, 180.8f, map_w, map_h);
        f32 wx = -1.0f;
        f32 wz = -1.0f;
        const bool hit =
            m && r.minimap().hit_test(m->rect.x + m->rect.w * 0.5f, m->rect.y + m->rect.h * 0.5f,
                                      r.width(), r.height(), map_w, map_h, wx, wz);
        t.check(hit && std::abs(wx - map_w * 0.5f) < 2.0f && std::abs(wz - map_h * 0.5f) < 2.0f,
                fmt::format("a click at the view's middle is the map's ({:.1f}, {:.1f})", wx, wz));
    }

    // Resized (as the minimap window is), the targets are made again
    (void)ctx.lua_state.do_string(place_view(100.0f, 20.2f, 150.6f, 260.0f));
    check_view(100.0f, 20.2f, 150.6f, 260.0f, "a tall view, after a resize");

    // Without the flag, the terrain texture over the fitted map
    (void)ctx.lua_state.do_string(
        "moho.UIWorldView.SetCartographic(rawget(_G, '__osc_carto_view'), false)\n");
    (void)shots.grab();
    {
        const auto& ps = r.painted_minimap();
        const renderer::MapArea area =
            renderer::fit_map_area(100.0f, 20.2f, 150.6f, 260.0f, map_w, map_h);
        t.check(!ps.empty() && ps[0].texture_ds && !is_frame(ps[0].texture_ds) &&
                    ps[0].inst.rect[0] == area.x && ps[0].inst.rect[1] == area.y &&
                    ps[0].inst.rect[2] == area.w && ps[0].inst.rect[3] == area.h,
                "SetCartographic(false): the terrain texture over the map, as before");
    }

    // FAF's patched game's ui_DepositIconScale (1 as retail) scales the
    // deposits' icons on the map too: the minimap is a world view, and
    // RenderResources draws its icons as the world's
    {
        const auto deposit_icons = [&](f32 scale) {
            int n = 0;
            for (const char* path :
                 {renderer::kMassIconTexture, renderer::kHydrocarbonIconTexture}) {
                const renderer::GPUTexture* tex = r.texture_cache().get(path);
                if (!tex || tex->width == 0) {
                    continue;
                }
                const f32 w = static_cast<f32>(tex->width >> 2) * 2.0f * scale;
                for (const renderer::UIQuad& q : r.painted_minimap()) {
                    n += q.texture_ds == tex->descriptor_set && q.inst.rect[2] == w ? 1 : 0;
                }
            }
            return n;
        };
        const int plain = deposit_icons(1.0f);
        r.set_deposit_icon_scale(2.0f);
        (void)shots.grab();
        const int scaled = deposit_icons(2.0f);
        r.set_deposit_icon_scale(1.0f);
        t.check(plain > 0 && scaled == plain,
                fmt::format("ui_DepositIconScale 2 doubles the map's {} deposit icons ({} so)",
                            plain, scaled));
    }

    // In a game, the minimap's frame takes the range rings and the fog as
    // Moho's Cartographic::Render does: the rings at the minimap camera's
    // zoom (its largest), the fog from the focus army's units' vision
    {
        (void)ctx.lua_state.do_string(
            "moho.UIWorldView.SetCartographic(rawget(_G, '__osc_carto_view'), true)\n");
        renderer::RangeProfile profile; // retail's DirectFire (rangeoverlayparams.lua)
        profile.name = "DirectFire";
        profile.categories = sim::CategoryExpr::all();
        profile.selected = renderer::range_color(0x08ff5253u);
        profile.inner = {0.02f, 2.0f};
        profile.outer = {0.04f, 4.0f};
        r.range_overlays().set_profile(profile);
        r.range_overlays().settings().render_selected = true;
        r.set_fog_enabled(true);
        r.set_player_army(0);
        // The army's units with vision, and one of its with a weapon picked
        size_t seeing = 0;
        u32 armed = 0;
        ctx.sim.entity_registry().for_each_unit([&](sim::Entity& e) {
            const auto& u = static_cast<const sim::Unit&>(e);
            if (u.army() != 0 || u.is_being_built() || u.is_dying()) return;
            const auto it = u.intel_states().find("Vision");
            if (it != u.intel_states().end() && std::floor(it->second.radius) > 0.0f) ++seeing;
            if (armed == 0 && !u.weapons().empty()) armed = u.entity_id();
        });
        const std::unordered_set<u32> selected{armed};
        ctx.sim.tick(); // the army's sight, which a tick paints
        shots.recapture();
        (void)shots.grab(&selected);
        const auto& vision = r.minimap_vision();
        t.check(seeing > 0 && vision.drawn() && vision.circles().size() == seeing,
                fmt::format("in a game the minimap's fog has the army's {} vision circles ({})",
                            seeing, vision.circles().size()));
        const auto& rings = r.minimap_ranges().drawn();
        const f32 span = std::max(map_w, map_h);
        const f32 thick = renderer::ring_thickness(
            profile.outer, r.range_overlays().settings().outer_thickness_coeff, span, 1.0f);
        t.check(armed != 0 && rings.size() == 1 &&
                    std::abs(rings[0].outer_thickness - thick) < 1e-4f,
                fmt::format("the selected unit's ring in the minimap's frame, its line {:.3f} "
                            "thick at the minimap's full zoom ({:.3f} in the world view)",
                            rings.empty() ? 0.0f : rings[0].outer_thickness,
                            r.range_renderer().drawn().empty()
                                ? 0.0f
                                : r.range_renderer().drawn()[0].outer_thickness));
        r.set_fog_enabled(false);
        r.set_player_army(-1);
        (void)shots.grab();
        t.check(!r.minimap_vision().drawn(), "an observer's minimap has no fog");
        r.range_overlays().settings().render_selected = false;
    }

    shots.set_ui(nullptr);
    lua_pushstring(L, core::kWorldUiActiveKey);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    t.check(renderer::Renderer::validation_error_count() == errors_before,
            "no Vulkan validation errors drawing it");
    spdlog::info("Cartographic minimap test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
