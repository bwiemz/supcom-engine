#pragma once

#include "core/types.hpp"
#include "sim/command_scheduler.hpp"

#include <map>
#include <unordered_map>
#include <vector>

namespace osc::sim {

class SimState;
class INetTransport;
class ByteReader;

/// Drives a SimState in deterministic lockstep with remote peers over an
/// INetTransport.
///
/// Strict-lockstep model: each "frame" corresponds to one sim tick, and the sim
/// advances tick T only once every participant has confirmed frame T. A round is
/// two phases so peers exchange symmetrically:
///   1. send_frame()          — broadcast this peer's commands + confirmation
///   2. receive_and_advance() — ingest peers' frames, then tick as far as allowed
/// Each frame message also carries the sender's last executed-tick checksum, so
/// a divergence between clients is detected (desynced()).
class LockstepSession {
public:
    LockstepSession(SimState& sim, INetTransport& transport, u32 local_source,
                    const std::vector<u32>& all_sources);

    /// Queue a local order for the frame currently being entered. It is applied
    /// to the local sim immediately (scheduled) and broadcast on the next
    /// send_frame().
    void submit_local(const std::vector<u32>& unit_ids, const UnitCommand& cmd,
                      bool clear_existing);

    /// Queue a local SimCallback the same way: every peer runs it on the
    /// frame's tick.
    void submit_local_callback(SimCallbackEntry callback);

    /// Phase 1: broadcast this peer's frame (queued commands + confirmation +
    /// last checksum) and confirm the frame locally.
    void send_frame();

    /// Phase 2: ingest peers' frames (commands + confirmations + checksums),
    /// then advance the sim while every peer has confirmed the next tick.
    void receive_and_advance();

    u32 local_source() const { return local_source_; }
    u32 current_frame() const { return next_frame_; }
    bool desynced() const { return desynced_; }

    // --- Peer drop, by agreement (M198b) ---
    // A peer more than `frames` command frames behind in confirmations
    // (default 30 ≈ 3s at 10 Hz; 0 disables detection) is dropped -- but not
    // by one survivor alone: survivors may hold different last frames from
    // it (it died mid-broadcast), and each notices at its own moment. So a
    // survivor that times it out stops taking its frames and reports the last
    // one it holds, with the peer's recent frames; a report from another
    // survivor draws its own. Once every survivor has reported, all take the
    // furthest frame reported as the peer's last, apply any of its frames they
    // missed (relayed in the reports), release it from the gate, and defeat
    // its army on the tick after -- the same tick on every survivor.
    void set_drop_timeout(u32 frames) { drop_timeout_frames_ = frames; }
    /// Sources newly declared dropped since the last call (drained on return).
    std::vector<u32> take_dropped();
    bool has_dropped(u32 src) const;

private:
    static constexpr u8 kFrameMessage = 0;
    static constexpr u8 kDropMessage = 1;
    /// How many of a peer's latest frames are kept to relay if it drops.
    static constexpr u32 kRelayFrames = 256;

    /// A drop being agreed: each survivor's report of the dropped peer's last
    /// frame it holds, and that peer's frames, merged from every report.
    struct DropVote {
        std::map<u32, u32> last_frame;                       // reporter -> frame
        std::map<u32, std::vector<ScheduledCommand>> frames; // frame -> commands
    };

    SimState& sim_;
    INetTransport& transport_;
    u32 local_source_;
    std::vector<u32> all_sources_;
    std::unordered_map<u32, std::map<u32, std::vector<ScheduledCommand>>> recent_frames_;
    std::map<u32, DropVote> drop_votes_;       // sources being dropped, by source
    u32 next_frame_ = 1;                       // frame currently accepting input
    std::vector<ScheduledCommand> pending_;      // local commands for next_frame_
    std::unordered_map<u32, u32> my_checksums_;   // tick -> local checksum
    std::unordered_map<u32, u32> peer_checksums_; // tick -> a peer's reported checksum
    bool desynced_ = false;
    u32 drop_timeout_frames_ = 30;
    std::unordered_map<u32, u32> peer_confirmed_; // source -> last confirmed frame
    std::vector<u32> dropped_;                    // sources already declared dropped
    std::vector<u32> newly_dropped_;              // drained by take_dropped()

    bool dropping(u32 source) const { return drop_votes_.count(source) > 0; }
    void begin_drop(u32 source);
    void take_drop_report(ByteReader& r);
    void finalize_drops();
    void finalize_drop(u32 source, const DropVote& vote);

    void note_peer_checksum(u32 tick, u32 checksum);
    void record_local_checksum(u32 tick, u32 checksum);
};

} // namespace osc::sim
