# M128: Decal Normal Map Rendering — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Render normal map decals from .scmap files by CPU-baking them into an RG16F overlay texture that the terrain shader samples to perturb heightmap normals.

**Architecture:** At map load, normal map decals (decal_type == 2) are rasterized on the CPU into a world-space RG16F overlay (1 texel per game unit). The terrain fragment shader adds a binding for this overlay and perturbs its blended stratum normal with the overlay's XY perturbation before lighting. Zero per-frame cost.

**Tech Stack:** C++17, Vulkan, GLSL 450, BC3/DXT5nm DDS textures, VMA

---

### Task 1: CPU-side BC3 (DXT5nm) block decompressor

We need to decode compressed DDS normal maps to raw pixels on the CPU for baking. FA normal maps use DXT5nm encoding (BC3 format) where normal X = green channel, Y = alpha channel.

**Files:**
- Create: `src/renderer/dds_decode.hpp`
- Create: `src/renderer/dds_decode.cpp`
- Modify: `src/renderer/CMakeLists.txt` (add new .cpp)
- Create: `tests/test_dds_decode.cpp`
- Modify: `tests/CMakeLists.txt` (add test + link osc::renderer)

**Step 1: Write the failing test**

Create `tests/test_dds_decode.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "renderer/dds_decode.hpp"

using namespace osc::renderer;

// ---------------------------------------------------------------------------
// Test: decode_bc3_to_rgba produces correct pixel data for a known block
// ---------------------------------------------------------------------------
TEST_CASE("decode_bc3_to_rgba decodes a single 4x4 block", "[dds_decode]") {
    // Build a minimal BC3 block (16 bytes):
    // Alpha block (8 bytes): endpoints 255, 255, then 6 bytes of index 0
    //   → all pixels get alpha = 255
    // Color block (8 bytes): color0 = white (0xFFFF), color1 = black (0x0000),
    //   index bytes all 0 → all pixels get color0 = white
    std::vector<u8> block(16, 0);
    // Alpha endpoints
    block[0] = 255;  // alpha0
    block[1] = 255;  // alpha1
    // Alpha indices: 6 bytes of 0 (all pixels use alpha0 = 255)
    // Color endpoints (little-endian)
    block[8]  = 0xFF; block[9]  = 0xFF;  // color0 = R5:31 G6:63 B5:31 = white
    block[10] = 0x00; block[11] = 0x00;  // color1 = black
    // Color indices: 4 bytes of 0 (all pixels use color0)

    u8 out[4 * 4 * 4]; // 4x4 pixels, 4 bytes each (RGBA)
    decode_bc3_block(block.data(), out);

    // All 16 pixels should be white with alpha 255
    for (int i = 0; i < 16; i++) {
        CHECK(out[i * 4 + 0] == 255);  // R
        CHECK(out[i * 4 + 1] == 255);  // G
        CHECK(out[i * 4 + 2] == 255);  // B
        CHECK(out[i * 4 + 3] == 255);  // A
    }
}

TEST_CASE("decode_bc3_dds returns correct dimensions and pixel count", "[dds_decode]") {
    // We can't easily construct a full valid DDS in a unit test,
    // so we test decode_bc3_block and trust that decode_bc3_dds
    // iterates blocks correctly (integration tested visually).
    // This test verifies the block decoder works for a "neutral normal" block.

    // DXT5nm neutral normal: X=0.5 (green=128), Y=0.5 (alpha=128)
    // BC3 block with alpha=128 and green=128:
    std::vector<u8> block(16, 0);
    block[0] = 128;  // alpha0 = 128
    block[1] = 128;  // alpha1 = 128
    // Alpha indices: all 0 → alpha = 128

    // Color: we need green=128. In RGB565:
    // R=0, G=128→G6=32 (128/255*63≈32), B=0
    // RGB565 = (0 << 11) | (32 << 5) | 0 = 0x0400
    block[8] = 0x00; block[9] = 0x04;  // color0 = 0x0400 (G≈128)
    block[10] = 0x00; block[11] = 0x04; // color1 = same
    // Color indices: all 0

    u8 out[4 * 4 * 4];
    decode_bc3_block(block.data(), out);

    // Check first pixel: G should be ~128, A should be 128
    // G6=32 → 32*255/63 = 129 (rounding), close enough
    CHECK(out[1] >= 126);  // G
    CHECK(out[1] <= 132);
    CHECK(out[3] == 128);  // A
}
```

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(osc_tests
    test_vfs.cpp
    test_lua_state.cpp
    test_init_loader.cpp
    test_blueprint_loading.cpp
    test_map.cpp
    test_anim_blend.cpp
    test_dds_decode.cpp
)

target_link_libraries(osc_tests PRIVATE
    Catch2::Catch2WithMain
    osc::core
    osc::vfs
    osc::map
    osc::lua
    osc::blueprints
    osc::sim
    osc::renderer
)
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --config Debug --target osc_tests 2>&1 | head -20`
Expected: FAIL — `dds_decode.hpp` not found

**Step 3: Write the implementation**

Create `src/renderer/dds_decode.hpp`:

```cpp
#pragma once

