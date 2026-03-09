#include "renderer/normal_overlay.hpp"
#include "renderer/dds_decode.hpp"
#include "renderer/dds_parser.hpp"
#include "vfs/virtual_file_system.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <unordered_map>

namespace osc::renderer {

namespace {

/// Decoded texture data for rasterization.
struct DecodedTexture {
    const u8* pixels = nullptr; // RGBA
    u32 width = 0;
    u32 height = 0;
};

/// Sample DXT5nm-encoded normal from RGBA pixels at integer coordinates.
/// DXT5nm: X = green channel, Y = alpha channel; both mapped from [0,255] to [-1,+1].
void sample_dxt5nm(const DecodedTexture& tex, u32 px, u32 py, f32& nx, f32& ny) {
    u32 idx = (py * tex.width + px) * 4;
    nx = static_cast<f32>(tex.pixels[idx + 1]) / 127.5f - 1.0f; // green
    ny = static_cast<f32>(tex.pixels[idx + 3]) / 127.5f - 1.0f; // alpha
}

/// Core baking logic shared by both entry points.
NormalOverlay bake_impl(
    const std::vector<osc::map::NormalDecalInfo>& decals,
    u32 overlay_w, u32 overlay_h,
    const std::unordered_map<std::string, DecodedTexture>& textures)
{
    NormalOverlay result;
    result.width = overlay_w;
    result.height = overlay_h;

    const u32 total_texels = overlay_w * overlay_h;
    result.pixels.resize(total_texels * 2, 0.0f);

    // Weight buffer for averaging overlapping decals.
    std::vector<f32> weights(total_texels, 0.0f);

    for (const auto& decal : decals) {
        auto it = textures.find(decal.texture_path);
        if (it == textures.end()) {
            continue;
        }
        const auto& tex = it->second;
        if (tex.width == 0 || tex.height == 0) {
            continue;
        }

        const f32 half_sx = decal.scale_x * 0.5f;
        const f32 half_sz = decal.scale_z * 0.5f;
        const f32 cos_r = std::cos(decal.rotation_y);
        const f32 sin_r = std::sin(decal.rotation_y);

        // Compute axis-aligned bounding box of rotated decal in world space.
        // The four corners in local space are (±half_sx, ±half_sz).
        // After rotation: wx = lx*cos - lz*sin, wz = lx*sin + lz*cos.
        const f32 abs_cos = std::abs(cos_r);
        const f32 abs_sin = std::abs(sin_r);
        const f32 extent_x = half_sx * abs_cos + half_sz * abs_sin;
        const f32 extent_z = half_sx * abs_sin + half_sz * abs_cos;

        // Bounding box in overlay texel coordinates.
        // Overlay maps 1:1 with world-space XZ (texel i covers world x in [i, i+1]).
        const f32 min_wx = decal.position_x - extent_x;
        const f32 max_wx = decal.position_x + extent_x;
        const f32 min_wz = decal.position_z - extent_z;
        const f32 max_wz = decal.position_z + extent_z;

        const i32 tx_min = std::max(0, static_cast<i32>(std::floor(min_wx)));
        const i32 tx_max = std::min(static_cast<i32>(overlay_w) - 1,
                                    static_cast<i32>(std::floor(max_wx)));
        const i32 tz_min = std::max(0, static_cast<i32>(std::floor(min_wz)));
        const i32 tz_max = std::min(static_cast<i32>(overlay_h) - 1,
                                    static_cast<i32>(std::floor(max_wz)));

        for (i32 tz = tz_min; tz <= tz_max; ++tz) {
            for (i32 tx = tx_min; tx <= tx_max; ++tx) {
                // Texel center in world space.
                const f32 wx = static_cast<f32>(tx) + 0.5f;
                const f32 wz = static_cast<f32>(tz) + 0.5f;

                // Transform to decal local space (inverse rotation).
                const f32 dx = wx - decal.position_x;
                const f32 dz = wz - decal.position_z;
                const f32 lx = dx * cos_r + dz * sin_r;
                const f32 lz = -dx * sin_r + dz * cos_r;

                // Check if inside decal footprint.
                if (std::abs(lx) > half_sx || std::abs(lz) > half_sz) {
                    continue;
                }

                // Compute UV in decal texture [0, 1].
                const f32 u = (lx / half_sx) * 0.5f + 0.5f;
                const f32 v = (lz / half_sz) * 0.5f + 0.5f;

                // Nearest-neighbor sample.
                const u32 px = std::min(static_cast<u32>(u * static_cast<f32>(tex.width)),
                                        tex.width - 1);
                const u32 py = std::min(static_cast<u32>(v * static_cast<f32>(tex.height)),
                                        tex.height - 1);

                f32 nx, ny;
                sample_dxt5nm(tex, px, py, nx, ny);

                // Rotate normal by decal's Y rotation to align with terrain tangent space.
                // Terrain tangent = +X, bitangent = +Z.
                // Rotated: nx' = nx*cos - ny*sin, ny' = nx*sin + ny*cos.
                const f32 rnx = nx * cos_r - ny * sin_r;
                const f32 rny = nx * sin_r + ny * cos_r;

                // Accumulate.
                const u32 idx = (static_cast<u32>(tz) * overlay_w + static_cast<u32>(tx)) * 2;
                result.pixels[idx + 0] += rnx;
                result.pixels[idx + 1] += rny;
                weights[static_cast<u32>(tz) * overlay_w + static_cast<u32>(tx)] += 1.0f;
            }
        }
    }

    // Normalize overlapping regions.
    for (u32 i = 0; i < total_texels; ++i) {
        if (weights[i] > 1.0f) {
            result.pixels[i * 2 + 0] /= weights[i];
            result.pixels[i * 2 + 1] /= weights[i];
        }
    }

    return result;
}

} // anonymous namespace

NormalOverlay bake_normal_overlay(
    const std::vector<osc::map::NormalDecalInfo>& decals,
    u32 overlay_w, u32 overlay_h,
    vfs::VirtualFileSystem* vfs)
{
    // Load and decode unique textures.
    std::unordered_map<std::string, std::vector<char>> file_buffers;
    std::unordered_map<std::string, std::vector<u8>> decoded_buffers;
    std::unordered_map<std::string, DecodedTexture> textures;

    for (const auto& decal : decals) {
        if (textures.count(decal.texture_path)) {
            continue;
        }

        auto file_data = vfs->read_file(decal.texture_path);
        if (!file_data) {
            spdlog::warn("NormalOverlay: failed to read texture '{}'", decal.texture_path);
            continue;
        }

        auto& buf = file_buffers[decal.texture_path];
        buf = std::move(*file_data);

        auto dds = parse_dds(buf);
        if (!dds || dds->mips.empty()) {
            spdlog::warn("NormalOverlay: failed to parse DDS '{}'", decal.texture_path);
            continue;
        }

        const auto& mip0 = dds->mips[0];
        auto rgba = decode_bc3_to_rgba(
            reinterpret_cast<const u8*>(mip0.data), mip0.width, mip0.height);

        DecodedTexture tex;
        tex.width = mip0.width;
        tex.height = mip0.height;

        auto& dbuf = decoded_buffers[decal.texture_path];
        dbuf = std::move(rgba);
        tex.pixels = dbuf.data();

        textures[decal.texture_path] = tex;
    }

    return bake_impl(decals, overlay_w, overlay_h, textures);
}

NormalOverlay bake_normal_overlay_with_predecoded(
    const std::vector<osc::map::NormalDecalInfo>& decals,
    u32 overlay_w, u32 overlay_h,
    const std::vector<PredecodedNormal>& predecoded)
{
    std::unordered_map<std::string, DecodedTexture> textures;
    for (const auto& pd : predecoded) {
        DecodedTexture tex;
        tex.pixels = pd.pixels.data();
        tex.width = pd.width;
        tex.height = pd.height;
        textures[pd.path] = tex;
    }

    return bake_impl(decals, overlay_w, overlay_h, textures);
}

} // namespace osc::renderer
