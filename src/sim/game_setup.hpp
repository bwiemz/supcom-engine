#pragma once

#include "core/types.hpp"
#include "sim/sim_random.hpp"

#include <string>
#include <utility>
#include <vector>

namespace osc::sim {

class ByteReader;
class ByteWriter;

/// One army's lobby slot: who plays it and how it starts.
struct ArmySlotConfig {
    bool configured = false;
    /// The lobby slot (1-based) this army was taken from: it plays that
    /// slot's army of the scenario (its FFA team's slot - 1), as Moho names
    /// them, and starts at that army's marker. 0: none (the first army
    /// free).
    int slot = 0;
    bool human = true;
    int faction = 1;
    int team = 1;
    /// PlayerOptions.StartSpot, passed on to ArmySetup. (Retail's lobby
    /// leaves it 1; an army starts at the marker bearing its name.)
    int start_spot = 0;
    int player_color = -1;
    int army_color = -1;
    int handicap = 0; // percent income reduction (0 = none)
    std::string ai_personality;
};

struct GameOptionValue {
    enum class Type {
        String,
        Number,
        Boolean,
    };

    Type type = Type::String;
    std::string string_value;
    double number_value = 0.0;
    bool bool_value = false;
};

/// The lobby's GameOptions, in its order.
struct GameOptionsConfig {
    bool configured = false;
    std::vector<std::pair<std::string, GameOptionValue>> values;
    std::vector<std::string> restricted_categories;

    void set_string(std::string key, std::string value) {
        GameOptionValue option;
        option.type = GameOptionValue::Type::String;
        option.string_value = std::move(value);
        values.emplace_back(std::move(key), std::move(option));
    }

    void set_number(std::string key, double value) {
        GameOptionValue option;
        option.type = GameOptionValue::Type::Number;
        option.number_value = value;
        values.emplace_back(std::move(key), option);
    }

    void set_bool(std::string key, bool value) {
        GameOptionValue option;
        option.type = GameOptionValue::Type::Boolean;
        option.bool_value = value;
        values.emplace_back(std::move(key), option);
    }
};

/// Everything that decides how a game starts: the scenario, the seed, which
/// of its armies play and who plays them, and the options. Every launch path
/// (command line, lobby, replay) builds one and starts the game from it, so a
/// replay's copy starts the same game.
struct GameSetup {
    std::string scenario; // VFS path of the _scenario.lua
    u64 seed = SimRandom::kDefaultSeed;
    /// How many of the scenario's armies play, in its order (0 = all), or
    /// (with slots) the lobby's.
    int army_count = 0;
    /// One per army, when a lobby configured them (empty otherwise): the
    /// slots taken, in order, as Moho packs them.
    std::vector<ArmySlotConfig> slots;
    GameOptionsConfig options;
    /// Armies (0-based) the AI plays when no slots say so, and its personality.
    std::vector<int> ai_armies;
    std::string ai_personality = "adaptive";
    double cheat_mult = 1.0;
    double build_mult = 1.0;
};

/// The game's armies, by name, in order: a lobby's (`setup.slots`) each
/// its slot's army of `scenario_armies` (the scenario's FFA team), as
/// Moho's LaunchGame names them; otherwise the scenario's first
/// `setup.army_count` (all when 0).
std::vector<std::string> session_army_names(const GameSetup& setup,
                                            const std::vector<std::string>& scenario_armies);

void write_game_setup(ByteWriter& w, const GameSetup& setup);
/// False (and `r` failed) on malformed input. `version`: the replay
/// format it was written in (Replay::kVersion's history).
bool read_game_setup(ByteReader& r, GameSetup& setup, u32 version);

} // namespace osc::sim
