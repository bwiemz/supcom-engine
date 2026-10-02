// --water-render-test (M213a): FA's water surface.
//
// The water is one quad over the map at the water's elevation, shaded as
// water2.fx's HighFidelityPS: four scrolling wave normal layers, the frame
// below refracted through them, the sky reflected, by the map's water
// parameters, a Fresnel table (depth × incidence) and the water map (R the
// map's flatness, G the depth to the abyss, B where land rises out of it, A
// its foam). SCMP_009's water lies in its north-west corner.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

#include "core/image.hpp"
#include "map/terrain.hpp"
#include "renderer/renderer.hpp"
#include "renderer/water_renderer.hpp"
#include "sim/sim_state.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <lua.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr const char* kRoot = "/osc_water_test";

/// The pixel of `img` at world point `p`, 0-1 a channel.
std::array<f32, 3> pixel(renderer::Renderer& r, const ImageRGBA8& img, const sim::Vector3& p) {
    const auto s = screen_of(r, p);
    if (!s || img.width == 0) return {0, 0, 0};
    const u32 x = static_cast<u32>(std::clamp((*s)[0], 0.0f, static_cast<f32>(img.width - 1)));
    const u32 y = static_cast<u32>(std::clamp((*s)[1], 0.0f, static_cast<f32>(img.height - 1)));
    const u8* q = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    return {q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f};
}

} // namespace

