// --material-test (M211a): meshes shade as FA's NormalMappedPS does.
//
// A UEF T1 land factory (big, still, and reflective) stands on flat ground of the
// test's own, lit by a white shadow fill and no sun: its light is 1, and a
// frame shows FA's material terms alone --
//   albedo (team colour masked in) * (glow + light + 2 * environment *
//   specular.r) + highlight.
// Frames of the same view are compared per pixel.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "lua/lua_state.hpp"
#include "map/heightmap.hpp"
#include "map/scmap_parser.hpp"
#include "map/terrain.hpp"
#include "sim/army_brain.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr u32 kSize = 64;
constexpr f32 kCentre = kSize / 2.0f;

/// No sun, a white fill: every surface's light is 1.
map::ScmapLighting white_fill() {
    map::ScmapLighting l;
    for (int i = 0; i < 3; ++i) {
        l.sun_color[i] = 0.0f;
        l.sun_ambience[i] = 0.0f;
        l.shadow_fill[i] = 1.0f;
    }
    for (f32& v : l.specular) v = 0.0f;
    return l;
}

/// A 4x4 cubemap (uncompressed BGRA DDS), white on face `lit` and black on
/// the other five.
void write_face_cube(const std::filesystem::path& path, int lit) {
    constexpr u32 kEdge = 4;
    std::vector<char> d(128 + 6 * kEdge * kEdge * 4, 0);
    const auto put = [&](size_t offset, u32 v) { std::memcpy(d.data() + offset, &v, 4); };
    std::memcpy(d.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000);
    put(12, kEdge);
    put(16, kEdge);
    put(20, kEdge * 4);
    put(28, 1);
    put(76, 32);
    put(80, 0x40 | 0x1); // RGB, alpha
    put(88, 32);
    put(92, 0x00FF0000);
    put(96, 0x0000FF00);
    put(100, 0x000000FF);
    put(104, 0xFF000000);
    put(108, 0x1000 | 0x8);   // a texture, complex
    put(112, 0x200 | 0xFC00); // a cubemap, all six faces
    for (int face = 0; face < 6; ++face) {
        const char v = face == lit ? static_cast<char>(255) : 0;
        for (u32 i = 0; i < kEdge * kEdge; ++i) {
            const size_t at = 128 + (static_cast<size_t>(face) * kEdge * kEdge + i) * 4;
            d[at] = d[at + 1] = d[at + 2] = v;
            d[at + 3] = static_cast<char>(255);
        }
    }
    std::ofstream(path, std::ios::binary).write(d.data(), static_cast<std::streamsize>(d.size()));
}

} // namespace

