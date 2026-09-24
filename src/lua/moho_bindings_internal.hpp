#pragma once

// Helpers moho_bindings.cpp shares with the other binding files (the user
// bindings, M191). Internal to the Lua libraries: not an API.

#include "core/types.hpp"

#include <string>
#include <vector>

struct lua_State;

namespace osc::sim {
class Entity;
class SimCallbackQueue;
class SimState;
struct UnitCommand;
} // namespace osc::sim

namespace osc::ui {
class UIControl;
class UIControlRegistry;
class WorldView;
} // namespace osc::ui

namespace osc::lua {

sim::SimState* get_sim(lua_State* L);
sim::Entity* check_entity(lua_State* L, int idx = 1);
ui::UIControl* check_control(lua_State* L, int idx = 1);
ui::WorldView* check_world_view(lua_State* L, int idx = 1);
ui::UIControlRegistry* get_ui_registry(lua_State* L);
sim::SimCallbackQueue* get_callback_queue(lua_State* L);
/// Push a unit as the UI state sees it (a UserUnit), or nil.
void push_unit_for_ui(lua_State* L, sim::Entity* entity);
/// Push an entity's blueprint table; false (nothing pushed) without one.
bool push_entity_blueprint(lua_State* L, const sim::Entity* e);
/// A LazyVar field `name` on the table at `self_idx`.
void create_lazyvar(lua_State* L, int self_idx, const char* name);
/// A table {x, y, z} (or {x, z}) at `idx`: its x and z.
bool read_xz(lua_State* L, int idx, f32& x, f32& z);
/// A player's order for these units, through the command path.
void issue_player_order(lua_State* L, const std::vector<u32>& ids, const sim::UnitCommand& cmd,
                        bool clear);
/// A targetless order by its UI name (RULEUCC_Stop, ...).
void issue_targetless_order(lua_State* L, const std::vector<u32>& ids, const std::string& name,
                            int data, bool clear);
/// The command name the UI gives an order (e.g. "Stop" for RULEUCC_Stop).
std::string ui_command_name(const char* name);

} // namespace osc::lua
