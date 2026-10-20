#pragma once

// The match records the game keeps beside each replay (the `.jsonl` of the
// same name, app/match_telemetry.hpp), read for the game's own screens: a
// player's match history and the statistics after a match (WOPC, 2026-10-14:
// "as a player, I expect to have everything related to this game in this
// game").
//
// GetMatchRecord(profile, base[, summary]) -> the record of the replay
// `base` in `profile`'s replays, as a Lua table, or nil when there is none.
// With `summary` true, the match and each army's totals only (for a list of
// matches); otherwise also each army's samples (at most kMaxPoints, evenly
// spaced), the structures it built and when, the other units it built, and
// the notable events (finished units destroyed, commanders hit, upgrades,
// results).

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

struct lua_State;

namespace osc::lua {

class LuaState;

/// Samples kept for each army: enough for a graph across a screen
inline constexpr size_t kMaxPoints = 240;
/// Notable events kept
inline constexpr size_t kMaxNotable = 1000;

/// The record at `path`, read: null when there is no such file or it holds
/// no match. See GetMatchRecord above for what a summary leaves out.
std::optional<nlohmann::json> read_match_record(const std::filesystem::path& path, bool summary);

/// GetMatchRecord and WopcMarkMoment, for the UI state (needs its special
/// files registered).
void register_match_record_bindings(LuaState& state);

/// The moments the player marked (WopcMarkMoment(note)) since the window
/// last wrote them into the match record, each its note.
std::vector<std::string>& pending_marks();

/// A JSON value as a Lua value on the stack (arrays 1-based).
void push_json(lua_State* L, const nlohmann::json& value);

} // namespace osc::lua
