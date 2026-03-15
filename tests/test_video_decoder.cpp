#include <catch2/catch_test_macros.hpp>
#include "video/video_decoder.hpp"

using namespace osc;

TEST_CASE("VideoDecoder basics", "[video]") {
    video::VideoDecoder decoder;

    SECTION("default state is closed") {
        REQUIRE_FALSE(decoder.is_open());
        REQUIRE(decoder.width() == 0);
        REQUIRE(decoder.height() == 0);
    }

    SECTION("open with null data fails gracefully") {
        REQUIRE_FALSE(decoder.open(nullptr, 0));
        REQUIRE_FALSE(decoder.is_open());
    }

    SECTION("open with garbage data fails gracefully") {
        u8 garbage[] = {0xDE, 0xAD, 0xBE, 0xEF};
        REQUIRE_FALSE(decoder.open(garbage, 4));
        REQUIRE_FALSE(decoder.is_open());
    }

    SECTION("decode_next_frame on closed decoder returns false") {
        REQUIRE_FALSE(decoder.decode_next_frame());
    }
}

TEST_CASE("SFD demuxer", "[video]") {
    SECTION("non-SFD data returns empty") {
        u8 data[] = {0x00, 0x01, 0x02, 0x03};
        auto result = video::demux_sfd(data, 4);
        REQUIRE(result.empty());
    }

    SECTION("null data returns empty") {
        auto result = video::demux_sfd(nullptr, 0);
        REQUIRE(result.empty());
    }
}
