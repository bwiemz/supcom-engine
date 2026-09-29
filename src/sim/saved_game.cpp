#include "sim/saved_game.hpp"
#include "sim/build_info.hpp"
#include "sim/command_codec.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/sim_state.hpp"

#include <spdlog/spdlog.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace osc::sim {

namespace {

constexpr std::array<u8, 7> kMagic = {'O', 'S', 'C', 'S', 'A', 'V', 'E'};
/// A snapshot larger than this is a damaged save (the late game's is 70 MB).
constexpr u64 kMaxSnapshot = u64{2} << 30;

} // namespace

const char* save_load_error_name(SaveLoadError error) {
    switch (error) {
    case SaveLoadError::None: return "None";
    case SaveLoadError::CantOpen: return "CantOpen";
    case SaveLoadError::InvalidFormat: return "InvalidFormat";
    case SaveLoadError::WrongVersion: return "WrongVersion";
    case SaveLoadError::InternalError: return "InternalError";
    }
    return "InternalError";
}

std::vector<u8> SavedGame::serialize() const {
    std::vector<u8> b;
    ByteWriter w(b);
    for (u8 c : kMagic) w.u8v(c);
    w.u32v(kVersion); // the layout written below, whatever version was read
    w.str(build);
    w.str(name);
    w.u32v(tick);
    // The snapshot, deflated (zlib, fastest: the late game's 60 MB is mostly
    // repeated structure)
    std::vector<u8> packed;
    if (!snapshot.empty()) {
        uLongf size = compressBound(static_cast<uLong>(snapshot.size()));
        packed.resize(size);
        if (compress2(packed.data(), &size, snapshot.data(), static_cast<uLong>(snapshot.size()),
                      Z_BEST_SPEED) == Z_OK)
            packed.resize(size);
        else packed.clear(); // saved without it: a load catches up
    }
    w.u64v(packed.empty() ? 0 : snapshot.size());
    w.u64v(packed.size());
    b.insert(b.end(), packed.begin(), packed.end());
    b.insert(b.end(), snapshot_mac.begin(), snapshot_mac.end());
    const std::vector<u8> recording = game.serialize();
    b.insert(b.end(), recording.begin(), recording.end());
    return b;
}

SaveLoadError SavedGame::deserialize(const std::vector<u8>& bytes, SavedGame& out) {
    out = SavedGame{};
    ByteReader r(bytes);
    for (u8 c : kMagic) {
        if (r.u8v() != c) return SaveLoadError::InvalidFormat;
    }
    const u32 version = r.u32v();
    if (!r.ok()) return SaveLoadError::InvalidFormat;
    if (version != kVersion) return SaveLoadError::WrongVersion;
    SavedGame save;
    save.build = r.str();
    save.name = r.str();
    save.tick = r.u32v();
    if (!r.ok()) return SaveLoadError::InvalidFormat;
    // The build first: another build's recording may not even parse.
    if (save.build != build_id()) return SaveLoadError::WrongVersion;
    const u64 snapshot_size = r.u64v();
    const u64 packed_size = r.u64v();
    if (!r.ok() || packed_size > bytes.size() - r.position() || snapshot_size > kMaxSnapshot)
        return SaveLoadError::InvalidFormat;
    const auto packed_at = bytes.begin() + static_cast<std::ptrdiff_t>(r.position());
    if (snapshot_size > 0) {
        save.snapshot.resize(static_cast<size_t>(snapshot_size));
        uLongf size = static_cast<uLongf>(snapshot_size);
        if (uncompress(save.snapshot.data(), &size, &*packed_at, static_cast<uLong>(packed_size)) !=
                Z_OK ||
            size != snapshot_size)
            return SaveLoadError::InvalidFormat;
    }
    const auto mac_at = packed_at + static_cast<std::ptrdiff_t>(packed_size);
    if (bytes.end() - mac_at < static_cast<std::ptrdiff_t>(save.snapshot_mac.size()))
        return SaveLoadError::InvalidFormat;
    std::copy_n(mac_at, save.snapshot_mac.size(), save.snapshot_mac.begin());
    const std::vector<u8> recording(mac_at + static_cast<std::ptrdiff_t>(save.snapshot_mac.size()),
                                    bytes.end());
    if (!Replay::deserialize(recording, save.game) || !save.game.has_setup ||
        save.game.final_tick != save.tick)
        return SaveLoadError::InvalidFormat;
    out = std::move(save);
    return SaveLoadError::None;
}

void sign_snapshot(SavedGame& save, const SnapshotKey& key) {
    save.snapshot_mac =
        core::hmac_sha256(key.data(), key.size(), save.snapshot.data(), save.snapshot.size());
}

bool snapshot_signed(const SavedGame& save, const SnapshotKey& key) {
    return !save.snapshot.empty() &&
           core::digest_equal(save.snapshot_mac,
                              core::hmac_sha256(key.data(), key.size(), save.snapshot.data(),
                                                save.snapshot.size()));
}

SavedGame save_game(SimState& sim, std::string name, bool snapshot) {
    SavedGame save;
    save.build = build_id();
    save.name = std::move(name);
    save.tick = sim.tick_count();
    if (snapshot) {
        const auto start = std::chrono::steady_clock::now();
        if (std::string err = save_snapshot(sim, save.snapshot); err.empty()) {
            spdlog::info(
                "Saved game '{}': snapshot of tick {} taken in {:.0f} ms ({:.1f} MB)", save.name,
                save.tick,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                    .count(),
                static_cast<double>(save.snapshot.size()) / 1e6);
        } else {
            // The history alone still loads, by catching up
            spdlog::warn("Saved game '{}': no snapshot ({}); a load will catch up", save.name, err);
            save.snapshot.clear();
        }
    }
    save.game = sim.recorded_replay();
    save.game.final_tick = save.tick;
    // Orders given and not yet run: a click just before saving, or while
    // paused. The engine's own (a dropped peer's defeat) aren't the
    // player's, and only a multiplayer game issues them.
    for (auto& c : sim.command_scheduler().pending()) {
        if (c.source != kEngineSource) save.game.commands.push_back(std::move(c));
    }
    return save;
}

} // namespace osc::sim
