#pragma once

#include "core/types.hpp"

#include <vector>

struct plm_t;

namespace osc::video {

/// Wraps pl_mpeg for MPEG-1 video decoding.
/// Owns its input (SFD demuxed or raw MPEG-1).
class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    /// Open from raw MPEG-1 data. Takes ownership of a copy.
    bool open(const u8* data, size_t size);

    /// Open a movie file, which it keeps: a CRI container (demuxed), else
    /// MPEG-1 (a program stream, as FA's .sfd files are, or video alone).
    bool open_file(std::vector<char> file);

    /// Decode the next frame, into RGBA unless `convert` is false (then
    /// convert_frame() can). False at the stream's end.
    bool decode_next_frame(bool convert = true);
    /// Convert the frame last decoded to RGBA.
    void convert_frame();

    const u8* rgba_data() const { return rgba_buf_.data(); }
    u32 width() const { return width_; }
    u32 height() const { return height_; }
    f64 framerate() const { return framerate_; }
    /// The stream's length in seconds (0 if unknown).
    f64 duration() const;
    bool is_open() const { return plm_ != nullptr; }

    void rewind();
    void close();

private:
    /// Open the stream held in mpeg_data_.
    bool open_held();

    plm_t* plm_ = nullptr;
    /// The last decoded frame (a plm_frame_t, pl_mpeg's unnamed struct),
    /// valid until the next decode.
    void* frame_ = nullptr;
    std::vector<char> mpeg_data_;
    std::vector<u8> rgba_buf_;
    u32 width_ = 0;
    u32 height_ = 0;
    f64 framerate_ = 0;
};

/// Attempt to demux SFD (CRI Sofdec) container into raw MPEG-1.
/// Returns empty vector if not an SFD file or demux fails.
std::vector<u8> demux_sfd(const u8* data, size_t size);

} // namespace osc::video
