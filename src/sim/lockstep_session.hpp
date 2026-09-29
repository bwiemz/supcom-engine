#pragma once

#include "core/types.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/sim_state.hpp"

#include <array>
#include <map>
#include <string>
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
    /// The first desync seen: its tick, and the checksum domains that
    /// differed (SimState::ChecksumParts::kNames), which say where to look.
    u32 desync_tick() const { return desync_tick_; }
    const std::vector<std::string>& desync_domains() const { return desync_domains_; }

    // --- Pace (M218h) ---
    /// How many frames a peer runs ahead of its sim: two seconds' worth, as
    /// Moho's issue thread. Frames go out a round at a time, so their lead
    /// over the sim is the network's latency (the delay of a local order
    /// adapts to it); past this, a peer waits for the others, and the game
    /// runs at the slowest peer's pace rather than dropping it.
    static constexpr u32 kMaxLead = 20;

    // --- Peer drop, by agreement (M198b) ---
    // A peer silent for more than `rounds` rounds (send_frame calls, one
    // each tick's time: default 30 ≈ 3s at 10 Hz; 0 disables detection) is
    // dropped -- but not by one survivor alone: survivors may hold different last frames from
    // it (it died mid-broadcast), and each notices at its own moment. So a
    // survivor that times it out stops taking its frames and reports the last
    // one it holds, with the peer's recent frames; a report from another
    // survivor draws its own. Once every survivor has reported, all take the
    // furthest frame reported as the peer's last, apply any of its frames they
    // missed (relayed in the reports), release it from the gate, and defeat
    // its army on the tick after -- the same tick on every survivor.
    void set_drop_timeout(u32 rounds) { drop_timeout_rounds_ = rounds; }
    /// Sources newly declared dropped since the last call (drained on return).
    std::vector<u32> take_dropped();
    bool has_dropped(u32 src) const;

    // --- Eject (EjectSessionClient, M218e) ---
    /// This peer ejects `source`: it starts the drop a timeout would, and the
    /// survivors agree it as they do one. False for this peer itself, one
    /// dropped or being dropped already, or no participant.
    bool eject(u32 source);
    /// The survivors that have reported `source` dropped (Moho's
    /// ejectedBy): while its drop is agreed, and once it is. Sorted.
    std::vector<u32> ejectors(u32 source) const;
    /// Whether a survivor reports this peer dropped: its game is over, and
    /// it no longer ticks or drops anyone.
    bool ejected() const { return ejected_; }

    // --- Pause (M218f) ---
    // A pause is a command, run by every peer on the same tick (the sim
    // takes or refuses it: SimState::request_pause); then no peer sends
    // frames or ticks until it is resumed. Resuming is a message with the
    // pause's serial, since no tick runs to carry a command; every peer is
    // held on the same tick, so each resumes at the same place.
    /// This peer asks to pause (nothing while paused already).
    void request_pause();
    /// This peer resumes the pause (any peer may); false if not paused.
    bool request_resume();

private:
    static constexpr u8 kFrameMessage = 0;
    static constexpr u8 kDropMessage = 1;
    static constexpr u8 kResumeMessage = 2;
    /// A peer held at kMaxLead sends no frame: this says it is still there.
    static constexpr u8 kAliveMessage = 3;
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
    using Parts = std::array<u64, SimState::ChecksumParts::kCount>;
    std::unordered_map<u32, Parts> my_checksums_;   // tick -> local checksum, by domain
    std::unordered_map<u32, Parts> peer_checksums_; // tick -> a peer's reported one
    u32 desync_tick_ = 0;
    std::vector<std::string> desync_domains_;
    bool desynced_ = false;
    u32 drop_timeout_rounds_ = 30;
    u32 round_ = 0;                                 // send_frame calls, while not paused
    std::unordered_map<u32, u32> peer_confirmed_;   // source -> last confirmed frame
    std::unordered_map<u32, u32> peer_heard_round_; // source -> round last heard from
    std::vector<u32> dropped_;                    // sources already declared dropped
    std::vector<u32> newly_dropped_;              // drained by take_dropped()
    std::map<u32, std::vector<u32>> dropped_by_;  // a dropped source's reporters
    bool ejected_ = false;                        // a survivor reports this peer dropped

    bool dropping(u32 source) const { return drop_votes_.count(source) > 0; }
    void begin_drop(u32 source);
    void take_drop_report(ByteReader& r);
    void finalize_drops();
    void finalize_drop(u32 source, const DropVote& vote);

    void note_peer_checksum(u32 tick, const Parts& parts);
    void record_local_checksum(u32 tick, const Parts& parts);
    /// Compare a tick's checksums; on the first desync, note its domains.
    void compare_checksums(u32 tick, const Parts& mine, const Parts& theirs);
};

} // namespace osc::sim
