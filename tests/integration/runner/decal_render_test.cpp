// --decal-render-test (M212b): the map's decals, projected and lit.
//
// A decal draws the terrain's own triangles under its footprint, placed by
// its corner and projected by its texture matrix (CWldTerrainDecal), lit by
// the terrain's formula (terrain.fx's DecalsPS / DecalAlbedoXP), its alpha
// the albedo's times retail's decal mask, faded by the camera's LOD metric
// (GetLODAlpha). Ground and decals of the test's own: frames with and
// without the decals, or under the map's light and a white fill (light 1:
// the albedo alone), at the same view.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "plate_fixtures.hpp"
#include "render_probe.hpp"

#include "core/image.hpp"
#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/camera.hpp"
#include "renderer/renderer.hpp"
#include "sim/sim_state.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr const char* kRoot = "/osc_decal_test";
constexpr u32 kSize = 64;
constexpr f32 kDistance = 40.0f;

/// The pixel of `img` at world point `p`, 0-1 a channel.
Rgb pixel_at(renderer::Renderer& r, const ImageRGBA8& img, const sim::Vector3& p) {
    const auto s = screen_of(r, p);
    if (!s || img.width == 0) return {0, 0, 0};
    const u32 x = static_cast<u32>(std::clamp((*s)[0], 0.0f, static_cast<f32>(img.width - 1)));
    const u32 y = static_cast<u32>(std::clamp((*s)[1], 0.0f, static_cast<f32>(img.height - 1)));
    const u8* q = &img.pixels[(static_cast<size_t>(y) * img.width + x) * 4];
    return {q[0] / 255.0f, q[1] / 255.0f, q[2] / 255.0f};
}

f32 redness(const Rgb& c) {
    return c[0] - std::max(c[1], c[2]);
}

f32 apart(const Rgb& a, const Rgb& b) {
    return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2])});
}

/// FA's light on flat ground (N = up), without specular:
/// L = sun * N.S + ambience; L = multiplier * L + fill * (1 - L).
Rgb flat_ground_light(const map::ScmapLighting& l) {
    const f32* s = l.sun_direction;
    const f32 up = s[1] / std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    Rgb out{};
    for (size_t c = 0; c < 3; ++c) {
        const f32 light = l.sun_color[c] * up + l.sun_ambience[c];
        out[c] = l.multiplier * light + l.shadow_fill[c] * (1.0f - light);
    }
    return out;
}

map::DecalInfo decal(map::DecalType type, const char* albedo, const char* spec, f32 x, f32 z,
                     f32 sx, f32 sz, f32 turn, f32 cutoff = 1000.0f) {
    map::DecalInfo d;
    d.type = type;
    d.texture_path = fmt::format("{}/{}", kRoot, albedo);
    d.texture2_path = fmt::format("{}/{}", kRoot, spec);
    d.position_x = x;
    d.position_z = z;
    d.scale_x = sx;
    d.scale_y = 1.0f;
    d.scale_z = sz;
    d.rotation_y = turn;
    d.cut_off_lod = cutoff;
    return d;
}

} // namespace

