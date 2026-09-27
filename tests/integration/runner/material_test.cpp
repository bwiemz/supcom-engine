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

#include "blueprints/blueprint_store.hpp"
#include "lua/lua_state.hpp"
#include "map/heightmap.hpp"
#include "map/scmap_parser.hpp"
#include "map/terrain.hpp"
#include "renderer/camera.hpp"
#include "renderer/mesh_cache.hpp"
#include "sim/army_brain.hpp"
#include "sim/entity.hpp"
#include "sim/sim_state.hpp"
#include "sim/world_snapshot.hpp"
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
#include <utility>
#include <vector>

namespace osc::test {

namespace {

constexpr u32 kSize = 64;
constexpr f32 kCentre = kSize / 2.0f;
/// The test's mirrors stand south of the factory, the tilted ones to the
/// east.
constexpr f32 kPlateZ = 58.0f;
constexpr f32 kTiltedX = 52.0f;
constexpr f32 kTiltedVZ = 42.0f;

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

/// An uncompressed BGRA DDS, `width` by `height`, of `faces` faces (6: a
/// cubemap, +X -X +Y -Y +Z -Z, as D3D and Vulkan address them alike), each
/// texel's RGBA from `texel(face, column, row)`.
template <typename Texel>
void write_dds(const std::filesystem::path& path, int faces, Texel texel, u32 width = 4,
               u32 height = 4) {
    std::vector<char> d(128 + static_cast<size_t>(faces) * width * height * 4, 0);
    const auto put = [&](size_t offset, u32 v) { std::memcpy(d.data() + offset, &v, 4); };
    std::memcpy(d.data(), "DDS ", 4);
    put(4, 124);
    put(8, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000);
    put(12, height);
    put(16, width);
    put(20, width * 4);
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
        for (u32 i = 0; i < width * height; ++i) {
            const std::array<u8, 4> rgba = texel(face, i % width, i / width);
            const size_t at = 128 + (static_cast<size_t>(face) * width * height + i) * 4;
            d[at] = static_cast<char>(rgba[2]);
            d[at + 1] = static_cast<char>(rgba[1]);
            d[at + 2] = static_cast<char>(rgba[0]);
            d[at + 3] = static_cast<char>(rgba[3]);
        }
    }
    std::ofstream(path, std::ios::binary).write(d.data(), static_cast<std::streamsize>(d.size()));
}

/// A cubemap white where `white(face, column, row)` says, black elsewhere.
template <typename White> void write_cube(const std::filesystem::path& path, White white) {
    write_dds(path, 6, [&](int face, u32 column, u32 row) {
        const u8 v = white(face, column, row) ? 255 : 0;
        return std::array<u8, 4>{v, v, v, 255};
    });
}

/// A one-bone SCM mesh: a square `half` units either side of the origin,
/// flat and facing up, cut into `segments` squared cells, wound both ways so
/// that either culling draws it.
void write_plate_scm(const std::filesystem::path& path, f32 half, u32 segments = 1) {
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
    const u32 side = segments + 1;
    for (u32 j = 0; j < side; ++j) {
        for (u32 i = 0; i < side; ++i) {
            const f32 u_at = static_cast<f32>(i) / static_cast<f32>(segments);
            const f32 v_at = static_cast<f32>(j) / static_cast<f32>(segments);
            f({-half + 2.0f * half * u_at, 0, -half + 2.0f * half * v_at}); // position
            f({0, 1, 0});                                                   // normal
            f({1, 0, 0});                                                   // tangent: along u
            f({0, 0, 1});                                                   // binormal: along v
            f({u_at, v_at});                                                // uv
            f({0, 0});                                                      // second uv
            u({0});                                                         // bones
        }
    }
    const auto index_offset = static_cast<u32>(d.size());
    std::vector<u16> indices;
    for (u32 j = 0; j < segments; ++j) {
        for (u32 i = 0; i < segments; ++i) {
            const auto a = static_cast<u16>(j * side + i);
            const auto b = static_cast<u16>(a + 1);
            const auto c = static_cast<u16>(a + side + 1);
            const auto e = static_cast<u16>(a + side);
            indices.insert(indices.end(), {a, b, c, a, c, e, a, c, b, a, e, c});
        }
    }
    append(indices.data(), indices.size() * sizeof(u16));
    put(8, bone_offset);
    put(12, 1); // bones the vertices use
    put(16, vert_offset);
    put(24, side * side);
    put(28, index_offset);
    put(32, static_cast<u32>(indices.size()));
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
    // black). A sun overhead lights the factory's roofs, most of what the
    // camera sees; FA doesn't mask the highlight by N . S, so one under the
    // ground still lights the walls facing the camera.
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
        t.check(lit_down > 200 && lit_up > lit_down * 3 / 2,
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
                   [f](int face, u32 /*column*/, u32 /*row*/) { return face == f; });
    const auto x_side = [](bool positive) {
        return [positive](int face, u32 column, u32 /*row*/) {
            if (face == 0 || face == 1) return (face == 0) == positive;
            const bool plus_x = face == 5 ? column < 2 : column >= 2;
            return plus_x == positive;
        };
    };
    const auto z_side = [](bool positive) {
        return [positive](int face, u32 column, u32 row) {
            bool plus_z = face == 4;
            if (face == 0) plus_z = column < 2; // +X's columns run along -z
            if (face == 1) plus_z = column >= 2;
            if (face == 2) plus_z = row >= 2; // +Y's rows run along +z
            if (face == 3) plus_z = row < 2;
            return plus_z == positive;
        };
    };
    write_cube(dir / "px_half.dds", x_side(true));
    write_cube(dir / "nx_half.dds", x_side(false));
    write_cube(dir / "pz_half.dds", z_side(true));
    write_cube(dir / "nz_half.dds", z_side(false));
    write_cube(dir / "white.dds", [](int, u32, u32) { return true; });
    // And a mirror: a flat plate 8 units square, white, reflecting fully
    // (SpecTeam red 1, nothing else), its normals flat.
    write_plate_scm(dir / "plate.scm", 4.0f);
    write_dds(dir / "plate_albedo.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{255, 255, 255, 255}; });
    write_dds(dir / "plate_specteam.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{255, 0, 0, 0}; });
    write_dds(dir / "plate_normals.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{0, 128, 0, 128}; });
    // Normal maps tilted 30 degrees along the tangent (by their alpha: FA
    // reads DXT5nm's x there) and along the binormal (by their green, y).
    write_dds(dir / "plate_normals_tilted.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{0, 128, 0, 191}; });
    write_dds(dir / "plate_normals_tilted_v.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{0, 191, 0, 128}; });
    // White albedos with their alpha clear, at FA's alpha reference (0x80),
    // and just over it.
    for (const auto& [name, alpha] :
         {std::pair<const char*, u8>{"clear", 0}, {"at_ref", 128}, {"over_ref", 129}})
        write_dds(dir / fmt::format("plate_albedo_{}.dds", name), 1,
                  [alpha](int, u32, u32) { return std::array<u8, 4>{255, 255, 255, alpha}; });
    // An opaque black albedo, and a SpecTeam that masks the team's colour in
    // everywhere (and reflects nothing).
    write_dds(dir / "plate_albedo_black.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{0, 0, 0, 255}; });
    write_dds(dir / "plate_specteam_team.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{0, 0, 0, 255}; });
    // For wrecks (M211d): a finely cut plate (it crumples by its vertices),
    // grey and red albedos, and crunch noises -- uniform, below and above
    // WreckagePS's 0.22 in green, and in stripes of the two.
    write_plate_scm(dir / "plate_fine.scm", 4.0f, 32);
    write_dds(dir / "plate_albedo_grey.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{128, 128, 128, 255}; });
    write_dds(dir / "plate_albedo_red.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{255, 0, 0, 255}; });
    write_dds(dir / "crunch_low.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{51, 51, 51, 0}; });
    write_dds(dir / "crunch_high.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{51, 64, 51, 0}; });
    write_dds(dir / "crunch_stripes.dds", 1, [](int, u32 column, u32) {
        return std::array<u8, 4>{51, static_cast<u8>(column < 2 ? 51 : 64), 51, 0};
    });
    // A small plate, to hover over another and shade it, and a blue albedo.
    write_plate_scm(dir / "plate_small.scm", 2.0f);
    write_dds(dir / "plate_albedo_blue.dds", 1,
              [](int, u32, u32) { return std::array<u8, 4>{0, 0, 255, 255}; });
    // A falloff lookup, 16 wide and 20 tall: red counts the column, green the
    // row; alpha is set on the right half (the rim's), which weighs the
    // environment.
    write_dds(
        dir / "falloff_lookup.dds", 1,
        [](int, u32 column, u32 row) {
            return std::array<u8, 4>{static_cast<u8>(column * 17), static_cast<u8>(row * 12), 0,
                                     static_cast<u8>(column >= 8 ? 255 : 0)};
        },
        16, 20);
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

    // A plate's material: its textures (the test's own) and technique.
    struct Plate {
        std::string normals = "plate_normals.dds";
        std::string albedo = "plate_albedo.dds";
        std::string specteam = "plate_specteam.dds";
        std::string shader = "Unit";
        std::string mesh = "plate.scm";
        std::string lookup; // its LookupName, if any
    };
    // Stand the test's plate in for blueprint `bp`'s mesh at (x, z), on the
    // ground, through a mesh blueprint of the test's own: a unit of ARMY_1's,
    // or a prop.
    const auto make_plate = [&](const std::string& bp, const Plate& plate, f32 x, f32 z,
                                bool prop = false, f32 lift = 0.0f) {
        std::string key = bp;
        for (char& c : key)
            if (c == '/' || c == '.') c = '_';
        const std::string lookup =
            plate.lookup.empty() ? "nil" : fmt::format("'/osc_material_test/{}'", plate.lookup);
        const std::string create =
            prop ? fmt::format("CreatePropHPR('{}', {}, {}, {}, 0, 0, 0)", bp, x, ground_y, z)
                 : fmt::format("CreateUnitHPR('{}', 'ARMY_1', {}, 0, {}, 0, 0, 0)", bp, x, z);
        const std::string lua =
            fmt::format("local mesh = '/osc_material_test/{0}_plate'\n"
                        "__blueprints[mesh] = {{ BlueprintId = mesh, LODs = {{ {{\n"
                        "  LODCutoff = 1000, ShaderName = '{1}',\n"
                        "  MeshName = '/osc_material_test/{11}',\n"
                        "  AlbedoName = '/osc_material_test/{2}',\n"
                        "  SpecularName = '/osc_material_test/{10}',\n"
                        "  NormalsName = '/osc_material_test/{3}', LookupName = {4} }} }} }}\n"
                        "local bp = __blueprints['{5}']\n"
                        "bp.Display = bp.Display or {{}}\n"
                        "bp.Display.MeshBlueprint = mesh\n"
                        "bp.Display.UniformScale = 1\n"
                        "Warp({6}, Vector({7}, {8}, {9}))\n",
                        key, plate.shader, plate.albedo, plate.normals, lookup, bp, create, x,
                        ground_y + 0.5f + lift, z, plate.specteam, plate.mesh);
        const auto made = ctx.lua_state.do_string(lua);
        if (!made) spdlog::warn("the plate: {}", made.error().message);
        ctx.sim.tick();
    };
    // No light: a plate shows 2 * env alone.
    map::ScmapLighting dark = white_fill();
    for (f32& c : dark.shadow_fill) c = 0.0f;

    // Test 4: FA reflects the view ray: reflect(-V, N), V pointing to the
    // eye. The roofs, most of what the camera sees, show the cube's sky; the
    // walls facing it, its ground.
    {
        const Pixels below = shoot(face_cube("ny"));
        const Pixels above = shoot(face_cube("py"));
        const size_t lit_below = brighter_in(below, above);
        const size_t lit_above = brighter_in(above, below);
        t.check(lit_below > 200 && lit_above > lit_below * 3 / 2,
                fmt::format("Test 4: a cube lit above brightens {} pixels more than one lit "
                            "below; the other way, {}",
                            lit_above, lit_below));
    }

    // Test 5: FA's V is the point's device position, turned into the world,
    // so its sideways part is mirrored. A flat mirror facing up reflects
    // with x = -V's: off to the right of the view it shows the world's left
    // (-X), and off to the left, its right, where V straight to the eye
    // would show the other side. The mirror is the test's plate, standing in
    // for the UEF wall's mesh, south of the factory and out of its view; a
    // cube white on one side of x = 0, then the other, shows which way it
    // reflects.
    {
        make_plate("ueb5101", Plate{}, kCentre, kPlateZ);

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

    // Test 9: FA's normal basis: a normal map's alpha tilts the normal along
    // the mesh's tangent (here +X, along u), its green along the binormal. In
    // the middle of the view, where V has no sideways part, a plate whose map
    // leans along the tangent reflects the world's right (+X) alone; leaning
    // along the binormal instead, it would reflect straight back.
    {
        Plate tilted;
        tilted.normals = "plate_normals_tilted.dds";
        make_plate("ueb2101", tilted, kTiltedX, kPlateZ);
        const auto centre = [&](const char* half) {
            map::ScmapEnvironment env;
            env.terrain_shader = "TTerrain";
            env.cubemaps.emplace_back("<default>", face_cube(half));
            ground.set_lighting(dark, std::move(env));
            shots.recapture();
            return shots.shoot(ground, kTiltedX, kPlateZ, 30.0f);
        };
        renderer::Camera& camera = shots.renderer().camera();
        const f32 pitch = camera.pitch();
        camera.set_pitch(1.1f);
        const Pixels nx = centre("nx_half");
        const Pixels px = centre("px_half");
        camera.set_pitch(pitch);
        size_t right = 0;
        size_t left = 0;
        for (size_t i = 0; i < nx.size() && i < px.size(); ++i) {
            if (px[i][1] - nx[i][1] > 0.5f) ++right;
            if (nx[i][1] - px[i][1] > 0.5f) ++left;
        }
        t.check(right > 20000 && left < 20,
                fmt::format("Test 9: a plate leaning along its tangent reflects the world's "
                            "right on {} pixels (its left on {})",
                            right, left));
    }

    // Test 10: and the green tilts it along the binormal the mesh stores
    // (here +Z, along v), not cross(N, T), which turns over where a mesh's
    // UVs are mirrored (and here). Leaning toward +Z, the plate reflects the
    // world's +Z side; leaning away, it would reflect the -Z side.
    {
        Plate tilted;
        tilted.normals = "plate_normals_tilted_v.dds";
        make_plate("ueb2301", tilted, kTiltedX, kTiltedVZ);
        const auto centre = [&](const char* half) {
            map::ScmapEnvironment env;
            env.terrain_shader = "TTerrain";
            env.cubemaps.emplace_back("<default>", face_cube(half));
            ground.set_lighting(dark, std::move(env));
            shots.recapture();
            return shots.shoot(ground, kTiltedX, kTiltedVZ, 30.0f);
        };
        renderer::Camera& camera = shots.renderer().camera();
        const f32 pitch = camera.pitch();
        camera.set_pitch(1.1f);
        const Pixels nz = centre("nz_half");
        const Pixels pz = centre("pz_half");
        camera.set_pitch(pitch);
        size_t plus = 0;
        size_t minus = 0;
        for (size_t i = 0; i < nz.size() && i < pz.size(); ++i) {
            if (pz[i][1] - nz[i][1] > 0.5f) ++plus;
            if (nz[i][1] - pz[i][1] > 0.5f) ++minus;
        }
        t.check(plus > 20000 && minus < 20,
                fmt::format("Test 10: a plate leaning along its binormal reflects the world's +Z "
                            "on {} pixels (its -Z on {})",
                            plus, minus));
    }

    renderer::Camera& camera = shots.renderer().camera();
    const f32 default_pitch = camera.pitch();

    // Test 11: FA alpha-tests props alone (NormalMappedAlpha: the albedo's
    // alpha over 0x80). A unit's albedo alpha is a mask its technique reads --
    // Seraphim's glow, over most of a Seraphim ACU -- and cuts no holes.
    {
        std::vector<std::string> props; // two the map has none of, so not yet drawn
        const std::vector<std::string> world = sim::world_blueprints(ctx.sim);
        for (const auto* e : ctx.sim.blueprint_store()->get_all(blueprints::BlueprintType::Prop)) {
            if (props.size() < 2 && e->id.find("/env/") != std::string::npos &&
                std::find(world.begin(), world.end(), e->id) == world.end())
                props.push_back(e->id);
        }
        Plate clear;
        clear.albedo = "plate_albedo_clear.dds";
        make_plate("ueb1105", clear, 8.0f, 14.0f);
        Plate at_ref;
        at_ref.albedo = "plate_albedo_at_ref.dds";
        Plate over_ref;
        over_ref.albedo = "plate_albedo_over_ref.dds";
        if (props.size() == 2) {
            make_plate(props[0], at_ref, 20.0f, 14.0f, /*prop=*/true);
            make_plate(props[1], over_ref, 32.0f, 14.0f, /*prop=*/true);
        }
        const auto white_at = [&](f32 x, f32 z) {
            map::ScmapEnvironment env;
            env.terrain_shader = "TTerrain";
            env.cubemaps.emplace_back("<default>", kBlack);
            ground.set_lighting(white_fill(), std::move(env));
            shots.recapture();
            size_t n = 0;
            for (const auto& px : shots.shoot(ground, x, z, 30.0f))
                if (px[0] > 0.9f && px[1] > 0.9f && px[2] > 0.9f) ++n;
            return n;
        };
        camera.set_pitch(1.1f);
        const size_t unit = white_at(8.0f, 14.0f);
        const size_t under = white_at(20.0f, 14.0f);
        const size_t over = white_at(32.0f, 14.0f);
        camera.set_pitch(default_pitch);
        t.check(props.size() == 2 && unit > 20000 && under < 100 && over > 20000,
                fmt::format("Test 11: a unit with its albedo's alpha clear shows {} pixels; "
                            "props at the alpha reference {}, just over it {}",
                            unit, under, over));
    }

    // Test 12: Seraphim's UnitFalloffPS reads the mesh's lookup (its
    // LookupName), point-sampled: across by pow(1 - N . V, 0.6), down at the
    // army's row, (i + 0.5) / #PlayerColors for its colour's index i in
    // ArmyColors (3 if it isn't there). With the albedo white and opaque, and
    // no light, a plate shows the lookup's texel. The texel's alpha weighs
    // the environment (none here, left of the rim), and the sun doesn't light
    // it: FA leaves its shadow at 0. Nor is the albedo tinted by the team's
    // colour, whatever the SpecTeam's mask: a black one stays black in the
    // light.
    {
        Plate falloff;
        falloff.shader = "Seraphim";
        falloff.lookup = "falloff_lookup.dds";
        make_plate("ueb1101", falloff, 8.0f, 30.0f);
        Plate masked = falloff;
        masked.albedo = "plate_albedo_black.dds";
        masked.specteam = "plate_specteam_team.dds";
        make_plate("ueb1102", masked, 8.0f, 42.0f);
        const auto centre = [&](const std::string& cube, const map::ScmapLighting& lighting,
                                f32 z = 30.0f) {
            map::ScmapEnvironment env;
            env.terrain_shader = "TTerrain";
            env.cubemaps.emplace_back("<default>", cube);
            ground.set_lighting(lighting, std::move(env));
            shots.recapture();
            const ImageRGBA8 image = shots.shoot_frame(ground, 8.0f, z, 30.0f);
            const size_t i =
                (static_cast<size_t>(image.height / 2) * image.width + image.width / 2) * 4;
            return i + 2 < image.pixels.size()
                       ? std::array<int, 3>{image.pixels[i], image.pixels[i + 1],
                                            image.pixels[i + 2]}
                       : std::array<int, 3>{-1, -1, -1};
        };
        constexpr f32 kPitch = 1.1f;
        // At the middle of the view, V is the view's axis: N . V = sin(pitch).
        const int column =
            std::min(15, static_cast<int>(16.0f * std::pow(1.0f - std::sin(kPitch), 0.6f)));
        map::ScmapLighting sun = dark;
        for (f32& c : sun.sun_color) c = 1.0f;
        sun.sun_direction[0] = 0.0f;
        sun.sun_direction[1] = 1.0f;
        sun.sun_direction[2] = 0.0f;
        camera.set_pitch(kPitch);
        if (army) army->set_color(0x13, 0x1C, 0xD3); // ArmyColors[3], i = 2
        const auto listed = centre(kBlack, dark);
        const auto white_env = centre(face_cube("white"), dark);
        const auto sunlit = centre(kBlack, sun);
        map::ScmapLighting fill = dark;
        for (f32& c : fill.shadow_fill) c = 1.0f;
        const auto black = centre(kBlack, fill, 42.0f);
        if (army) army->set_color(1, 2, 3); // no army colour: i = 3
        const auto unlisted = centre(kBlack, dark);
        camera.set_pitch(default_pitch);
        const auto near = [](const std::array<int, 3>& px, int r, int g) {
            return std::abs(px[0] - r) <= 2 && std::abs(px[1] - g) <= 2 && px[2] <= 2;
        };
        // Rows: (2 + 0.5) / 10 * 20 = 5, (3 + 0.5) / 10 * 20 = 7.
        t.check(near(listed, column * 17, 5 * 12) && near(unlisted, column * 17, 7 * 12) &&
                    white_env == listed && sunlit == listed && near(black, column * 17, 5 * 12),
                fmt::format("Test 12: a Seraphim plate shows its lookup's column {} ({}), row "
                            "5 for its army's colour ({}) and 7 for one not listed ({}); a "
                            "white environment gives ({}, {}, {}), a sun ({}, {}, {}); a "
                            "black albedo under the team's mask, lit, ({}, {}, {})",
                            column, column * 17, listed[1], unlisted[1], white_env[0], white_env[1],
                            white_env[2], sunlit[0], sunlit[1], sunlit[2], black[0], black[1],
                            black[2]));
    }

    // The wreck tests' plates stand off the test's ground, on its level, away
    // from everything else. A frame's middle pixel, and its middle row.
    const auto frame_at = [&](f32 x, f32 z, f32 distance) {
        map::ScmapEnvironment env;
        env.terrain_shader = "TTerrain";
        env.cubemaps.emplace_back("<default>", kBlack);
        ground.set_lighting(white_fill(), std::move(env));
        shots.recapture();
        return shots.shoot_frame(ground, x, z, distance);
    };
    const auto middle = [](const ImageRGBA8& image) {
        const size_t i =
            (static_cast<size_t>(image.height / 2) * image.width + image.width / 2) * 4;
        return i + 2 < image.pixels.size()
                   ? std::array<int, 3>{image.pixels[i], image.pixels[i + 1], image.pixels[i + 2]}
                   : std::array<int, 3>{-1, -1, -1};
    };
    const auto grey_near = [](const std::array<int, 3>& px, int v) {
        return std::abs(px[0] - v) <= 2 && std::abs(px[1] - v) <= 2 && std::abs(px[2] - v) <= 2;
    };

    // Test 13: WreckagePS: the albedo lit without shadow (here by the fill,
    // 1), then by the crunch: (albedo + c.r + c.a) * c.b * 2.5 where its green
    // is under 0.22, else c.b * 2. With c = (0.2, g, 0.2, 0), g 0.2 or 0.25:
    // white, 0.6; grey (0.5), 0.5 * 0.7 * 0.5; white over 0.22, 0.4.
    {
        const auto wreck = [](const char* albedo, const char* crunch) {
            Plate p;
            p.shader = "Wreckage";
            p.albedo = albedo;
            p.specteam = crunch;
            return p;
        };
        make_plate("ueb3101", wreck("plate_albedo.dds", "crunch_low.dds"), 100.0f, 100.0f);
        make_plate("ueb3201", wreck("plate_albedo_grey.dds", "crunch_low.dds"), 120.0f, 100.0f);
        make_plate("ueb2104", wreck("plate_albedo.dds", "crunch_high.dds"), 140.0f, 100.0f);
        camera.set_pitch(1.1f);
        const auto white_low = middle(frame_at(100.0f, 100.0f, 30.0f));
        const auto grey_low = middle(frame_at(120.0f, 100.0f, 30.0f));
        const auto white_high = middle(frame_at(140.0f, 100.0f, 30.0f));
        camera.set_pitch(default_pitch);
        const f32 grey = 128.0f / 255.0f;
        const int grey_expected =
            static_cast<int>(std::lround(grey * (grey + 0.2f) * 0.2f * 2.5f * 255.0f));
        t.check(grey_near(white_low, 153) && grey_near(grey_low, grey_expected) &&
                    grey_near(white_high, 102),
                fmt::format("Test 13: wrecks show {} (white, green under 0.22), {} (grey; "
                            "{} expected), {} (white, over)",
                            white_low[0], grey_low[0], grey_expected, white_high[0]));
    }

    // Test 14: the crunch repeats 5.15 times across a wreck's texture, and
    // shifts by fract(0.01 * t) for t the tick its mesh instance was made. A
    // crunch in stripes, half under 0.22 in green and half over, crosses the
    // middle row eight or nine times over 160 pixels of an 8-unit plate
    // (from 30 away), and two wrecks made ticks apart are out of step.
    {
        Plate stripes;
        stripes.shader = "Wreckage";
        stripes.specteam = "crunch_stripes.dds";
        make_plate("ueb1106", stripes, 160.0f, 100.0f);
        camera.set_pitch(1.1f);
        const ImageRGBA8 first = frame_at(160.0f, 100.0f, 30.0f);
        for (int i = 0; i < 5; ++i) ctx.sim.tick();
        make_plate("ueb1201", stripes, 180.0f, 100.0f);
        const ImageRGBA8 later = frame_at(180.0f, 100.0f, 30.0f);
        camera.set_pitch(default_pitch);
        const auto row = [](const ImageRGBA8& image) {
            std::vector<int> out;
            const u32 y = image.height / 2;
            for (u32 x = image.width / 2 - 80; x < image.width / 2 + 80; ++x)
                out.push_back(image.pixels[(static_cast<size_t>(y) * image.width + x) * 4]);
            return out;
        };
        const std::vector<int> a = row(first);
        const std::vector<int> b = row(later);
        int crossings = 0;
        int apart = 0;
        for (size_t i = 1; i < a.size(); ++i)
            if ((a[i - 1] < 128) != (a[i] < 128)) ++crossings;
        for (size_t i = 0; i < a.size() && i < b.size(); ++i)
            if (std::abs(a[i] - b[i]) > 20) ++apart;
        t.check(crossings >= 6 && crossings <= 12 && apart > 20,
                fmt::format("Test 14: the crunch crosses the middle row {} times; two wrecks "
                            "made 6 ticks apart differ on {} of its pixels",
                            crossings, apart));
    }

    // Test 15: a wreck crumples (WreckageVS): each vertex moves by up to
    // 0.15 of the x of its direction from the world's origin. A finely cut
    // red plate far along x has a ragged edge as a wreck, a straight one as a
    // unit. The edge: in each row, the plate's rightmost pixel, against the
    // line through them.
    {
        Plate unit;
        unit.mesh = "plate_fine.scm";
        unit.albedo = "plate_albedo_red.dds";
        Plate wreck = unit;
        wreck.shader = "Wreckage";
        wreck.specteam = "crunch_low.dds";
        make_plate("ueb2204", unit, 200.0f, 100.0f);
        make_plate("ueb1202", wreck, 200.0f, 130.0f);
        const auto ragged = [](const ImageRGBA8& image) {
            // (row, the plate's rightmost column): walking right from the
            // middle, the last red pixel. (Its left edge passes under the
            // minimap.)
            std::vector<std::pair<f64, f64>> edge;
            const auto red = [&](u32 x, u32 y) {
                const size_t i = (static_cast<size_t>(y) * image.width + x) * 4;
                return image.pixels[i] > 100 && image.pixels[i + 1] < 30 &&
                       image.pixels[i + 2] < 30;
            };
            for (u32 y = 0; y < image.height; ++y) {
                u32 x = image.width / 2;
                if (!red(x, y)) continue;
                while (x + 1 < image.width && red(x + 1, y)) ++x;
                edge.emplace_back(y, x);
            }
            if (edge.size() < 50) return -1.0;
            // Leave out the corners' rows; fit the rest.
            edge = std::vector<std::pair<f64, f64>>(edge.begin() + 5, edge.end() - 5);
            f64 sy = 0, sx = 0, syy = 0, syx = 0;
            for (const auto& [y, x] : edge) {
                sy += y;
                sx += x;
                syy += y * y;
                syx += y * x;
            }
            const auto n = static_cast<f64>(edge.size());
            const f64 slope = (n * syx - sy * sx) / (n * syy - sy * sy);
            const f64 base = (sx - slope * sy) / n;
            f64 worst = 0;
            for (const auto& [y, x] : edge)
                worst = std::max(worst, std::abs(x - (base + slope * y)));
            return worst;
        };
        camera.set_pitch(1.1f);
        const f64 straight = ragged(frame_at(200.0f, 100.0f, 15.0f));
        const f64 crumpled = ragged(frame_at(200.0f, 130.0f, 15.0f));
        camera.set_pitch(default_pitch);
        t.check(straight >= 0.0 && straight <= 1.5 && crumpled >= 3.0,
                fmt::format("Test 15: a plate's edge strays from a line by {:.1f} pixels as a "
                            "unit, {:.1f} as a wreck",
                            straight, crumpled));
    }

    // Test 16: no shadow falls on a wreck ("the random crunchiness makes for
    // bad artifacts"). A small blue plate hovers 2.5 over a wreck, and over a
    // unit, under a sun overhead (and no fill): the camera sees under its near
    // edge, into the shadow it casts. The unit's plate is dark there; the
    // wreck's isn't.
    {
        Plate hover;
        hover.mesh = "plate_small.scm";
        hover.albedo = "plate_albedo_blue.dds";
        Plate wreck;
        wreck.shader = "Wreckage";
        wreck.specteam = "crunch_low.dds";
        make_plate("ueb3104", wreck, 100.0f, 130.0f);
        make_plate("ueb4201", hover, 100.0f, 130.0f, false, 2.5f);
        make_plate("ueb1104", Plate{}, 140.0f, 130.0f);
        make_plate("ueb2303", hover, 140.0f, 130.0f, false, 2.5f);
        map::ScmapLighting sun = white_fill();
        for (f32& c : sun.shadow_fill) c = 0.0f;
        for (f32& c : sun.sun_color) c = 1.0f;
        sun.sun_direction[0] = 0.0f;
        sun.sun_direction[1] = 1.0f;
        sun.sun_direction[2] = 0.0f;
        struct Seen {
            size_t dark = 0; // the plate beneath, in shadow
            size_t blue = 0; // the plate hovering
        };
        const auto look = [&](f32 x) {
            map::ScmapEnvironment env;
            env.terrain_shader = "TTerrain";
            env.cubemaps.emplace_back("<default>", kBlack);
            ground.set_lighting(sun, std::move(env));
            shots.recapture();
            Seen seen;
            for (const auto& px : shots.shoot(ground, x, 130.0f, 30.0f)) {
                if (px[0] < 0.08f && px[1] < 0.08f && px[2] < 0.08f) ++seen.dark;
                if (px[2] > 0.5f && px[0] < 0.1f && px[1] < 0.1f) ++seen.blue;
            }
            return seen;
        };
        camera.set_pitch(1.1f);
        const Seen wreck_seen = look(100.0f);
        const Seen unit_seen = look(140.0f);
        camera.set_pitch(default_pitch);
        t.check(wreck_seen.blue > 1000 && unit_seen.blue > 1000 && unit_seen.dark > 500 &&
                    wreck_seen.dark < 50,
                fmt::format("Test 16: under a hovering plate ({} and {} pixels), a unit's plate "
                            "is dark on {} pixels, a wreck's on {}",
                            unit_seen.blue, wreck_seen.blue, unit_seen.dark, wreck_seen.dark));
    }

    spdlog::info("Material test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