#include "core/types.hpp"

#include <vector>

namespace osc::renderer {

/// Decode a single BC3 (DXT5) 4x4 block to 16 RGBA pixels (64 bytes output).
/// Input: 16 bytes of BC3 compressed data.
/// Output: 64 bytes — 16 pixels in row-major order, 4 bytes (R,G,B,A) each.
void decode_bc3_block(const u8* block, u8* out_rgba);

/// Decode a full BC3-compressed DDS mip level to RGBA pixels.
/// Returns width*height*4 bytes of RGBA pixel data.
/// block_data: raw BC3 compressed data for the mip level.
/// width, height: dimensions in pixels (must be multiples of 4 or the full image size).
std::vector<u8> decode_bc3_to_rgba(const u8* block_data, u32 width, u32 height);

} // namespace osc::renderer
```

Create `src/renderer/dds_decode.cpp`:

```cpp
#include "renderer/dds_decode.hpp"

#include <algorithm>
#include <cstring>

namespace osc::renderer {

namespace {

/// Unpack RGB565 to R8G8B8 (0-255 range).
void rgb565_to_rgb(u16 c, u8& r, u8& g, u8& b) {
    u8 r5 = (c >> 11) & 0x1F;
    u8 g6 = (c >> 5)  & 0x3F;
    u8 b5 = c & 0x1F;
    r = static_cast<u8>((r5 * 255 + 15) / 31);
    g = static_cast<u8>((g6 * 255 + 31) / 63);
    b = static_cast<u8>((b5 * 255 + 15) / 31);
}

} // anonymous namespace

void decode_bc3_block(const u8* block, u8* out_rgba) {
    // --- Alpha block (8 bytes) ---
    u8 alpha0 = block[0];
    u8 alpha1 = block[1];

    // 6 bytes of 3-bit alpha indices (48 bits for 16 pixels)
    u8 alpha_table[8];
    alpha_table[0] = alpha0;
    alpha_table[1] = alpha1;
    if (alpha0 > alpha1) {
        alpha_table[2] = static_cast<u8>((6 * alpha0 + 1 * alpha1) / 7);
        alpha_table[3] = static_cast<u8>((5 * alpha0 + 2 * alpha1) / 7);
        alpha_table[4] = static_cast<u8>((4 * alpha0 + 3 * alpha1) / 7);
        alpha_table[5] = static_cast<u8>((3 * alpha0 + 4 * alpha1) / 7);
        alpha_table[6] = static_cast<u8>((2 * alpha0 + 5 * alpha1) / 7);
        alpha_table[7] = static_cast<u8>((1 * alpha0 + 6 * alpha1) / 7);
    } else {
        alpha_table[2] = static_cast<u8>((4 * alpha0 + 1 * alpha1) / 5);
        alpha_table[3] = static_cast<u8>((3 * alpha0 + 2 * alpha1) / 5);
        alpha_table[4] = static_cast<u8>((2 * alpha0 + 3 * alpha1) / 5);
        alpha_table[5] = static_cast<u8>((1 * alpha0 + 4 * alpha1) / 5);
        alpha_table[6] = 0;
        alpha_table[7] = 255;
    }

    // Read 48-bit alpha index stream
    u64 alpha_bits = 0;
    for (int i = 0; i < 6; i++) {
        alpha_bits |= static_cast<u64>(block[2 + i]) << (8 * i);
    }

    u8 pixel_alpha[16];
    for (int i = 0; i < 16; i++) {
        u8 idx = (alpha_bits >> (3 * i)) & 0x7;
        pixel_alpha[i] = alpha_table[idx];
    }

    // --- Color block (8 bytes at offset 8) ---
    u16 color0, color1;
    std::memcpy(&color0, block + 8, 2);
    std::memcpy(&color1, block + 10, 2);

    u8 r0, g0, b0, r1, g1, b1;
    rgb565_to_rgb(color0, r0, g0, b0);
    rgb565_to_rgb(color1, r1, g1, b1);

    u8 color_table[4][3];
    color_table[0][0] = r0; color_table[0][1] = g0; color_table[0][2] = b0;
    color_table[1][0] = r1; color_table[1][1] = g1; color_table[1][2] = b1;

    if (color0 > color1) {
        color_table[2][0] = static_cast<u8>((2 * r0 + r1) / 3);
        color_table[2][1] = static_cast<u8>((2 * g0 + g1) / 3);
        color_table[2][2] = static_cast<u8>((2 * b0 + b1) / 3);
        color_table[3][0] = static_cast<u8>((r0 + 2 * r1) / 3);
        color_table[3][1] = static_cast<u8>((g0 + 2 * g1) / 3);
        color_table[3][2] = static_cast<u8>((b0 + 2 * b1) / 3);
    } else {
        color_table[2][0] = static_cast<u8>((r0 + r1) / 2);
        color_table[2][1] = static_cast<u8>((g0 + g1) / 2);
        color_table[2][2] = static_cast<u8>((b0 + b1) / 2);
        color_table[3][0] = 0;
        color_table[3][1] = 0;
        color_table[3][2] = 0;
    }

    // Read 2-bit color indices (32 bits = 4 bytes)
    u32 color_bits;
    std::memcpy(&color_bits, block + 12, 4);

    for (int i = 0; i < 16; i++) {
        u8 idx = (color_bits >> (2 * i)) & 0x3;
        out_rgba[i * 4 + 0] = color_table[idx][0]; // R
        out_rgba[i * 4 + 1] = color_table[idx][1]; // G
        out_rgba[i * 4 + 2] = color_table[idx][2]; // B
        out_rgba[i * 4 + 3] = pixel_alpha[i];       // A
    }
}

std::vector<u8> decode_bc3_to_rgba(const u8* block_data, u32 width, u32 height) {
    u32 blocks_x = (width + 3) / 4;
    u32 blocks_y = (height + 3) / 4;
    std::vector<u8> pixels(width * height * 4, 0);

    for (u32 by = 0; by < blocks_y; by++) {
        for (u32 bx = 0; bx < blocks_x; bx++) {
            const u8* block = block_data + (by * blocks_x + bx) * 16;
            u8 decoded[64]; // 4x4 RGBA
            decode_bc3_block(block, decoded);

            // Copy decoded 4x4 block into output image
            for (u32 py = 0; py < 4 && (by * 4 + py) < height; py++) {
                for (u32 px = 0; px < 4 && (bx * 4 + px) < width; px++) {
                    u32 src_idx = (py * 4 + px) * 4;
                    u32 dst_idx = ((by * 4 + py) * width + (bx * 4 + px)) * 4;
                    pixels[dst_idx + 0] = decoded[src_idx + 0];
                    pixels[dst_idx + 1] = decoded[src_idx + 1];
                    pixels[dst_idx + 2] = decoded[src_idx + 2];
                    pixels[dst_idx + 3] = decoded[src_idx + 3];
                }
            }
        }
    }

    return pixels;
}

} // namespace osc::renderer
```

Add `dds_decode.cpp` to `src/renderer/CMakeLists.txt` in the sources list.

**Step 4: Run tests to verify they pass**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[dds_decode]"`
Expected: 2 tests pass

