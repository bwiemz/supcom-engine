#include "ui/texture_alpha.hpp"

#include <array>
#include <cstring>

namespace osc::ui {

namespace {

constexpr size_t kHeaderSize = 128;
constexpr u32 kMagic = 0x20534444;
constexpr u32 kFourCC = 0x4;
constexpr u32 kAlphaPixels = 0x1;
constexpr u32 kRgb = 0x40;
/// The largest side a hit-test mask is made for (FA's UI textures are at
/// most 2048 across); past it the texture has no mask.
constexpr u32 kMaxSide = 16384;

template <typename T> T read(const char* data, size_t offset) {
    T value;
    std::memcpy(&value, data + offset, sizeof(T));
    return value;
}

std::array<bool, 16> dxt1_block(const char* block) {
    const auto c0 = read<u16>(block, 0);
    const auto c1 = read<u16>(block, 2);
    const auto indices = read<u32>(block, 4);
    std::array<bool, 16> out{};
    for (u32 i = 0; i < 16; ++i) {
        out[i] = c0 > c1 || ((indices >> (2 * i)) & 3) != 3;
    }
    return out;
}

std::array<bool, 16> dxt3_block(const char* block) {
    const auto alphas = read<u64>(block, 0);
    std::array<bool, 16> out{};
    for (u32 i = 0; i < 16; ++i) {
        out[i] = ((alphas >> (4 * i)) & 0xF) != 0;
    }
    return out;
}

std::array<bool, 16> dxt5_block(const char* block) {
    const u32 a0 = static_cast<u8>(block[0]);
    const u32 a1 = static_cast<u8>(block[1]);
    std::array<u32, 8> table{a0, a1};
    if (a0 > a1) {
        for (u32 i = 1; i < 7; ++i) {
            table[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
        }
    } else {
        for (u32 i = 1; i < 5; ++i) {
            table[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        }
        table[6] = 0;
        table[7] = 255;
    }
    u64 indices = 0;
    std::memcpy(&indices, block + 2, 6);
    std::array<bool, 16> out{};
    for (u32 i = 0; i < 16; ++i) {
        out[i] = table[(indices >> (3 * i)) & 7] != 0;
    }
    return out;
}

} // namespace

std::optional<AlphaMask> AlphaMask::from_dds(const std::vector<char>& file) {
    if (file.size() < kHeaderSize || read<u32>(file.data(), 0) != kMagic) {
        return std::nullopt;
    }
    const auto height = read<u32>(file.data(), 12);
    const auto width = read<u32>(file.data(), 16);
    const auto flags = read<u32>(file.data(), 80);
    const auto fourcc = read<u32>(file.data(), 84);
    if (width == 0 || height == 0 || width > kMaxSide || height > kMaxSide) {
        return std::nullopt;
    }
    const char* data = file.data() + kHeaderSize;
    const u64 available = file.size() - kHeaderSize;

    // The format and the payload it needs, both before the mask is made:
    // a file that declares more texels than it holds is refused, so the
    // mask is never larger than the file's own data allows.
    std::array<bool, 16> (*decode)(const char*) = nullptr;
    u64 block_bytes = 0;
    u32 alpha_bits = 0;
    const u64 texels = static_cast<u64>(width) * height;
    const u64 blocks_wide = (static_cast<u64>(width) + 3) / 4;
    const u64 blocks_high = (static_cast<u64>(height) + 3) / 4;
    u64 needed = 0;
    if (flags & kFourCC) {
        if (fourcc == 0x31545844) {
            decode = dxt1_block;
            block_bytes = 8;
        } else if (fourcc == 0x33545844) {
            decode = dxt3_block;
            block_bytes = 16;
        } else if (fourcc == 0x35545844) {
            decode = dxt5_block;
            block_bytes = 16;
        } else {
            return std::nullopt;
        }
        needed = blocks_wide * blocks_high * block_bytes;
    } else if ((flags & kRgb) && read<u32>(file.data(), 88) == 32) {
        alpha_bits = (flags & kAlphaPixels) ? read<u32>(file.data(), 104) : 0;
        needed = texels * 4;
    } else {
        return std::nullopt;
    }
    if (available < needed) {
        return std::nullopt;
    }

    AlphaMask mask;
    mask.width_ = width;
    mask.height_ = height;
    mask.bits_.assign(static_cast<size_t>(texels), false);
    if (decode) {
        for (u64 by = 0; by < blocks_high; ++by) {
            for (u64 bx = 0; bx < blocks_wide; ++bx) {
                const auto block = decode(data + (by * blocks_wide + bx) * block_bytes);
                for (u32 i = 0; i < 16; ++i) {
                    const u64 x = bx * 4 + i % 4;
                    const u64 y = by * 4 + i / 4;
                    if (x < width && y < height) {
                        mask.bits_[static_cast<size_t>(y * width + x)] = block[i];
                    }
                }
            }
        }
        return mask;
    }
    for (u64 i = 0; i < texels; ++i) {
        mask.bits_[static_cast<size_t>(i)] =
            alpha_bits == 0 || (read<u32>(data, static_cast<size_t>(i * 4)) & alpha_bits) != 0;
    }
    return mask;
}

bool AlphaMask::opaque(i64 x, i64 y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return false;
    }
    return bits_[static_cast<size_t>(y) * width_ + static_cast<size_t>(x)];
}

} // namespace osc::ui
