// --sky-test (M210b): FA's sky dome.
//
// SCMP_009's sky block, read as SkyDome::Load reads it (the props still
// found after it), then drawn:
// - the Atmosphere pass's colour at each pixel of a column, against
//   sky.fx's AtmospherePS where the pixel's ray meets the dome's own
//   triangles;
// - the dome culled from above it (CullMode = CW), where the black clear
//   shows past the map;
// - the decals' albedo, and their glow in alpha alone;
// - the cirrus, drifting with the tick.

#include "integration_tests.hpp"
#include "intel_probe.hpp"
#include "render_probe.hpp"

#include "map/scmap_parser.hpp"
#include "map/terrain.hpp"
#include "renderer/dds_decode.hpp"
#include "renderer/dds_parser.hpp"
#include "renderer/renderer.hpp"
#include "renderer/sky_dome.hpp"
#include "renderer/sky_renderer.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr const char* kScmap = "/maps/SCMP_009/SCMP_009.scmap";
constexpr const char* kLookup = "/textures/environment/horizonLookup.dds";
/// The view north over SCMP_009's edge: its focus, pitch and distance.
constexpr f32 kNorthX = 512;
constexpr f32 kNorthZ = 4;
constexpr f32 kNorthPitch = 0.1f;
constexpr f32 kNorthDistance = 150;
/// A colour's allowance: the lookup's point sample may land a texel over at
/// a boundary (a step of its ramp is under 0.005 of the colours' gap).
constexpr f32 kTolerance = 0.01f;

using Scene = renderer::Renderer::SceneImage;

/// Where a ray first meets the dome's triangles: the point, and the azimuth
/// the rasterizer interpolates there.
struct DomeHit {
    std::array<f32, 3> pos{};
    f32 theta = 0;
};

std::optional<DomeHit> hit_dome(const renderer::SkyDomeMesh& dome, const f32 o[3], const f32 d[3]) {
    std::optional<DomeHit> best;
    f32 nearest = 1e30f;
    for (size_t i = 0; i + 2 < dome.indices.size(); i += 3) {
        const auto& a = dome.vertices[dome.indices[i]];
        const auto& b = dome.vertices[dome.indices[i + 1]];
        const auto& c = dome.vertices[dome.indices[i + 2]];
        // Moller-Trumbore
        const std::array<f32, 3> e1 = {b.pos[0] - a.pos[0], b.pos[1] - a.pos[1],
                                       b.pos[2] - a.pos[2]};
        const std::array<f32, 3> e2 = {c.pos[0] - a.pos[0], c.pos[1] - a.pos[1],
                                       c.pos[2] - a.pos[2]};
        const std::array<f32, 3> p = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                                      d[0] * e2[1] - d[1] * e2[0]};
        const f32 det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (std::abs(det) < 1e-9f) continue;
        const f32 inv = 1.0f / det;
        const std::array<f32, 3> s = {o[0] - a.pos[0], o[1] - a.pos[1], o[2] - a.pos[2]};
        const f32 u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
        if (u < 0 || u > 1) continue;
        const std::array<f32, 3> q = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2],
                                      s[0] * e1[1] - s[1] * e1[0]};
        const f32 v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
        if (v < 0 || u + v > 1) continue;
        const f32 t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
        if (t <= 0 || t >= nearest) continue;
        nearest = t;
        best = DomeHit{{o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t},
                       a.theta * (1 - u - v) + b.theta * u + c.theta * v};
    }
    return best;
}

/// Whether a ray meets the ground (the terrain, or the water) over the map,
/// or passes within `margin` of it: its pixel is the world's, not the sky's.
bool meets_ground(const map::Terrain& terrain, const f32 o[3], const f32 d[3], f32 margin = 2.0f) {
    const auto w = static_cast<f32>(terrain.map_width());
    const auto h = static_cast<f32>(terrain.map_height());
    for (int step = 0; step < 8000; ++step) {
        const auto t = static_cast<f32>(step);
        const f32 x = o[0] + d[0] * t;
        const f32 z = o[2] + d[2] * t;
        if (x < 0 || z < 0 || x > w || z > h) continue;
        if (o[1] + d[1] * t < terrain.get_surface_height(x, z) + margin) return true;
    }
    return false;
}