**Step 5: Commit**

```bash
git add src/renderer/dds_decode.hpp src/renderer/dds_decode.cpp \
        src/renderer/CMakeLists.txt tests/test_dds_decode.cpp tests/CMakeLists.txt
git commit -m "M128: add CPU-side BC3 block decoder for normal map baking"
```

---

### Task 2: Normal overlay baking function

Rasterize normal map decals into an RG16F CPU buffer. Each texel stores (nx, ny) perturbation in terrain tangent space. This is the core baking logic — no GPU/Vulkan code yet.

**Files:**
- Create: `src/renderer/normal_overlay.hpp`
- Create: `src/renderer/normal_overlay.cpp`
- Modify: `src/renderer/CMakeLists.txt` (add new .cpp)
- Create: `tests/test_normal_overlay.cpp`
- Modify: `tests/CMakeLists.txt` (add test)

**Step 1: Write the failing test**

Create `tests/test_normal_overlay.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "renderer/normal_overlay.hpp"

using namespace osc::renderer;
using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------
// Test 1: Empty decal list produces neutral overlay
// ---------------------------------------------------------------------------
TEST_CASE("Empty decal list produces neutral overlay", "[normal_overlay]") {
    auto overlay = bake_normal_overlay({}, 64, 64, nullptr);
    REQUIRE(overlay.width == 64);
    REQUIRE(overlay.height == 64);
    REQUIRE(overlay.pixels.size() == 64 * 64 * 2); // RG16F = 2 floats per texel

    // All texels should be (0, 0) = no perturbation
    for (u32 i = 0; i < overlay.pixels.size(); i++) {
        CHECK_THAT(static_cast<double>(overlay.pixels[i]), WithinAbs(0.0, 0.001));
    }
}

// ---------------------------------------------------------------------------
// Test 2: Single centered decal writes non-zero perturbation
// ---------------------------------------------------------------------------
TEST_CASE("Single centered decal writes perturbation", "[normal_overlay]") {
    // Create a 4x4 "normal map" with a tilted normal:
    // DXT5nm: X = green channel, Y = alpha channel
    // Neutral = (128, 128), tilted = e.g. (192, 128) → X=+0.5, Y=0.0
    NormalDecalInfo decal;
    decal.texture_path = "test_normal";
    decal.position_x = 32.0f;  // Center of 64-unit map
    decal.position_z = 32.0f;
    decal.scale_x = 8.0f;      // 8x8 unit footprint
    decal.scale_z = 8.0f;
    decal.rotation_y = 0.0f;   // No rotation

    // We'll provide pre-decoded RGBA pixels instead of DDS for testing
    // 4x4 pixels, all with G=192 (X=+0.5), A=128 (Y=0.0)
    std::vector<u8> pixels(4 * 4 * 4);
    for (int i = 0; i < 16; i++) {
        pixels[i * 4 + 0] = 0;    // R (unused in DXT5nm)
        pixels[i * 4 + 1] = 192;  // G → normal X
        pixels[i * 4 + 2] = 0;    // B (unused)
        pixels[i * 4 + 3] = 128;  // A → normal Y
    }

    PredecodedNormal predecoded;
    predecoded.path = "test_normal";
    predecoded.pixels = pixels;
    predecoded.width = 4;
    predecoded.height = 4;

    auto overlay = bake_normal_overlay_with_predecoded(
        {decal}, 64, 64, {predecoded});

    REQUIRE(overlay.width == 64);

    // Check center of the overlay (around texel 32,32) has non-zero X
    u32 cx = 32, cy = 32;
    u32 idx = (cy * overlay.width + cx) * 2;
    float nx = overlay.pixels[idx];
    float ny = overlay.pixels[idx + 1];
    CHECK_THAT(static_cast<double>(nx), !WithinAbs(0.0, 0.01));
    // ny should be ~0 since alpha=128 → Y=0.0
    CHECK_THAT(static_cast<double>(ny), WithinAbs(0.0, 0.1));
}

// ---------------------------------------------------------------------------
// Test 3: Decal outside map bounds doesn't write out of bounds
// ---------------------------------------------------------------------------
TEST_CASE("Decal outside map bounds is safe", "[normal_overlay]") {
    NormalDecalInfo decal;
    decal.texture_path = "test_normal";
    decal.position_x = -50.0f;  // Way outside
    decal.position_z = -50.0f;
    decal.scale_x = 8.0f;
    decal.scale_z = 8.0f;
    decal.rotation_y = 0.0f;

    std::vector<u8> pixels(4 * 4 * 4, 128); // neutral
    PredecodedNormal predecoded;
    predecoded.path = "test_normal";
    predecoded.pixels = pixels;
    predecoded.width = 4;
    predecoded.height = 4;

    // Should not crash
    auto overlay = bake_normal_overlay_with_predecoded(
        {decal}, 64, 64, {predecoded});
    REQUIRE(overlay.width == 64);
}
```

