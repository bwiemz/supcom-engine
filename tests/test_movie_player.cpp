#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "video/movie_player.hpp"

#include <cstring>
#include <vector>

using Catch::Matchers::WithinAbs;
using osc::u8;
using osc::video::sofdec_duration;
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

/// Add an MPEG-1 sequence header at `at` (FA's movies: 6174) with frame
/// rate code `rate_code` (320x240, square pixels).
void add_sequence_header(std::vector<u8>& file, size_t at, u8 rate_code) {
    const u8 header[] = {0x00, 0x00, 0x01, 0xB3,
                         0x14, 0x00, 0xF0, static_cast<u8>(0x10 | rate_code)};
    std::memcpy(file.data() + at, header, sizeof(header));
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

TEST_CASE("A Sofdec file's length, as GetMovieDuration gives it", "[video]") {
    // AllyCom.sfd: 150 frames at 29.97 fps.
    auto ally = sofdec_file(4, 2048 + 0x20, 150);
    add_sequence_header(ally, 6174, 4);
    CHECK_THAT(sofdec_duration(ally.data(), ally.size()), WithinAbs(150.0 / 29.97, 1e-4));
    // aeon_load.sfd: 180 frames at 30.
    auto load = sofdec_file(4, 2048 + 0x20, 180);
    add_sequence_header(load, 6174, 5);
    CHECK_THAT(sofdec_duration(load.data(), load.size()), WithinAbs(6.0, 1e-4));
    // The first sequence header counts.
    add_sequence_header(load, 7000, 3);
    CHECK_THAT(sofdec_duration(load.data(), load.size()), WithinAbs(6.0, 1e-4));

    // No sequence header, a forbidden or reserved rate code, one cut off,
    // or no Sofdec header: 0.
    auto bare = sofdec_file(4, 2048 + 0x20, 150);
    CHECK(sofdec_duration(bare.data(), bare.size()) == 0.0f);
    for (const u8 code : {u8{0}, u8{9}, u8{15}}) {
        auto bad = sofdec_file(4, 2048 + 0x20, 150);
        add_sequence_header(bad, 6174, code);
        CHECK(sofdec_duration(bad.data(), bad.size()) == 0.0f);
    }
    auto cut = sofdec_file(4, 2048 + 0x20, 150);
    add_sequence_header(cut, 6174, 4);
    CHECK(sofdec_duration(cut.data(), 6174 + 7) == 0.0f);
    CHECK(sofdec_duration(cut.data(), 6174 + 8) > 0.0f);
    std::vector<u8> plain(8192, 0);
    add_sequence_header(plain, 6174, 4);
    CHECK(sofdec_duration(plain.data(), plain.size()) == 0.0f);
}
