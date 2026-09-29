// The lobby's connections' wire framing: u32-length messages, handed out
// whole however they arrive, and a hostile length refused.

#include <catch2/catch_test_macros.hpp>

#include "sim/net_transport.hpp"

#include <vector>

TEST_CASE("Wire framing hands out whole messages, however they arrive", "[net]") {
    using osc::sim::extract_wire_frames;
    const auto framed = [](std::vector<osc::u8> msg) {
        const auto len = static_cast<osc::u32>(msg.size());
        std::vector<osc::u8> out{static_cast<osc::u8>(len), static_cast<osc::u8>(len >> 8),
                                 static_cast<osc::u8>(len >> 16), static_cast<osc::u8>(len >> 24)};
        out.insert(out.end(), msg.begin(), msg.end());
        return out;
    };
    std::vector<osc::u8> wire = framed({1, 2, 3});
    const auto second = framed({4, 5});
    wire.insert(wire.end(), second.begin(), second.end());

    // Byte by byte: a message is handed out only once it is whole.
    std::vector<osc::u8> buf;
    std::vector<std::vector<osc::u8>> out;
    for (osc::u8 b : wire) {
        buf.push_back(b);
        REQUIRE(extract_wire_frames(buf, out));
    }
    REQUIRE(out.size() == 2);
    CHECK(out[0] == std::vector<osc::u8>{1, 2, 3});
    CHECK(out[1] == std::vector<osc::u8>{4, 5});
    CHECK(buf.empty());
}

TEST_CASE("A peer announcing an oversized message is refused before its body", "[net]") {
    using osc::sim::extract_wire_frames;
    using osc::sim::kMaxWireMessage;
    // A forged header claims 0xF0000000 bytes: refused at once, rather than
    // buffering while it trickles in.
    std::vector<osc::u8> buf{0x00, 0x00, 0x00, 0xF0, 1, 2, 3};
    std::vector<std::vector<osc::u8>> out;
    CHECK_FALSE(extract_wire_frames(buf, out));
    CHECK(out.empty());

    // The largest allowed message waits for its body.
    const osc::u32 max = kMaxWireMessage;
    buf = {static_cast<osc::u8>(max), static_cast<osc::u8>(max >> 8),
           static_cast<osc::u8>(max >> 16), static_cast<osc::u8>(max >> 24), 9};
    CHECK(extract_wire_frames(buf, out));
    CHECK(out.empty());
    CHECK(buf.size() == 5);
}
