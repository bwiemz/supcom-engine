#pragma once

#include "core/types.hpp"

#include <vector>

struct plm_t;

namespace osc::video {

/// Wraps pl_mpeg for MPEG-1 video decoding.
/// Owns a copy of the input data (SFD demuxed or raw MPEG-1).
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /// Open from raw MPEG-1 data. Takes ownership of a copy.
    bool open(const u8* data, size_t size);

    /// Open from VFS file data. Attempts SFD demux first, then raw MPEG-1.
    bool open_file(const u8* file_data, size_t file_size);

    /// Decode next frame. Returns false if no more frames (unless looping).
    bool decode_next_frame();

    const u8* rgba_data() const { return rgba_buf_.data(); }
    u32 width() const { return width_; }
    u32 height() const { return height_; }
    f64 framerate() const { return framerate_; }
    bool is_open() const { return plm_ != nullptr; }

    void set_loop(bool loop);
    void rewind();
    void close();

private:
    plm_t* plm_ = nullptr;
    std::vector<u8> mpeg_data_;
    std::vector<u8> rgba_buf_;
    u32 width_ = 0;
    u32 height_ = 0;
    f64 framerate_ = 0;
    bool loop_ = false;
};

/// Attempt to demux SFD (CRI Sofdec) container into raw MPEG-1.
/// Returns empty vector if not an SFD file or demux fails.
std::vector<u8> demux_sfd(const u8* data, size_t size);

} // namespace osc::video
