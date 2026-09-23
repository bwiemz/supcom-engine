#include "core/image.hpp"

#include <cstdlib>

// stb is vendored as single-header libraries; this TU owns their
// implementations. Their internals trip -Wall/-Wextra, which is not ours to fix.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace osc {

bool write_png(const std::filesystem::path& path, const ImageRGBA8& image) {
    if (image.width == 0 || image.height == 0 ||
        image.pixels.size() != static_cast<size_t>(image.width) * image.height * 4) {
        return false;
    }
    const std::string file = path.string();
    return stbi_write_png(file.c_str(), static_cast<int>(image.width),
                          static_cast<int>(image.height), 4, image.pixels.data(),
                          static_cast<int>(image.width * 4)) != 0;
}

std::optional<ImageRGBA8> read_png(const std::filesystem::path& path) {
    int w = 0, h = 0, channels = 0;
    const std::string file = path.string();
    stbi_uc* data = stbi_load(file.c_str(), &w, &h, &channels, 4);
    if (!data) return std::nullopt;
    ImageRGBA8 image;
    image.width = static_cast<u32>(w);
    image.height = static_cast<u32>(h);
    image.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
    stbi_image_free(data);
    return image;
}

ImageDiff compare_images(const ImageRGBA8& actual, const ImageRGBA8& expected,
                         u8 channel_threshold) {
    ImageDiff diff;
    diff.same_size = actual.width == expected.width &&
                     actual.height == expected.height &&
                     actual.pixels.size() == expected.pixels.size();
    if (!diff.same_size || actual.pixels.empty()) {
        diff.fraction_over_threshold = diff.same_size ? 0.0 : 1.0;
        return diff;
    }

    const size_t pixel_count = actual.pixels.size() / 4;
    u64 total_abs = 0;
    for (size_t p = 0; p < pixel_count; ++p) {
        bool over = false;
        for (size_t c = 0; c < 3; ++c) {
            const int delta = std::abs(static_cast<int>(actual.pixels[p * 4 + c]) -
                                       static_cast<int>(expected.pixels[p * 4 + c]));
            total_abs += static_cast<u64>(delta);
            over = over || delta > channel_threshold;
        }
        if (over) ++diff.pixels_over_threshold;
    }
    diff.fraction_over_threshold =
        static_cast<double>(diff.pixels_over_threshold) / static_cast<double>(pixel_count);
    diff.mean_abs_error =
        static_cast<double>(total_abs) / static_cast<double>(pixel_count * 3);
    return diff;
}

} // namespace osc
