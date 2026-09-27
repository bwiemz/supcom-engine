#pragma once

// Test fixtures of the render tests' own: textures, a flat plate mesh, and
// a plate stood in for a blueprint's mesh (M211, M211e).

#include "core/types.hpp"
#include "map/scmap_parser.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace osc::test {

struct TestContext;

/// No sun, a white fill: every surface's light is 1.
inline map::ScmapLighting white_fill() {
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
void write_plate_scm(const std::filesystem::path& path, f32 half, u32 segments = 1);

/// A plate's material: its textures (files under the test's mount) and
/// technique.
struct Plate {
    std::string normals = "plate_normals.dds";
    std::string albedo = "plate_albedo.dds";
    std::string specteam = "plate_specteam.dds";
    std::string shader = "Unit";
    std::string mesh = "plate.scm";
    std::string lookup;    // its LookupName, if any
    std::string secondary; // its SecondaryName, if any (the build shaders', M211f)
};

/// Stand a plate in for blueprint `bp`'s mesh at (x, z), `lift` over half a
/// unit above `ground_y`, through a mesh blueprint of the test's own whose
/// files are under `root` (a VFS mount): a unit of ARMY_1's, or a prop. Lua's
/// `__osc_last_plate` holds it after.
void stand_plate(TestContext& ctx, const std::string& root, const std::string& bp,
                 const Plate& plate, f32 x, f32 z, f32 ground_y, bool prop = false,
                 f32 lift = 0.0f);

} // namespace osc::test
