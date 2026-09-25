#pragma once

// What the binding files share (M191): moho_bindings.cpp's helpers, the
// class files' (lua/bindings/) and the user bindings'. Internal to the Lua
// libraries: not an API.

#include "core/types.hpp"

#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <lua.h>
}

namespace osc::audio {
class SoundManager;
}

namespace osc::sim {
class Entity;
class Manipulator;
class Platoon;
class Projectile;
class SimCallbackQueue;
class SimState;
class Unit;
class Weapon;
struct UnitCommand;
struct Vector3;
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

// ── Shared by the binding files (M191 step 2: moho_bindings.cpp split by
// class into lua/bindings/) ──

/// A binding method: its Lua name and C function.
struct MethodEntry {
    const char* name;
    lua_CFunction func;
};

// lua/moho_bindings.cpp
sim::Unit* check_unit(lua_State* L, int idx = 1);
sim::Weapon* check_weapon(lua_State* L, int idx = 1);
sim::Platoon* check_platoon(lua_State* L, int idx = 1);
audio::SoundManager* get_sound_mgr(lua_State* L);
bool extract_sound_table(lua_State* L, int idx, std::string& bank, std::string& cue,
                         std::string* lod_cutoff = nullptr);
void push_vector3(lua_State* L, const sim::Vector3& v);
f32 get_unit_threat_for_type(const sim::Unit* unit, const char* type);
void stop_ambient(audio::SoundManager* mgr, sim::Entity* e, const char* name);
i32 resolve_bone_index(const sim::Entity* e, lua_State* L, int arg);
sim::Vector3 bone_world_position(const sim::Entity* e, i32 bone_idx);
bool under_water(const sim::SimState* sim, const sim::Vector3& p);
void apply_script_projectile_physics(lua_State* L, sim::Projectile& p);
std::string lowercase_arg(lua_State* L, int idx);
sim::Manipulator* check_manip_base(lua_State* L);
int been_destroyed_check(lua_State* L);
std::pair<i32, i32> read_dds_dimensions(lua_State* L, const std::string& path);
u32 parse_color_hex(const char* s);
void update_font_metrics(ui::UIControl* ctrl);
void update_text_advance(ui::UIControl* ctrl);
void push_font_lazyvars(lua_State* L, int self_idx, ui::UIControl* ctrl);

// lua/bindings/sim/entity.cpp
int entity_IsValidBone(lua_State* L);
int entity_GetBoneDirection(lua_State* L);
int entity_GetHeading(lua_State* L);
int entity_Destroy(lua_State* L);
int entity_Kill(lua_State* L);
int entity_SetMesh(lua_State* L);
int entity_SetCollisionShape(lua_State* L);
int entity_RevertCollisionShape(lua_State* L);
int entity_SetUnSelectable(lua_State* L);
int entity_SetDoNotTarget(lua_State* L);
int entity_SetReclaimable(lua_State* L);
bool sound_arg(lua_State* L, int idx, std::string& bank, std::string& cue);

// lua/bindings/sim/manipulators.cpp
void aim_projectile(lua_State* L, sim::SimState* sim, sim::Projectile& p, const sim::Vector3& dir);
int sequence_count(lua_State* L, int table_idx);

// lua/bindings/sim/unit.cpp
bool unit_is_idle(const sim::Unit& u);

/// The classes' method tables (register_moho_bindings makes moho.* of them).
extern const MethodEntry aibrain_methods[];
extern const MethodEntry aim_manipulator_methods[];
extern const MethodEntry animation_manipulator_methods[];
extern const MethodEntry blip_methods[];
extern const MethodEntry builder_arm_methods[];
extern const MethodEntry camera_methods[];
extern const MethodEntry collision_beam_methods[];
extern const MethodEntry entity_category_methods[];
extern const MethodEntry entity_methods[];
extern const MethodEntry ieffect_methods[];
extern const MethodEntry manipulator_methods[];
extern const MethodEntry navigator_methods[];
extern const MethodEntry platoon_methods[];
extern const MethodEntry projectile_methods[];
extern const MethodEntry prop_methods[];
extern const MethodEntry rotate_manipulator_methods[];
extern const MethodEntry shield_methods[];
extern const MethodEntry slide_manipulator_methods[];
extern const MethodEntry ui_bitmap_methods[];
extern const MethodEntry ui_border_methods[];
extern const MethodEntry ui_control_methods[];
extern const MethodEntry ui_cursor_methods[];
extern const MethodEntry ui_discovery_methods[];
extern const MethodEntry ui_dragger_methods[];
extern const MethodEntry ui_edit_methods[];
extern const MethodEntry ui_frame_methods[];
extern const MethodEntry ui_histogram_methods[];
extern const MethodEntry ui_item_list_methods[];
extern const MethodEntry ui_lobby_methods[];
extern const MethodEntry ui_map_preview_methods[];
extern const MethodEntry ui_movie_methods[];
extern const MethodEntry ui_scrollbar_methods[];
extern const MethodEntry ui_text_methods[];
extern const MethodEntry ui_wlduiprovider_methods[];
extern const MethodEntry ui_world_mesh_methods[];
extern const MethodEntry ui_worldview_methods[];
extern const MethodEntry unit_methods[];
extern const MethodEntry weapon_methods[];

} // namespace osc::lua