void test_water_render(TestContext& ctx) {
    spdlog::info("=== WATER TEST: FA's water surface (M213a) ===");
    Tally t;
    const map::Terrain& terrain = *ctx.sim.terrain();
    if (!terrain.has_water()) {
        t.check(false, "SCMP_009 has water");
        return;
    }
    const f32 w = terrain.water_elevation();
    const f32 abyss = terrain.water_abyss_elevation();
    const map::ScmapWater& params = terrain.water();
    spdlog::info("Water test: water at {:.2f}, abyss {:.2f}; waves {} {} {} {}", w, abyss,
                 params.normal_texture[0], params.normal_texture[1], params.normal_texture[2],
                 params.normal_texture[3]);

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();

    // Open water, a shore and dry land, on the half-size water map's lattice.
    const map::Heightmap& hm = terrain.heightmap();
    std::optional<std::array<u32, 2>> deep;
    std::optional<std::array<u32, 2>> shore;
    std::optional<std::array<u32, 2>> land;
    std::optional<std::array<u32, 2>> slope;
    const auto corners = [&](u32 hx, u32 hz) {
        const auto at = [&](u32 x, u32 z) {
            return hm.get_height_at_grid(std::min(x, hm.map_width()), std::min(z, hm.map_height()));
        };
        return std::array<f32, 4>{at(hx * 2, hz * 2), at(hx * 2 + 2, hz * 2),
                                  at(hx * 2, hz * 2 + 2), at(hx * 2 + 2, hz * 2 + 2)};
    };
    for (u32 hz = 8; hz + 8 < hm.map_height() / 2 && !(deep && shore && land); ++hz)
        for (u32 hx = 8; hx + 8 < hm.map_width() / 2; ++hx) {
            const auto c = corners(hx, hz);
            const bool under = std::all_of(c.begin(), c.end(), [&](f32 h) { return h < w - 1; });
            const bool over = std::all_of(c.begin(), c.end(), [&](f32 h) { return h > w + 1; });
            const bool both = std::any_of(c.begin(), c.end(), [&](f32 h) { return h < w; }) &&
                              std::any_of(c.begin(), c.end(), [&](f32 h) { return h > w; });
            if (under && !deep) deep = std::array<u32, 2>{hx, hz};
            if (over && !land && deep) land = std::array<u32, 2>{hx, hz};
            if (both && !shore) shore = std::array<u32, 2>{hx, hz};
        }
    // Under the water on a slope: the corner the depth is read at (h00)
    // stands out from the other three by several of the depth's steps.
    {
        f32 steepest = 0;
        for (u32 hz = 1; hz + 1 < hm.map_height() / 2; ++hz)
            for (u32 hx = 1; hx + 1 < hm.map_width() / 2; ++hx) {
                const auto c = corners(hx, hz);
                if (!std::all_of(c.begin(), c.end(), [&](f32 h) { return h < w && h > abyss; }))
                    continue;
                const f32 apart =
                    std::min({std::abs(c[0] - c[1]), std::abs(c[0] - c[2]), std::abs(c[0] - c[3])});
                if (apart > steepest) {
                    steepest = apart;
                    slope = std::array<u32, 2>{hx, hz};
                }
            }
    }
    const Spot view{deep ? static_cast<f32>((*deep)[0] * 2) + 20 : 48.0f,
                    deep ? static_cast<f32>((*deep)[1] * 2) + 20 : 48.0f};
    (void)shots.shoot(terrain, view.x, view.z, 70.0f);
    const renderer::WaterRenderer* water = nullptr;
    water = &r.water_renderer();

    // Test 1: the water map: open water has depth (to the abyss, read at the
    // texel's first corner) and no land; the shore both; dry land land only.
    {
        const auto texel = [&](const std::array<u32, 2>& at) {
            const size_t i = (static_cast<size_t>(at[1]) * water->water_map_width() + at[0]) * 4;
            return std::array<u8, 4>{water->water_map()[i], water->water_map()[i + 1],
                                     water->water_map()[i + 2], water->water_map()[i + 3]};
        };
        bool ok = deep && shore && land && slope && water->water_map_width() == hm.map_width() / 2;
        std::string seen = "none";
        if (ok) {
            const auto d = texel(*slope);
            const auto s = texel(*shore);
            const auto l = texel(*land);
            const auto c = corners((*slope)[0], (*slope)[1]);
            const auto depth_at = [&](f32 h) { return std::floor((w - h) / (w - abyss) * 255.0f); };
            // Its depth is h00's, and the other corners' would read otherwise.
            const f32 g = static_cast<f32>(d[1]);
            ok = std::abs(g - depth_at(c[0])) <= 1.0f && std::abs(g - depth_at(c[1])) > 1.0f &&
                 std::abs(g - depth_at(c[2])) > 1.0f && std::abs(g - depth_at(c[3])) > 1.0f &&
                 d[2] == 0 && s[2] == 255 && l[1] == 0 && l[2] == 255;
            seen = fmt::format("sloped G{} B{} (G {:.0f}; its other corners {:.0f} {:.0f} {:.0f}), "
                               "shore G{} B{}, land G{} B{}",
                               d[1], d[2], depth_at(c[0]), depth_at(c[1]), depth_at(c[2]),
                               depth_at(c[3]), s[1], s[2], l[1], l[2]);
        }
        t.check(ok, fmt::format("Test 1: the water map: {}", seen));
    }

    // Test 2: the masks in order (foam, flatness, depth bias): R the
    // flatness, mostly full (255) on SCMP_009; A the foam, mostly none.
    {
        size_t flat = 0;
        size_t foamless = 0;
        const size_t texels = water->water_map().size() / 4;
        for (size_t i = 0; i < texels; ++i) {
            flat += water->water_map()[i * 4] == 255 ? 1 : 0;
            foamless += water->water_map()[i * 4 + 3] == 0 ? 1 : 0;
        }
        const map::ScmapWaterMasks& masks = terrain.water_masks();
        const auto share = [texels](size_t n) {
            return static_cast<f32>(n) / std::max<f32>(1, texels);
        };
        t.check(texels > 0 && share(flat) > 0.5f && share(foamless) > 0.5f &&
                    masks.depth_bias.size() == texels,
                fmt::format("Test 2: flatness full in {:.0f}% (R), foam none in {:.0f}% (A)",
                            share(flat) * 100, share(foamless) * 100));
    }

    // Test 3: the Fresnel table: at no incidence all reflection; head-on,
    // the depth times the map's bias; between, the formula.
    {
        const auto& f = water->fresnel_table();
        const auto at = [&](u32 row, u32 col) {
            return f.empty() ? -1 : f[(static_cast<size_t>(row) * 128 + col) * 4];
        };
        const f32 inc = 64.0f / 127.0f;
        const f32 blend = 32.0f / 127.0f * params.fresnel_bias;
        const f32 mid =
            std::clamp(blend + (1 - blend) * std::pow(1 - inc, params.fresnel_power), 0.0f, 1.0f);
        const bool ok = at(0, 50) == 255 &&
                        std::abs(at(127, 127) - std::lround(params.fresnel_bias * 255)) <= 1 &&
                        std::abs(at(64, 32) - std::lround(mid * 255)) <= 1;
        t.check(ok, fmt::format("Test 3: the Fresnel table {} {} {} (255, {}, {})", at(0, 50),
                                at(127, 127), at(64, 32), std::lround(params.fresnel_bias * 255),
                                std::lround(mid * 255)));
    }

    const ImageRGBA8 plain = shots.grab();
    if (const char* dump = std::getenv("OSC_WATER_TEST_PNG")) write_png(dump, plain);

    // Test 4: under the water the terrain is lerped to the map's water ramp
    // by depth (terrain.fx's ApplyWaterColor): deep water reads navy
    // through the surface, where shallow water shows the ground.
    {
        // A water-map texel whose own and neighbours' depth (G) `wanted`
        // accepts, all of them open water; its middle, on the surface. Away
        // from the map's edge, where the water shows beyond the terrain.
        const auto find = [&](const auto& wanted) -> std::optional<sim::Vector3> {
            const std::vector<u8>& map = water->water_map();
            const u32 mw = water->water_map_width();
            for (u32 hz = 16; hz + 16 < water->water_map_height(); ++hz)
                for (u32 hx = 16; hx + 16 < mw; ++hx) {
                    bool ok = true;
                    for (u32 z = hz - 1; z <= hz + 1 && ok; ++z)
                        for (u32 x = hx - 1; x <= hx + 1 && ok; ++x) {
                            const size_t i = (static_cast<size_t>(z) * mw + x) * 4;
                            ok = map[i + 2] == 0 && wanted(map[i + 1]);
                        }
                    if (ok)
                        return sim::Vector3{static_cast<f32>(hx * 2 + 1), w,
                                            static_cast<f32>(hz * 2 + 1)};
                }
            return std::nullopt;
        };
        // The frame's mean colour over nine points a unit apart around `p`.
        const auto mean = [&](const ImageRGBA8& frame, const sim::Vector3& p) {
            std::array<f32, 3> sum{};
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const auto c = pixel(
                        r, frame, {p.x + static_cast<f32>(dx), p.y, p.z + static_cast<f32>(dz)});
                    for (int ch = 0; ch < 3; ++ch) sum[ch] += c[ch] / 9.0f;
                }
            return sum;
        };
        const auto deep_at = find([](u8 g) { return g > 200; });
        const auto shallow_at = find([](u8 g) { return g > 15 && g < 60; });
        std::array<f32, 3> d{};
        std::array<f32, 3> s{};
        if (deep_at && shallow_at) {
            d = mean(shots.shoot_frame(terrain, deep_at->x, deep_at->z, 70.0f), *deep_at);
            s = mean(shots.shoot_frame(terrain, shallow_at->x, shallow_at->z, 70.0f), *shallow_at);
            (void)shots.shoot(terrain, view.x, view.z, 70.0f);
        }
        // Untinted, deep water is a grey near 0.45 a channel.
        const f32 deep_level = (d[0] + d[1] + d[2]) / 3;
        const f32 shallow_level = (s[0] + s[1] + s[2]) / 3;
        t.check(deep_at && shallow_at && d[2] - d[0] > 0.2f && deep_level < 0.3f &&
                    shallow_level - deep_level > 0.15f,
                fmt::format("Test 4: under the water the terrain takes the water ramp: deep "
                            "({:.2f} {:.2f} {:.2f}), shallow ({:.2f} {:.2f} {:.2f})",
                            d[0], d[1], d[2], s[0], s[1], s[2]));
    }

    // Test 5: the frame drawn before the water shows through it, refracted:
    // a red glow hung 1 under the surface over open water.
    {
        const auto dir = std::filesystem::temp_directory_path() / "osc_water_test";
        std::filesystem::create_directories(dir);
        write_dds(dir / "white.dds", 1,
                  [](int, u32, u32) { return std::array<u8, 4>{255, 255, 255, 255}; });
        write_dds(dir / "red.dds", 1,
                  [](int, u32, u32) { return std::array<u8, 4>{255, 0, 0, 255}; });
        std::ofstream(dir / "glow.bp") << fmt::format(
            "EmitterBlueprint {{\n"
            "    Lifetime = -1, Repeattime = 10, LODCutoff = 500, Blendmode = 3, SortOrder = -1,\n"
            "    EmitIfVisible = false, InterpolateEmission = false, SnapToWaterline = false,\n"
            "    Texture = '{0}/white.dds', RampTexture = '{0}/red.dds',\n"
            "    EmitRateCurve = {{ Keys = {{ {{ x = 0, y = 1, z = 0 }} }} }},\n"
            "    LifetimeCurve = {{ Keys = {{ {{ x = 0, y = 50, z = 0 }} }} }},\n"
            "    StartSizeCurve = {{ Keys = {{ {{ x = 0, y = 4, z = 0 }} }} }},\n"
            "    EndSizeCurve = {{ Keys = {{ {{ x = 0, y = 4, z = 0 }} }} }},\n"
            "}}\n",
            kRoot);
        ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));
        (void)r.texture_cache().get_blocking(fmt::format("{}/white.dds", kRoot));
        (void)r.texture_cache().get_blocking(fmt::format("{}/red.dds", kRoot));
        const Spot g{view.x - 20, view.z - 20};
        (void)spawn_unit(ctx, "__osc_wt_g", "uel0105", "ARMY_1", g, 0);
        run_lua(ctx,
                fmt::format("Warp(__osc_wt_g, Vector({}, {}, {}))\n"
                            "__osc_wt_e = CreateEmitterAtEntity(__osc_wt_g, 1, '{}/glow.bp')\n",
                            g.x, w - 1.0f, g.z, kRoot));
        for (int i = 0; i < 3; ++i) {
            ctx.sim.tick();
            shots.recapture();
            shots.redraw();
        }
        const ImageRGBA8 lit = shots.grab();
        // Beside the engineer (which sinks from view), inside the glow.
        const auto v = r.camera().view();
        const sim::Vector3 right{v[0], v[4], v[8]};
        const sim::Vector3 in{g.x + right.x * 2, w, g.z + right.z * 2};
        const f32 red = pixel(r, lit, in)[0] - pixel(r, plain, in)[0];
        t.check(red > 0.1f, fmt::format("Test 5: a red glow under the water shows through it "
                                        "({:+.2f} red)",
                                        red));
    }

    // Test 6: the waves scroll: ten ticks on, the open water has changed;
    // dry land, looked at from above it, hasn't. The glow goes first.
    {
        run_lua(ctx, "__osc_wt_e:Destroy()\n__osc_wt_g:Destroy()\n");
        // Its last particles die (their lifetime 50) before anything is
        // compared.
        for (int i = 0; i < 60; ++i) ctx.sim.tick();
        // The 3x3 samples 4 apart around `at`'s middle, before and after.
        const auto change = [&](const std::array<u32, 2>& at, bool on_water) {
            shots.recapture();
            const ImageRGBA8 before = shots.grab();
            for (int i = 0; i < 10; ++i) {
                ctx.sim.tick();
                shots.recapture();
                shots.redraw();
            }
            const ImageRGBA8 after = shots.grab();
            f32 sum = 0;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const f32 x = static_cast<f32>(at[0] * 2) + 1 + static_cast<f32>(dx) * 4.0f;
                    const f32 z = static_cast<f32>(at[1] * 2) + 1 + static_cast<f32>(dz) * 4.0f;
                    const sim::Vector3 p{x, on_water ? w : terrain.get_terrain_height(x, z), z};
                    const auto a = pixel(r, before, p);
                    const auto b = pixel(r, after, p);
                    sum += std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]);
                }
            return sum;
        };
        const f32 water_change = deep ? change(*deep, true) : 0;
        f32 land_change = 1;
        if (land) {
            (void)shots.shoot(terrain, static_cast<f32>((*land)[0] * 2) + 1,
                              static_cast<f32>((*land)[1] * 2) + 1, 70.0f);
            land_change = change(*land, false);
        }
        t.check(water_change > 0.05f && land_change < 0.02f,
                fmt::format("Test 6: ten ticks on, the water changed {:.3f}, the land {:.3f}",
                            water_change, land_change));
    }

    // Test 7: the surface follows the map's water parameters. The view's
    // open water is drawn again, at the same tick (the same waves), with one
    // parameter changed at a time.
    {
        map::Terrain& wet = *ctx.sim.terrain();
        // Copies: `params` is the terrain's own, which each look replaces.
        const map::ScmapWater original = params;
        const map::ScmapWaterMasks masks = terrain.water_masks();
        // The mean colour and the frame over the open water in the view's
        // middle (clear of the HUD and the minimap).
        struct Look {
            std::array<f32, 3> mean{};
            ImageRGBA8 frame;
        };
        const auto look = [&](const map::ScmapWater& water_params, const map::ScmapWaterMasks& m) {
            wet.set_water(water_params, m, abyss);
            Look out;
            out.frame = shots.shoot_frame(terrain, view.x, view.z, 70.0f);
            const ImageRGBA8& f = out.frame;
            size_t n = 0;
            for (u32 y = f.height * 2 / 5; y < f.height * 19 / 20; ++y)
                for (u32 x = f.width * 7 / 20; x < f.width * 19 / 20; ++x, ++n)
                    for (int ch = 0; ch < 3; ++ch)
                        out.mean[ch] +=
                            f.pixels[(static_cast<size_t>(y) * f.width + x) * 4 + ch] / 255.0f;
            for (f32& c : out.mean) c /= static_cast<f32>(std::max<size_t>(n, 1));
            return out;
        };
        // The mean change a pixel over that region, 0-1 a channel.
        const auto moved = [](const Look& a, const Look& b) {
            f64 sum = 0;
            size_t n = 0;
            const ImageRGBA8& f = a.frame;
            for (u32 y = f.height * 2 / 5; y < f.height * 19 / 20; ++y)
                for (u32 x = f.width * 7 / 20; x < f.width * 19 / 20; ++x, ++n)
                    for (int ch = 0; ch < 3; ++ch) {
                        const size_t i = (static_cast<size_t>(y) * f.width + x) * 4 + ch;
                        sum += std::abs(f.pixels[i] - b.frame.pixels[i]) / 255.0;
                    }
            return static_cast<f32>(sum / static_cast<f64>(std::max<size_t>(n, 1)));
        };
        const Look base = look(original, masks);

        map::ScmapWater red = original;
        red.surface_color[0] = 1.0f;
        red.surface_color[1] = red.surface_color[2] = 0.0f;
        red.color_lerp[0] = red.color_lerp[1] = 1.0f;
        const Look reddened = look(red, masks);

        map::ScmapWater no_sky = original;
        no_sky.sky_reflection = 0.0f;
        const Look skyless = look(no_sky, masks);

        map::ScmapWater no_sun = original;
        no_sun.sun_reflection = 0.0f;
        const Look sunless = look(no_sun, masks);

        map::ScmapWaterMasks foamed = masks;
        std::fill(foamed.foam.begin(), foamed.foam.end(), u8{255});
        const Look crestless = look(original, foamed);

        // Test 9 (M213d): at graphics fidelity 1 (and 0), FA's
        // Water_LowFidelity: a pale blue by the depth, up to 0.3, and the
        // waves' crests, over the frame. Neither the map's water colour nor
        // its sky changes it, and over deep water it is paler than the high
        // fidelity water.
        r.video_options().graphics_fidelity = 1;
        const Look low = look(original, masks);
        const Look low_red = look(red, masks);
        const Look low_skyless = look(no_sky, masks);
        r.video_options().graphics_fidelity = 2;
        {
            const f32 by_colour = moved(low, low_red);
            const f32 by_sky_low = moved(low, low_skyless);
            const f32 apart_fidelity = moved(base, low);
            t.check(by_colour == 0.0f && by_sky_low == 0.0f && apart_fidelity > 0.02f &&
                        low.mean[2] > 0.3f,
                    fmt::format("Test 9: at fidelity 1 the low water: its colour {:.4f} and sky "
                                "{:.4f} a pixel change nothing; {:.3f} a pixel from the high "
                                "water; mean ({:.2f} {:.2f} {:.2f})",
                                by_colour, by_sky_low, apart_fidelity, low.mean[0], low.mean[1],
                                low.mean[2]));
        }

        wet.set_water(original, masks, abyss);
        // A frame drawn again unchanged is the same to the bit, so even the
        // crests' few pixels show.
        const auto level = [](const Look& l) { return l.mean[0] + l.mean[1] + l.mean[2]; };
        const f32 by_sky = moved(base, skyless);
        const f32 by_sun = moved(base, sunless);
        const f32 by_crest = moved(base, crestless);
        t.check(reddened.mean[0] - base.mean[0] > 0.5f && by_sky > 0.05f &&
                    level(skyless) < level(base) && by_sun > 0.005f &&
                    level(sunless) < level(base) && by_crest > 0.0003f &&
                    level(crestless) <= level(base),
                fmt::format("Test 7: the surface follows the map: a red water colour {:+.2f} red; "
                            "without the sky {:.3f}, the sun {:.3f}, the crests {:.4f} a pixel",
                            reddened.mean[0] - base.mean[0], by_sky, by_sun, by_crest));
    }

    // Test 8: the water's albedo decals (M212g). A Water Albedo decal over
    // open water lies on the surface, drawn after it (TDecalsWaterAlbedo);
    // a decal whose texture is numbered ("frame_01.dds") shows its frames in
    // turn, half a frame a tick (CAnimTexture).
    {
        std::optional<Spot> open;
        const auto mw = static_cast<f32>(terrain.map_width());
        const auto mh = static_cast<f32>(terrain.map_height());
        for (int iz = 24; static_cast<f32>(iz + 24) < mh && !open; iz += 4)
            for (int ix = 24; static_cast<f32>(ix + 24) < mw && !open; ix += 4) {
                const auto x = static_cast<f32>(ix);
                const auto z = static_cast<f32>(iz);
                bool under = true;
                for (int dz = -10; dz <= 10 && under; dz += 2)
                    for (int dx = -10; dx <= 10 && under; dx += 2)
                        under = terrain.get_terrain_height(x + static_cast<f32>(dx),
                                                           z + static_cast<f32>(dz)) < w - 1;
                if (under) open = Spot{x, z};
            }
        const auto dir = std::filesystem::temp_directory_path() / "osc_water_decal_test";
        std::filesystem::create_directories(dir);
        const auto flat = [](u8 r_, u8 g_, u8 b_) {
            return [=](int, u32, u32) { return std::array<u8, 4>{r_, g_, b_, 255}; };
        };
        write_dds(dir / "red.dds", 1, flat(255, 0, 0));
        write_dds(dir / "frame_01.dds", 1, flat(255, 0, 0));
        write_dds(dir / "frame_02.dds", 1, flat(0, 255, 0));
        ctx.vfs.mount("/osc_water_decal_test", std::make_unique<vfs::DirectoryMount>(dir));
        r.set_decals_enabled(true);
        const auto forget = [&] {
            for (const auto& fx : ctx.sim.effect_registry().all())
                if (fx && fx->decal()) fx->mark_destroyed();
            ctx.sim.tick();
        };
        const auto decal = [&](const char* texture) {
            run_lua(ctx, fmt::format("CreateDecal({{{}, {}, {}}}, 0, '/osc_water_decal_test/{}', "
                                     "'', 'Water Albedo', 12, 12, 1000, 0, 1)",
                                     open->x, w, open->z, texture));
        };
        // The open water's middle, the sim's decals as they are now
        const auto look = [&] {
            shots.recapture();
            (void)shots.shoot_frame(terrain, open->x, open->z, 40.0f); // textures load
            return middle(shots.shoot_frame(terrain, open->x, open->z, 40.0f));
        };
        if (open) {
            forget();
            const Rgb bare = look();
            decal("red.dds");
            ctx.sim.tick();
            const Rgb red = look();
            forget();
            decal("frame_01.dds");
            ctx.sim.tick();
            const Rgb first = look();
            ctx.sim.tick();
            ctx.sim.tick(); // a frame on
            const Rgb second = look();
            forget();
            const auto green = [](const Rgb& c) { return c[1] - std::max(c[0], c[2]); };
            const auto reds = [](const Rgb& c) { return c[0] - std::max(c[1], c[2]); };
            const bool turned = (reds(first) > 0.3f && green(second) > 0.3f) ||
                                (green(first) > 0.3f && reds(second) > 0.3f);
            t.check(reds(red) - reds(bare) > 0.4f && turned,
                    fmt::format("Test 8: a Water Albedo decal lies on the open water ({} with it, "
                                "{} without); a numbered one turns frames ({} then {})",
                                show(red), show(bare), show(first), show(second)));
        } else {
            t.check(false, "Test 8: open water 20 across");
        }
        r.set_decals_enabled(false);
    }

    spdlog::info("Water test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
