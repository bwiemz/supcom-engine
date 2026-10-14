#include "renderer/cartographic_math.hpp"

#include "map/terrain.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace osc::renderer {

namespace {

constexpr f32 kInverseRampRange = 1.0f / 255.0f;
constexpr f32 kContourShadeScale = 239.0f;
constexpr f32 kContourShadeBias = 16.0f;
constexpr i32 kContourShadeMinimum = 16;
constexpr i32 kContourShadeMaximum = 255;
constexpr i32 kMinBands = 1;
constexpr i32 kMaxBands = 100;

} // namespace

i32 truncate_to_i32(f32 value) {
    if (!(value >= -2147483648.0f && value < 2147483648.0f)) return std::numeric_limits<i32>::min();
    return static_cast<i32>(value);
}

u16 float_to_half(f32 value) {
    const u32 bits = std::bit_cast<u32>(value);
    const u32 sign = (bits >> 16u) & 0x8000u;
    const i32 exponent = static_cast<i32>((bits >> 23u) & 0xFFu) - 127 + 15;
    u32 mantissa = bits & 0x007FFFFFu;
    if (exponent <= 0) {
        if (exponent < -10) return static_cast<u16>(sign);
        mantissa = (mantissa | 0x00800000u) >> static_cast<u32>(1 - exponent);
        return static_cast<u16>(sign | ((mantissa + 0x00001000u) >> 13u));
    }
    if (exponent >= 31) return static_cast<u16>(sign | 0x7C00u);
    return static_cast<u16>(sign | (static_cast<u32>(exponent) << 10u) |
                            ((mantissa + 0x00001000u) >> 13u));
}

u32 blend_packed_color(u32 from, u32 to, f32 t) {
    u32 blended = 0;
    for (u32 shift = 0; shift < 32; shift += 8) {
        const i32 a = static_cast<i32>((from >> shift) & 0xFFu);
        const i32 b = static_cast<i32>((to >> shift) & 0xFFu);
        const f32 channel = static_cast<f32>(b - a) * t + static_cast<f32>(a);
        blended |= (static_cast<u32>(truncate_to_i32(channel)) & 0xFFu) << shift;
    }
    return blended;
}

CartographicParams cartographic_params(const map::Terrain& terrain) {
    const map::Heightmap& field = terrain.heightmap();
    CartographicParams p;
    const i32 span_x = static_cast<i32>(field.grid_width()) - 1;
    const i32 span_z = static_cast<i32>(field.grid_height()) - 1;
    p.grid_width = span_x / 2; // Moho's span >> 1; the spans aren't negative
    p.grid_height = span_z / 2;
    p.grid_size_coeff = {1.0f / static_cast<f32>(p.grid_width),
                         1.0f / static_cast<f32>(p.grid_height), 0.0f, 1.0f};
    p.terrain_size_coeff = {static_cast<f32>(p.grid_width) / static_cast<f32>(span_x),
                            static_cast<f32>(p.grid_height) / static_cast<f32>(span_z), 0.0f, 1.0f};
    // The heightfield's bounding box (CHeightField::GetBounds3D)
    f32 lo = std::numeric_limits<f32>::max();
    f32 hi = std::numeric_limits<f32>::lowest();
    for (u32 z = 0; z < field.grid_height(); ++z) {
        for (u32 x = 0; x < field.grid_width(); ++x) {
            const f32 h = field.get_height_at_grid(x, z);
            lo = std::min(lo, h);
            hi = std::max(hi, h);
        }
    }
    p.elev_min = lo;
    p.elev_max = hi;
    p.has_bounds = terrain.has_water();
    p.surface = p.has_bounds ? terrain.water_elevation() : lo;
    p.water = p.has_bounds ? terrain.water_deep_elevation() : lo;
    p.deep_lane = p.water;
    const map::ScmapCartographic& c = terrain.cartographic();
    std::copy(std::begin(c.hypsometric), std::end(c.hypsometric), p.palette.begin());
    p.bands = std::clamp(c.topographic_samples, kMinBands, kMaxBands);
    return p;
}

f32 cartographic_height(const map::Terrain& terrain, i32 x, i32 z, f32 baseline, f32 scale) {
    static constexpr std::array<f32, 9> kWeights{0, 1, 0, 1, 3, 1, 0, 1, 0};
    constexpr f32 kNormalize = 0.142857149f;
    const map::Heightmap& field = terrain.heightmap();
    const i32 w = static_cast<i32>(field.grid_width());
    const i32 h = static_cast<i32>(field.grid_height());
    const i32 cx = std::max(0, std::min(x - 1, w - 1));
    const i32 cz = std::max(0, std::min(z - 1, h - 1));
    const f32 normalized =
        field.get_height_at_grid(static_cast<u32>(cx), static_cast<u32>(cz)) - baseline;
    f32 weighted = 0.0f;
    for (const f32 weight : kWeights) weighted += (normalized * weight) * scale;
    return weighted * kNormalize;
}

