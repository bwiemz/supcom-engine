#pragma once

#include "core/result.hpp"
#include "core/types.hpp"
#include "lua/scenario_loader.hpp"
#include "sim/game_colors.hpp"
#include "sim/game_setup.hpp"

#include <algorithm>
#include <string>
#include <vector>

struct lua_State;

namespace osc::lua {
class LuaState;
}
namespace osc::vfs {
class VirtualFileSystem;
}
namespace osc::sim {
class ArmyBrain;
class SimState;
}

namespace osc::lua {

// The lobby's setup types live with the rest of a game's setup, in the sim
// (a replay carries them).
using ArmySlotConfig = sim::ArmySlotConfig;
using GameOptionValue = sim::GameOptionValue;
using GameOptionsConfig = sim::GameOptionsConfig;

/// Read a lobby GameOptions table from Lua into a C++ config that can be
/// applied on the sim Lua state after reload.
GameOptionsConfig read_game_options(lua_State* L, int table_idx);

/// A lobby's sessionConfig table (GameOptions, PlayerOptions) as a game
/// setup: which armies play, who plays each, and the options. The scenario
/// and seed are the caller's.
sim::GameSetup read_session_config(lua_State* L, int table_idx);

/// Give an army's brain its slot's faction, colour and handicap. The slot's
/// PlayerColor is a Lua index into colors.player_colors; a civilian army
/// takes colors.civilian_army_color instead (Moho's CArmyImpl).
void apply_config_to_brain(const ArmySlotConfig* cfg, sim::ArmyBrain* brain,
                           const sim::GameColors& colors);

/// Whether the session's scripts decide the game, so the engine's own
/// adjudication stands down, as Moho has none: retail's /lua/victory.lua
/// (its CheckVictory, started by a schook hook of BeginSession), or FAF's
/// victory condition (/lua/sim/victorycondition/, started by its
/// BeginSession). Asked once BeginSession has run: `__modules` holds what
/// has been imported (names lower-cased, as both import.lua's keep them).
bool scripts_decide_victory(lua_State* L);

class SessionManager {
public:
    /// Run the full session lifecycle:
    /// 1. Populate ScenarioInfo.ArmySetup
    /// 2. Call SetupSession() (loads save/script files)
    /// 3. Extract start positions from Scenario.MasterChain markers
    /// 4. Create army brains (C++ + Lua tables + OnCreateArmyBrain calls)
    /// 5. Call BeginSession()
    Result<void> start_session(LuaState& state,
                               const vfs::VirtualFileSystem& vfs,
                               sim::SimState& sim,
                               const ScenarioMetadata& meta);

    bool session_active() const { return session_active_; }

    /// Mark specific army indices (0-based) as AI (Human=false).
    void set_ai_armies(const std::vector<int>& indices) {
        ai_army_indices_ = indices;
    }

    /// Set AI personality key for AI armies (e.g., "adaptive", "rush", "turtle",
    /// "tech", "adaptivecheat").  Default: "adaptive".
    void set_ai_personality(const std::string& p) { ai_personality_ = p; }

    void set_army_slot_configs(const std::vector<ArmySlotConfig>& configs) {
        army_slot_configs_ = configs;
    }

    void set_game_options(const GameOptionsConfig& options) {
        game_options_ = options;
        if (options.configured) {
            for (const auto& [key, value] : options.values) {
                if (key == "CheatMult" && value.type == GameOptionValue::Type::Number) {
                    cheat_mult_ = value.number_value;
                } else if (key == "BuildMult" &&
                           value.type == GameOptionValue::Type::Number) {
                    build_mult_ = value.number_value;
                }
            }
        }
    }

    const ArmySlotConfig* slot_config_for_army(int index) const {
        if (index < 0 ||
            index >= static_cast<int>(army_slot_configs_.size()) ||
            !army_slot_configs_[static_cast<size_t>(index)].configured) {
            return nullptr;
        }
        return &army_slot_configs_[static_cast<size_t>(index)];
    }

    /// Set cheat multipliers (only used when personality ends with "cheat").
    /// Configure the session from a game's setup (its slots, options and AI
    /// armies), as every launch path does.
    void configure(const sim::GameSetup& setup);

    void set_cheat_mult(double m) { cheat_mult_ = m; }
    void set_build_mult(double m) { build_mult_ = m; }

    bool is_ai_army(int index) const {
        if (const auto* cfg = slot_config_for_army(index)) {
            return !cfg->human;
        }
        return std::find(ai_army_indices_.begin(), ai_army_indices_.end(),
                         index) != ai_army_indices_.end();
    }

    /// The lobby's prebuilt units, after BeginSession (Moho's
    /// Sim::PostInitialize): with ScenarioInfo.Options.PrebuiltUnits "On",
    /// the script's InitializePrebuiltUnits for each army but the civilians.
    void spawn_prebuilt_units(lua_State* L, sim::SimState& sim);

private:
    /// ScenarioInfo.ArmySetup, one entry per army of the game, by name, and
    /// ScenarioInfo.Options' defaults (`operation`: a campaign operation's
    /// launch, which names no victory condition).
    void setup_army_info(lua_State* L, const std::vector<std::string>& armies, bool operation);
    Result<void> call_setup_session(lua_State* L);
    void extract_start_positions(lua_State* L, sim::SimState& sim);
    Result<void> create_army_brain(lua_State* L, sim::SimState& sim,
                                   i32 index, const std::string& name,
                                   const std::string& nickname);
    Result<void> call_begin_session(lua_State* L);

    bool session_active_ = false;
    std::vector<int> ai_army_indices_;
    std::vector<ArmySlotConfig> army_slot_configs_;
    GameOptionsConfig game_options_;
    std::string ai_personality_ = "adaptive";
    double cheat_mult_ = 2.0;
    double build_mult_ = 2.0;
    std::string campaign_info_; ///< sim::lua_to_bytes's, or empty
    bool tutorial_ = false;
};

} // namespace osc::lua