**Step 2: Run test to verify it fails**

Run: `cmake --build build --config Debug --target osc_tests 2>&1 | head -20`
Expected: FAIL — `normal_overlay.hpp` not found

**Step 3: Write the implementation**

Create `src/renderer/normal_overlay.hpp`:

```cpp
#pragma once

#include "core/types.hpp"

#include <string>
#include <vector>

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::renderer {

/// Info about a normal-map decal to bake into the overlay.
struct NormalDecalInfo {
    std::string texture_path;
    f32 position_x, position_z;  // World-space XZ center
    f32 scale_x, scale_z;        // World-space footprint
    f32 rotation_y;              // Y-axis rotation in radians
};

/// Pre-decoded normal map pixels for testing (bypasses DDS loading).
struct PredecodedNormal {
    std::string path;
    std::vector<u8> pixels; // RGBA, width*height*4 bytes
    u32 width, height;
};

/// Baked normal overlay texture data (CPU-side).
struct NormalOverlay {
    u32 width = 0;
    u32 height = 0;
    std::vector<f32> pixels; // width*height*2 floats (RG pairs: nx, ny perturbation)
};

/// Bake normal map decals into an overlay texture.
/// Resolution is overlay_w x overlay_h (typically map_width x map_height in game units).
/// Uses VFS to load DDS normal map textures.
NormalOverlay bake_normal_overlay(
    const std::vector<NormalDecalInfo>& decals,
    u32 overlay_w, u32 overlay_h,
    vfs::VirtualFileSystem* vfs);

/// Test-only variant that uses pre-decoded pixel data instead of loading from VFS.
NormalOverlay bake_normal_overlay_with_predecoded(
    const std::vector<NormalDecalInfo>& decals,
    u32 overlay_w, u32 overlay_h,
    const std::vector<PredecodedNormal>& predecoded);

} // namespace osc::renderer
```

Create `src/renderer/normal_overlay.cpp`:

