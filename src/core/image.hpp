#pragma once

#include "core/types.hpp"

#include <filesystem>
#include <optional>
#include <vector>

namespace osc {

/// Tightly packed 8-bit RGBA image, rows top to bottom.
struct ImageRGBA8 {
    u32 width = 0;
    u32 height = 0;
    std::vector<u8> pixels; ///< width * height * 4 bytes
};

/// Write a PNG. Returns false on I/O failure or an inconsistent image.
bool write_png(const std::filesystem::path& path, const ImageRGBA8& image);

/// Read a PNG (any channel layout) as RGBA8, or nullopt on failure.
std::optional<ImageRGBA8> read_png(const std::filesystem::path& path);

/// Result of comparing two images for golden-image tests. Alpha is ignored:
/// the swapchain's alpha carries no meaning.
struct ImageDiff {
    bool same_size = false;
    u64 pixels_over_threshold = 0;       ///< pixels with any RGB delta > threshold
    double fraction_over_threshold = 1.0; ///< of all pixels; 1.0 if sizes differ
    double mean_abs_error = 0.0;          ///< mean |delta| over all RGB channels
};

/// Compare `actual` against `expected`. A pixel counts as different when any
/// of its RGB channels differs by more than `channel_threshold`, which
/// absorbs driver-to-driver rounding while catching real regressions.
ImageDiff compare_images(const ImageRGBA8& actual, const ImageRGBA8& expected,
                         u8 channel_threshold);

} // namespace osc