/// retail's horizonLookup (A8), point-sampled and clamped.
struct Lookup {
    std::vector<u8> texels;
    u32 w = 0, h = 0;
    f32 at(f32 u, f32 v) const {
        const auto x = std::clamp(static_cast<i64>(std::floor(u * static_cast<f32>(w))), i64{0},
                                  static_cast<i64>(w) - 1);
        const auto y = std::clamp(static_cast<i64>(std::floor(v * static_cast<f32>(h))), i64{0},
                                  static_cast<i64>(h) - 1);
        return texels[static_cast<size_t>(y) * w + static_cast<size_t>(x)] / 255.0f;
    }
};

/// AtmospherePS at a point of the dome, and the lookup's product t.
std::array<f32, 3> atmosphere(const map::ScmapSky& sky, const Lookup& lookup, const DomeHit& hit,
                              f32* t_out) {
    const f32 th = 0.159155f * std::clamp(hit.theta, 0.0f, 6.283185f);
    const f32 tv = std::clamp((hit.pos[1] - sky.elevation) / sky.horizon_size, 0.0f, 1.0f);
    const f32 t = lookup.at(th, 0.25f) * lookup.at(tv, 0.75f);
    *t_out = t;
    std::array<f32, 3> c{};
    for (int k = 0; k < 3; ++k)
        c[k] = std::clamp(
            sky.horizon_color[k] + (sky.sky_color[k] - sky.horizon_color[k]) * (1 - t), 0.0f, 1.0f);
    return c;
}

/// A texture's RGBA (0-1) at (u, v), filtered as a linear, clamped sampler.
std::array<f32, 4> bilinear(const std::vector<u8>& rgba, u32 w, u32 h, f32 u, f32 v) {
    std::array<f32, 4> out{};
    if (rgba.empty() || w == 0 || h == 0) return out;
    const f32 x = u * static_cast<f32>(w) - 0.5f;
    const f32 y = v * static_cast<f32>(h) - 0.5f;
    const f32 x0 = std::floor(x);
    const f32 y0 = std::floor(y);
    const f32 fx = x - x0;
    const f32 fy = y - y0;
    const auto at = [&](f32 xi, f32 yi, int c) {
        const auto cx = std::clamp(static_cast<i64>(xi), i64{0}, static_cast<i64>(w) - 1);
        const auto cy = std::clamp(static_cast<i64>(yi), i64{0}, static_cast<i64>(h) - 1);
        return rgba[(static_cast<size_t>(cy) * w + static_cast<size_t>(cx)) * 4 +
                    static_cast<size_t>(c)] /
               255.0f;
    };
    for (int c = 0; c < 4; ++c) {
        const f32 top = at(x0, y0, c) * (1 - fx) + at(x0 + 1, y0, c) * fx;
        const f32 bottom = at(x0, y0 + 1, c) * (1 - fx) + at(x0 + 1, y0 + 1, c) * fx;
        out[static_cast<size_t>(c)] = top * (1 - fy) + bottom * fy;
    }
    return out;
}

/// A frame of the empty world (the terrain, the water and the sky) at a
/// tick, as the scene holds it: colour, and glow in alpha.
Scene scene_at(renderer::Renderer& r, lua_State* L, u32 tick, f32 interpolant = 0.0f) {
    sim::WorldSnapshot snap;
    snap.tick = tick;
    sim::WorldEvents events;
    Scene out;
    r.request_scene_capture([&](Scene image) { out = std::move(image); });
    r.render(sim::FrameView(&snap, &snap, interpolant), events, nullptr, L);
    r.poll_events(0.016);
    return out;
}

std::array<f32, 4> texel(const Scene& s, u32 x, u32 y) {
    const size_t i = (static_cast<size_t>(y) * s.width + x) * 4;
    return {s.rgba[i], s.rgba[i + 1], s.rgba[i + 2], s.rgba[i + 3]};
}

/// Aim the camera from (x, z) toward a world point's bearing, nearly level.
void aim(renderer::Renderer& r, f32 x, f32 z, f32 toward_x, f32 toward_z, f32 pitch) {
    // The eye sits behind the focus, away from where it looks.
    r.camera().set_yaw(std::atan2(-(toward_x - x), -(toward_z - z)));
    r.camera().set_pitch(pitch);
}

} // namespace

