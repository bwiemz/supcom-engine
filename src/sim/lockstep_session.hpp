#pragma once

#include "core/types.hpp"
#include "sim/command_scheduler.hpp"

#include <unordered_map>
#include <vector>

namespace osc::sim {

class SimState;
class INetTransport;

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

    /// Phase 1: broadcast this peer's frame (queued commands + confirmation +
    /// last checksum) and confirm the frame locally.
    void send_frame();

    /// Phase 2: ingest peers' frames (commands + confirmations + checksums),
    /// then advance the sim while every peer has confirmed the next tick.
    void receive_and_advance();

    u32 local_source() const { return local_source_; }
    u32 current_frame() const { return next_frame_; }
    bool desynced() const { return desynced_; }

private:
    SimState& sim_;
    INetTransport& transport_;
    u32 local_source_;
    u32 next_frame_ = 1;                       // frame currently accepting input
    std::vector<ScheduledCommand> pending_;      // local commands for next_frame_
    std::unordered_map<u32, u32> my_checksums_;   // tick -> local checksum
    std::unordered_map<u32, u32> peer_checksums_; // tick -> a peer's reported checksum
    bool desynced_ = false;

    void note_peer_checksum(u32 tick, u32 checksum);
    void record_local_checksum(u32 tick, u32 checksum);
};

} // namespace osc::sim
