#pragma once

#include <cstddef>
#include <cstring>
#include <iterator>
#include <vector>

namespace osc::test {

/// A 16x16 movie of `frames` (2 to 120) black frames at 30 fps, laid out
/// as FA's .sfd files are: an MPEG-1 program stream whose first 2 KiB pack
/// holds the video, and whose second holds Sofdec's header, with the frame
/// count, in a private stream. The video is ffmpeg's mpeg1video (an
/// I-frame, then P-frames of skipped macroblocks); with 30 frames the first
/// pack is ffmpeg's output byte for byte.
inline std::vector<char> tiny_movie(int frames) {
    using byte = unsigned char;
    constexpr std::size_t kBlock = 2048;
    const auto hi = [](std::size_t v) { return static_cast<byte>(v >> 8); };
    const auto lo = [](std::size_t v) { return static_cast<byte>(v & 0xFF); };

    std::vector<byte> es = {
        0x00, 0x00, 0x01, 0xB3, 0x01, 0x00, 0x10, 0x15, 0xFF, 0xFF, 0xE0, 0x18, // sequence
        0x00, 0x00, 0x01, 0xB8, 0x00, 0x08, 0x00, 0x40,                         // group
        0x00, 0x00, 0x01, 0x00, 0x00, 0x0F, 0xFF, 0xF8,                         // picture 0: I
        0x00, 0x00, 0x01, 0x01, 0x13, 0xF8, 0x7D, 0x29, 0x48, 0x88,             // its slice
    };
    for (int n = 1; n < frames; ++n) {
        // Picture n: P, its temporal reference the first 10 bits; then a
        // slice of its one macroblock, skipped.
        const auto tr = static_cast<unsigned>(n);
        const auto tr_high = static_cast<byte>(tr >> 2);
        const auto tr_low = static_cast<byte>(((tr & 3u) << 6) | 0x17u);
        const byte picture[] = {0x00, 0x00, 0x01, 0x00, tr_high, tr_low, 0xFF, 0xF8,
                                0x80, 0x00, 0x00, 0x01, 0x01,    0x12,   0x70};
        es.insert(es.end(), std::begin(picture), std::end(picture));
    }

    const byte pack[] = {0x00, 0x00, 0x01, 0xBA, 0x21, 0x00, 0x01, 0x00, 0x01, 0xC3, 0x33, 0x67};
    std::vector<byte> m(std::begin(pack), std::end(pack));
    // The system header (one video stream), then the video's packet: its
    // PTS and DTS, then the whole stream
    const byte system[] = {0x00, 0x00, 0x01, 0xBB, 0x00, 0x09, 0xC3, 0x33,
                           0x67, 0x00, 0x21, 0xFF, 0xE0, 0xE0, 0xE6};
    m.insert(m.end(), std::begin(system), std::end(system));
    const std::size_t video = 10 + es.size();
    const byte video_header[] = {0x00, 0x00, 0x01, 0xE0, hi(video), lo(video), 0x31, 0x00,
                                 0x03, 0x77, 0x01, 0x11, 0x00,      0x03,      0x5F, 0x91};
    m.insert(m.end(), std::begin(video_header), std::end(video_header));
    m.insert(m.end(), es.begin(), es.end());
    // A padding packet fills the pack
    const std::size_t pad = kBlock - m.size() - 6;
    const byte padding[] = {0x00, 0x00, 0x01, 0xBE, hi(pad), lo(pad), 0x0F};
    m.insert(m.end(), std::begin(padding), std::end(padding));
    m.resize(kBlock, byte{0xFF});

    // The second pack: a private stream with Sofdec's header name 0x20 into
    // the block, and the frame count 0xA0 past the name
    const std::size_t second = m.size();
    m.insert(m.end(), std::begin(pack), std::end(pack));
    const std::size_t priv = kBlock - sizeof(pack) - 6;
    const byte private_header[] = {0x00, 0x00, 0x01, 0xBF, hi(priv), lo(priv)};
    m.insert(m.end(), std::begin(private_header), std::end(private_header));
    m.resize(2 * kBlock, byte{0x00});
    std::memcpy(m.data() + second + 0x20, "SofdecStream", 12);
    for (std::size_t i = 0; i < 4; ++i)
        m[second + 0x20 + 0xA0 + i] = static_cast<byte>(static_cast<unsigned>(frames) >> (8 * i));
    return std::vector<char>(m.begin(), m.end());
}

} // namespace osc::test
