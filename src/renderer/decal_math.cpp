#include "renderer/decal_math.hpp"

#include "renderer/camera.hpp"

#include <algorithm>
#include <cmath>

namespace osc::renderer {

void decal_texture_matrix(const map::DecalInfo& d, f32 u[4], f32 v[4]) {
    using M = std::array<std::array<f32, 4>, 4>; // m[row][column]
    const auto identity = [] {
        M m{};
        for (size_t i = 0; i < 4; ++i) m[i][i] = 1.0f;
        return m;
    };
    const auto mul = [](const M& a, const M& b) {
        M r{};
        for (size_t i = 0; i < 4; ++i)
            for (size_t j = 0; j < 4; ++j)
                for (size_t k = 0; k < 4; ++k) r[i][j] += a[i][k] * b[k][j];
        return r;
    };
    M m = identity();
    m[3][0] = -d.position_x;
    m[3][1] = -d.position_y;
    m[3][2] = -d.position_z;
    M ry = identity();
    ry[0][0] = std::cos(d.rotation_y);
    ry[0][2] = -std::sin(d.rotation_y);
    ry[2][0] = std::sin(d.rotation_y);
    ry[2][2] = std::cos(d.rotation_y);
    M rx = identity();
    rx[1][1] = std::cos(d.rotation_x);
    rx[1][2] = std::sin(d.rotation_x);
    rx[2][1] = -std::sin(d.rotation_x);
    rx[2][2] = std::cos(d.rotation_x);
    M rz = identity();
    rz[0][0] = std::cos(d.rotation_z);
    rz[0][1] = std::sin(d.rotation_z);
    rz[1][0] = -std::sin(d.rotation_z);
    rz[1][1] = std::cos(d.rotation_z);
    m = mul(mul(mul(m, ry), rx), rz);
    // ApplyInverseScaleToTextureMatrix: each row's x, y and z over the scale.
    for (auto& row : m) {
        row[0] /= d.scale_x;
        row[1] /= d.scale_y;
        row[2] /= d.scale_z;
    }
    for (size_t r = 0; r < 4; ++r) {
        u[r] = m[r][0];
        v[r] = m[r][2];
    }
}

void decal_bounds(const map::DecalInfo& d, f32& min_x, f32& min_z, f32& max_x, f32& max_z) {
    const f32 c = std::cos(d.rotation_y);
    const f32 s = std::sin(d.rotation_y);
    const f32 xx = d.scale_x * c;
    const f32 xz = d.scale_x * s;
    const f32 zx = -d.scale_z * s;
    const f32 zz = d.scale_z * c;
    min_x = d.position_x + std::min({0.0f, xx, zx, xx + zx});
    max_x = d.position_x + std::max({0.0f, xx, zx, xx + zx});
    min_z = d.position_z + std::min({0.0f, xz, zz, xz + zz});
    max_z = d.position_z + std::max({0.0f, xz, zz, xz + zz});
}

f32 decal_lod_metric(const std::array<f32, 16>& view, const std::array<f32, 3>& eye, f32 half_width,
                     f32 x, f32 y, f32 z) {
    const f32 depth =
        -(view[2] * (x - eye[0]) + view[6] * (y - eye[1]) + view[10] * (z - eye[2])); // along -Z
    return 2.0f * half_width * depth;
}

f32 decal_lod_alpha(f32 cutoff, f32 near_cutoff, f32 distance) {
    constexpr f32 kFadeFraction = 0.75f;
    if (near_cutoff > 0.0f) {
        const f32 begin = near_cutoff * kFadeFraction;
        return (std::clamp(distance, begin, near_cutoff) - begin) / (near_cutoff - begin);
    }
    if (cutoff <= 0.0f) return 1.0f;
    const f32 begin = cutoff * kFadeFraction;
    return 1.0f - (std::clamp(distance, begin, cutoff) - begin) / (cutoff - begin);
}

std::optional<DecalTechnique> decal_technique(map::DecalType type) {
    switch (type) {
    case map::DecalType::Normals: return DecalTechnique::Normals;
    case map::DecalType::AlphaNormals: return DecalTechnique::Normals;
    case map::DecalType::GlowMask: return DecalTechnique::GlowMask;
    case map::DecalType::Albedo: return DecalTechnique::Albedo;
    case map::DecalType::AlbedoXP: return DecalTechnique::AlbedoXP;
    case map::DecalType::Glow: return DecalTechnique::Glow;
    case map::DecalType::WaterAlbedo: return DecalTechnique::WaterAlbedo;
    default: return std::nullopt;
    }
}

bool next_decal_frame_name(std::string& name) {
    // The last digit in the name, which must end it or come just before a
    // dot (CAnimTexture's IncrementFrameNameSuffix).
    const size_t last = name.find_last_of("0123456789");
    if (last == std::string::npos) return false;
    if (last + 1 < name.size() && name[last + 1] != '.') return false;
    // Counted up with its carries, the run as wide as it was
    for (size_t i = last + 1; i-- > 0;) {
        char& c = name[i];
        if (c < '0' || c > '9') return true;
        if (c < '9') {
            ++c;
            return true;
        }
        c = '0';
    }
    return true;
}

std::vector<std::string> decal_frame_names(const std::string& first,
                                           const std::function<bool(const std::string&)>& exists) {
    std::vector<std::string> frames{first};
    std::string name = first;
    while (frames.size() < 1000 && next_decal_frame_name(name) && name != first && exists(name))
        frames.push_back(name);
    return frames;
}

size_t decal_frame_at(f32 ticks, size_t count) {
    if (count <= 1) return 0;
    const auto n = static_cast<f64>(count);
    f64 phase = std::fmod(static_cast<f64>(ticks) * 0.5, n);
    if (phase < 0) phase += n;
    return std::min(static_cast<size_t>(phase), count - 1);
}

std::array<f32, 2> decal_corner(const map::DecalInfo& d, f32 lx, f32 lz) {
    const f32 c = std::cos(d.rotation_y);
    const f32 s = std::sin(d.rotation_y);
    return {d.position_x + lz * (-d.scale_z * s) + lx * (d.scale_x * c),
            d.position_z + lz * (d.scale_z * c) + lx * (d.scale_x * s)};
}

} // namespace osc::renderer
