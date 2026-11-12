#pragma once

#include "core/types.hpp"
#include "video/video_decoder.hpp"

#include <string>
#include <utility>
#include <vector>

namespace osc::video {

/// The frame count a Sofdec file's header records (Moho's
/// mwsffrm_AnalyTotalFrm): the SofdecStream block in its second or third
/// 2 KiB block. -1 if it has none.
i32 sofdec_frame_count(const u8* data, size_t size);

/// A Sofdec file's length in seconds, from its header (Moho's
/// MOV_GetDuration): the header's frame count over the first MPEG
/// sequence header's frame rate. 0 if either is missing.
f32 sofdec_duration(const u8* data, size_t size);

/// A movie's subtitle: its text, shown from `start` until `end` (seconds
/// on the movie's clock).
struct Subtitle {
    f64 start = 0;
    f64 end = 0;
    std::string text;
};

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
    /// StartMoviePlaybackFromName (a loop): from the first frame, running;
    /// decoded unless `decode` is false (a movie off screen).
    void restart(bool decode = true);

    /// Run the clock on by `dt` seconds, unless paused.
    void advance(f64 dt);
    /// Whether the clock has passed the last frame (Sofdec's play end).
    bool finished() const;
    /// Decode up to the frame due (UpdatePlaybackFrame), at most
    /// kMaxDecodes a call: a movie that decodes slower than it plays drops
    /// behind, and ends on time.
    void update_frame();

    /// The movie's subtitles. Moho's come from Sofdec's mwPlyGetSubtitle,
    /// which looks up a SUBTITLE tag in the movie's own data (CRI's tags:
    /// the exe names CONTENT, INTIME, DURTIME and TUNIT beside it). The
    /// engine reads none from a file: where and how an .sfd holds them is
    /// CRI's and undocumented, and no FA movie has any (each holds only its
    /// SofdecStream header and the encoder's CRITAGS). A reader of them
    /// would give them here; until then only tests do.
    void set_subtitles(std::vector<Subtitle> subtitles) { subtitles_ = std::move(subtitles); }
    /// Take the subtitle due on the clock, or none, unless paused (with the
    /// frame, in Moho's CMovie::UpdatePlaybackFrame).
    void update_subtitle();
    /// The subtitle taken last (CMovie's mSubtitleText): empty for none.
    const std::string& subtitle() const { return subtitle_; }

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
    std::vector<Subtitle> subtitles_;
    std::string subtitle_;
};

} // namespace osc::video
