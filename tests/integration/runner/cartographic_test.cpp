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

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "core/image.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/cartographic_math.hpp"
#include "renderer/cartographic_renderer.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

    t.check(renderer::Renderer::validation_error_count() == errors_before,
            "no Vulkan validation errors drawing it");
    spdlog::info("Cartographic frame test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
