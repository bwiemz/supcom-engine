#include "sim/saved_game.hpp"
#include "sim/build_info.hpp"
#include "sim/command_codec.hpp"
#include "sim/sim_state.hpp"

#include <array>
#include <utility>

namespace osc::sim {

namespace {

constexpr std::array<u8, 7> kMagic = {'O', 'S', 'C', 'S', 'A', 'V', 'E'};

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
    const std::vector<u8> recording(bytes.begin() + static_cast<std::ptrdiff_t>(r.position()),
                                    bytes.end());
    if (!Replay::deserialize(recording, save.game) || !save.game.has_setup ||
        save.game.final_tick != save.tick)
        return SaveLoadError::InvalidFormat;
    out = std::move(save);
    return SaveLoadError::None;
}

SavedGame save_game(const SimState& sim, std::string name) {
    SavedGame save;
    save.build = build_id();
    save.name = std::move(name);
    save.tick = sim.tick_count();
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
