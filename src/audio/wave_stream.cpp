#include "audio/wave_stream.hpp"

#include "audio/xwb_parser.hpp"

#include <algorithm>

namespace osc::audio {

// A minimal RIFF/WAVE header: PCM (format 1) or MS-ADPCM (format 2).
std::vector<u8> wav_header(const WaveInfo& info, u32 data_length) {
    // For PCM (format_tag 0): RIFF/WAVE with fmt + data chunks
    // For ADPCM (format_tag 2): RIFF/WAVE with extended fmt + data chunks

    bool is_adpcm = (info.format_tag == 2);
    u16 wav_format_tag = is_adpcm ? 0x0002 : 0x0001;

    u32 channels = info.channels ? info.channels : 1;
    u32 sample_rate = info.sample_rate;
    u16 bits_per_sample = is_adpcm ? 4 : static_cast<u16>(info.bits_per_sample);
    u16 block_align = static_cast<u16>(info.block_align);
    u32 avg_bytes_per_sec;
    if (is_adpcm && block_align > 7 * channels) {
        u32 samples_per_block = (block_align - 7 * channels) * 2 / channels + 2;
        avg_bytes_per_sec = samples_per_block > 0
            ? (sample_rate / samples_per_block) * block_align
            : sample_rate;
    } else {
        avg_bytes_per_sec = sample_rate * channels * (info.bits_per_sample / 8);
    }

    // ADPCM needs extended fmt chunk with coefficient table
    // Standard MS-ADPCM has 7 coefficient pairs
    static const i16 adpcm_coeffs[7][2] = {
        {256, 0}, {512, -256}, {0, 0}, {192, 64},
        {240, 0}, {460, -208}, {392, -232}
    };

    u16 adpcm_samples_per_block = 0;
    if (is_adpcm && block_align > 0) {
        adpcm_samples_per_block = static_cast<u16>(
            (block_align - 7 * channels) * 2 / channels + 2);
    }

    // Calculate fmt chunk sizes
    u32 fmt_extra_size = is_adpcm ? (2 + 2 + 7 * 4) : 0; // cbSize data
    u32 fmt_chunk_size = 16 + (is_adpcm ? (2 + fmt_extra_size) : 0);
    u32 data_chunk_size = data_length;
    u32 riff_size = 4 + (8 + fmt_chunk_size) + (8 + data_chunk_size);

    std::vector<u8> wav;
    wav.reserve(12 + 8 + fmt_chunk_size + 8);

    auto write_u16 = [&](u16 v) {
        wav.push_back(static_cast<u8>(v));
        wav.push_back(static_cast<u8>(v >> 8));
    };
    auto write_u32 = [&](u32 v) {
        wav.push_back(static_cast<u8>(v));
        wav.push_back(static_cast<u8>(v >> 8));
        wav.push_back(static_cast<u8>(v >> 16));
        wav.push_back(static_cast<u8>(v >> 24));
    };
    auto write_tag = [&](const char* tag) {
        wav.insert(wav.end(), tag, tag + 4);
    };
    auto write_i16 = [&](i16 v) {
        write_u16(static_cast<u16>(v));
    };

    // RIFF header
    write_tag("RIFF");
    write_u32(riff_size);
    write_tag("WAVE");

    // fmt chunk
    write_tag("fmt ");
    write_u32(fmt_chunk_size);
    write_u16(wav_format_tag);
    write_u16(static_cast<u16>(channels));
    write_u32(sample_rate);
    write_u32(avg_bytes_per_sec);
    write_u16(block_align);
    write_u16(bits_per_sample);

    if (is_adpcm) {
        write_u16(static_cast<u16>(fmt_extra_size)); // cbSize
        write_u16(adpcm_samples_per_block);
        write_u16(7); // num coefficients
        for (int i = 0; i < 7; i++) {
            write_i16(adpcm_coeffs[i][0]);
            write_i16(adpcm_coeffs[i][1]);
        }
    }

    // data chunk
    write_tag("data");
    write_u32(data_chunk_size);
    return wav;
}

/// A WAV file in memory wrapping raw PCM or ADPCM data, for miniaudio's decoder.
std::vector<u8> build_wav(const WaveInfo& info, const std::vector<u8>& raw) {
    std::vector<u8> wav = wav_header(info, static_cast<u32>(raw.size()));
    wav.insert(wav.end(), raw.begin(), raw.end());
    return wav;
}

WaveStream::WaveStream(std::vector<u8> header, const fs::path& file, u64 offset, u64 length)
    : header_(std::move(header)), file_(file, std::ios::binary), offset_(offset), length_(length) {}

size_t WaveStream::read(void* out, size_t bytes) {
    auto* dst = static_cast<u8*>(out);
    size_t done = 0;
    while (done < bytes && pos_ < size()) {
        if (pos_ < header_.size()) {
            const auto n = static_cast<size_t>(std::min<u64>(bytes - done, header_.size() - pos_));
            std::copy_n(header_.data() + pos_, n, dst + done);
            done += n;
            pos_ += n;
            continue;
        }
        const u64 at = pos_ - header_.size();
        const auto n = static_cast<size_t>(std::min<u64>(bytes - done, length_ - at));
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset_ + at));
        file_.read(reinterpret_cast<char*>(dst + done), static_cast<std::streamsize>(n));
        const auto got = static_cast<size_t>(std::max<std::streamsize>(0, file_.gcount()));
        done += got;
        pos_ += got;
        if (got < n) break; // the file ended early
    }
    return done;
}

void WaveStream::seek(i64 offset, Origin origin) {
    const auto end = static_cast<i64>(size());
    i64 to = offset;
    if (origin == Origin::Current) to = static_cast<i64>(pos_) + offset;
    else if (origin == Origin::End) to = end - (offset < 0 ? -offset : offset);
    pos_ = static_cast<u64>(std::clamp<i64>(to, 0, end));
}

} // namespace osc::audio