void test_decal_render(TestContext& ctx) {
    spdlog::info("=== DECAL TEST: the map's decals, projected and lit (M212b) ===");
    Tally t;
    const map::ScmapLighting original = ctx.sim.terrain()->lighting();
    const map::ScmapEnvironment env = ctx.sim.terrain()->environment();

    const auto dir = std::filesystem::temp_directory_path() / "osc_decal_test";
    std::filesystem::create_directories(dir);
    const auto flat = [](u8 r_, u8 g_, u8 b_, u8 a_) {
        return [=](int, u32, u32) { return std::array<u8, 4>{r_, g_, b_, a_}; };
    };
    write_dds(dir / "red.dds", 1, flat(255, 0, 0, 255));
    write_dds(dir / "red_half.dds", 1, flat(255, 0, 0, 128));
    write_dds(dir / "grey.dds", 1, flat(128, 128, 128, 255));
    write_dds(dir / "spec_none.dds", 1, flat(0, 0, 0, 0));
    write_dds(dir / "spec_r.dds", 1, flat(255, 0, 0, 0));
    write_dds(dir / "spec_a.dds", 1, flat(0, 0, 0, 255));
    ctx.vfs.mount(kRoot, std::make_unique<vfs::DirectoryMount>(dir));

    // Ground of the test's own, one stratum: flat at 0, or rising along z.
    const auto make_ground = [&](const std::function<f32(u32, u32)>& height, f32 water = 0.0f) {
        constexpr f32 kScale = 1.0f / 128.0f;
        std::vector<u16> heights(static_cast<size_t>(kSize + 1) * (kSize + 1));
        for (u32 z = 0; z <= kSize; ++z)
            for (u32 x = 0; x <= kSize; ++x)
                heights[static_cast<size_t>(z) * (kSize + 1) + x] =
                    static_cast<u16>(height(x, z) / kScale);
        auto ground = std::make_unique<map::Terrain>(
            map::Heightmap(kSize, kSize, kScale, std::move(heights)), water, water > 0.0f);
        if (water > 0.0f) {
            // SCMP_009's water, over ground 10 under it: its ramp nearly
            // opaque there.
            ground->set_water(ctx.sim.terrain()->water(), {}, 0.0f);
        }
        std::vector<map::StratumInfo> strata(10);
        for (auto& st : strata) st.albedo_scale = st.normal_scale = 4.0f;
        strata[0].albedo_path = "/env/evergreen2/layers/eg_gravel005_albedo.dds";
        ground->set_strata(std::move(strata), {}, {});
        return ground;
    };
    auto ground = make_ground([](u32, u32) { return 0.0f; });

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_decals_enabled(true); // the offscreen shots draw none by default
    const auto frame = [&](map::Terrain& g, std::vector<map::DecalInfo> decals,
                           const map::ScmapLighting& lighting, f32 x, f32 z) {
        g.set_decals(std::move(decals));
        g.set_lighting(lighting, env);
        // Twice: the decals' textures load on the first build.
        (void)shots.shoot_frame(g, x, z, kDistance);
        return shots.shoot_frame(g, x, z, kDistance);
    };
    const map::ScmapLighting fill = white_fill();
    constexpr f32 kQuarter = 1.5707964f;
    constexpr f32 kEighth = 0.7853982f;
    using map::DecalType;

    // Test 1: placed by its corner and turned about it. Turned a quarter, a
    // red decal 8 by 4 at (24, 24) runs along +z its 8 and along -x its 4:
    // x in [20, 24], z in [24, 32]. (Centred on its position, it would
    // cover x in [22, 26], z in [20, 28].)
    {
        const ImageRGBA8 img = frame(
            *ground, {decal(DecalType::Albedo, "red.dds", "spec_none.dds", 24, 24, 8, 4, kQuarter)},
            fill, 32, 32);
        const f32 in = redness(pixel_at(r, img, {22, 0, 29}));
        f32 out = 0;
        for (const sim::Vector3& p : {sim::Vector3{25, 0, 29}, sim::Vector3{22, 0, 22},
                                      sim::Vector3{18, 0, 29}, sim::Vector3{22, 0, 34}})
            out = std::max(out, redness(pixel_at(r, img, p)));
        t.check(in > 0.5f && out < 0.05f,
                fmt::format("Test 1: a turned decal colours its footprint {:.2f} red, around it "
                            "at most {:.2f}",
                            in, out));
    }

    // Test 2: lit as the terrain is. A grey decal's light (its frame under
    // SCMP_009's light over under the white fill) is FA's formula for flat
    // ground, as the ground's own beside it.
    {
        const std::vector<map::DecalInfo> grey = {
            decal(DecalType::Albedo, "grey.dds", "spec_none.dds", 32, 32, 8, 8, 0)};
        const ImageRGBA8 lit = frame(*ground, grey, original, 36, 36);
        const ImageRGBA8 plain = frame(*ground, grey, fill, 36, 36);
        const Rgb want = flat_ground_light(original);
        const Rgb a = pixel_at(r, lit, {36, 0, 36});
        const Rgb b = pixel_at(r, plain, {36, 0, 36});
        bool ok = true;
        Rgb got{};
        for (size_t c = 0; c < 3; ++c) {
            got[c] = b[c] > 0 ? a[c] / b[c] : 0;
            ok = ok && std::abs(got[c] - want[c]) < 0.04f;
        }
        t.check(ok, fmt::format("Test 2: a decal's light ({:.3f}, {:.3f}, {:.3f}); FA's formula "
                                "gives ({:.3f}, {:.3f}, {:.3f})",
                                got[0], got[1], got[2], want[0], want[1], want[2]));
    }

    // Test 3: its alpha is the albedo's times the mask's. At albedo alpha a
    // half, a red decal is half over the ground; turned an eighth, its
    // bounds' corners outside the footprint take none (the mask's border,
    // clamped).
    {
        const ImageRGBA8 bare = frame(*ground, {}, fill, 32, 32);
        const ImageRGBA8 img =
            frame(*ground,
                  {decal(DecalType::Albedo, "red_half.dds", "spec_none.dds", 36, 20, 8, 8, 0),
                   decal(DecalType::Albedo, "red.dds", "spec_none.dds", 44, 34, 8, 8, kEighth)},
                  fill, 32, 32);
        if (const char* dump = std::getenv("OSC_DECAL_TEST_PNG")) write_png(dump, img);
        const Rgb g = pixel_at(r, bare, {40, 0, 24});
        const Rgb h = pixel_at(r, img, {40, 0, 24});
        const Rgb want = {0.5f * 1.0f + 0.5f * g[0], 0.5f * g[1], 0.5f * g[2]};
        // The turned one's footprint is a diamond from (44, 34) (clear of
        // the minimap): (46, 0, 35) lies within its bounds' rectangle,
        // outside the diamond.
        const f32 corner = apart(pixel_at(r, img, {46, 0, 35}), pixel_at(r, bare, {46, 0, 35}));
        const f32 inside = redness(pixel_at(r, img, {44, 0, 40}));
        t.check(apart(h, want) < 0.03f && corner < 0.02f && inside > 0.5f,
                fmt::format("Test 3: at alpha a half ({:.2f} {:.2f} {:.2f} for {:.2f} {:.2f} "
                            "{:.2f}); the turned one's bounds outside it {:.3f} off, inside "
                            "{:.2f} red",
                            h[0], h[1], h[2], want[0], want[1], want[2], corner, inside));
    }

    // Test 4: each technique's specular from its own channel. Under a sun
    // the view of the decal glints off, with a full specular colour: an
    // Albedo decal glints by its specular's red, an AlbedoXP one by its
    // alpha, and neither by the other.
    {
        map::ScmapLighting glint = original;
        {
            renderer::Camera cam; // placed as shoot_frame places the renderer's
            cam.init(static_cast<f32>(kSize), static_cast<f32>(kSize));
            cam.set_target(36.0f, 36.0f);
            cam.set_distance(kDistance);
            f32 ex = 0;
            f32 ey = 0;
            f32 ez = 0;
            cam.eye_position(ex, ey, ez);
            const f32 vx = 36.0f - ex;
            const f32 vy = -ey;
            const f32 vz = 36.0f - ez;
            const f32 len = std::sqrt(vx * vx + vy * vy + vz * vz);
            glint.sun_direction[0] = vx / len;
            glint.sun_direction[1] = -vy / len;
            glint.sun_direction[2] = vz / len;
            for (f32& c : glint.specular) c = 1.0f;
        }
        const auto shine = [&](DecalType type, const char* spec) {
            const ImageRGBA8 img =
                frame(*ground, {decal(type, "grey.dds", spec, 32, 32, 8, 8, 0)}, glint, 36, 36);
            const Rgb c = pixel_at(r, img, {36, 0, 36});
            return (c[0] + c[1] + c[2]) / 3.0f;
        };
        const f32 base = shine(DecalType::Albedo, "spec_none.dds");
        const f32 by_red = shine(DecalType::Albedo, "spec_r.dds");
        const f32 by_alpha = shine(DecalType::Albedo, "spec_a.dds");
        const f32 xp_base = shine(DecalType::AlbedoXP, "spec_none.dds");
        const f32 xp_alpha = shine(DecalType::AlbedoXP, "spec_a.dds");
        const f32 xp_red = shine(DecalType::AlbedoXP, "spec_r.dds");
        t.check(by_red - base > 0.1f && std::abs(by_alpha - base) < 0.02f &&
                    xp_alpha - xp_base > 0.1f && std::abs(xp_red - xp_base) < 0.02f,
                fmt::format("Test 4: glinting, an Albedo decal {:.2f} by red, {:.2f} by alpha, "
                            "{:.2f} by neither; an AlbedoXP one {:.2f} by alpha, {:.2f} by red, "
                            "{:.2f} by neither",
                            by_red, by_alpha, base, xp_alpha, xp_red, xp_base));
    }

    // Test 5: the fade (GetLODAlpha). By the camera's metric at its middle
    // (the width the screen spans there), a decal whose cutoff puts the
    // metric at 7/8 of it is half there; within 3/4, whole; past it, gone.
    {
        const ImageRGBA8 bare = frame(*ground, {}, fill, 32, 32);
        (void)frame(*ground, {}, fill, 32, 32);
        const renderer::Camera& cam = r.camera();
        f32 ex = 0;
        f32 ey = 0;
        f32 ez = 0;
        cam.eye_position(ex, ey, ez);
        const auto v = cam.view();
        const sim::Vector3 mid{36, 0, 36};
        const f32 depth = -(v[2] * (mid.x - ex) + v[6] * (mid.y - ey) + v[10] * (mid.z - ez));
        const f32 aspect = static_cast<f32>(bare.width) / static_cast<f32>(bare.height);
        const f32 metric = 2.0f * std::tan(renderer::Camera::kFovY * 0.5f) * aspect * depth;
        const auto shown = [&](f32 cutoff) {
            const ImageRGBA8 img = frame(
                *ground,
                {decal(DecalType::Albedo, "red.dds", "spec_none.dds", 32, 32, 8, 8, 0, cutoff)},
                fill, 32, 32);
            const Rgb c = pixel_at(r, img, mid);
            const Rgb g = pixel_at(r, bare, mid);
            // The share of the way from the ground to the decal's red.
            return (c[0] - g[0]) / std::max(1.0f - g[0], 1e-3f);
        };
        const f32 whole = shown(metric / 0.7f);
        const f32 half = shown(metric / 0.875f);
        const f32 gone = shown(metric / 1.1f);
        t.check(whole > 0.95f && std::abs(half - 0.5f) < 0.06f && gone < 0.02f,
                fmt::format("Test 5: at metric {:.1f}, a decal is {:.2f} there within 3/4 of its "
                            "cutoff, {:.2f} at 7/8, {:.2f} past it",
                            metric, whole, half, gone));
    }

    // Test 6: projected onto the ground. On a slope rising 1 in 4 along z, a
    // red decal colours the surface at both its ends, 16 apart in z.
    {
        auto slope = make_ground([](u32, u32 z) { return 0.25f * static_cast<f32>(z); });
        const ImageRGBA8 img =
            frame(*slope, {decal(DecalType::Albedo, "red.dds", "spec_none.dds", 24, 24, 16, 16, 0)},
                  fill, 32, 32);
        const f32 near_end = redness(pixel_at(r, img, {32, 0.25f * 26.0f, 26}));
        const f32 far_end = redness(pixel_at(r, img, {32, 0.25f * 38.0f, 38}));
        t.check(near_end > 0.5f && far_end > 0.5f,
                fmt::format("Test 6: on a slope, a decal colours the surface at its ends {:.2f} "
                            "and {:.2f} red",
                            near_end, far_end));
    }

    // Test 7: the types drawn apart from the terrain's colour draw nothing
    // here: a Water Mask decal leaves the ground as it was.
    {
        const ImageRGBA8 bare = frame(*ground, {}, fill, 32, 32);
        const ImageRGBA8 img = frame(
            *ground, {decal(DecalType::WaterMask, "red.dds", "spec_none.dds", 28, 28, 8, 8, 0)},
            fill, 32, 32);
        const f32 moved = apart(pixel_at(r, img, {32, 0, 32}), pixel_at(r, bare, {32, 0, 32}));
        t.check(moved < 0.01f,
                fmt::format("Test 7: a Water Mask decal moves the ground {:.3f}", moved));
    }

    // Test 8: under the water, tinted as the ground is (ApplyWaterColor).
    // With ground 10 under the water, whose ramp is nearly opaque there, a
    // red decal looks as the ground beside it does.
    {
        auto sea = make_ground([](u32, u32) { return 0.0f; }, 10.0f);
        const ImageRGBA8 bare = frame(*sea, {}, original, 32, 32);
        const ImageRGBA8 img =
            frame(*sea, {decal(DecalType::Albedo, "red.dds", "spec_none.dds", 28, 28, 8, 8, 0)},
                  original, 32, 32);
        const Rgb a = pixel_at(r, img, {32, 0, 32});
        const Rgb b = pixel_at(r, bare, {32, 0, 32});
        t.check(apart(a, b) < 0.05f,
                fmt::format("Test 8: under 10 of water, a red decal shows ({:.2f} {:.2f} {:.2f}), "
                            "the ground ({:.2f} {:.2f} {:.2f})",
                            a[0], a[1], a[2], b[0], b[1], b[2]));
    }

    // The scene (its colour, and its glow in alpha) as the frame after
    // `frame` draws it, at world point p.
    const auto scene_frame = [&](map::Terrain& g, std::vector<map::DecalInfo> decals) {
        (void)frame(g, std::move(decals), fill, 32, 32);
        renderer::Renderer::SceneImage scene;
        r.request_scene_capture(
            [&](renderer::Renderer::SceneImage image) { scene = std::move(image); });
        (void)shots.grab();
        return scene;
    };
    const auto scene_at = [&](const renderer::Renderer::SceneImage& img, const sim::Vector3& p) {
        const auto s = screen_of(r, p);
        if (!s || img.width == 0) return std::array<f32, 4>{};
        const u32 x = static_cast<u32>(std::clamp((*s)[0], 0.0f, static_cast<f32>(img.width - 1)));
        const u32 y = static_cast<u32>(std::clamp((*s)[1], 0.0f, static_cast<f32>(img.height - 1)));
        const f32* q = &img.rgba[(static_cast<size_t>(y) * img.width + x) * 4];
        return std::array<f32, 4>{q[0], q[1], q[2], q[3]};
    };

    // Test 9: a Glow decal (M212d; TDecalsGlow) adds its albedo's alpha times
    // a quarter of the mask to the frame's glow, One/One into alpha alone,
    // and leaves the colour as it was.
    {
        const sim::Vector3 mid{32, 0, 32};
        const auto bare = scene_frame(*ground, {});
        const auto glow = scene_frame(
            *ground, {decal(DecalType::Glow, "red.dds", "spec_none.dds", 28, 28, 8, 8, 0)});
        const auto a = scene_at(bare, mid);
        const auto b = scene_at(glow, mid);
        const f32 added = b[3] - a[3];
        const f32 moved =
            std::max({std::abs(b[0] - a[0]), std::abs(b[1] - a[1]), std::abs(b[2] - a[2])});
        t.check(std::abs(added - 0.25f) < 0.005f && moved < 0.002f,
                fmt::format("Test 9: a Glow decal adds {:.3f} glow (0.25 wanted), moves the "
                            "colour {:.4f}",
                            added, moved));
    }

    // Test 10: a Glow Mask decal (TDecalGlowMask) draws, lit, where its
    // alpha is at least 0.9, and sets the glow there to 0.01; at half alpha
    // it draws nothing. It draws before the glowing decals: one over it
    // still adds its 0.25.
    {
        const sim::Vector3 mid{32, 0, 32};
        const auto bare = scene_frame(*ground, {});
        const auto masked = scene_frame(
            *ground, {decal(DecalType::GlowMask, "red.dds", "spec_none.dds", 28, 28, 8, 8, 0)});
        const auto half = scene_frame(*ground, {decal(DecalType::GlowMask, "red_half.dds",
                                                      "spec_none.dds", 28, 28, 8, 8, 0)});
        const auto both = scene_frame(
            *ground, {decal(DecalType::Glow, "red.dds", "spec_none.dds", 28, 28, 8, 8, 0),
                      decal(DecalType::GlowMask, "red.dds", "spec_none.dds", 28, 28, 8, 8, 0)});
        const auto m = scene_at(masked, mid);
        const auto h = scene_at(half, mid);
        const auto g = scene_at(bare, mid);
        const auto o = scene_at(both, mid);
        const f32 half_moved = std::max({std::abs(h[0] - g[0]), std::abs(h[1] - g[1]),
                                         std::abs(h[2] - g[2]), std::abs(h[3] - g[3])});
        t.check(m[0] > 0.9f && m[1] < 0.05f && std::abs(m[3] - 0.01f) < 0.002f &&
                    half_moved < 0.002f && std::abs(o[3] - 0.26f) < 0.005f,
                fmt::format("Test 10: a Glow Mask decal draws ({:.2f} {:.2f} {:.2f}) with glow "
                            "{:.3f}; at half alpha it moves the scene {:.4f}; a Glow decal "
                            "over it leaves glow {:.3f}",
                            m[0], m[1], m[2], m[3], half_moved, o[3]));
    }

    spdlog::info("Decal test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
