#include <catch2/catch_test_macros.hpp>

#include "video/movie_player.hpp"

#include <cstring>
#include <vector>

using osc::u8;
using osc::video::sofdec_frame_count;

namespace {

/// A file of `blocks` 2 KiB blocks with Sofdec's header name at `at` and a
/// frame count 0xA0 past it (FA's movies: the second block, 0x20 in).
std::vector<u8> sofdec_file(size_t blocks, size_t at, osc::u32 frames) {
    std::vector<u8> file(blocks * 2048, 0);
    std::memcpy(file.data() + at, "SofdecStream", 12);
    for (int i = 0; i < 4; ++i) file[at + 0xA0 + i] = static_cast<u8>(frames >> (8 * i));
    return file;
}

} // namespace

TEST_CASE("A Sofdec header's frame count, where Moho reads it", "[video]") {
    auto thq = sofdec_file(3, 2048 + 0x20, 201);
    CHECK(sofdec_frame_count(thq.data(), thq.size()) == 201);
    // The third block is looked in too.
    auto third = sofdec_file(4, 4096 + 0x20, 6211);
    CHECK(sofdec_frame_count(third.data(), third.size()) == 6211);
    // The first block is not (Moho starts at the second), nor past the third.
    auto first = sofdec_file(3, 0x20, 99);
    CHECK(sofdec_frame_count(first.data(), first.size()) == -1);
    auto fourth = sofdec_file(5, 6144 + 0x20, 99);
    CHECK(sofdec_frame_count(fourth.data(), fourth.size()) == -1);
    // Nothing there, or a file too short to hold it.
    std::vector<u8> plain(8192, 0);
    CHECK(sofdec_frame_count(plain.data(), plain.size()) == -1);
    CHECK(sofdec_frame_count(thq.data(), 3000) == -1);
    auto zero = sofdec_file(3, 2048 + 0x20, 0);
    CHECK(sofdec_frame_count(zero.data(), zero.size()) == -1);
}