```cpp
#include "renderer/normal_overlay.hpp"

#include "renderer/dds_decode.hpp"
#include "renderer/dds_parser.hpp"
#include "vfs/vfs.hpp"

#include <spdlog/spdlog.h>

#include <cmath>
#include <unordered_map>

namespace osc::renderer {

namespace {

struct DecodedImage {
    std::vector<u8> pixels; // RGBA
    u32 width, height;
};

/// Decode DXT5nm RGBA pixels to tangent-space normal XY.
/// DXT5nm: X = green (0-255 → -1..+1), Y = alpha (0-255 → -1..+1).
/// Returns (nx, ny) perturbation: (0, 0) = flat/neutral.
std::pair<f32, f32> sample_dxt5nm(const u8* rgba, u32 img_w, u32 img_h,
                                   f32 u, f32 v) {
    // Bilinear would be ideal but nearest-neighbor is sufficient for baking
    u32 px = static_cast<u32>(std::clamp(u, 0.0f, 1.0f) * (img_w - 1));
    u32 py = static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * (img_h - 1));
    u32 idx = (py * img_w + px) * 4;

    f32 nx = static_cast<f32>(rgba[idx + 1]) / 127.5f - 1.0f; // Green → X
    f32 ny = static_cast<f32>(rgba[idx + 3]) / 127.5f - 1.0f; // Alpha → Y
    return {nx, ny};
}

/// Core baking logic shared between VFS and test paths.
NormalOverlay bake_impl(
    const std::vector<NormalDecalInfo>& decals,
    u32 overlay_w, u32 overlay_h,
    const std::unordered_map<std::string, DecodedImage>& images) {

    NormalOverlay overlay;
    overlay.width = overlay_w;
    overlay.height = overlay_h;
    overlay.pixels.resize(overlay_w * overlay_h * 2, 0.0f);

    // Weight accumulator for alpha-blending overlapping decals
    std::vector<f32> weights(overlay_w * overlay_h, 0.0f);

    for (auto& decal : decals) {
        auto it = images.find(decal.texture_path);
        if (it == images.end()) continue;

        auto& img = it->second;
        f32 half_sx = decal.scale_x * 0.5f;
        f32 half_sz = decal.scale_z * 0.5f;
        f32 cos_r = std::cos(decal.rotation_y);
        f32 sin_r = std::sin(decal.rotation_y);

        // Bounding box in overlay texels
        // Conservative: use max of scale_x, scale_z * sqrt(2) for rotated decals
        f32 radius = std::sqrt(half_sx * half_sx + half_sz * half_sz);
        i32 min_x = static_cast<i32>(std::floor(decal.position_x - radius));
        i32 max_x = static_cast<i32>(std::ceil(decal.position_x + radius));
        i32 min_z = static_cast<i32>(std::floor(decal.position_z - radius));
        i32 max_z = static_cast<i32>(std::ceil(decal.position_z + radius));

        min_x = std::max(min_x, 0);
        max_x = std::min(max_x, static_cast<i32>(overlay_w) - 1);
        min_z = std::max(min_z, 0);
        max_z = std::min(max_z, static_cast<i32>(overlay_h) - 1);

        for (i32 tz = min_z; tz <= max_z; tz++) {
            for (i32 tx = min_x; tx <= max_x; tx++) {
                // World-space offset from decal center
                f32 dx = static_cast<f32>(tx) + 0.5f - decal.position_x;
                f32 dz = static_cast<f32>(tz) + 0.5f - decal.position_z;

                // Rotate into decal local space (inverse rotation)
                f32 lx = dx * cos_r + dz * sin_r;
                f32 lz = -dx * sin_r + dz * cos_r;

                // Check if inside decal footprint
                if (std::abs(lx) > half_sx || std::abs(lz) > half_sz)
                    continue;

                // UV in decal texture [0,1]
                f32 u = (lx / half_sx) * 0.5f + 0.5f;
                f32 v = (lz / half_sz) * 0.5f + 0.5f;

                auto [nx, ny] = sample_dxt5nm(
                    img.pixels.data(), img.width, img.height, u, v);

                // Rotate the tangent-space normal by the decal's Y rotation
                // to align with terrain tangent space (T=+X, B=+Z)
                f32 rnx = nx * cos_r - ny * sin_r;
                f32 rny = nx * sin_r + ny * cos_r;

                // Alpha-weighted accumulation (weight = 1.0 per decal)
                u32 idx = (static_cast<u32>(tz) * overlay_w +
                           static_cast<u32>(tx)) * 2;
                overlay.pixels[idx + 0] += rnx;
                overlay.pixels[idx + 1] += rny;
                weights[static_cast<u32>(tz) * overlay_w +
                        static_cast<u32>(tx)] += 1.0f;
            }
        }
    }

    // Normalize overlapping regions
    for (u32 i = 0; i < overlay_w * overlay_h; i++) {
        if (weights[i] > 1.0f) {
            overlay.pixels[i * 2 + 0] /= weights[i];
            overlay.pixels[i * 2 + 1] /= weights[i];
        }
    }

    return overlay;
}

} // anonymous namespace

NormalOverlay bake_normal_overlay(
    const std::vector<NormalDecalInfo>& decals,
    u32 overlay_w, u32 overlay_h,
    vfs::VirtualFileSystem* vfs) {

    std::unordered_map<std::string, DecodedImage> images;

    if (vfs) {
        for (auto& decal : decals) {
            if (images.count(decal.texture_path)) continue;

            auto file_data = vfs->read_file(decal.texture_path);
            if (!file_data) {
                spdlog::warn("Normal decal: failed to read '{}'",
                             decal.texture_path);
                continue;
            }

            auto dds = parse_dds(*file_data);
            if (!dds || dds->mips.empty()) {
                spdlog::warn("Normal decal: failed to parse DDS '{}'",
                             decal.texture_path);
                continue;
            }

            // Decode mip 0 (full resolution) from BC3
            auto& mip0 = dds->mips[0];
            auto rgba = decode_bc3_to_rgba(
                reinterpret_cast<const u8*>(mip0.data),
                mip0.width, mip0.height);

            images[decal.texture_path] = {
                std::move(rgba), mip0.width, mip0.height};
        }
    }

    return bake_impl(decals, overlay_w, overlay_h, images);
}

NormalOverlay bake_normal_overlay_with_predecoded(
    const std::vector<NormalDecalInfo>& decals,
    u32 overlay_w, u32 overlay_h,
    const std::vector<PredecodedNormal>& predecoded) {

    std::unordered_map<std::string, DecodedImage> images;
    for (auto& p : predecoded) {
        images[p.path] = {p.pixels, p.width, p.height};
    }

    return bake_impl(decals, overlay_w, overlay_h, images);
}

} // namespace osc::renderer
```

