#include <catch2/catch_test_macros.hpp>

#include "audio/wave_stream.hpp"
#include "audio/xwb_parser.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace osc;
using audio::WaveStream;

namespace {

/// A file of 100 bytes of something else, then `data` (as a wave sits in
/// its bank); removed with it.
struct BankFile {
    std::filesystem::path path;
    explicit BankFile(const std::vector<u8>& data) {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() /
               ("osc_wave_stream_test_" + std::to_string(rd()) + std::to_string(rd()));
        std::ofstream out(path, std::ios::binary);
        const std::vector<char> before(100, 'x');
        out.write(before.data(), static_cast<std::streamsize>(before.size()));
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    ~BankFile() { std::filesystem::remove(path); }
};

audio::WaveInfo pcm(u32 length) {
    audio::WaveInfo info;
    info.format_tag = 0;
    info.channels = 1;
    info.sample_rate = 22050;
    info.block_align = 2;
    info.bits_per_sample = 16;
    info.data_offset = 100;
    info.data_length = length;
    return info;
}

} // namespace

TEST_CASE("Wave stream: a streamed wave reads as the WAV built in memory", "[audio][stream]") {
    std::vector<u8> data(5000);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<u8>(i * 7 + 3);
    const BankFile bank(data);
    const auto info = pcm(static_cast<u32>(data.size()));
    const std::vector<u8> whole = audio::build_wav(info, data);

    WaveStream stream(audio::wav_header(info, info.data_length), bank.path, 100, data.size());
    REQUIRE(stream.ok());
    CHECK(stream.size() == whole.size());
    std::vector<u8> read;
    std::vector<u8> chunk(333); // across the header's end and the data's
    while (const size_t n = stream.read(chunk.data(), chunk.size()))
        read.insert(read.end(), chunk.begin(), chunk.begin() + static_cast<long>(n));
    CHECK(read == whole);
    CHECK(stream.read(chunk.data(), chunk.size()) == 0); // at the end
}

TEST_CASE("Wave stream: seeks clamp as miniaudio's memory decoder's do", "[audio][stream]") {
    const std::vector<u8> data(1000, 0x5A);
    const BankFile bank(data);
    const auto info = pcm(1000);
    WaveStream stream(audio::wav_header(info, 1000), bank.path, 100, 1000);
    const u64 size = stream.size();
    stream.seek(10, WaveStream::Origin::Start);
    CHECK(stream.tell() == 10);
    stream.seek(5, WaveStream::Origin::Current);
    CHECK(stream.tell() == 15);
    stream.seek(-100, WaveStream::Origin::Current);
    CHECK(stream.tell() == 0);
    stream.seek(4, WaveStream::Origin::End); // a distance back from the end
    CHECK(stream.tell() == size - 4);
    stream.seek(-4, WaveStream::Origin::End);
    CHECK(stream.tell() == size - 4);
    stream.seek(static_cast<i64>(size) + 50, WaveStream::Origin::Start);
    CHECK(stream.tell() == size);
    u8 byte = 0;
    CHECK(stream.read(&byte, 1) == 0);
    stream.seek(static_cast<i64>(size) - 1, WaveStream::Origin::Start);
    CHECK(stream.read(&byte, 1) == 1);
    CHECK(byte == 0x5A);
}

TEST_CASE("Wave stream: a missing bank file doesn't open", "[audio][stream]") {
    const WaveStream stream({}, std::filesystem::temp_directory_path() / "osc_no_such_bank.xwb", 0,
                            10);
    CHECK_FALSE(stream.ok());
}
