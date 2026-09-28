#pragma once

#include "core/types.hpp"
#include "video/video_decoder.hpp"

#include <vector>

namespace osc::video {

/// The frame count a Sofdec file's header records (Moho's
/// mwsffrm_AnalyTotalFrm): the SofdecStream block in its second or third
/// 2 KiB block. -1 if it has none.
i32 sofdec_frame_count(const u8* data, size_t size);

/// Moho's CMovie: a movie played on its own clock, as Sofdec plays one.
/// It opens paused on its first frame. Unpaused, its clock runs, and the
/// frame shown is the one due at the clock: frame n from n / rate. It has
/// finished once the clock passes its last frame.
class MoviePlayer {
public:
    /// Open a movie file (an SFD, or plain MPEG-1). False if it can't be
    /// decoded.
    bool open(std::vector<char> file);

    bool loaded() const { return decoder_.is_open(); }
    u32 width() const { return decoder_.width(); }
    u32 height() const { return decoder_.height(); }
    /// Its frames: the Sofdec header's count, else the stream's length.
    i32 frame_count() const { return frame_count_; }
    f32 frame_rate() const { return static_cast<f32>(decoder_.framerate()); }

    /// PlayMovie: the clock runs.
    void play() { paused_ = false; }
    /// Stop: the clock stops (Play resumes it).
    void pause() { paused_ = true; }
    bool paused() const { return paused_; }
    /// StartMoviePlaybackFromName (a loop): from the first frame, running.
    void restart();

    /// Run the clock on by `dt` seconds, unless paused.
    void advance(f64 dt);
    /// Whether the clock has passed the last frame (Sofdec's play end).
    bool finished() const;
    /// Decode up to the frame due (UpdatePlaybackFrame), at most
    /// kMaxDecodes a call: a movie that decodes slower than it plays drops
    /// behind, and ends on time.
    void update_frame();

    /// The frame shown (0-based), and its pixels (RGBA, width x height).
    i32 frame_shown() const { return shown_; }
    const u8* rgba() const { return decoder_.rgba_data(); }
    /// Changes whenever the pixels do.
    u64 frame_serial() const { return serial_; }
    /// Different for every movie opened (for a texture made for it).
    u64 id() const { return id_; }
    f64 clock() const { return clock_; }

    static constexpr int kMaxDecodes = 8;

private:
    VideoDecoder decoder_;
    i32 frame_count_ = 0;
    f64 clock_ = 0;
    bool paused_ = true;
    i32 shown_ = -1;
    bool exhausted_ = false;
    u64 serial_ = 0;
    u64 id_ = 0;
};

} // namespace osc::video
