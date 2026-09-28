#include "video/movie_player.hpp"

#include <atomic>
#include <cmath>
#include <cstring>
#include <string_view>

namespace osc::video {

namespace {

std::atomic<u64> g_next_movie_id{1};

u32 read_u32_le(const u8* p) {
    return static_cast<u32>(p[0]) | static_cast<u32>(p[1]) << 8 | static_cast<u32>(p[2]) << 16 |
           static_cast<u32>(p[3]) << 24;
}

} // namespace

i32 sofdec_frame_count(const u8* data, size_t size) {
    // Moho looks in the second and third 2 KiB blocks (a pack, then the
    // SofdecStream header in a private stream). The total frame count sits
    // 0xA0 past the header's name.
    constexpr std::string_view kMarker = "SofdecStream";
    constexpr size_t kBlock = 2048;
    constexpr size_t kCountOffset = 0xA0;
    for (size_t block = 1; block <= 2; ++block) {
        const size_t start = block * kBlock;
        if (start + kBlock > size) break;
        // The name follows the pack and private-stream headers.
        for (size_t at = start; at + kMarker.size() <= start + 64; ++at) {
            if (std::memcmp(data + at, kMarker.data(), kMarker.size()) != 0) continue;
            if (at + kCountOffset + 4 > start + kBlock) return -1;
            const u32 count = read_u32_le(data + at + kCountOffset);
            return count > 0 && count < 0x7fffffffU ? static_cast<i32>(count) : -1;
        }
    }
    return -1;
}

bool MoviePlayer::open(std::vector<char> file) {
    const i32 header_count =
        sofdec_frame_count(reinterpret_cast<const u8*>(file.data()), file.size());
    if (!decoder_.open_file(std::move(file))) return false;
    frame_count_ = header_count > 0
                       ? header_count
                       : static_cast<i32>(std::lround(decoder_.duration() * decoder_.framerate()));
    id_ = g_next_movie_id++;
    paused_ = true;
    restart();
    paused_ = true; // opened paused, on its first frame
    return true;
}

void MoviePlayer::restart() {
    decoder_.rewind();
    clock_ = 0;
    shown_ = -1;
    exhausted_ = false;
    paused_ = false;
    update_frame();
}

void MoviePlayer::advance(f64 dt) {
    if (!paused_ && dt > 0) clock_ += dt;
}

bool MoviePlayer::finished() const {
    const f64 rate = decoder_.framerate();
    if (rate <= 0) return true;
    // Past its last frame's time; a stream shorter than its header ends
    // when it runs out.
    const i32 frames = exhausted_ ? shown_ + 1 : frame_count_;
    return clock_ >= frames / rate;
}

void MoviePlayer::update_frame() {
    const f64 rate = decoder_.framerate();
    const i32 due = rate > 0 ? static_cast<i32>(std::floor(clock_ * rate)) : 0;
    bool decoded = false;
    for (int n = 0; n < kMaxDecodes && shown_ < due && !exhausted_; ++n) {
        if (decoder_.decode_next_frame(/*convert=*/false)) {
            ++shown_;
            decoded = true;
        } else {
            exhausted_ = true;
        }
    }
    if (decoded) {
        decoder_.convert_frame();
        ++serial_;
    }
}

} // namespace osc::video
