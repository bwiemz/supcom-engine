#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/image.hpp"

#include <filesystem>
#include <random>

using namespace osc;
using Catch::Matchers::WithinAbs;

namespace {

ImageRGBA8 solid(u32 w, u32 h, u8 r, u8 g, u8 b) {
    ImageRGBA8 img;
    img.width = w;
    img.height = h;
    img.pixels.resize(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < img.pixels.size(); i += 4) {
        img.pixels[i] = r;
        img.pixels[i + 1] = g;
        img.pixels[i + 2] = b;
        img.pixels[i + 3] = 255;
    }
    return img;
}

} // namespace

TEST_CASE("identical images have zero difference", "[image]") {
    auto a = solid(10, 10, 10, 20, 30);
    auto diff = compare_images(a, a, 16);
    REQUIRE(diff.same_size);
    CHECK(diff.pixels_over_threshold == 0);
    CHECK_THAT(diff.fraction_over_threshold, WithinAbs(0.0, 1e-12));
    CHECK_THAT(diff.mean_abs_error, WithinAbs(0.0, 1e-12));
}

TEST_CASE("one changed pixel in a hundred is a 1% difference", "[image]") {
    auto a = solid(10, 10, 100, 100, 100);
    auto b = a;
    b.pixels[0] = 255; // red channel of pixel (0,0): delta 155
    auto diff = compare_images(a, b, 16);
    REQUIRE(diff.same_size);
    CHECK(diff.pixels_over_threshold == 1);
    CHECK_THAT(diff.fraction_over_threshold, WithinAbs(0.01, 1e-12));
    // Mean over all RGB channels of all pixels: 155 / (100 * 3).
    CHECK_THAT(diff.mean_abs_error, WithinAbs(155.0 / 300.0, 1e-9));
}

TEST_CASE("differences at or below the threshold are tolerated", "[image]") {
    auto a = solid(4, 4, 100, 100, 100);
    auto b = solid(4, 4, 116, 84, 100); // deltas exactly 16
    auto diff = compare_images(a, b, 16);
    CHECK(diff.pixels_over_threshold == 0);
    CHECK(diff.mean_abs_error > 0.0);
}

TEST_CASE("alpha is ignored when comparing", "[image]") {
    auto a = solid(2, 2, 1, 2, 3);
    auto b = a;
    b.pixels[3] = 0;
    CHECK(compare_images(a, b, 0).pixels_over_threshold == 0);
}

TEST_CASE("images of different size never match", "[image]") {
    auto diff = compare_images(solid(4, 4, 0, 0, 0), solid(4, 5, 0, 0, 0), 16);
    CHECK_FALSE(diff.same_size);
    CHECK_THAT(diff.fraction_over_threshold, WithinAbs(1.0, 1e-12));
}

TEST_CASE("PNG write/read round-trips pixels", "[image]") {
    std::random_device rd;
    auto path = std::filesystem::temp_directory_path() /
                ("osc_image_test_" + std::to_string(rd()) + ".png");
    auto a = solid(7, 3, 12, 34, 56);
    a.pixels[5] = 200;
    REQUIRE(write_png(path, a));
    auto b = read_png(path);
    std::filesystem::remove(path);
    REQUIRE(b.has_value());
    CHECK(b->width == 7);
    CHECK(b->height == 3);
    CHECK(b->pixels == a.pixels);
    CHECK_FALSE(read_png(path).has_value()); // gone
}