void test_sky(TestContext& ctx) {
    spdlog::info("=== SKY TEST: FA's sky dome (M210b) ===");
    Tally t;
    const map::Terrain& terrain = *ctx.sim.terrain();
    const map::ScmapSky& sky = terrain.sky();

    // Test 1: SCMP_009's block, as SkyDome::Load reads it (values from the
    // file), and the props still found after it and the cartographic decals.
    {
        const auto near = [](f32 a, f32 b) { return std::abs(a - b) < 1e-3f; };
        bool ok = near(sky.origin[0], 512) && near(sky.origin[1], 0) && near(sky.origin[2], 512) &&
                  near(sky.elevation, -42.5f) && near(sky.radius, 2343.1628f) &&
                  near(sky.start_angle, 1.256637f) && sky.width == 16 && sky.height == 6 &&
                  near(sky.horizon_size, 293.50708f) && near(sky.horizon_color[0], 0.648594f) &&
                  near(sky.horizon_color[2], 0.84f) && near(sky.sky_color[0], 0.22f) &&
                  near(sky.sky_color[2], 0.72f) && near(sky.decal_glow, 0.1f);
        t.check(ok, "the dome and horizon: origin (512, 0, 512), radius 2343.16, 16 x 6, "
                    "horizon 293.51 from -42.5");
        ok = sky.decal_albedo == "/textures/environment/Decal_test_Albedo003.dds" &&
             sky.decal_glow_texture == "/textures/environment/Decal_test_Glow003.dds" &&
             sky.decals.size() == 9;
        if (ok) {
            const map::ScmapSkyDecal& d = sky.decals[0];
            ok = near(d.position[0], 2190.581f) && near(d.position[1], 570.743f) &&
                 near(d.position[2], -1020.003f) && std::abs(d.rotation - -1.585f) < 1e-3f &&
                 near(d.size[0], 183) && near(d.uv[0], 0) && near(d.uv[1], 0.5f) &&
                 near(d.uv[3], 0.5f);
        }
        t.check(ok, "the decals: 9 of Decal_test_Albedo003/Glow003, the first at (2190.58, "
                    "570.74, -1020.00), 183 across, the atlas's lower-left quarter");
        ok = std::all_of(std::begin(sky.cumulus), std::end(sky.cumulus),
                         [](const std::string& s) { return s.empty(); }) &&
             near(sky.cirrus_multiplier, 1.8f) && near(sky.cirrus_color[0], 1.16f) &&
             near(sky.cirrus_color[2], 1.23f) &&
             sky.cirrus_texture == "/textures/environment/cirrus000.dds" &&
             near(sky.cirrus[0].frequency[0], 0.0001f) && near(sky.cirrus[0].speed, 7.8f) &&
             near(sky.cirrus[0].direction[0], 0.43209f) &&
             near(sky.cirrus[3].direction[1], 0.02967f) && near(sky.cirrus[1].speed, 1.28f);
        t.check(ok, "the cirrus: x1.8, (1.16, 1.16, 1.23), cirrus000, its four layers");

        const auto file = ctx.vfs.read_file(kScmap);
        std::optional<map::ScmapData> data;
        if (file) {
            auto parsed = map::parse_scmap(std::vector<u8>(file->begin(), file->end()));
            if (parsed) data = std::move(parsed.value());
        }
        t.check(data && data->props.size() == 5182,
                "the props after the sky and cartographic blocks: " +
                    std::to_string(data ? data->props.size() : 0) + " of 5182");
    }

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    renderer::Renderer& r = shots.renderer();
    r.set_clear_color(renderer::Renderer::kClearColor); // Moho's, as a game clears
    const auto sw = static_cast<f32>(r.width());
    const auto sh = static_cast<f32>(r.height());

    Lookup lookup;
    {
        const auto file = ctx.vfs.read_file(kLookup);
        const auto dds = file ? renderer::parse_dds(*file) : std::nullopt;
        if (dds && dds->alpha_only && !dds->mips.empty()) {
            lookup.w = dds->width;
            lookup.h = dds->height;
            const auto* bytes = reinterpret_cast<const u8*>(dds->mips[0].data);
            lookup.texels.assign(bytes, bytes + static_cast<size_t>(lookup.w) * lookup.h);
        }
        t.check(lookup.w == 128 && lookup.h == 4, "horizonLookup.dds reads as A8, 128 x 4");
        if (lookup.texels.empty()) return;
    }

    // The sky alone: no cirrus (its multiplier 0) and no decals.
    map::Terrain plain = terrain;
    {
        map::ScmapSky bare = sky;
        bare.cirrus_multiplier = 0;
        bare.decals.clear();
        plain.set_sky(bare);
    }
    const renderer::SkyDomeMesh dome = renderer::build_sky_dome(sky);

    // Test 2: the Atmosphere pass, looking north over the map's edge (flat
    // there, 13.5 high) from 15 above it: each sky pixel of the middle column
    // is AtmospherePS where its ray meets the dome: the horizon's colour low,
    // the sky's above the horizon's end.
    struct Column {
        int compared = 0, matched = 0, glowing = 0;
        f32 t_min = 1, t_max = 0;
    };
    const auto column = [&](const map::Terrain& ground) {
        const map::ScmapSky& s = ground.sky();
        const renderer::SkyDomeMesh mesh = renderer::build_sky_dome(s);
        aim(r, kNorthX, kNorthZ, kNorthX, -1000, kNorthPitch);
        (void)shots.shoot(ground, kNorthX, kNorthZ, kNorthDistance, false);
        const Scene scene = scene_at(r, ctx.L, 0);
        Column c;
        std::string worst;
        f32 worst_err = 0;
        for (u32 y = 2; scene.width > 0 && y < scene.height; y += 4) {
            const u32 x = scene.width / 2;
            f32 o[3];
            f32 d[3];
            if (!r.camera().screen_ray(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f, sw,
                                       sh, o, d))
                continue;
            if (meets_ground(ground, o, d)) continue;
            const auto hit = hit_dome(mesh, o, d);
            if (!hit) continue;
            f32 tt = 0;
            const auto want = atmosphere(s, lookup, *hit, &tt);
            const auto got = texel(scene, x, y);
            f32 err = 0;
            for (int k = 0; k < 3; ++k) err = std::max(err, std::abs(got[k] - want[k]));
            ++c.compared;
            if (err <= kTolerance) ++c.matched;
            if (got[3] >= 1e-3f) ++c.glowing;
            c.t_max = std::max(c.t_max, tt);
            c.t_min = std::min(c.t_min, tt);
            if (err > worst_err) {
                worst_err = err;
                worst = "row " + std::to_string(y) + " at height " + std::to_string(hit->pos[1]) +
                        ": (" + std::to_string(got[0]) + ", " + std::to_string(got[1]) + ", " +
                        std::to_string(got[2]) + ") for (" + std::to_string(want[0]) + ", " +
                        std::to_string(want[1]) + ", " + std::to_string(want[2]) + ")";
            }
        }
        spdlog::info("Sky test: radius {:.0f}: {} sky pixels compared, t {:.2f}-{:.2f}; worst {}",
                     s.radius, c.compared, c.t_min, c.t_max, worst.empty() ? "none" : worst);
        return c;
    };
    {
        const Column c = column(plain);
        t.check(c.compared >= 40 && c.t_max > 0.75f && c.t_min < 0.1f,
                "the column sees the sky from the horizon's colour to the sky's: " +
                    std::to_string(c.compared) + " pixels");
        t.check(c.compared > 0 && c.matched == c.compared,
                "each is AtmospherePS where its ray meets the dome (" + std::to_string(c.matched) +
                    " of " + std::to_string(c.compared) + ")");
        t.check(c.compared > 0 && c.glowing == 0,
                "the Atmosphere pass leaves the glow (alpha) at 0");
    }
    // A dome four times as wide, past the camera's far plane (5000): still
    // drawn, as Moho's is never clipped.
    {
        map::Terrain wide = plain;
        map::ScmapSky big = plain.sky();
        big.radius *= 4;
        big.horizon_size *= 4;
        wide.set_sky(big);
        const Column c = column(wide);
        t.check(c.compared >= 40 && c.matched == c.compared,
                "a dome past the far plane draws all the same (" + std::to_string(c.matched) +
                    " of " + std::to_string(c.compared) + ")");
    }

    // Test 3: from above the dome, looking past the map's corner: the dome
    // faces away (CullMode = CW), so the frame there is the black clear.
    {
        aim(r, 0, 0, -1000, -1000, 1.1f);
        (void)shots.shoot(terrain, 0, 0, 1600.0f, false);
        const Scene scene = scene_at(r, ctx.L, 0);
        f32 eye[3];
        r.camera().eye_position(eye[0], eye[1], eye[2]);
        // The decals' footprints on the screen, which the dome's cull
        // doesn't touch
        const auto near_decal = [&](f32 px, f32 py) {
            for (const map::ScmapSkyDecal& dcl : sky.decals) {
                const auto s = screen_of(r, {dcl.position[0], dcl.position[1], dcl.position[2]});
                if (!s) continue;
                const f32 dx = dcl.position[0] - eye[0];
                const f32 dy = dcl.position[1] - eye[1];
                const f32 dz = dcl.position[2] - eye[2];
                const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                const f32 radius = std::max(dcl.size[0], dcl.size[1]) * 1.5f / dist *
                                   (sh * 0.5f / std::tan(renderer::Camera::kFovY * 0.5f));
                if (std::hypot(px - (*s)[0], py - (*s)[1]) < radius + 4) return true;
            }
            return false;
        };
        int over_dome = 0;
        int black = 0;
        for (u32 y = 10; scene.width > 0 && y < scene.height; y += 20)
            for (u32 x = 10; x < scene.width; x += 20) {
                f32 o[3];
                f32 d[3];
                const f32 px = static_cast<f32>(x) + 0.5f;
                const f32 py = static_cast<f32>(y) + 0.5f;
                if (!r.camera().screen_ray(px, py, sw, sh, o, d) || meets_ground(terrain, o, d) ||
                    !hit_dome(dome, o, d) || near_decal(px, py))
                    continue;
                ++over_dome;
                const auto c = texel(scene, x, y);
                if (std::max({c[0], c[1], c[2], c[3]}) < 1e-3f) ++black;
            }
        spdlog::info("Sky test: from above, {} of {} pixels over the dome black", black, over_dome);
        t.check(over_dome >= 20 && black == over_dome,
                "from above, the dome isn't drawn: the clear's black past the map (" +
                    std::to_string(black) + " of " + std::to_string(over_dome) + ")");
    }

    // Test 4: the decals (the cirrus off). Aimed at one whose glow has alpha:
    // its albedo draws over the atmosphere (in colour), its glow into alpha
    // alone; at points across its billboard, each pixel is DecalVS's texel
    // of the atlas over the atmosphere.
    {
        map::Terrain lit = terrain;
        {
            map::ScmapSky decals_only = sky;
            decals_only.cirrus_multiplier = 0;
            lit.set_sky(decals_only);
        }
        std::vector<u8> glow_rgba;
        std::vector<u8> albedo_rgba;
        u32 gw = 0;
        u32 gh = 0;
        const auto decode = [&](const std::string& path, std::vector<u8>& out) {
            const auto file = ctx.vfs.read_file(path);
            const auto dds = file ? renderer::parse_dds(*file) : std::nullopt;
            if (!dds || dds->format != VK_FORMAT_BC3_UNORM_BLOCK || dds->mips.empty()) return;
            gw = dds->width;
            gh = dds->height;
            out = renderer::decode_bc3_to_rgba(reinterpret_cast<const u8*>(dds->mips[0].data), gw,
                                               gh);
        };
        decode(sky.decal_glow_texture, glow_rgba);
        decode(sky.decal_albedo, albedo_rgba);
        // The most alpha in a decal's rectangle of a texture
        const auto most = [&](const std::vector<u8>& rgba, const map::ScmapSkyDecal& d) {
            u8 best = 0;
            if (rgba.empty()) return best;
            // v is flipped (DecalVS): the rectangle's rows count from the bottom
            const auto x0 = static_cast<u32>(d.uv[0] * static_cast<f32>(gw));
            const auto x1 = static_cast<u32>((d.uv[0] + d.uv[2]) * static_cast<f32>(gw));
            const auto y0 = static_cast<u32>((1 - d.uv[1] - d.uv[3]) * static_cast<f32>(gh));
            const auto y1 = static_cast<u32>((1 - d.uv[1]) * static_cast<f32>(gh));
            for (u32 y = y0; y < std::min(y1, gh); ++y)
                for (u32 x = x0; x < std::min(x1, gw); ++x)
                    best = std::max(best, rgba[(static_cast<size_t>(y) * gw + x) * 4 + 3]);
            return best;
        };
        // One low over the map's edge, which fits the frame
        std::optional<size_t> chosen;
        f32 fx = 0;
        f32 fz = 0;
        for (size_t i = 0; i < sky.decals.size() && !chosen; ++i) {
            const map::ScmapSkyDecal& d = sky.decals[i];
            const f32 mw = static_cast<f32>(terrain.map_width());
            const f32 mh = static_cast<f32>(terrain.map_height());
            const f32 x = std::clamp(d.position[0], 16.0f, mw - 16);
            const f32 z = std::clamp(d.position[2], 16.0f, mh - 16);
            const f32 across = std::hypot(d.position[0] - x, d.position[2] - z);
            const f32 up = std::atan2(d.position[1] - terrain.get_surface_height(x, z), across);
            if (up < 0.03f || up > 0.2f || most(glow_rgba, d) < 64 || most(albedo_rgba, d) < 128)
                continue;
            chosen = i;
            fx = x;
            fz = z;
        }
        t.check(chosen.has_value(), "a decal to look at, with glow, low in the sky");
        if (chosen) {
            const map::ScmapSkyDecal& d = sky.decals[*chosen];
            // From well above the ground, as test 2 looks
            aim(r, fx, fz, d.position[0], d.position[2], kNorthPitch);
            (void)shots.shoot(lit, fx, fz, kNorthDistance, false);
            const Scene with = scene_at(r, ctx.L, 0);
            // The billboard's axes, as the renderer hands them to DecalVS
            const std::array<f32, 16> view = r.camera().view();
            const std::array<f32, 3> right = {view[0], view[4], view[8]};
            const std::array<f32, 3> up = {view[1], view[5], view[9]};
            // Points across it, each where the pixel's ray meets its plane
            int sampled = 0;
            int exact = 0;
            f32 opaque = 0; // the most albedo alpha sampled
            std::string worst;
            f32 worst_err = 0;
            for (const std::array<f32, 2> c : {std::array<f32, 2>{0, 0},
                                               {0.5f, 0.5f},
                                               {-0.5f, 0.5f},
                                               {0.5f, -0.5f},
                                               {-0.5f, -0.5f},
                                               {0.8f, 0.1f}}) {
                // The corner's world point, then its pixel
                const f32 cs = std::cos(d.rotation);
                const f32 sn = std::sin(d.rotation);
                const f32 lx = c[0] * d.size[0];
                const f32 ly = c[1] * d.size[1];
                const f32 rx = lx * cs - ly * sn;
                const f32 ry = lx * sn + ly * cs;
                const sim::Vector3 world{d.position[0] + rx * right[0] + ry * up[0],
                                         d.position[1] + rx * right[1] + ry * up[1],
                                         d.position[2] + rx * right[2] + ry * up[2]};
                const auto at = screen_of(r, world);
                if (!at || (*at)[0] < 0 || (*at)[1] < 0 || (*at)[0] >= sw || (*at)[1] >= sh)
                    continue;
                const auto px = static_cast<u32>((*at)[0]);
                const auto py = static_cast<u32>((*at)[1]);
                f32 o[3];
                f32 dir[3];
                if (!r.camera().screen_ray(static_cast<f32>(px) + 0.5f, static_cast<f32>(py) + 0.5f,
                                           sw, sh, o, dir) ||
                    meets_ground(lit, o, dir))
                    continue;
                // The pixel's ray on the billboard's plane, back to its corner
                const std::array<f32, 3> n = {right[1] * up[2] - right[2] * up[1],
                                              right[2] * up[0] - right[0] * up[2],
                                              right[0] * up[1] - right[1] * up[0]};
                const f32 denom = dir[0] * n[0] + dir[1] * n[1] + dir[2] * n[2];
                const f32 along = ((d.position[0] - o[0]) * n[0] + (d.position[1] - o[1]) * n[1] +
                                   (d.position[2] - o[2]) * n[2]) /
                                  denom;
                const std::array<f32, 3> q = {o[0] + dir[0] * along - d.position[0],
                                              o[1] + dir[1] * along - d.position[1],
                                              o[2] + dir[2] * along - d.position[2]};
                const f32 qx = q[0] * right[0] + q[1] * right[1] + q[2] * right[2];
                const f32 qy = q[0] * up[0] + q[1] * up[1] + q[2] * up[2];
                const f32 kx = (qx * cs + qy * sn) / d.size[0];
                const f32 ky = (-qx * sn + qy * cs) / d.size[1];
                const f32 u = d.uv[0] + 0.5f * d.uv[2] * (kx + 1);
                const f32 v = 1 - (d.uv[1] + 0.5f * d.uv[3] * (ky + 1));
                const auto albedo = bilinear(albedo_rgba, gw, gh, u, v);
                const f32 glow_a = bilinear(glow_rgba, gw, gh, u, v)[3];
                // Over the atmosphere, or the clear where the dome isn't
                std::array<f32, 3> under{};
                if (const auto hit = hit_dome(dome, o, dir)) {
                    f32 tt = 0;
                    under = atmosphere(sky, lookup, *hit, &tt);
                }
                const auto got = texel(with, px, py);
                f32 err = std::abs(got[3] - std::clamp(sky.decal_glow * glow_a, 0.0f, 1.0f)) * 3;
                for (int k = 0; k < 3; ++k) {
                    const f32 want = under[k] * (1 - albedo[3]) + albedo[k] * albedo[3];
                    err = std::max(err, std::abs(got[k] - want));
                }
                ++sampled;
                if (err <= 0.03f) ++exact;
                opaque = std::max(opaque, albedo[3]);
                if (err > worst_err) {
                    worst_err = err;
                    worst = "(" + std::to_string(c[0]) + ", " + std::to_string(c[1]) + ") uv (" +
                            std::to_string(u) + ", " + std::to_string(v) + "): (" +
                            std::to_string(got[0]) + ", " + std::to_string(got[1]) + ", " +
                            std::to_string(got[2]) + ", " + std::to_string(got[3]) + ")";
                }
            }
            spdlog::info("Sky test: decal points {} of {} as DecalVS maps the atlas, albedo alpha "
                         "up to {:.2f}; worst {} (off {:.4f})",
                         exact, sampled, opaque, worst.empty() ? "none" : worst, worst_err);
            t.check(sampled >= 4 && exact == sampled,
                    "across the billboard, each pixel is its atlas texel over the atmosphere, and "
                    "its glow the multiplier x the glow's alpha (" +
                        std::to_string(exact) + " of " + std::to_string(sampled) + ")");
            (void)shots.shoot(plain, fx, fz, kNorthDistance, false);
            const Scene without = scene_at(r, ctx.L, 0);
            const auto s = screen_of(r, {d.position[0], d.position[1], d.position[2]});
            f32 eye[3];
            r.camera().eye_position(eye[0], eye[1], eye[2]);
            const f32 dist =
                std::hypot(d.position[0] - eye[0], d.position[1] - eye[1], d.position[2] - eye[2]);
            const f32 radius = std::min(d.size[0], d.size[1]) / dist *
                               (sh * 0.5f / std::tan(renderer::Camera::kFovY * 0.5f)) * 0.7f;
            f32 glow = 0;
            f32 glow_without = 0;
            f32 bright_with = 0;
            f32 bright_without = 0;
            f32 diff = 0;
            int n = 0;
            if (s && with.width > 0 && without.width > 0) {
                for (i32 dy = -static_cast<i32>(radius); dy <= static_cast<i32>(radius); ++dy)
                    for (i32 dx = -static_cast<i32>(radius); dx <= static_cast<i32>(radius); ++dx) {
                        if (std::hypot(static_cast<f32>(dx), static_cast<f32>(dy)) > radius)
                            continue;
                        const i32 px = static_cast<i32>((*s)[0]) + dx;
                        const i32 py = static_cast<i32>((*s)[1]) + dy;
                        if (px < 0 || py < 0 || px >= static_cast<i32>(with.width) ||
                            py >= static_cast<i32>(with.height))
                            continue;
                        // The sky's pixels alone (the terrain glows 0.01)
                        f32 o[3];
                        f32 dir[3];
                        if (!r.camera().screen_ray(static_cast<f32>(px) + 0.5f,
                                                   static_cast<f32>(py) + 0.5f, sw, sh, o, dir) ||
                            meets_ground(terrain, o, dir))
                            continue;
                        const auto a = texel(with, static_cast<u32>(px), static_cast<u32>(py));
                        const auto b = texel(without, static_cast<u32>(px), static_cast<u32>(py));
                        glow = std::max(glow, a[3]);
                        glow_without = std::max(glow_without, b[3]);
                        bright_with += a[0] + a[1] + a[2];
                        bright_without += b[0] + b[1] + b[2];
                        diff +=
                            std::abs(a[0] - b[0]) + std::abs(a[1] - b[1]) + std::abs(a[2] - b[2]);
                        ++n;
                    }
            }
            if (n > 0) {
                bright_with /= static_cast<f32>(n);
                bright_without /= static_cast<f32>(n);
                diff /= static_cast<f32>(n) * 3;
            }
            spdlog::info("Sky test: decal {} over {} pixels: glow {:.4f} (without {:.4f}), "
                         "brightness {:.3f} against {:.3f}, colour apart {:.4f}",
                         *chosen, n, glow, glow_without, bright_with, bright_without, diff);
            t.check(n >= 20, "the decal is on the screen: " + std::to_string(n) + " pixels");
            t.check(diff > 0.001f && opaque > 0.2f,
                    "its albedo draws over the atmosphere (the points sampled include its "
                    "opaque texels)");
            t.check(glow > 0.004f && glow <= sky.decal_glow + 1e-3f && glow_without < 1e-3f,
                    "its glow goes into alpha, at most the multiplier (0.1)");
            t.check(bright_with > 0.5f * bright_without, "its glow pass leaves the colour alone");
        }
    }

    // Test 5: the cirrus, over test 2's view: drawn over the atmosphere, and
    // drifting with the tick.
    {
        aim(r, kNorthX, kNorthZ, kNorthX, -1000, kNorthPitch);
        (void)shots.shoot(plain, kNorthX, kNorthZ, kNorthDistance, false);
        const Scene bare = scene_at(r, ctx.L, 0);
        (void)shots.shoot(terrain, kNorthX, kNorthZ, kNorthDistance, false);
        const Scene now = scene_at(r, ctx.L, 0);
        const Scene later = scene_at(r, ctx.L, 300, 0.5f);
        f32 clouds = 0;
        f32 drift = 0;
        f32 glow = 0;
        int n = 0;
        for (u32 y = 0; now.width > 0 && y < now.height / 3; y += 2)
            for (u32 x = 0; x < now.width; x += 8) {
                const auto a = texel(bare, x, y);
                const auto b = texel(now, x, y);
                const auto c = texel(later, x, y);
                for (int k = 0; k < 3; ++k) {
                    clouds += std::abs(b[k] - a[k]);
                    drift += std::abs(c[k] - b[k]);
                }
                glow = std::max({glow, b[3], c[3]});
                ++n;
            }
        if (n > 0) {
            clouds /= static_cast<f32>(n) * 3;
            drift /= static_cast<f32>(n) * 3;
        }
        spdlog::info("Sky test: cirrus {:.4f} over the bare sky, drift {:.4f} in 300 ticks", clouds,
                     drift);
        t.check(clouds > 0.003f, "the cirrus draws over the atmosphere");
        t.check(drift > 0.003f, "the cirrus drifts with the tick");
        t.check(n > 0 && glow < 1e-3f, "the cirrus writes no glow (alpha)");
    }

    // Test 6: the cirrus's blend, saturated as Moho's 8-bit target stores it.
    // At frequency 0 every layer reads the texture's corner (its wrapped
    // corners' mean, about 0.52, 0.81, 0.017, 0.74: a product near 0.005);
    // times 1000 the alpha is far past 1, so each sky pixel is the colour
    // (2, 0.5, 0.25) saturated, whatever the atmosphere under it.
    {
        map::Terrain veiled = plain;
        map::ScmapSky s = plain.sky();
        s.cirrus_multiplier = 1000;
        s.cirrus_color[0] = 2.0f;
        s.cirrus_color[1] = 0.5f;
        s.cirrus_color[2] = 0.25f;
        for (map::ScmapCirrusLayer& layer : s.cirrus) layer.frequency[0] = layer.frequency[1] = 0;
        veiled.set_sky(s);
        aim(r, kNorthX, kNorthZ, kNorthX, -1000, kNorthPitch);
        (void)shots.shoot(veiled, kNorthX, kNorthZ, kNorthDistance, false);
        const Scene scene = scene_at(r, ctx.L, 0);
        int sky_pixels = 0;
        int veil = 0;
        for (u32 y = 2; scene.width > 0 && y < scene.height; y += 8)
            for (u32 x = 4; x < scene.width; x += 40) {
                f32 o[3];
                f32 d[3];
                if (!r.camera().screen_ray(static_cast<f32>(x) + 0.5f, static_cast<f32>(y) + 0.5f,
                                           sw, sh, o, d) ||
                    meets_ground(veiled, o, d) || !hit_dome(dome, o, d))
                    continue;
                ++sky_pixels;
                const auto c = texel(scene, x, y);
                if (std::abs(c[0] - 1.0f) < 0.01f && std::abs(c[1] - 0.5f) < 0.01f &&
                    std::abs(c[2] - 0.25f) < 0.01f)
                    ++veil;
            }
        t.check(sky_pixels >= 40 && veil == sky_pixels,
                "a cirrus past full: each sky pixel its colour, saturated (" +
                    std::to_string(veil) + " of " + std::to_string(sky_pixels) + ")");
    }

    spdlog::info("Sky test: {} passed, {} failed", t.pass, t.fail);
}

} // namespace osc::test
