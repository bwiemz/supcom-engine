// Disable FILE*-based functions (not needed, avoids MSVC warnings)
#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

#include "video/video_decoder.hpp"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cstring>

namespace osc::video {

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close() {
    if (plm_) {
        plm_destroy(plm_);
        plm_ = nullptr;
    }
    mpeg_data_.clear();
    rgba_buf_.clear();
    width_ = height_ = 0;
    framerate_ = 0;
}

bool VideoDecoder::open(const u8* data, size_t size) {
    close();
    if (!data || size == 0) return false;

    mpeg_data_.assign(data, data + size);
    plm_ = plm_create_with_memory(mpeg_data_.data(),
                                   static_cast<int>(mpeg_data_.size()), 0);
    if (!plm_) {
        spdlog::warn("VideoDecoder: pl_mpeg failed to open stream");
        mpeg_data_.clear();
        return false;
    }

    plm_set_audio_enabled(plm_, 0);
    width_ = static_cast<u32>(plm_get_width(plm_));
    height_ = static_cast<u32>(plm_get_height(plm_));
    framerate_ = plm_get_framerate(plm_);

    if (width_ == 0 || height_ == 0) {
        spdlog::warn("VideoDecoder: zero dimensions");
        close();
        return false;
    }

    rgba_buf_.resize(width_ * height_ * 4, 0);
    spdlog::info("VideoDecoder: opened {}x{} @ {:.1f} fps", width_, height_, framerate_);
    return true;
}

bool VideoDecoder::open_file(const u8* file_data, size_t file_size) {
    if (!file_data || file_size < 4) return false;

    // Check for MPEG-1 sequence header (0x000001B3)
    if (file_size >= 4 &&
        file_data[0] == 0x00 && file_data[1] == 0x00 &&
        file_data[2] == 0x01 && file_data[3] == 0xB3) {
        return open(file_data, file_size);
    }

    // Try SFD demux
    auto mpeg = demux_sfd(file_data, file_size);
    if (!mpeg.empty()) {
        return open(mpeg.data(), mpeg.size());
    }

    // Last resort: try as raw MPEG-1 anyway
    return open(file_data, file_size);
}

bool VideoDecoder::decode_next_frame() {
    if (!plm_) return false;

    plm_frame_t* frame = plm_decode_video(plm_);
    if (!frame) {
        if (loop_) {
            plm_rewind(plm_);
            frame = plm_decode_video(plm_);
        }
        if (!frame) return false;
    }

    plm_frame_to_rgba(frame, rgba_buf_.data(),
                      static_cast<int>(width_) * 4);
    return true;
}

void VideoDecoder::set_loop(bool loop) {
    loop_ = loop;
    if (plm_) plm_set_loop(plm_, loop ? 1 : 0);
}

void VideoDecoder::rewind() {
    if (plm_) plm_rewind(plm_);
}

// --- SFD Demuxer ---

std::vector<u8> demux_sfd(const u8* data, size_t size) {
    if (size < 8) return {};

    // CRI Sofdec magic: "CRID" or "SFD\x00"
    bool is_crid = (data[0] == 'C' && data[1] == 'R' &&
                    data[2] == 'I' && data[3] == 'D');
    bool is_sfd = (data[0] == 'S' && data[1] == 'F' &&
                   data[2] == 'D' && data[3] == '\0');

    if (!is_crid && !is_sfd) return {};

    spdlog::debug("SFD demuxer: detected CRI Sofdec container");

    // Scan for MPEG-1 video elementary stream packets (0x000001E0-0x000001EF).
    // Reassemble video payload bytes, stripping PES headers.
    std::vector<u8> video;
    video.reserve(size);

    size_t pos = 0;
    while (pos + 4 <= size) {
        // Look for start code prefix 0x000001
        if (data[pos] == 0x00 && data[pos + 1] == 0x00 &&
            data[pos + 2] == 0x01) {

            u8 stream_id = data[pos + 3];

            // MPEG-1 video elementary stream: 0xE0-0xEF
            if (stream_id >= 0xE0 && stream_id <= 0xEF && pos + 6 <= size) {
                u16 packet_len = static_cast<u16>(data[pos + 4]) << 8 |
                                 static_cast<u16>(data[pos + 5]);

                // Skip PES header stuffing bytes (0xFF padding)
                size_t hdr_start = pos + 6;
                size_t hdr_pos = hdr_start;
                while (hdr_pos < pos + 6 + packet_len && hdr_pos < size &&
                       data[hdr_pos] == 0xFF) {
                    hdr_pos++;
                }
                // Skip STD buffer flag (01xxxxxx) and PTS/DTS flags
                if (hdr_pos < size && (data[hdr_pos] & 0xC0) == 0x40) {
                    hdr_pos += 2;
                }
                if (hdr_pos < size && (data[hdr_pos] & 0xF0) == 0x20) {
                    hdr_pos += 5;
                } else if (hdr_pos < size && (data[hdr_pos] & 0xF0) == 0x30) {
                    hdr_pos += 10;
                } else if (hdr_pos < size && data[hdr_pos] == 0x0F) {
                    hdr_pos += 1;
                }

                size_t payload_len = packet_len - (hdr_pos - hdr_start);
                if (hdr_pos + payload_len <= size) {
                    video.insert(video.end(),
                                 data + hdr_pos,
                                 data + hdr_pos + payload_len);
                }
                pos = pos + 6 + packet_len;
                continue;
            }
            // Pack header (0x000001BA) — skip 12 bytes (MPEG-1 pack)
            else if (stream_id == 0xBA) {
                pos += 12;
                continue;
            }
            // System header or other streams — skip by length
            else if (stream_id == 0xBB || stream_id == 0xBD ||
                     stream_id == 0xBE || stream_id == 0xBF ||
                     (stream_id >= 0xC0 && stream_id <= 0xDF)) {
                if (pos + 6 <= size) {
                    u16 plen = static_cast<u16>(data[pos + 4]) << 8 |
                               static_cast<u16>(data[pos + 5]);
                    pos = pos + 6 + plen;
                    continue;
                }
            }
        }
        pos++;
    }

    if (video.size() < 16) {
        spdlog::debug("SFD demuxer: insufficient video data extracted");
        return {};
    }

    spdlog::info("SFD demuxer: extracted {} bytes of MPEG-1 video", video.size());
    return video;
}

} // namespace osc::video