std::vector<u16> cartographic_elevation(const map::Terrain& terrain, const CartographicParams& p) {
    const map::Heightmap& field = terrain.heightmap();
    const f32 span = p.elev_max - p.elev_min;
    const f32 inverse = span > 0.0f ? 1.0f / span : 1.0f;
    const f32 step_x = p.grid_width > 1 ? static_cast<f32>(field.grid_width() - 1) /
                                              static_cast<f32>(p.grid_width - 1)
                                        : 0.0f;
    const f32 step_z = p.grid_height > 1 ? static_cast<f32>(field.grid_height() - 1) /
                                               static_cast<f32>(p.grid_height - 1)
                                         : 0.0f;
    std::vector<u16> texels;
    texels.reserve(static_cast<size_t>(std::max(0, p.grid_width)) *
                   static_cast<size_t>(std::max(0, p.grid_height)));
    for (i32 z = 0; z < p.grid_height; ++z) {
        const i32 sample_z = static_cast<i32>(static_cast<f32>(z) * step_z);
        for (i32 x = 0; x < p.grid_width; ++x) {
            const i32 sample_x = static_cast<i32>(static_cast<f32>(x) * step_x);
            texels.push_back(float_to_half(
                cartographic_height(terrain, sample_x, sample_z, p.elev_min, inverse)));
        }
    }
    return texels;
}

namespace {

/// The ramps' places of the water lanes, 0 at the floor and 1 at the
/// ceiling.
struct Lanes {
    f32 surface, water, deep;
};

Lanes lanes(const CartographicParams& p) {
    const f32 span = p.elev_max - p.elev_min;
    const f32 inverse = span > 0.0f ? 1.0f / span : 1.0f;
    return {(p.surface - p.elev_min) * inverse, (p.water - p.elev_min) * inverse,
            (p.deep_lane - p.elev_min) * inverse};
}

} // namespace

std::array<u32, kCartographicRampTexels> hypsometric_ramp(const CartographicParams& p) {
    const Lanes n = lanes(p);
    std::array<u32, kCartographicRampTexels> ramp{};
    for (i32 i = 0; i < kCartographicRampTexels; ++i) {
        const f32 at = static_cast<f32>(i) * kInverseRampRange;
        const auto land = [&] {
            return blend_packed_color(p.palette[3], p.palette[4],
                                      (at - n.surface) / (1.0f - n.surface));
        };
        if (!p.has_bounds) ramp[i] = land();
        else if (n.deep >= at) ramp[i] = p.palette[0];
        else if (n.water >= at) ramp[i] = p.palette[1];
        else if (n.surface >= at) ramp[i] = p.palette[2];
        else ramp[i] = land();
    }
    return ramp;
}

std::array<u8, kCartographicRampTexels> topographic_ramp(const CartographicParams& p) {
    const Lanes n = lanes(p);
    const i32 bands = std::clamp(p.bands, kMinBands, kMaxBands);
    const f32 land_span = 1.0f - n.water;
    const f32 step = land_span / static_cast<f32>(bands);
    // One more than the bands: a texel past the last band's ceiling (float
    // drift at the top) reads the extra, zero, ceiling, as in faf-re's port.
    std::array<f32, kMaxBands + 1> ceilings{};
    ceilings[0] = step + n.water;
    for (i32 b = 1; b < bands; ++b) ceilings[b] = ceilings[b - 1] + step;
    std::array<u8, kCartographicRampTexels> ramp{};
    for (i32 i = 0; i < kCartographicRampTexels; ++i) {
        const f32 at = static_cast<f32>(i) * kInverseRampRange;
        if (p.has_bounds && n.water >= at) {
            ramp[i] = 0;
            continue;
        }
        i32 band = 0;
        while (band < bands && ceilings[band] < at) ++band;
        const f32 shade =
            ((ceilings[band] - n.water) / land_span) * kContourShadeScale + kContourShadeBias;
        ramp[i] = static_cast<u8>(
            std::clamp(truncate_to_i32(shade), kContourShadeMinimum, kContourShadeMaximum));
    }
    return ramp;
}

} // namespace osc::renderer
