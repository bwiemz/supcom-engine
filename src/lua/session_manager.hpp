#pragma once

#include "core/result.hpp"
#include "core/types.hpp"
#include "lua/scenario_loader.hpp"

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
class SimState;
}

namespace osc::lua {

struct ArmySlotConfig {
    bool configured = false;
    bool human = true;
    int faction = 1;
    int team = 1;
    int start_spot = 0; // 1-based scenario marker index; 0 = default slot
    int player_color = -1;
    int army_color = -1;
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

/// Read a lobby GameOptions table from Lua into a C++ config that can be
/// applied on the sim Lua state after reload.
GameOptionsConfig read_game_options(lua_State* L, int table_idx);

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

    /// Limit the number of non-civilian armies created (0 = no limit).
    void set_max_armies(int n) { max_armies_ = n; }

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
    void set_cheat_mult(double m) { cheat_mult_ = m; }
    void set_build_mult(double m) { build_mult_ = m; }

    bool is_ai_army(int index) const {
        if (const auto* cfg = slot_config_for_army(index)) {
            return !cfg->human;
        }
        return std::find(ai_army_indices_.begin(), ai_army_indices_.end(),
                         index) != ai_army_indices_.end();
    }

private:
    void setup_army_info(lua_State* L, const ScenarioMetadata& meta);
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
    int max_armies_ = 0; // 0 = no limit
    std::string ai_personality_ = "adaptive";
    double cheat_mult_ = 2.0;
    double build_mult_ = 2.0;
};

} // namespace osc::lua