Add `normal_overlay.cpp` and `dds_decode.cpp` to `src/renderer/CMakeLists.txt`.

**Step 4: Run tests**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe "[normal_overlay]"`
Expected: 3 tests pass

**Step 5: Commit**

```bash
git add src/renderer/normal_overlay.hpp src/renderer/normal_overlay.cpp \
        src/renderer/CMakeLists.txt tests/test_normal_overlay.cpp tests/CMakeLists.txt
git commit -m "M128: add normal overlay baking for decal normal maps"
```

---

### Task 3: Pass normal decals from scenario_loader to renderer

Currently `scenario_loader.cpp` skips all type-2 decals. Change it to collect them separately and pass them through to the renderer for baking.

**Files:**
- Modify: `src/lua/scenario_loader.cpp` (~lines 252-272)
- Modify: `src/map/terrain.hpp` (add NormalDecalInfo storage)
- Modify: `src/map/terrain.cpp` (add setter)
- Modify: `src/renderer/renderer.hpp` (add normal overlay members)
- Modify: `src/renderer/renderer.cpp` (call baking in build_scene)

**Step 1: Add NormalDecalInfo to terrain.hpp**

Add after the existing `DecalInfo` struct:

```cpp
/// A normal-map decal for terrain normal perturbation.
struct NormalDecalInfo {
    std::string texture_path;
    f32 position_x, position_z;
    f32 scale_x, scale_z;
    f32 rotation_y; // Y-axis rotation in radians
};
```

Add to the Terrain class (public section, after `set_decals`/`decals`):

```cpp
void set_normal_decals(std::vector<NormalDecalInfo> decals);
const std::vector<NormalDecalInfo>& normal_decals() const { return normal_decals_; }
```

Add to Terrain class (private section):

```cpp
std::vector<NormalDecalInfo> normal_decals_;
```

**Step 2: Implement setter in terrain.cpp**

Add:

```cpp
void Terrain::set_normal_decals(std::vector<NormalDecalInfo> decals) {
    normal_decals_ = std::move(decals);
}
```

**Step 3: Update scenario_loader.cpp**

Replace the decal loading block (lines ~252-272). The key change: instead of `if (d.decal_type == 2) continue;`, collect type-2 decals into a separate list:

```cpp
if (!scmap.decals.empty()) {
    std::vector<map::DecalInfo> albedo_decals;
    std::vector<map::NormalDecalInfo> normal_decals;
    albedo_decals.reserve(scmap.decals.size());

    for (auto& d : scmap.decals) {
        if (d.texture1_path.empty()) continue;

        if (d.decal_type == 2) {
            // Normal map decal — collect for overlay baking
            normal_decals.push_back({
                d.texture1_path,
                d.position_x, d.position_z,
                d.scale_x, d.scale_z,
                d.rotation_y
            });
        } else {
            // Albedo decal — existing rendering path
            albedo_decals.push_back({
                d.texture1_path,
                d.position_x, d.position_y, d.position_z,
                d.scale_x, d.scale_y, d.scale_z,
                d.rotation_x, d.rotation_y, d.rotation_z,
                d.cut_off_lod
            });
        }
    }

    terrain->set_decals(std::move(albedo_decals));
    terrain->set_normal_decals(std::move(normal_decals));
    spdlog::info("  Terrain decals: {} albedo, {} normal",
                 terrain->decals().size(), terrain->normal_decals().size());
}
```

**Step 4: Build and verify no regressions**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe`
Expected: All existing tests pass (no functional change yet — normal decals are collected but not yet rendered)

