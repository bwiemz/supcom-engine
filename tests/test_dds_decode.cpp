#include <catch2/catch_test_macros.hpp>

#include "renderer/dds_decode.hpp"

#include <cstring>

using namespace osc;
using namespace osc::renderer;

// Helper: build a BC3 block from explicit alpha and color sub-blocks
static void make_bc3_block(const u8 alpha_block[8], const u8 color_block[8], u8 out[16]) {
    std::memcpy(out, alpha_block, 8);
    std::memcpy(out + 8, color_block, 8);
}

TEST_CASE("decode_bc3_block: solid white opaque", "[dds_decode]") {
    // Alpha block: a0=255, a1=255, all indices = 0 (use a0 = 255)
    u8 alpha_blk[8] = {255, 255, 0, 0, 0, 0, 0, 0};

    // Color block: c0 = white (0xFFFF in RGB565), c1 = white, all indices = 0
    // RGB565 white: R=31, G=63, B=31 -> 0xFFFF
    u8 color_blk[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};

    u8 block[16];
    make_bc3_block(alpha_blk, color_blk, block);

    u8 pixels[64];
    decode_bc3_block(block, pixels);

    for (u32 i = 0; i < 16; ++i) {
        INFO("pixel " << i);
        CHECK(pixels[i * 4 + 0] == 255); // R
        CHECK(pixels[i * 4 + 1] == 255); // G
        CHECK(pixels[i * 4 + 2] == 255); // B
        CHECK(pixels[i * 4 + 3] == 255); // A
    }
}

TEST_CASE("decode_bc3_block: neutral normal (DXT5nm G=128 A=128)", "[dds_decode]") {
    // DXT5nm encodes normal X in alpha, normal Y in green.
    // Neutral normal = (0,0,1) encoded as G=128, A=128.

    // Alpha block: a0=128, a1=128, all indices = 0 -> all alpha = 128
    u8 alpha_blk[8] = {128, 128, 0, 0, 0, 0, 0, 0};

    // Color block: we want G=128, R and B don't matter much for DXT5nm
    // but let's encode a solid color with G≈128.
    // RGB565 with R=0, G=32 (out of 63), B=0:
    // G=32 in 6-bit -> expanded: (32<<2)|(32>>4) = 128|2 = 130 ≈ 128
    // Actually: G6=32 -> (32<<2)|(32>>4) = 128+2 = 130. Close enough.
    // Let's use G6=31: (31<<2)|(31>>4) = 124+1 = 125. Not great.
    // G6=32 gives 130. That's the closest to 128 in RGB565.
    // RGB565 value: (0 << 11) | (32 << 5) | 0 = 0x0400
    u8 color_blk[8] = {0x00, 0x04, 0x00, 0x04, 0, 0, 0, 0};

    u8 block[16];
    make_bc3_block(alpha_blk, color_blk, block);

    u8 pixels[64];
    decode_bc3_block(block, pixels);

    for (u32 i = 0; i < 16; ++i) {
        INFO("pixel " << i);
        // Green should be ~128 (actually 130 due to RGB565 quantization)
        CHECK(pixels[i * 4 + 1] >= 125);
        CHECK(pixels[i * 4 + 1] <= 132);
        // Alpha should be exactly 128
        CHECK(pixels[i * 4 + 3] == 128);
    }
}

TEST_CASE("decode_bc3_to_rgba: 4x4 single block", "[dds_decode]") {
    // Single block = 4x4 image, solid black with alpha=200
    u8 alpha_blk[8] = {200, 200, 0, 0, 0, 0, 0, 0};
    u8 color_blk[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // black

    u8 block[16];
    make_bc3_block(alpha_blk, color_blk, block);

    auto pixels = decode_bc3_to_rgba(block, 4, 4);
    REQUIRE(pixels.size() == 4 * 4 * 4);

    for (u32 i = 0; i < 16; ++i) {
        CHECK(pixels[i * 4 + 0] == 0);   // R
        CHECK(pixels[i * 4 + 1] == 0);   // G
        CHECK(pixels[i * 4 + 2] == 0);   // B
        CHECK(pixels[i * 4 + 3] == 200); // A
    }
}

TEST_CASE("decode_bc3_to_rgba: non-multiple-of-4 dimensions", "[dds_decode]") {
    // 5x5 image = 2x2 blocks = 4 blocks
    // All solid white opaque
    u8 alpha_blk[8] = {255, 255, 0, 0, 0, 0, 0, 0};
    u8 color_blk[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};

    u8 blocks[4 * 16]; // 4 blocks
    for (u32 i = 0; i < 4; ++i) {
        make_bc3_block(alpha_blk, color_blk, blocks + i * 16);
    }

    auto pixels = decode_bc3_to_rgba(blocks, 5, 5);
    REQUIRE(pixels.size() == 5 * 5 * 4);

    // Check corner pixels
    // Top-left (0,0)
    CHECK(pixels[0] == 255);
    CHECK(pixels[3] == 255);
    // Bottom-right (4,4)
    u32 off = (4 * 5 + 4) * 4;
    CHECK(pixels[off + 0] == 255);
    CHECK(pixels[off + 3] == 255);
}
