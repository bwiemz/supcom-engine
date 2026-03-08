#include "renderer/dds_parser.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>

namespace osc::renderer {

// DDS magic number: "DDS " = 0x20534444
static constexpr u32 DDS_MAGIC = 0x20534444;

// FourCC codes
static constexpr u32 FOURCC_DXT1 = 0x31545844; // 'DXT1'
static constexpr u32 FOURCC_DXT3 = 0x33545844; // 'DXT3'
static constexpr u32 FOURCC_DXT5 = 0x35545844; // 'DXT5'

// DDS header offsets (relative to byte 4, i.e. after magic)
// DDS_HEADER:
//   dwSize(4) dwFlags(4) dwHeight(4) dwWidth(4) dwPitchOrLinearSize(4)
//   dwDepth(4) dwMipMapCount(4) dwReserved1[11](44)
//   ddspf { dwSize(4) dwFlags(4) dwFourCC(4) dwRGBBitCount(4)
//           dwRBitMask(4) dwGBitMask(4) dwBBitMask(4) dwABitMask(4) }(32)
//   ...
// Total DDS_HEADER = 124 bytes, so file header = 4 (magic) + 124 = 128 bytes

static constexpr size_t HEADER_SIZE = 128; // magic + DDS_HEADER

// Offsets from file start
static constexpr size_t OFF_HEIGHT    = 4 + 8;    // byte 12
static constexpr size_t OFF_WIDTH     = 4 + 12;   // byte 16
static constexpr size_t OFF_MIPCOUNT  = 4 + 24;   // byte 28
static constexpr size_t OFF_PF_FLAGS  = 4 + 72 + 4; // byte 80 (ddspf.dwFlags)
static constexpr size_t OFF_FOURCC    = 4 + 72 + 8; // byte 84 (ddspf.dwFourCC)
static constexpr size_t OFF_RGBBITCNT = 4 + 72 + 12; // byte 88 (ddspf.dwRGBBitCount)
static constexpr size_t OFF_RBITMASK  = 4 + 72 + 16; // byte 92 (ddspf.dwRBitMask)
static constexpr size_t OFF_GBITMASK  = 4 + 72 + 20; // byte 96 (ddspf.dwGBitMask)
static constexpr size_t OFF_BBITMASK  = 4 + 72 + 24; // byte 100 (ddspf.dwBBitMask)
static constexpr size_t OFF_ABITMASK  = 4 + 72 + 28; // byte 104 (ddspf.dwABitMask)

// Pixel format flags
static constexpr u32 DDPF_FOURCC = 0x4;
static constexpr u32 DDPF_RGB    = 0x40;

static u32 read_u32(const char* data, size_t offset) {
    u32 val;
    std::memcpy(&val, data + offset, 4);
    return val;
}

std::optional<DDSTexture> parse_dds(const std::vector<char>& file_data) {
    if (file_data.size() < HEADER_SIZE) {
        spdlog::debug("DDS: file too small ({} bytes)", file_data.size());
        return std::nullopt;
    }

    const char* raw = file_data.data();

    // Validate magic
    u32 magic = read_u32(raw, 0);
    if (magic != DDS_MAGIC) {
        spdlog::debug("DDS: bad magic 0x{:08X}", magic);
        return std::nullopt;
    }

    u32 height   = read_u32(raw, OFF_HEIGHT);
    u32 width    = read_u32(raw, OFF_WIDTH);
    u32 mip_raw  = read_u32(raw, OFF_MIPCOUNT);
    u32 fourcc   = read_u32(raw, OFF_FOURCC);

    // Detect format: compressed (FourCC) or uncompressed (RGB flags)
    u32 pf_flags = read_u32(raw, OFF_PF_FLAGS);
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 bytes_per_block = 0;
    bool compressed = false;

    if (pf_flags & DDPF_FOURCC) {
        compressed = true;
        switch (fourcc) {
            case FOURCC_DXT1:
                format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
                bytes_per_block = 8;
                break;
            case FOURCC_DXT3:
                format = VK_FORMAT_BC2_UNORM_BLOCK;
                bytes_per_block = 16;
                break;
            case FOURCC_DXT5:
                format = VK_FORMAT_BC3_UNORM_BLOCK;
                bytes_per_block = 16;
                break;
            default:
                spdlog::debug("DDS: unsupported FourCC 0x{:08X}", fourcc);
                return std::nullopt;
        }
    } else if (pf_flags & DDPF_RGB) {
        // Uncompressed RGB/RGBA — used by SCMAP blend maps
        compressed = false;
        u32 bpp = read_u32(raw, OFF_RGBBITCNT);
        if (bpp == 32) {
            // Check bit masks to determine channel order
            u32 r_mask = read_u32(raw, OFF_RBITMASK);
            u32 g_mask = read_u32(raw, OFF_GBITMASK);
            u32 b_mask = read_u32(raw, OFF_BBITMASK);
            u32 a_mask = read_u32(raw, OFF_ABITMASK);
            spdlog::debug("DDS: 32bpp masks R=0x{:08X} G=0x{:08X} B=0x{:08X} A=0x{:08X}",
                          r_mask, g_mask, b_mask, a_mask);
            // BGRA byte order (DirectX default): R=0x00FF0000, B=0x000000FF
            if (r_mask == 0x00FF0000 && b_mask == 0x000000FF) {
                format = VK_FORMAT_B8G8R8A8_UNORM;
            } else {
                format = VK_FORMAT_R8G8B8A8_UNORM;
            }
            bytes_per_block = 4; // bytes per pixel
        } else if (bpp == 24) {
            // 24-bit RGB — we'll need to expand to RGBA during upload
            // For now, treat as unsupported and handle it later
            spdlog::debug("DDS: 24-bit RGB not yet supported");
            return std::nullopt;
        } else if (bpp == 8) {
            format = VK_FORMAT_R8_UNORM;
            bytes_per_block = 1;
        } else {
            spdlog::debug("DDS: unsupported uncompressed bpp={}", bpp);
            return std::nullopt;
        }
    } else {
        spdlog::debug("DDS: unsupported pixel format flags 0x{:08X}", pf_flags);
        return std::nullopt;
    }

    if (width == 0 || height == 0) {
        spdlog::debug("DDS: zero dimensions {}x{}", width, height);
        return std::nullopt;
    }

    // Treat 0 as 1 mip level
    u32 mip_count = (mip_raw > 0) ? mip_raw : 1;

    DDSTexture tex;
    tex.format = format;
    tex.width = width;
    tex.height = height;
    tex.mip_count = mip_count;
    tex.mips.reserve(mip_count);

    size_t offset = HEADER_SIZE;
    u32 mw = width;
    u32 mh = height;

    for (u32 i = 0; i < mip_count; i++) {
        u32 mip_size;
        if (compressed) {
            u32 block_w = std::max(1u, (mw + 3) / 4);
            u32 block_h = std::max(1u, (mh + 3) / 4);
            mip_size = block_w * block_h * bytes_per_block;
        } else {
            mip_size = mw * mh * bytes_per_block; // bytes_per_block = bytes per pixel
        }

        if (offset + mip_size > file_data.size()) {
            spdlog::debug("DDS: mip {} data truncated (need {} at offset {}, file={})",
                           i, mip_size, offset, file_data.size());
            // Use whatever mips we got
            if (tex.mips.empty()) return std::nullopt;
            tex.mip_count = static_cast<u32>(tex.mips.size());
            break;
        }

        DDSMipLevel level;
        level.data = raw + offset;
        level.width = mw;
        level.height = mh;
        level.size = mip_size;
        tex.mips.push_back(level);

        offset += mip_size;
        mw = std::max(1u, mw / 2);
        mh = std::max(1u, mh / 2);
    }

    spdlog::debug("DDS: {}x{} format=0x{:X} mips={}", width, height,
                   static_cast<u32>(format), tex.mip_count);
    return tex;
}

} // namespace osc::renderer