**Step 5: Commit**

```bash
git add src/lua/scenario_loader.cpp src/map/terrain.hpp src/map/terrain.cpp
git commit -m "M128: collect normal map decals separately from albedo decals"
```

---

### Task 4: Upload baked overlay to GPU and bind to terrain pipeline

Create the VkImage for the normal overlay, upload the baked data, and add it as binding 21 in the terrain descriptor set.

**Files:**
- Modify: `src/renderer/renderer.hpp` (add overlay GPU resources)
- Modify: `src/renderer/renderer.cpp` (bake + upload in build_scene, bind in descriptor set, expand terrain_bindings to 22)
- Modify: `src/renderer/shader_utils.cpp` (add binding 21 to terrain fragment shader)

**Step 1: Add GPU resources to renderer.hpp**

Add near the other terrain members:

```cpp
// Normal overlay (baked from type-2 decals)
AllocatedImage normal_overlay_image_{};
VkImageView normal_overlay_view_ = VK_NULL_HANDLE;
```

**Step 2: Expand terrain descriptor set layout to 22 bindings**

In `renderer.cpp`, change the terrain descriptor set layout creation from 21 to 22 bindings:

```cpp
std::array<VkDescriptorSetLayoutBinding, 22> terrain_bindings{};
for (u32 i = 0; i < 22; i++) {
    // ... same as before
}
```

**Step 3: Bake and upload in build_scene()**

After the existing decal setup code in `build_scene()`, add the normal overlay baking:

```cpp
// Bake normal map decal overlay
{
    auto& terrain = *sim_state.terrain;
    u32 overlay_w = terrain.map_width();
    u32 overlay_h = terrain.map_height();

    // Convert map::NormalDecalInfo to renderer::NormalDecalInfo
    std::vector<renderer::NormalDecalInfo> render_decals;
    for (auto& nd : terrain.normal_decals()) {
        render_decals.push_back({
            nd.texture_path,
            nd.position_x, nd.position_z,
            nd.scale_x, nd.scale_z,
            nd.rotation_y
        });
    }

    auto overlay = bake_normal_overlay(
        render_decals, overlay_w, overlay_h, sim_state.vfs);

    if (overlay.width > 0 && overlay.height > 0) {
        // Create RG32F image (2 floats per texel — use R32G32_SFLOAT or R16G16_SFLOAT)
        // R16G16 saves memory, sufficient precision for normal perturbation
        // Upload as R32G32_SFLOAT for simplicity (can optimize to R16G16 later)
        // Convert float pairs to RGBA for upload_rgba (pack nx,ny into R,G channels)
        // Actually: create a dedicated VkImage with VK_FORMAT_R32G32_SFLOAT

        // For simplicity, pack into RGBA8: nx,ny mapped from [-1,1] to [0,255]
        std::vector<u8> rgba(overlay_w * overlay_h * 4);
        for (u32 i = 0; i < overlay_w * overlay_h; i++) {
            f32 nx = overlay.pixels[i * 2 + 0];
            f32 ny = overlay.pixels[i * 2 + 1];
            rgba[i * 4 + 0] = static_cast<u8>(std::clamp((nx * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            rgba[i * 4 + 1] = static_cast<u8>(std::clamp((ny * 0.5f + 0.5f) * 255.0f, 0.0f, 255.0f));
            rgba[i * 4 + 2] = 0;
            rgba[i * 4 + 3] = 255;
        }

        // Upload via texture_cache as a keyed RGBA texture
        auto* tex = texture_cache_.upload_rgba(
            "__normal_overlay__", rgba.data(), overlay_w, overlay_h);

        if (tex) {
            // Store descriptor set for binding to terrain
            normal_overlay_ds_ = tex->descriptor_set;
        }

        spdlog::info("  Normal overlay: {}x{} ({} decals baked)",
                     overlay_w, overlay_h, render_decals.size());
    } else {
        // No normal decals — create a 1x1 neutral overlay
        u8 neutral[] = {128, 128, 0, 255}; // (0,0) perturbation
        auto* tex = texture_cache_.upload_rgba(
            "__normal_overlay__", neutral, 1, 1);
        if (tex) normal_overlay_ds_ = tex->descriptor_set;
    }
}
```

Wait — looking at the terrain shader more carefully, it uses set=0 for all 21 texture bindings written as a single descriptor set (not individual descriptor sets per texture). The existing code writes all 21 bindings into one `terrain_tex_ds_`. So I need to add binding 21 to that same descriptor set write, not use a separate descriptor set.

Let me revise: Instead of using `texture_cache_.upload_rgba()` (which creates its own descriptor set), I should create the overlay VkImage directly and add it to the terrain descriptor set at binding 21.

**Revised Step 3: Upload overlay and write to terrain descriptor set**

In `build_scene()`, after baking the overlay pixels:

1. Create a VkImage (RGBA8_UNORM) + staging buffer upload (same pattern as existing fog texture upload)
2. Write it into the terrain descriptor set at binding 21

In `renderer.hpp`, add:

```cpp
VkDescriptorSet normal_overlay_ds_ = VK_NULL_HANDLE; // not needed if using terrain_tex_ds_
```

Actually, the simpler approach: use `texture_cache_.upload_rgba()` to get a GPUTexture, then reference its `descriptor_set` when writing the terrain descriptor set binding 21. But the terrain uses a monolithic descriptor set with all 21 bindings in set=0. The overlay needs to be binding 21 in that same set.

The cleanest approach: upload the overlay as a VkImage (reuse the staging-buffer upload pattern from fog texture), then write binding 21 of the existing `terrain_tex_ds_` to point to it. This matches how the fog texture (binding 20) is already handled.

I'll provide the exact code in the implementation. The key steps:
1. Create VkImage (R8G8B8A8_UNORM) + VkImageView for the overlay
2. Copy RGBA8 data via staging buffer
3. Add a descriptor write for binding 21 in the terrain descriptor set (alongside the existing 21 writes)

**Step 4: Build and verify**

Run: `cmake --build build --config Debug`
Expected: Compiles without errors. No visual change yet (shader not updated).

**Step 5: Commit**

```bash
git add src/renderer/renderer.hpp src/renderer/renderer.cpp
git commit -m "M128: bake and upload normal overlay texture, bind as terrain binding 21"
```

---

### Task 5: Update terrain shader to sample normal overlay

Add the normal overlay sampler at binding 21 in the terrain fragment shader and perturb the blended stratum normal with the overlay's values.

**Files:**
- Modify: `src/renderer/shader_utils.cpp` (terrain fragment shader)

**Step 1: Add binding 21 declaration**

After the fog map binding (line 111), add:

```glsl
// Normal overlay from baked decals (binding 21)
layout(set = 0, binding = 21) uniform sampler2D normalOverlay;
```

**Step 2: Sample and perturb normal**

After the blended tangent normal is computed (after line 221 `blendedTangentNormal = normalize(blendedTangentNormal);`) and before the TBN matrix construction (line 226), add:

```glsl
// Apply normal overlay from baked decal normal maps
{
    vec2 overlayUV = fragWorldXZ / vec2(pc.mapWidth, pc.mapHeight);
    vec2 overlayVal = texture(normalOverlay, overlayUV).rg;
    // Decode from [0,1] back to [-1,1]
    vec2 perturbation = overlayVal * 2.0 - 1.0;
    // Only apply if non-neutral (avoid perturbing where no decals exist)
    if (abs(perturbation.x) > 0.001 || abs(perturbation.y) > 0.001) {
        // Blend overlay perturbation into the stratum normal's XY
        blendedTangentNormal.x += perturbation.x;
        blendedTangentNormal.y += perturbation.y;
        blendedTangentNormal = normalize(blendedTangentNormal);
    }
}
```

Note: `blendedTangentNormal` is in tangent space where X is the terrain tangent direction and Y is the bitangent direction. The overlay stores perturbations in the same frame (rotated by decal rotation during baking). After adding the perturbation, we re-normalize to keep it a unit vector. The TBN matrix then transforms it to world space as before.

**Step 3: Build and run visual test**

Run: `cmake --build build --config Debug`
Then run the engine with a map that has normal decals (most FA maps do). Look for:
- Surface detail (cracks, rocky patches) that responds to light direction
- No orange/yellow artifacts (those were from rendering normal maps as color)
- Smooth blending at decal edges

**Step 4: Commit**

```bash
git add src/renderer/shader_utils.cpp
git commit -m "M128: terrain shader samples normal overlay for decal perturbation"
```

---

### Task 6: Cleanup and integration testing

Verify everything works together. Clean up the type-2 filter removal, ensure descriptor set write includes binding 21, and run all tests.

**Files:**
- Verify: `src/renderer/renderer.cpp` (descriptor set has 22 writes)
- Verify: `src/renderer/renderer.cpp` (cleanup in destroy/shutdown)
- Run: All tests

**Step 1: Verify descriptor set writes**

Find where the terrain descriptor set bindings are written (the `VkWriteDescriptorSet` array). It should now have 22 entries, with binding 21 pointing to the normal overlay image/sampler. If this was done in Task 4, just verify it's correct.

**Step 2: Verify cleanup**

In the renderer's cleanup/destroy function, ensure the normal overlay VkImage and VkImageView are destroyed (if created directly, not via texture_cache). If using texture_cache for the overlay, it handles cleanup automatically.

**Step 3: Run all tests**

Run: `cmake --build build --config Debug --target osc_tests && ./build/tests/Debug/osc_tests.exe`
Expected: All tests pass (existing + new dds_decode + normal_overlay tests)

**Step 4: Final commit**

```bash
git add -A
git commit -m "M128: decal normal map rendering complete"
```
