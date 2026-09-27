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
#include "renderer/camera.hpp"
#include "renderer/mesh_cache.hpp"
#include "sim/army_brain.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace osc::test {

namespace {

constexpr u32 kSize = 64;
constexpr f32 kCentre = kSize / 2.0f;
/// The test's mirror stands south of the factory.
constexpr f32 kPlateZ = 58.0f;

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

/// A 4x4 uncompressed BGRA DDS of `faces` faces (6: a cubemap, +X -X +Y
/// -Y +Z -Z, as D3D and Vulkan address them alike), each texel's RGBA from
/// `texel(face, column)`.
template <typename Texel>
void write_dds(const std::filesystem::path& path, int faces, Texel texel) {
    constexpr u32 kEdge = 4;
    std::vector<char> d(128 + static_cast<size_t>(faces) * kEdge * kEdge * 4, 0);
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
    put(108, faces == 6 ? 0x1000 | 0x8 : 0x1000); // a texture (complex, a cube's)
    put(112, faces == 6 ? 0x200 | 0xFC00 : 0);    // a cubemap, all six faces
    for (int face = 0; face < faces; ++face) {
        for (u32 i = 0; i < kEdge * kEdge; ++i) {
            const std::array<u8, 4> rgba = texel(face, i % kEdge);
            const size_t at = 128 + (static_cast<size_t>(face) * kEdge * kEdge + i) * 4;
            d[at] = static_cast<char>(rgba[2]);
            d[at + 1] = static_cast<char>(rgba[1]);
            d[at + 2] = static_cast<char>(rgba[0]);
            d[at + 3] = static_cast<char>(rgba[3]);
        }
    }
    std::ofstream(path, std::ios::binary).write(d.data(), static_cast<std::streamsize>(d.size()));
}

/// A cubemap white where `white(face, column)` says, black elsewhere.
template <typename White> void write_cube(const std::filesystem::path& path, White white) {
    write_dds(path, 6, [&](int face, u32 column) {
        const u8 v = white(face, column) ? 255 : 0;
        return std::array<u8, 4>{v, v, v, 255};
    });
}

/// A one-bone SCM mesh: a square `half` units either side of the origin,
/// flat and facing up, wound both ways so that either culling draws it.
void write_plate_scm(const std::filesystem::path& path, f32 half) {
    std::vector<char> d(48, 0);
    const auto put = [&](size_t offset, u32 v) { std::memcpy(d.data() + offset, &v, 4); };
    const auto append = [&](const void* p, size_t n) {
        const char* c = static_cast<const char*>(p);
        d.insert(d.end(), c, c + n);
    };
    const auto f = [&](std::initializer_list<f32> vs) {
        for (const f32 v : vs) append(&v, 4);
    };
    const auto u = [&](std::initializer_list<u32> vs) {
        for (const u32 v : vs) append(&v, 4);
    };
    std::memcpy(d.data(), "MODL", 4);
    put(4, 5); // version
    append("NAME", 4);
    append("root", 5); // and its terminator
    d.resize(60, 0);
    append("SKEL", 4);
    const auto bone_offset = static_cast<u32>(d.size());
    f({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}); // rest pose: identity
    f({0, 0, 0});                                        // position
    f({1, 0, 0, 0});                                     // rotation (w, x, y, z)
    u({52, 0xFFFFFFFFu, 0, 0});                          // name, no parent
    d.resize(188, 0);
    append("VTXL", 4);
    const auto vert_offset = static_cast<u32>(d.size());
    const f32 corners[4][2] = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
    for (const auto& c : corners) {
        f({c[0], 0, c[1]});                                        // position
        f({1, 0, 0});                                              // tangent
        f({0, 1, 0});                                              // normal
        f({0, 0, 1});                                              // binormal
        f({c[0] > 0.0f ? 1.0f : 0.0f, c[1] > 0.0f ? 1.0f : 0.0f}); // uv
        f({0, 0});                                                 // second uv
        u({0});                                                    // bones
    }
    const auto index_offset = static_cast<u32>(d.size());
    const u16 indices[] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    append(indices, sizeof(indices));
    put(8, bone_offset);
    put(12, 1); // bones the vertices use
    put(16, vert_offset);
    put(24, 4);
    put(28, index_offset);
    put(32, static_cast<u32>(std::size(indices)));
    put(44, 1); // bones in all
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

    // Test 3: the highlight, (0.6, 0.8, 0.9) * (reflect(S, N) . -V)^2 *
    // specular.g, is added whatever the light (the sun's colour here is
    // black), and FA doesn't mask it by N . S. A sun overhead lights the
    // factory's roofs; one under the ground, the walls facing the camera, of
    // which it sees more.
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
        size_t lit_down = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
            const f32 d = std::max({a[i][0] - b[i][0], a[i][1] - b[i][1], a[i][2] - b[i][2]});
            if (d > 0.02f) ++lit_up;
            if (d < -0.02f) ++lit_down;
        }
        t.check(lit_up > 200 && lit_down > 5 * lit_up,
                fmt::format("Test 3: a sun overhead highlights {} pixels of the factory; one "
                            "under the ground, {}",
                            lit_up, lit_down));
    }

    // Cubes of the test's own: white on one face only (faces +X -X +Y -Y +Z
    // -Z), on one side of x = 0, or all over. A face's columns run along +x
    // on +Y, -Y and +Z, along -x on -Z.
    const auto dir = std::filesystem::temp_directory_path() / "osc_material_test";
    std::filesystem::create_directories(dir);
    const char* const kFaces[] = {"px", "nx", "py", "ny", "pz", "nz"};
    for (int f = 0; f < 6; ++f)
        write_cube(dir / fmt::format("{}.dds", kFaces[f]),
                   [f](int face, u32 /*column*/) { return face == f; });
    const auto x_side = [](bool positive) {
        return [positive](int face, u32 column) {
            if (face == 0 || face == 1) return (face == 0) == positive;
            const bool plus_x = face == 5 ? column < 2 : column >= 2;
            return plus_x == positive;
        };
    };
    write_cube(dir / "px_half.dds", x_side(true));
    write_cube(dir / "nx_half.dds", x_side(false));
    write_cube(dir / "white.dds", [](int, u32) { return true; });
    // And a mirror: a flat plate 8 units square, white, reflecting fully
    // (SpecTeam red 1, nothing else), its normals flat.
    write_plate_scm(dir / "plate.scm", 4.0f);
    write_dds(dir / "plate_albedo.dds", 1,
              [](int, u32) { return std::array<u8, 4>{255, 255, 255, 255}; });
    write_dds(dir / "plate_specteam.dds", 1,
              [](int, u32) { return std::array<u8, 4>{255, 0, 0, 0}; });
    write_dds(dir / "plate_normals.dds", 1,
              [](int, u32) { return std::array<u8, 4>{0, 128, 0, 128}; });
    ctx.vfs.mount("/osc_material_test", std::make_unique<vfs::DirectoryMount>(dir));
    const auto face_cube = [](const char* face) {
        return fmt::format("/osc_material_test/{}.dds", face);
    };
    // How many pixels are brighter in `a` than in `b`, by their green.
    const auto brighter_in = [](const auto& a, const auto& b) {
        size_t n = 0;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            if (a[i][1] - b[i][1] > 0.05f) ++n;
        return n;
    };

    // Test 4: FA reflects the view ray: reflect(-V, N), V pointing to the
    // eye. The walls facing the camera, most of what it sees of the factory's
    // metal, show the cube's ground; the roofs, its sky.
    {
        const Pixels below = shoot(face_cube("ny"));
        const Pixels above = shoot(face_cube("py"));
        const size_t lit_below = brighter_in(below, above);
        const size_t lit_above = brighter_in(above, below);
        t.check(lit_below > 500 && lit_above < lit_below / 4,
                fmt::format("Test 4: a cube lit below brightens {} pixels more than one lit "
                            "above; the other way, {}",
                            lit_below, lit_above));
    }

    // Test 5: FA's V is the point's device position, turned into the world,
    // so its sideways part is mirrored. A flat mirror facing up reflects
    // with x = -V's: off to the right of the view it shows the world's left
    // (-X), and off to the left, its right, where V straight to the eye
    // would show the other side. The mirror is the test's plate, standing in
    // for the UEF wall's mesh, south of the factory and out of its view; a
    // cube white on one side of x = 0, then the other, shows which way it
    // reflects. No light: the plate shows 2 * env alone.
    {
        const std::string plate_made = fmt::format(
            "__blueprints['/osc_material_test/plate_mesh'] = {{\n"
            "  BlueprintId = '/osc_material_test/plate_mesh',\n"
            "  LODs = {{ {{ LODCutoff = 1000, ShaderName = 'Unit',\n"
            "    MeshName = '/osc_material_test/plate.scm',\n"
            "    AlbedoName = '/osc_material_test/plate_albedo.dds',\n"
            "    SpecularName = '/osc_material_test/plate_specteam.dds',\n"
            "    NormalsName = '/osc_material_test/plate_normals.dds' }} }},\n"
            "}}\n"
            "__blueprints.ueb5101.Display.MeshBlueprint = '/osc_material_test/plate_mesh'\n"
            "__blueprints.ueb5101.Display.UniformScale = 1\n"
            "__osc_test_plate = CreateUnitHPR('ueb5101', 'ARMY_1', {0}, 0, {1}, 0, 0, 0)\n"
            "Warp(__osc_test_plate, Vector({0}, {2}, {1}))\n",
            kCentre, kPlateZ, ground_y + 0.5f);
        const auto made_plate = ctx.lua_state.do_string(plate_made);
        if (!made_plate) spdlog::warn("the plate: {}", made_plate.error().message);
        ctx.sim.tick();

        map::ScmapLighting dark = white_fill();
        for (f32& c : dark.shadow_fill) c = 0.0f;
        const auto frame = [&](f32 target_x, const char* half) {
            map::ScmapEnvironment env;
            env.terrain_shader = "TTerrain";
            env.cubemaps.emplace_back("<default>", face_cube(half));
            ground.set_lighting(dark, std::move(env));
            shots.recapture();
            const ImageRGBA8 image = shots.shoot_frame(ground, target_x, kPlateZ, 30.0f);
            Pixels px;
            for (size_t i = 0; i + 3 < image.pixels.size(); i += 4)
                px.push_back({image.pixels[i] / 255.0f, image.pixels[i + 1] / 255.0f,
                              image.pixels[i + 2] / 255.0f});
            return px;
        };
        struct Side {
            size_t left = 0;  // pixels the cube white on -x lights, and not the one on +x
            size_t right = 0; // ... the other way
        };
        const auto side = [&](f32 target_x) {
            const Pixels nx = frame(target_x, "nx_half");
            const Pixels px = frame(target_x, "px_half");
            Side out;
            for (size_t i = 0; i < nx.size() && i < px.size(); ++i) {
                if (nx[i][1] - px[i][1] > 0.5f) ++out.left;
                if (px[i][1] - nx[i][1] > 0.5f) ++out.right;
            }
            return out;
        };
        renderer::Camera& camera = shots.renderer().camera();
        const f32 pitch = camera.pitch();
        camera.set_pitch(1.1f); // the factory, to the north, out of the view
        const Side on_right = side(kCentre - 10.0f);
        const Side on_left = side(kCentre + 10.0f);
        camera.set_pitch(pitch);
        t.check(on_right.left > 2000 && on_right.right < 20 && on_left.right > 2000 &&
                    on_left.left < 20,
                fmt::format("Test 5: right of the view, the mirror shows the world's left on {} "
                            "pixels (its right on {}); left of the view, its right on {} (its "
                            "left on {})",
                            on_right.left, on_right.right, on_left.right, on_left.left));
    }

    // Test 6: glow (2 * specular.b) shines in the dark. With no light, a
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
                fmt::format("Test 6: in the dark, {} pixels of the power generator glow", glowing));
    }

    // Test 7: each mesh draws with its blueprint's technique (M211b).
    {
        renderer::Renderer& r = shots.renderer();
        const bool ok = r.mesh_technique("ueb0101", ctx.L) == renderer::MeshTechnique::Unit &&
                        r.mesh_technique("uab0101", ctx.L) == renderer::MeshTechnique::Aeon &&
                        r.mesh_technique("urb0101", ctx.L) == renderer::MeshTechnique::Insect &&
                        r.mesh_technique("xsb0101", ctx.L) == renderer::MeshTechnique::Seraphim &&
                        r.mesh_technique("uxl0021", ctx.L) == renderer::MeshTechnique::Metal;
        t.check(ok, "Test 7: UEF, Aeon, Cybran, Seraphim and Metal meshes take their techniques");
    }

    // Test 8: an Aeon mesh reflects the map's "<aeon>" cube (AeonPS), while
    // the UEF factory beside it keeps to "<default>".
    {
        const auto made_aeon =
            ctx.lua_state.do_string("CreateUnitHPR('uab0101', 'ARMY_1', 16, 0, 48, 0, 0, 0)\n");
        if (!made_aeon) spdlog::warn("CreateUnitHPR failed: {}", made_aeon.error().message);
        ctx.sim.tick();
        const auto shoot_at = [&](f32 x, f32 z, const std::string& aeon_cube) {
            map::ScmapEnvironment env;
            env.terrain_shader = "TTerrain";
            env.cubemaps.emplace_back("<default>", kBlack);
            env.cubemaps.emplace_back("<aeon>", aeon_cube);
            ground.set_lighting(white_fill(), std::move(env));
            shots.recapture();
            return shots.shoot(ground, x, z, 30.0f);
        };
        const std::string sky = face_cube("white");
        const auto brightened = [](const Pixels& a, const Pixels& b) {
            size_t n = 0;
            for (size_t i = 0; i < a.size() && i < b.size(); ++i)
                if (a[i][1] - b[i][1] > 0.05f) ++n;
            return n;
        };
        const size_t aeon = brightened(shoot_at(16.0f, 48.0f, sky), shoot_at(16.0f, 48.0f, kBlack));
        const size_t uef = brightened(shoot_at(32.0f, 32.0f, sky), shoot_at(32.0f, 32.0f, kBlack));
        t.check(aeon > 500 && uef < 50,
                fmt::format("Test 8: an \"<aeon>\" cube lights {} pixels of the Aeon factory "
                            "and {} of the UEF one",
                            aeon, uef));
    }

    spdlog::info("Material test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