void test_material(TestContext& ctx) {
    spdlog::info("=== MATERIAL TEST: FA's mesh material (M211a) ===");
    Tally t;

    // The structure, in the corner of the map the test's ground stands in.
    const auto made = ctx.lua_state.do_string(
        "__osc_test_pgen = CreateUnitHPR('ueb0101', 'ARMY_1', 32, 0, 32, 0, 0, 0)\n");
    if (!made) spdlog::warn("CreateUnitHPR failed: {}", made.error().message);
    ctx.sim.tick();
    const sim::Entity* pgen = nullptr;
    ctx.sim.entity_registry().for_each([&](const sim::Entity& e) {
        if (e.is_unit() && !e.destroyed() && e.blueprint_id() == "ueb0101") pgen = &e;
    });
    if (!pgen) {
        t.check(false, "the land factory");
        return;
    }
    const f32 ground_y = pgen->position().y;

    // Flat ground at the structure's height, of one grey stratum.
    constexpr f32 kScale = 1.0f / 128.0f;
    map::Terrain ground(
        map::Heightmap(kSize, kSize, kScale,
                       std::vector<u16>(static_cast<size_t>(kSize + 1) * (kSize + 1),
                                        static_cast<u16>(ground_y / kScale))),
        0.0f, false);
    {
        std::vector<map::StratumInfo> strata(10);
        for (auto& st : strata) st.albedo_scale = st.normal_scale = 4.0f;
        strata[0].albedo_path = "/env/evergreen2/layers/eg_gravel005_albedo.dds";
        ground.set_strata(std::move(strata), {}, {});
    }

    OffscreenShots shots(ctx);
    if (!shots.ok()) {
        t.check(false, "renderer init (no Vulkan?)");
        return;
    }
    auto* army = ctx.sim.get_army(0);
    const auto shoot = [&](const std::string& cube,
                           const map::ScmapLighting& lighting = white_fill()) {
        map::ScmapEnvironment env;
        env.terrain_shader = "TTerrain";
        env.cubemaps.emplace_back("<default>", cube);
        ground.set_lighting(lighting, std::move(env));
        shots.recapture();
        return shots.shoot(ground, kCentre, kCentre, 30.0f);
    };
    const std::string kBlack = "/textures/environment/no_such_cube.dds"; // the black fallback

    const std::string kCube = "/textures/environment/envcube_evergreen01a.dds";
    if (army) army->set_color(40, 80, 200);
    const Pixels lit = shoot(kCube);
    const Pixels unlit = shoot(kBlack);

    // Test 1: the environment is reflected: 2 * env * specular.r only adds
    // light. The factory's metal brightens under the map's cube; nothing
    // darkens.
    size_t brighter = 0;
    {
        f32 worst = 0.0f;
        for (size_t i = 0; i < lit.size() && i < unlit.size(); ++i) {
            f32 most = 0.0f;
            for (int c = 0; c < 3; ++c) {
                most = std::max(most, lit[i][c] - unlit[i][c]);
                worst = std::min(worst, lit[i][c] - unlit[i][c]);
            }
            if (most > 0.02f) ++brighter;
        }
        t.check(brighter > 500 && worst > -0.02f,
                fmt::format("Test 1: the map's cube brightens {} pixels (and darkens none by "
                            "more than {:.4f})",
                            brighter, -worst));
    }

    // Test 2: the team's colour where the mask is set. Between a red army
    // and a green one, the pixels that change turn red and green, and their
    // blue stays.
    {
        if (army) army->set_color(220, 30, 30);
        const Pixels red = shoot(kCube);
        if (army) army->set_color(30, 220, 30);
        const Pixels green = shoot(kCube);
        size_t changed = 0;
        f32 dr = 0.0f, dg = 0.0f, db = 0.0f;
        for (size_t i = 0; i < red.size() && i < green.size(); ++i) {
            const f32 r = red[i][0] - green[i][0];
            const f32 g = green[i][1] - red[i][1];
            if (std::max(std::abs(r), std::abs(g)) < 0.02f) continue;
            ++changed;
            dr += r;
            dg += g;
            db += std::abs(red[i][2] - green[i][2]);
        }
        // The team's colour covers a part of the factory, not most of it.
        const f32 n = static_cast<f32>(std::max<size_t>(1, changed));
        t.check(changed > 200 && changed < brighter / 2 && dr / n > 0.05f && dg / n > 0.05f &&
                    db / n < 0.02f,
                fmt::format("Test 2: {} pixels change with the army's colour: {:.3f} redder "
                            "for red, {:.3f} greener for green, blue apart by {:.3f}",
                            changed, dr / n, dg / n, db / n));
    }

    // Test 3: glow (2 * specular.b) shines in the dark. With no light, a
    // black cube and no sun (whose direction FA's highlight needs), only the
    // glowing parts show: a T3 power generator's yellow core. (Counting
    // yellow pixels leaves out the overlay's grey marker.)
    {
        const auto pgen_made =
            ctx.lua_state.do_string("CreateUnitHPR('ueb1301', 'ARMY_1', 48, 0, 14, 0, 0, 0)\n");
        if (!pgen_made) spdlog::warn("CreateUnitHPR failed: {}", pgen_made.error().message);
        ctx.sim.tick();
        map::ScmapLighting dark = white_fill();
        for (f32& c : dark.shadow_fill) c = 0.0f;
        for (f32& d : dark.sun_direction) d = 0.0f;
        map::ScmapEnvironment env;
        env.terrain_shader = "TTerrain";
        env.cubemaps.emplace_back("<default>", kBlack);
        ground.set_lighting(dark, std::move(env));
        shots.recapture();
        const Pixels px = shots.shoot(ground, 48.0f, 14.0f, 30.0f);
        size_t glowing = 0;
        for (const auto& p : px)
            if (p[0] > 0.1f && p[0] > p[2] + 0.05f) ++glowing;
        t.check(glowing > 200,
                fmt::format("Test 3: in the dark, {} pixels of the power generator glow", glowing));
    }

    // Test 4: the highlight, (0.6, 0.8, 0.9) * (reflect(S, N) . -V)^2 *
    // specular.g, is added whatever the light: turning the sun's direction
    // over (its colour black) moves it, and nothing else.
    {
        map::ScmapLighting up = white_fill();
        up.sun_direction[0] = 0.0f;
        up.sun_direction[1] = 1.0f;
        up.sun_direction[2] = 0.0f;
        map::ScmapLighting down = up;
        down.sun_direction[1] = -1.0f;
        const Pixels a = shoot(kBlack, up);
        const Pixels b = shoot(kBlack, down);
        size_t lit_up = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
            const f32 d = std::max({b[i][0] - a[i][0], b[i][1] - a[i][1], b[i][2] - a[i][2]});
            if (d > 0.02f) ++lit_up;
        }
        t.check(
            lit_up > 200,
            fmt::format("Test 4: the highlight lights {} pixels of the factory's tops", lit_up));
    }

    // Test 5: FA reflects the direction to the eye, reflect(-V, N), not the
    // view ray. Off the walls that face the camera -- most of what it sees of
    // the factory -- that points up, so they show the cube's sky; the view
    // ray's reflection would show its ground. Two cubes of the test's own,
    // each lit on one face only: above (+Y) and below (-Y).
    {
        const auto dir = std::filesystem::temp_directory_path() / "osc_material_test";
        std::filesystem::create_directories(dir);
        write_face_cube(dir / "below.dds", 3); // faces: +X -X +Y -Y +Z -Z
        write_face_cube(dir / "above.dds", 2);
        ctx.vfs.mount("/osc_material_test", std::make_unique<vfs::DirectoryMount>(dir));
        const Pixels below = shoot("/osc_material_test/below.dds");
        const Pixels above = shoot("/osc_material_test/above.dds");
        size_t lit_below = 0;
        size_t lit_above = 0;
        for (size_t i = 0; i < below.size() && i < above.size(); ++i) {
            const f32 d = below[i][1] - above[i][1];
            if (d > 0.05f) ++lit_below;
            if (d < -0.05f) ++lit_above;
        }
        t.check(lit_above > 500 && lit_below < lit_above / 10,
                fmt::format("Test 5: a cube lit above brightens {} pixels more than one lit "
                            "below; the other way, {}",
                            lit_above, lit_below));
    }

    spdlog::info("Material test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
