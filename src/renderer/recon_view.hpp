#pragma once

#include "core/types.hpp"
#include "sim/world_snapshot.hpp"

#include <array>
#include <span>
#include <unordered_map>
#include <vector>

namespace osc::renderer {

/// How the player's army sees an entity: Moho's recon (CAiReconDBImpl,
/// ReconBlip, UserUnit::UpdateVisibility; M215a).
enum class Sight : u8 {
    Hidden,     ///< not drawn
    Blip,       ///< detected, not seen since: a generic icon in UnidentifiedColor
    SeenBlip,   ///< detected, seen before: its own icon, no mesh
    Seen,       ///< drawn as itself
    Remembered, ///< a structure seen before, out of sight: drawn as last seen
};

inline bool shows_mesh(Sight s) {
    return s == Sight::Seen || s == Sight::Remembered;
}
inline bool shows_icon(Sight s) {
    return s != Sight::Hidden;
}

/// The player's army's intel of the world, brought up to date once per tick.
/// Its own and its allies' entities are seen (the sim shares allied senses
/// into each army's grid, so the player's grid holds its allies' too);
/// another army's mobile unit in its line of sight, else as a blip while any
/// sense detects it; another army's structure, once seen, for good (frozen
/// as last seen while out of sight); another army's projectile in its line
/// of sight; props and wrecks always. An observer (no focus army) and a
/// world without a visibility grid see everything.
class ReconView {
public:
    /// The player's army, 0-based; -1 for an observer. A new one forgets
    /// what the last saw.
    void set_focus_army(i32 army);
    i32 focus_army() const { return focus_; }

    /// Bring the sights up to the view's newest tick (a tick seen already is
    /// skipped). `flushes`, the FlushIntelInRects since the last update:
    /// what they took from the player's army is forgotten -- the units whose
    /// blips it lost, and the structures gone unseen within their rects.
    void update(const sim::FrameView& view, std::span<const sim::IntelFlushRecord> flushes = {});

    /// How the player's army sees `e` (as of the last update).
    Sight sight(const sim::EntityRecord& e) const;

    /// Whether the player's army sees an effect of `army`'s at (x, z) in
    /// `view`'s newest tick: its own or an ally's anywhere, another's (or
    /// no army's) in its line of sight, as Moho shows an emitter.
    bool sees_at(const sim::FrameView& view, i32 army, f32 x, f32 z) const;

    /// Whether it sees a beam from `a` to `b`: either end in its line of
    /// sight (CEfxBeam::CanSeeCam; M215b).
    bool sees_beam(const sim::FrameView& view, const sim::Vector3& a, const sim::Vector3& b) const {
        return sees_at(view, -1, a.x, a.z) || sees_at(view, -1, b.x, b.z);
    }

    /// A remembered structure's bone pose when last seen; null for any other
    /// entity, or one with no pose.
    const std::vector<sim::BoneMatrix>* frozen_pose(u32 id) const;

    /// A remembered structure's fraction complete when last seen; `live`
    /// for any other entity.
    f32 frozen_fraction(u32 id, f32 live) const;

    /// Remembered structures gone from the world while out of the player's
    /// sight: Moho's MaybeDead, drawn as last seen until it sees the spot
    /// (their records as last seen, in id order; M215d).
    const std::vector<sim::EntityRecord>& ghosts() const { return ghosts_; }
    /// Whether `id` is one of them (its icon is drawn darkened).
    bool maybe_dead(u32 id) const;

    /// The jammers' fake blips the player's army senses and doesn't know
    /// fake (M215e): each its jammer's record at the fake's place, under an
    /// id of its own (kFakeBlip set), seen as a blip.
    const std::vector<sim::EntityRecord>& fakes() const { return fakes_; }
    static constexpr u32 kFakeBlip = 0x80000000u;

    /// Whether it sees everything (an observer, or no grid).
    bool sees_everything() const { return everything_; }

    /// A never-seen blip's icon colour (GameColors' UnidentifiedColor, ARGB).
    void set_unidentified_color(u32 argb) { unidentified_color_ = argb; }
    u32 unidentified_color() const { return unidentified_color_; }
    /// Its red, green and blue in [0, 1].
    std::array<f32, 3> unidentified_rgb() const {
        const auto channel = [&](int shift) {
            return static_cast<f32>(unidentified_color_ >> shift & 0xFFu) / 255.0f;
        };
        return {channel(16), channel(8), channel(0)};
    }

    /// Forget everything seen (a new game).
    void clear();

private:
    /// Another army's unit or projectile, as the player's army knows it.
    struct Memory {
        Sight sight = Sight::Hidden;
        bool seen_ever = false;            ///< Moho's LOSEver
        std::vector<sim::BoneMatrix> pose; ///< a structure's, as last seen
        f32 fraction = 1.0f;               ///< likewise
        sim::EntityRecord last;            ///< likewise, the record (id 0: none)
        bool ghost = false;                ///< gone from the world, unseen
        u64 touched = 0;                   ///< the update that last found it
    };

    /// Whether `e` is judged by the player's intel (not its own or an ally's,
    /// and a unit or a projectile).
    bool judged(const sim::EntityRecord& e) const;

    i32 focus_ = -1;
    u32 allies_ = 0; ///< the focus army's allies, bit per army
    u32 last_tick_ = 0;
    bool updated_ = false;
    bool everything_ = true;
    u64 update_count_ = 0;
    u32 unidentified_color_ = 0xFF808080u;
    std::unordered_map<u32, Memory> memory_;
    std::vector<sim::EntityRecord> ghosts_;
    std::vector<sim::EntityRecord> fakes_;
};

} // namespace osc::renderer
