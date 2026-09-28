#include "plate_fixtures.hpp"

#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <cstdlib>
#include <initializer_list>

namespace osc::test {

Rgb over(const Rgb& below, const Rgb& colour, f32 alpha) {
    Rgb out{};
    for (int c = 0; c < 3; ++c) out[c] = below[c] * (1.0f - alpha) + colour[c] * alpha;
    return out;
}

Rgb scaled(const Rgb& colour, f32 by) {
    return {colour[0] * by, colour[1] * by, colour[2] * by};
}

/// A frame's middle pixel, 0..1.
Rgb middle(const ImageRGBA8& image) {
    const size_t i = (static_cast<size_t>(image.height / 2) * image.width + image.width / 2) * 4;
    if (i + 2 >= image.pixels.size()) return {-1, -1, -1};
    return {image.pixels[i] / 255.0f, image.pixels[i + 1] / 255.0f, image.pixels[i + 2] / 255.0f};
}

/// Within 3.5 of 255 in each channel.
bool near(const Rgb& a, const Rgb& b) {
    for (int c = 0; c < 3; ++c)
        if (std::abs(a[c] - b[c]) > 3.5f / 255.0f) return false;
    return true;
}

std::string show(const Rgb& c) {
    return fmt::format("({:.0f}, {:.0f}, {:.0f})", c[0] * 255.0f, c[1] * 255.0f, c[2] * 255.0f);
}

/// The first and last columns of the run of pixels along the frame's middle
/// row, through the middle, that differ from `sky` by more than 12 of 255
/// ({0, -1} if the middle is sky).
std::pair<int, int> plate_span(const ImageRGBA8& image, const Rgb& sky, int row) {
    const u32 y = row < 0 ? image.height / 2 : static_cast<u32>(row);
    const auto plate = [&](u32 x) {
        for (int c = 0; c < 3; ++c) {
            const int v = image.pixels[(static_cast<size_t>(y) * image.width + x) * 4 + c];
            if (std::abs(v - static_cast<int>(std::lround(sky[c] * 255.0f))) > 12) return true;
        }
        return false;
    };
    u32 left = image.width / 2;
    u32 right = left;
    if (!plate(left)) return {0, -1};
    while (left > 0 && plate(left - 1)) --left;
    while (right + 1 < image.width && plate(right + 1)) ++right;
    return {static_cast<int>(left), static_cast<int>(right)};
}

int plate_width(const ImageRGBA8& image, const Rgb& sky, int row) {
    const auto [left, right] = plate_span(image, sky, row);
    return right - left + 1;
}

void write_plate_scm(const std::filesystem::path& path, f32 half, u32 segments, bool child_bone) {
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
    append("root", 5);  // and its terminator, at 52
    append("child", 6); // at 57
    d.resize(64, 0);
    append("SKEL", 4);
    const auto bone_offset = static_cast<u32>(d.size());
    const u32 bones = child_bone ? 2 : 1;
    for (u32 b = 0; b < bones; ++b) {
        f({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});      // rest pose: identity
        f({0, 0, 0});                                             // position
        f({1, 0, 0, 0});                                          // rotation (w, x, y, z)
        u({b == 0 ? 52u : 57u, b == 0 ? 0xFFFFFFFFu : 0u, 0, 0}); // name, parent
    }
    d.resize(bone_offset + 108 * bones + 16, 0);
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
            u({child_bone ? 1u : 0u}); // bones: [1, 0, 0, 0] names the child first
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
    put(12, bones); // bones the vertices use
    put(16, vert_offset);
    put(24, side * side);
    put(28, index_offset);
    put(32, static_cast<u32>(indices.size()));
    put(44, bones); // bones in all
    std::ofstream(path, std::ios::binary).write(d.data(), static_cast<std::streamsize>(d.size()));
}

void write_wall_scm(const std::filesystem::path& path, f32 half, f32 height, f32 facing_z) {
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
    append("root", 5); // at 52
    d.resize(64, 0);
    append("SKEL", 4);
    const auto bone_offset = static_cast<u32>(d.size());
    f({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}); // rest pose: identity
    f({0, 0, 0});                                        // position
    f({1, 0, 0, 0});                                     // rotation (w, x, y, z)
    u({52, 0xFFFFFFFFu, 0, 0});                          // name, no parent
    d.resize(bone_offset + 108 + 16, 0);
    append("VTXL", 4);
    const auto vert_offset = static_cast<u32>(d.size());
    for (u32 j = 0; j < 2; ++j) {
        for (u32 i = 0; i < 2; ++i) {
            f({-half + 2.0f * half * static_cast<f32>(i), height * static_cast<f32>(j), 0});
            f({0, 0, facing_z});                               // normal
            f({1, 0, 0});                                      // tangent: along u
            f({0, 1, 0});                                      // binormal: along v
            f({static_cast<f32>(i), static_cast<f32>(1 - j)}); // uv
            f({0, 0});                                         // second uv
            u({0});                                            // bones
        }
    }
    const auto index_offset = static_cast<u32>(d.size());
    const std::vector<u16> indices = {0, 1, 3, 0, 3, 2, 0, 3, 1, 0, 2, 3};
    append(indices.data(), indices.size() * sizeof(u16));
    put(8, bone_offset);
    put(12, 1);
    put(16, vert_offset);
    put(24, 4);
    put(28, index_offset);
    put(32, static_cast<u32>(indices.size()));
    put(44, 1);
    std::ofstream(path, std::ios::binary).write(d.data(), static_cast<std::streamsize>(d.size()));
}

void stand_plate(TestContext& ctx, const std::string& root, const std::string& bp,
                 const Plate& plate, f32 x, f32 z, f32 ground_y, bool prop, f32 lift) {
    std::string key = bp;
    for (char& c : key)
        if (c == '/' || c == '.') c = '_';
    const auto file = [&](const std::string& name) {
        return name.empty() ? std::string("nil") : fmt::format("'{}/{}'", root, name);
    };
    const std::string create =
        prop ? fmt::format("CreatePropHPR('{}', {}, {}, {}, 0, 0, 0)", bp, x, ground_y, z)
             : fmt::format("CreateUnitHPR('{}', 'ARMY_1', {}, 0, {}, 0, 0, 0)", bp, x, z);
    // A mesh blueprint as retail's are, with IconFadeInZoom (130, as most of
    // retail's): without one, Moho shows its unit's strategic icon at any
    // zoom (M215c), over what the test measures.
    const std::string lua = fmt::format("local mesh = '{12}/{0}_plate'\n"
                                        "__blueprints[mesh] = {{ BlueprintId = mesh, "
                                        "IconFadeInZoom = 130, LODs = {{ {{\n"
                                        "  LODCutoff = 1000, ShaderName = '{1}',\n"
                                        "  MeshName = '{12}/{11}',\n"
                                        "  AlbedoName = '{12}/{2}',\n"
                                        "  SpecularName = '{12}/{10}',\n"
                                        "  NormalsName = '{12}/{3}', LookupName = {4},\n"
                                        "  SecondaryName = {13} }} }} }}\n"
                                        "local bp = __blueprints['{5}']\n"
                                        "bp.Display = bp.Display or {{}}\n"
                                        "bp.Display.MeshBlueprint = mesh\n"
                                        "bp.Display.UniformScale = 1\n"
                                        "__osc_last_plate = {6}\n"
                                        "Warp(__osc_last_plate, Vector({7}, {8}, {9}))\n",
                                        key, plate.shader, plate.albedo, plate.normals,
                                        file(plate.lookup), bp, create, x, ground_y + 0.5f + lift,
                                        z, plate.specteam, plate.mesh, root, file(plate.secondary));
    const auto made = ctx.lua_state.do_string(lua);
    if (!made) spdlog::warn("the plate: {}", made.error().message);
    ctx.sim.tick();
}

} // namespace osc::test
