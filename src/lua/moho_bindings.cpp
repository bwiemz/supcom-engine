#include "lua/moho_bindings.hpp"
#include "lua/moho_bindings_internal.hpp"
#include "core/dmath.hpp"
#include "sim/blueprint_categories.hpp"
#include "lua/category_utils.hpp"
#include "video/video_decoder.hpp"
#include "map/scmap_parser.hpp"
#include "lua/factory_queue.hpp"
#include "lua/order_helpers.hpp"
#include "lua/lua_state.hpp"
#include "lua/sim_bindings.hpp"
#include "sim/army_brain.hpp"
#include "sim/build_placement.hpp"
#include "sim/bone_data.hpp"
#include "sim/collision.hpp"
#include "sim/entity.hpp"
#include "sim/entity_registry.hpp"
#include "sim/ieffect.hpp"
#include "sim/manipulator.hpp"
#include "core/test_status.hpp"
#include "sim/category_expr.hpp"
#include "sim/prop.hpp"
#include "sim/prop_script.hpp"
#include "sim/sim_state.hpp"
#include "sim/collision_beam.hpp"
#include "sim/projectile_script.hpp"
#include "sim/thread_manager.hpp"
#include "map/visibility_grid.hpp"
#include "sim/unit.hpp"
#include "sim/navigator.hpp"
#include "sim/platoon.hpp"
#include "sim/projectile.hpp"
#include "sim/shield.hpp"
#include "sim/unit_command.hpp"
#include "map/pathfinder.hpp"
#include "map/pathfinding_grid.hpp"
#include "sim/weapon.hpp"
#include "blueprints/blueprint_store.hpp"
#include "audio/sound_manager.hpp"
#include "ui/ui_control.hpp"
#include "ui/world_view.hpp"
#include "ui/font_metrics_provider.hpp"
#include "ui/keymap.hpp"
#include "ui/wld_ui_provider.hpp"
#include "sim/sim_callback_queue.hpp"
#include "map/terrain.hpp"
#include "vfs/virtual_file_system.hpp"
#include "core/front_end_data.hpp"
#include "core/game_state.hpp"
#include "core/localization.hpp"
#include "core/preferences.hpp"
#include "lua/beat_system.hpp"
#include "lua/mp_net_state.hpp"
#include "lua/sim_sync.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::lua {

// ====================================================================
// Helper: extract C++ pointers from Lua self tables
// ====================================================================

sim::SimState* get_sim(lua_State* L) {
    lua_pushstring(L, "osc_sim_state");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* sim = static_cast<sim::SimState*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return sim;
}

/// True for a weapon's Lua table: its _c_object is a Weapon*, not an Entity*.
static bool is_weapon_table(lua_State* L, int idx) {
    lua_pushstring(L, "_c_unit");
    lua_rawget(L, idx);
    const bool weapon = lua_isuserdata(L, -1);
    lua_pop(L, 1);
    return weapon;
}

sim::Entity* check_entity(lua_State* L, int idx) {
    if (!lua_istable(L, idx) || is_weapon_table(L, idx)) return nullptr;

    // Check sim generation — stale handles from a previous SimState return nullptr
    lua_pushstring(L, "_c_sim_gen");
    lua_rawget(L, idx);
    if (lua_isnumber(L, -1)) {
        u32 stored_gen = static_cast<u32>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        if (stored_gen != sim::SimState::sim_generation()) {
            return nullptr;  // Stale handle from old SimState
        }
    } else {
        lua_pop(L, 1);
        // No generation stored — legacy table, allow through
    }

    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    if (lua_isuserdata(L, -1)) {
        auto* entity = static_cast<sim::Entity*>(lua_touserdata(L, -1));
        lua_pop(L, 1);
        return entity;
    }
    lua_pop(L, 1);

    // A handle by id (the UI state's unit objects): those outlive their
    // entity -- scripts keep avatars, idle lists and selections -- so each
    // call resolves the id; the registry stops listing an entity as soon as
    // it is unregistered, long before its memory is freed.
    lua_pushstring(L, "_c_entity_id");
    lua_rawget(L, idx);
    const bool by_id = lua_isnumber(L, -1);
    const auto id = by_id ? static_cast<u32>(lua_tonumber(L, -1)) : 0u;
    lua_pop(L, 1);
    if (!by_id) return nullptr;
    auto* sim = get_sim(L);
    return sim ? sim->entity_registry().find(id) : nullptr;
}

sim::Unit* check_unit(lua_State* L, int idx) {
    auto* e = check_entity(L, idx);
    if (e && e->is_unit())
        return static_cast<sim::Unit*>(e);
    return nullptr;
}

sim::Weapon* check_weapon(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* w = lua_isuserdata(L, -1)
                  ? static_cast<sim::Weapon*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    return w;
}

sim::Platoon* check_platoon(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* p = lua_isuserdata(L, -1)
                  ? static_cast<sim::Platoon*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    return (p && !p->destroyed()) ? p : nullptr;
}

audio::SoundManager* get_sound_mgr(lua_State* L) {
    lua_pushstring(L, "osc_sound_manager");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* mgr = static_cast<audio::SoundManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return mgr;
}

static osc::FrontEndData* get_front_end_data(lua_State* L) {
    lua_pushstring(L, "__osc_front_end_data");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* d = static_cast<osc::FrontEndData*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return d;
}

/// Extract Bank and Cue strings from a sound table at the given stack index.
/// Returns false if the table is missing or lacks Bank/Cue keys.
bool extract_sound_table(lua_State* L, int idx, std::string& bank, std::string& cue,
                         std::string* lod_cutoff) {
    if (!lua_istable(L, idx)) return false;
    if (lod_cutoff) {
        // Sound{..., LodCutoff = 'Weapon_LodCutoff'}: the variable whose value
        // is how far away the sound is still heard.
        lua_pushstring(L, "LodCutoff");
        lua_rawget(L, idx);
        lod_cutoff->assign(lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
    }

    lua_pushstring(L, "Bank");
    lua_rawget(L, idx);
    if (lua_type(L, -1) != LUA_TSTRING) { lua_pop(L, 1); return false; }
    bank = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_pushstring(L, "Cue");
    lua_rawget(L, idx);
    if (lua_type(L, -1) != LUA_TSTRING) { lua_pop(L, 1); return false; }
    cue = lua_tostring(L, -1);
    lua_pop(L, 1);

    return true;
}

// Push a Vector3 as a Lua table {[1]=x, [2]=y, [3]=z}
void push_vector3(lua_State* L, const sim::Vector3& v) {
    lua_newtable(L);
    lua_pushnumber(L, 1);
    lua_pushnumber(L, v.x);
    lua_settable(L, -3);
    lua_pushnumber(L, 2);
    lua_pushnumber(L, v.y);
    lua_settable(L, -3);
    lua_pushnumber(L, 3);
    lua_pushnumber(L, v.z);
    lua_settable(L, -3);
    push_vector_metatable(L); // pos.x as well as pos[1], as Moho's vectors
    lua_setmetatable(L, -2);
}

// ====================================================================
// Stub helpers
// ====================================================================

// Stub functions — shared definitions in lua_stubs.hpp, local aliases for brevity
#include "lua/lua_stubs.hpp"
static int (*const stub_noop)(lua_State*) = lua_stubs::noop;
// ====================================================================
// Threat helper
// ====================================================================

/// Map a threat type string to the appropriate cached threat value on a unit.
/// Returns 0 if the type doesn't apply (e.g. "Commander" for a non-COMMAND unit).
f32 get_unit_threat_for_type(const sim::Unit* unit, const char* type) {
    if (!type || !unit) return 0;

    if (std::strcmp(type, "AntiSurface") == 0 ||
        std::strcmp(type, "Surface") == 0 ||
        std::strcmp(type, "Land") == 0) {
        return unit->surface_threat();
    }
    if (std::strcmp(type, "AntiAir") == 0 ||
        std::strcmp(type, "Air") == 0) {
        return unit->air_threat();
    }
    if (std::strcmp(type, "Sub") == 0 ||
        std::strcmp(type, "SubSurface") == 0) {
        return unit->sub_threat();
    }
    if (std::strcmp(type, "Economy") == 0) {
        return unit->economy_threat();
    }
    if (std::strcmp(type, "Commander") == 0) {
        return unit->has_category("COMMAND") ? unit->surface_threat() : 0;
    }
    if (std::strcmp(type, "Structures") == 0) {
        return unit->has_category("STRUCTURE") ? unit->surface_threat() : 0;
    }
    if (std::strcmp(type, "StructuresNotMex") == 0) {
        return (unit->has_category("STRUCTURE") &&
                !unit->has_category("MASSEXTRACTION"))
                   ? unit->surface_threat()
                   : 0;
    }
    if (std::strcmp(type, "Overall") == 0) {
        return unit->surface_threat() + unit->air_threat() +
               unit->sub_threat() + unit->economy_threat();
    }
    // Unknown type — return overall as fallback
    return unit->surface_threat() + unit->air_threat() +
           unit->sub_threat() + unit->economy_threat();
}

// ====================================================================
// Sound methods
// ====================================================================

/// Stop the ambient loop `name` of `e` (every one when `name` is null).
void stop_ambient(audio::SoundManager* mgr, sim::Entity* e, const char* name) {
    if (!name) {
        for (const auto& a : e->take_ambient_sounds())
            if (mgr) mgr->stop(a.handle, false);
        return;
    }
    if (const u32 h = e->ambient_sound(name)) {
        if (mgr) mgr->stop(h, false);
        e->set_ambient_sound(name, 0);
    }
}

/// Push the entity's blueprint table from the blueprint store (what
/// GetBlueprint returns). Engine code reads blueprints this way, never through
/// self.Blueprint: that is a field FAF's scripts set and retail's do not.
/// Pushes nothing and returns false when the entity has no blueprint.
bool push_entity_blueprint(lua_State* L, const sim::Entity* e) {
    if (!e || e->blueprint_id().empty()) return false;
    auto* store = LuaState::get_blueprint_store(L);
    if (!store) return false;
    auto* entry = store->find(e->blueprint_id());
    if (!entry) return false;
    store->push_lua_table(*entry, L);
    if (lua_istable(L, -1)) return true;
    lua_pop(L, 1);
    return false;
}

// --- Bone helper functions ---

/// Resolve bone argument (string name or integer index) → bone index.
/// Returns 0 (root) if not found.
i32 resolve_bone_index(const sim::Entity* e, lua_State* L, int arg) {
    auto* bd = e->bone_data();
    if (!bd) return 0; // fallback root

    if (lua_type(L, arg) == LUA_TSTRING) {
        std::string name = lua_tostring(L, arg);
        i32 idx = bd->find_bone(name);
        return (idx >= 0) ? idx : 0; // fallback to root
    }
    if (lua_type(L, arg) == LUA_TNUMBER) {
        i32 idx = static_cast<i32>(lua_tonumber(L, arg));
        return bd->is_valid(idx) ? idx : 0;
    }
    return 0; // default to root
}

/// Compute world-space bone position for an entity: a unit's as posed by
/// its manipulators (turrets, rotators), anything else's in bind pose.
sim::Vector3 bone_world_position(const sim::Entity* e, i32 bone_idx) {
    if (e->is_unit()) return static_cast<const sim::Unit*>(e)->bone_world_position(bone_idx);
    auto* bd = e->bone_data();
    if (!bd || !bd->is_valid(bone_idx)) return e->position();

    auto& bone = bd->bones[static_cast<size_t>(bone_idx)];
    const f32 s = bd->model_scale;
    auto rotated = sim::quat_rotate(e->orientation(), sim::Vector3{bone.world_position.x * s,
                                                                   bone.world_position.y * s,
                                                                   bone.world_position.z * s});
    return {
        e->position().x + rotated.x,
        e->position().y + rotated.y,
        e->position().z + rotated.z
    };
}

/// Whether a point is under the map's water (a projectile's OnCreate(inWater)).
bool under_water(const sim::SimState* sim, const sim::Vector3& p) {
    const auto* t = sim ? sim->terrain() : nullptr;
    return t && t->has_water() && p.y < t->water_elevation();
}

/// A projectile a script creates takes its blueprint's Physics, falling at
/// Moho's gravity unless that says not (debris and cluster bomblets fall).
void apply_script_projectile_physics(lua_State* L, sim::Projectile& p) {
    const sim::Projectile::BlueprintPhysics physics = p.apply_blueprint_physics(L);
    if (physics.use_gravity.value_or(true)) p.ballistic_accel = -sim::Projectile::GRAVITY;
    p.lifetime = physics.lifetime.value_or(10.0f);
}

std::string lowercase_arg(lua_State* L, int idx) {
    if (lua_type(L, idx) != LUA_TSTRING) return {};
    std::string s = lua_tostring(L, idx);
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// ====================================================================
// Manipulator method implementations
// ====================================================================

/// Extract Manipulator* from self table's _c_object.
sim::Manipulator* check_manip_base(lua_State* L) {
    if (!lua_istable(L, 1)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, 1);
    auto* m = lua_isuserdata(L, -1)
                  ? static_cast<sim::Manipulator*>(lua_touserdata(L, -1))
                  : nullptr;
    lua_pop(L, 1);
    return (m && !m->is_destroyed()) ? m : nullptr;
}

// Destroy/BeenDestroyed for tracked objects (IEffect, CollisionBeam, decal)
// Sets _destroyed = true on the Lua self table
static int destroy_tracked_object(lua_State* L) {
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "_destroyed");
        lua_pushboolean(L, 1);
        lua_rawset(L, 1);
    }
    return 0;
}

// Checks _destroyed field on self
int been_destroyed_check(lua_State* L) {
    if (lua_istable(L, 1)) {
        lua_pushstring(L, "_destroyed");
        lua_rawget(L, 1);
        bool destroyed = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_pushboolean(L, destroyed ? 1 : 0);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

// CollisionManipulator: WatchBone(boneName)
static int coldet_WatchBone(lua_State* L) {
    auto* m = check_manip_base(L);
    if (!m || !m->owner()) return 0;
    auto* cd = static_cast<sim::CollisionDetectorManipulator*>(m);
    i32 bone_idx = resolve_bone_index(m->owner(), L, 2);
    cd->watch_bone(bone_idx);
    // Return self for chaining
    lua_pushvalue(L, 1);
    return 1;
}

// clang-format off
static const MethodEntry collision_manipulator_methods[] = {
    {"WatchBone",               coldet_WatchBone},
    {nullptr, nullptr},
};
// clang-format on

// Minimal entries for other classes
// A thrust controller turns an aircraft's engines with its motion
// (UEA0107 sets their arcs). It only moves bones on screen, which our air
// movement doesn't drive yet, so its arcs change nothing.
// clang-format off
static const MethodEntry thrust_manipulator_methods[] = {
    {"SetThrustingParam", stub_noop},
    {nullptr, nullptr},
};
// clang-format on

// clang-format off
static const MethodEntry empty_methods[] = {
    {nullptr, nullptr},
};
// clang-format on

// clang-format off
static const MethodEntry economy_event_methods[] = {
    {"Destroy",                 stub_noop},
    {nullptr, nullptr},
};
// clang-format on

// clang-format off
static const MethodEntry decal_handle_methods[] = {
    {"Destroy",                 destroy_tracked_object},
    {"BeenDestroyed",           been_destroyed_check},
    {nullptr, nullptr},
};
// clang-format on

// ====================================================================
// UI Control methods (M71)
// ====================================================================

ui::UIControlRegistry* get_ui_registry(lua_State* L) {
    lua_pushstring(L, "osc_ui_registry");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* reg = static_cast<ui::UIControlRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return reg;
}

ui::UIControl* check_control(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_pushstring(L, "_c_object");
    lua_rawget(L, idx);
    auto* ctrl = static_cast<ui::UIControl*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return ctrl;
}

// clang-format off
static const MethodEntry ui_group_methods[] = {
    // Group has no extra methods beyond control_methods
    {nullptr, nullptr},
};
// clang-format on

// --- Bitmap methods (M72) ---

/// Helper: read DDS width/height from VFS file data.
/// DDS header: bytes 12-15 = height, bytes 16-19 = width.
std::pair<i32, i32> read_dds_dimensions(lua_State* L, const std::string& path) {
    auto* vfs = lua::LuaState::get_vfs(L);
    if (!vfs) return {0, 0};
    auto data = vfs->read_file(path);
    if (!data || data->size() < 128) return {0, 0};
    // Validate DDS magic "DDS " = 0x20534444
    u32 magic;
    std::memcpy(&magic, data->data(), 4);
    if (magic != 0x20534444) return {0, 0};
    u32 h_raw, w_raw;
    std::memcpy(&h_raw, data->data() + 12, 4);
    std::memcpy(&w_raw, data->data() + 16, 4);
    if (h_raw > 65536 || w_raw > 65536) return {0, 0};
    return {static_cast<i32>(w_raw), static_cast<i32>(h_raw)};
}

// ====================================================================
// Text methods (M73)
// ====================================================================

/// Helper: parse a hex color string like "ff00ff00" or "AARRGGBB" to u32.
u32 parse_color_hex(const char* s) {
    if (!s) return 0xFFFFFFFF;
    // Count hex digits
    int len = 0;
    for (int i = 0; i < 8 && s[i]; i++) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))
            len++;
        else
            break;
    }
    u32 val = 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        u32 nibble = 0;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') nibble = 10 + (c - 'A');
        val = (val << 4) | nibble;
    }
    // 6-char hex = RRGGBB → default alpha to FF (fully opaque)
    if (len <= 6) val |= 0xFF000000;
    return val;
}

/// Helper: update font metrics on a control using stb_truetype via FontMetricsProvider.
/// Falls back to heuristics if the font file is not available.
void update_font_metrics(ui::UIControl* ctrl) {
    f32 ps = static_cast<f32>(ctrl->font_pointsize());
    auto& fmp = ui::FontMetricsProvider::instance();
    ui::FontMetricsProvider::Metrics m;
    if (fmp.get_metrics(ctrl->font_family(), ctrl->font_pointsize(), m)) {
        ctrl->set_font_ascent(m.ascent);
        ctrl->set_font_descent(m.descent);
        ctrl->set_font_external_leading(m.external_leading);
    } else {
        // Heuristic fallback
        ctrl->set_font_ascent(ps * 0.8f);
        ctrl->set_font_descent(ps * 0.2f);
        ctrl->set_font_external_leading(ps * 0.05f);
    }
}

/// Helper: update text advance (width) for the current text content.
/// Uses stb_truetype per-glyph advances; falls back to heuristic.
void update_text_advance(ui::UIControl* ctrl) {
    auto& fmp = ui::FontMetricsProvider::instance();
    f32 adv = fmp.string_advance(ctrl->font_family(), ctrl->font_pointsize(),
                                  ctrl->text_content());
    if (adv >= 0.0f) {
        ctrl->set_text_advance(adv);
    } else {
        f32 ps = static_cast<f32>(ctrl->font_pointsize());
        ctrl->set_text_advance(ps * 0.6f * ctrl->text_content().size());
    }
}

/// Helper: call LazyVar:Set(value) on self[name].
/// Uses lua_gettable (not lua_rawget) to find Set through metatables.
static void set_lazyvar_value(lua_State* L, int self_idx, const char* name, f32 value) {
    if (self_idx < 0) self_idx = lua_gettop(L) + self_idx + 1;
    int top = lua_gettop(L);
    lua_pushstring(L, name);
    lua_rawget(L, self_idx);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "Set");
        lua_gettable(L, -2); // use gettable to check metatables
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2); // LazyVar self
            lua_pushnumber(L, value);
            if (lua_pcall(L, 2, 0, 0) != 0) {
                lua_pop(L, 1); // pop error message
            }
        } else {
            lua_pop(L, 1);
        }
    }
    lua_settop(L, top); // restore stack cleanly
}

/// Helper: update FontAscent/FontDescent/FontExternalLeading/TextAdvance LazyVars
/// on the Lua self table after font metrics change.
void push_font_lazyvars(lua_State* L, int self_idx, ui::UIControl* ctrl) {
    if (self_idx < 0) self_idx = lua_gettop(L) + self_idx + 1;
    set_lazyvar_value(L, self_idx, "FontAscent", ctrl->font_ascent());
    set_lazyvar_value(L, self_idx, "FontDescent", ctrl->font_descent());
    set_lazyvar_value(L, self_idx, "FontExternalLeading", ctrl->font_external_leading());
    set_lazyvar_value(L, self_idx, "TextAdvance", ctrl->text_advance());

    // Auto-size text control: Width = TextAdvance, Height = ascent + descent
    // FA's engine does this internally so layout works for text-based controls.
    if (ctrl->text_advance() > 0) {
        set_lazyvar_value(L, self_idx, "Width", ctrl->text_advance());
    }
    f32 line_height = ctrl->font_ascent() + std::abs(ctrl->font_descent());
    if (line_height > 0) {
        set_lazyvar_value(L, self_idx, "Height", line_height);
    }
}

// --- Factory functions (registered as globals) ---

/// Helper: create a LazyVar for a control and store it as self[name].
/// Calls LazyVar.Create(0) from /lua/lazyvar.lua.
void create_lazyvar(lua_State* L, int self_idx, const char* name) {
    // Normalize to absolute index before pushing anything
    if (self_idx < 0) self_idx = lua_gettop(L) + self_idx + 1;

    // Get the LazyVar Create function from the lazyvar module
    lua_pushstring(L, "__osc_lazyvar_create");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isfunction(L, -1)) {
        lua_pushnumber(L, 0);
        lua_call(L, 1, 1); // returns LazyVar table
    } else {
        lua_pop(L, 1);
        // Fallback: create a simple table with __call that returns [1]
        lua_newtable(L);
        lua_pushnumber(L, 0);
        lua_rawseti(L, -2, 1);
    }
    lua_pushstring(L, name);
    lua_pushvalue(L, -2); // dup LazyVar
    lua_rawset(L, self_idx);
    lua_pop(L, 1); // pop LazyVar
}

/// InternalCreateGroup(self, parent)
static int l_InternalCreateGroup(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateGroup: no UIControlRegistry");

    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateGroup: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateGroup: failed to create control");

    // Store Lua table reference
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    // Set _c_object lightuserdata
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    // Set parent if provided
    if (lua_istable(L, 2)) {
        auto* parent = check_control(L, 2);
        if (parent) ctrl->set_parent(parent);
    }

    // Create 7 LazyVars: Left, Top, Right, Bottom, Width, Height, Depth
    create_lazyvar(L, 1, "Left");
    create_lazyvar(L, 1, "Top");
    create_lazyvar(L, 1, "Right");
    create_lazyvar(L, 1, "Bottom");
    create_lazyvar(L, 1, "Width");
    create_lazyvar(L, 1, "Height");
    create_lazyvar(L, 1, "Depth");

    // Call OnInit if it exists (lua_gettable for metatable lookup)
    lua_pushstring(L, "OnInit");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("InternalCreateGroup: OnInit error: {}",
                         lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    spdlog::debug("InternalCreateGroup: control #{}", id);
    return 0;
}

/// InternalCreateFrame(self)
static int l_InternalCreateFrame(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateFrame: no UIControlRegistry");

    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateFrame: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateFrame: failed to create control");

    // Store Lua table reference
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    // Set _c_object lightuserdata
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    // Frame has no parent (it IS the root)

    // Create 7 LazyVars
    create_lazyvar(L, 1, "Left");
    create_lazyvar(L, 1, "Top");
    create_lazyvar(L, 1, "Right");
    create_lazyvar(L, 1, "Bottom");
    create_lazyvar(L, 1, "Width");
    create_lazyvar(L, 1, "Height");
    create_lazyvar(L, 1, "Depth");

    // Call OnInit (lua_gettable for metatable lookup)
    lua_pushstring(L, "OnInit");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("InternalCreateFrame: OnInit error: {}",
                         lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    spdlog::debug("InternalCreateFrame: control #{}", id);
    return 0;
}

/// InternalCreateBitmap(self, parent)
static int l_InternalCreateBitmap(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateBitmap: no UIControlRegistry");

    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateBitmap: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateBitmap: failed to create control");

    // Store Lua table reference
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    // Set _c_object lightuserdata
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    // Set parent if provided
    if (lua_istable(L, 2)) {
        auto* parent = check_control(L, 2);
        if (parent) ctrl->set_parent(parent);
    }

    // Create 7 LazyVars
    create_lazyvar(L, 1, "Left");
    create_lazyvar(L, 1, "Top");
    create_lazyvar(L, 1, "Right");
    create_lazyvar(L, 1, "Bottom");
    create_lazyvar(L, 1, "Width");
    create_lazyvar(L, 1, "Height");
    create_lazyvar(L, 1, "Depth");

    // Call OnInit (lua_gettable for metatable lookup)
    lua_pushstring(L, "OnInit");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("InternalCreateBitmap: OnInit error: {}",
                         lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    spdlog::debug("InternalCreateBitmap: control #{}", id);
    return 0;
}

/// InternalCreateText(self, parent)
static int l_InternalCreateText(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateText: no UIControlRegistry");

    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateText: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateText: failed to create control");

    // Store Lua table reference
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    // Set _c_object lightuserdata
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    // Set parent if provided
    if (lua_istable(L, 2)) {
        auto* parent = check_control(L, 2);
        if (parent) ctrl->set_parent(parent);
    }

    // Create 7 layout LazyVars
    create_lazyvar(L, 1, "Left");
    create_lazyvar(L, 1, "Top");
    create_lazyvar(L, 1, "Right");
    create_lazyvar(L, 1, "Bottom");
    create_lazyvar(L, 1, "Width");
    create_lazyvar(L, 1, "Height");
    create_lazyvar(L, 1, "Depth");

    // Create 4 font metric LazyVars
    create_lazyvar(L, 1, "FontAscent");
    create_lazyvar(L, 1, "FontDescent");
    create_lazyvar(L, 1, "FontExternalLeading");
    create_lazyvar(L, 1, "TextAdvance");

    // Set initial font metrics from defaults
    update_font_metrics(ctrl);
    update_text_advance(ctrl);
    push_font_lazyvars(L, 1, ctrl);

    // Call OnInit (lua_gettable for metatable lookup)
    lua_pushstring(L, "OnInit");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("InternalCreateText: OnInit error: {}",
                         lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    spdlog::debug("InternalCreateText: control #{}", id);
    return 0;
}

/// InternalCreateEdit(self, parent)
static int l_InternalCreateEdit(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateEdit: no UIControlRegistry");

    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateEdit: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateEdit: failed to create control");
    ctrl->set_control_type(ui::UIControl::ControlType::Edit);

    // Store Lua table reference
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    // Set _c_object lightuserdata
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    // Set parent if provided
    if (lua_istable(L, 2)) {
        auto* parent = check_control(L, 2);
        if (parent) ctrl->set_parent(parent);
    }

    // Create 7 LazyVars
    create_lazyvar(L, 1, "Left");
    create_lazyvar(L, 1, "Top");
    create_lazyvar(L, 1, "Right");
    create_lazyvar(L, 1, "Bottom");
    create_lazyvar(L, 1, "Width");
    create_lazyvar(L, 1, "Height");
    create_lazyvar(L, 1, "Depth");

    // Call OnInit (lua_gettable for metatable lookup)
    lua_pushstring(L, "OnInit");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("InternalCreateEdit: OnInit error: {}",
                         lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    spdlog::debug("InternalCreateEdit: control #{}", id);
    return 0;
}

/// InternalCreateItemList(self, parent)
static int l_InternalCreateItemList(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateItemList: no UIControlRegistry");

    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateItemList: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateItemList: failed to create control");
    ctrl->set_control_type(ui::UIControl::ControlType::ItemList);

    // Store Lua table reference
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    // Set _c_object lightuserdata
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    // Set parent if provided
    if (lua_istable(L, 2)) {
        auto* parent = check_control(L, 2);
        if (parent) ctrl->set_parent(parent);
    }

    // Create 7 LazyVars
    create_lazyvar(L, 1, "Left");
    create_lazyvar(L, 1, "Top");
    create_lazyvar(L, 1, "Right");
    create_lazyvar(L, 1, "Bottom");
    create_lazyvar(L, 1, "Width");
    create_lazyvar(L, 1, "Height");
    create_lazyvar(L, 1, "Depth");

    // Call OnInit (lua_gettable for metatable lookup)
    lua_pushstring(L, "OnInit");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("InternalCreateItemList: OnInit error: {}",
                         lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    spdlog::debug("InternalCreateItemList: control #{}", id);
    return 0;
}

/// InternalCreateScrollbar(self, parent, axis)
static int l_InternalCreateScrollbar(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateScrollbar: no UIControlRegistry");

    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateScrollbar: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateScrollbar: failed to create control");
    ctrl->set_control_type(ui::UIControl::ControlType::Scrollbar);

    // Store Lua table reference
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    // Set _c_object lightuserdata
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    // Set parent if provided
    if (lua_istable(L, 2)) {
        auto* parent = check_control(L, 2);
        if (parent) ctrl->set_parent(parent);
    }

    // Set scroll axis (arg 3)
    if (lua_type(L, 3) == LUA_TSTRING)
        ctrl->set_scroll_axis(lua_tostring(L, 3));

    // Create 7 LazyVars
    create_lazyvar(L, 1, "Left");
    create_lazyvar(L, 1, "Top");
    create_lazyvar(L, 1, "Right");
    create_lazyvar(L, 1, "Bottom");
    create_lazyvar(L, 1, "Width");
    create_lazyvar(L, 1, "Height");
    create_lazyvar(L, 1, "Depth");

    // Call OnInit (lua_gettable for metatable lookup)
    lua_pushstring(L, "OnInit");
    lua_gettable(L, 1);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("InternalCreateScrollbar: OnInit error: {}",
                         lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    spdlog::debug("InternalCreateScrollbar: control #{}", id);
    return 0;
}

/// GetTextureDimensions(filename, border) → width, height
static int l_GetTextureDimensions(lua_State* L) {
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    std::string path = lua_tostring(L, 1);
    auto [w, h] = read_dds_dimensions(L, path);
    if (w == 0 && h == 0) {
        lua_pushnil(L);
        lua_pushnil(L);
    } else {
        lua_pushnumber(L, w);
        lua_pushnumber(L, h);
    }
    return 2;
}

static int l_InternalCreateBorder(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateBorder: no UIControlRegistry");
    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateBorder: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateBorder: failed to create control");

    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    if (lua_istable(L, 2)) {
        auto* parent = check_control(L, 2);
        if (parent) ctrl->set_parent(parent);
    }

    create_lazyvar(L, 1, "Left");
    create_lazyvar(L, 1, "Top");
    create_lazyvar(L, 1, "Right");
    create_lazyvar(L, 1, "Bottom");
    create_lazyvar(L, 1, "Width");
    create_lazyvar(L, 1, "Height");
    create_lazyvar(L, 1, "Depth");
    // Border-specific LazyVars
    create_lazyvar(L, 1, "BorderWidth");
    create_lazyvar(L, 1, "BorderHeight");

    spdlog::debug("InternalCreateBorder: control #{}", id);
    return 0;
}

static int l_InternalCreateDragger(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateDragger: no UIControlRegistry");
    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateDragger: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateDragger: failed to create control");

    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    // Dragger has no parent — it's a standalone drag-tracking object
    spdlog::debug("InternalCreateDragger: control #{}", id);
    return 0;
}

/// PostDragger(originFrame, keycode, dragger)
/// Registers a dragger to receive mouse events until release/cancel.
/// For now, we just store the dragger ref so Lua side can call OnMove/OnRelease/OnCancel.
static int l_PostDragger(lua_State* L) {
    // Args: originFrame (table), keycode (number), dragger (table)
    // Store dragger in registry key for the UI dispatch to find
    if (!lua_istable(L, 3)) return 0;

    lua_pushstring(L, "__osc_active_dragger");
    lua_pushvalue(L, 3);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}

/// _c_CreateCursor(self, parent_or_nil)
static int l_c_CreateCursor(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "_c_CreateCursor: no UIControlRegistry");
    if (!lua_istable(L, 1))
        return luaL_error(L, "_c_CreateCursor: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "_c_CreateCursor: failed to create control");

    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    // Cursor has no parent (standalone)
    spdlog::debug("_c_CreateCursor: control #{}", id);
    return 0;
}

static int l_InternalCreateMovie(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateMovie: no UIControlRegistry");
    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateMovie: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateMovie: failed to create control");

    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    if (lua_istable(L, 2)) {
        auto* parent = check_control(L, 2);
        if (parent) ctrl->set_parent(parent);
    }

    create_lazyvar(L, 1, "Left");
    create_lazyvar(L, 1, "Top");
    create_lazyvar(L, 1, "Right");
    create_lazyvar(L, 1, "Bottom");
    create_lazyvar(L, 1, "Width");
    create_lazyvar(L, 1, "Height");
    create_lazyvar(L, 1, "Depth");
    // Movie-specific LazyVars
    create_lazyvar(L, 1, "MovieWidth");
    create_lazyvar(L, 1, "MovieHeight");

    // Movie controls are non-interactive backgrounds — disable hit test
    // so they don't intercept mouse events from interactive controls above.
    ctrl->set_hit_test_disabled(true);

    spdlog::debug("InternalCreateMovie: control #{}", id);
    return 0;
}

static void set_map_preview_metatable(lua_State* L, int self_idx) {
    lua_newtable(L);

    lua_pushstring(L, "__index");
    lua_newtable(L);

    lua_pushstring(L, "moho");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        const char* method_tables[] = {"control_methods", "ui_map_preview_methods"};
        for (const char* table_name : method_tables) {
            lua_pushstring(L, table_name);
            lua_rawget(L, -2);
            if (lua_istable(L, -1)) {
                lua_pushnil(L);
                while (lua_next(L, -2) != 0) {
                    lua_pushvalue(L, -2);
                    lua_pushvalue(L, -2);
                    lua_rawset(L, -7);
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    lua_rawset(L, -3);
    lua_setmetatable(L, self_idx);
}

static int create_map_preview_control(lua_State* L, int self_idx, int parent_idx) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateMapPreview: no UIControlRegistry");
    if (!lua_istable(L, self_idx))
        return luaL_error(L, "InternalCreateMapPreview: self must be a table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateMapPreview: failed to create control");

    lua_pushvalue(L, self_idx);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, self_idx);

    if (parent_idx > 0 && lua_istable(L, parent_idx)) {
        auto* parent = check_control(L, parent_idx);
        if (parent) ctrl->set_parent(parent);
    }

    create_lazyvar(L, self_idx, "Left");
    create_lazyvar(L, self_idx, "Top");
    create_lazyvar(L, self_idx, "Right");
    create_lazyvar(L, self_idx, "Bottom");
    create_lazyvar(L, self_idx, "Width");
    create_lazyvar(L, self_idx, "Height");
    create_lazyvar(L, self_idx, "Depth");

    spdlog::debug("MapPreview: control #{}", id);
    return 0;
}

static int l_InternalCreateMapPreview(lua_State* L) {
    return create_map_preview_control(L, 1, 2);
}

static int l_MapPreview(lua_State* L) {
    const int parent_idx = lua_istable(L, 1) ? 1 : 0;
    lua_newtable(L);
    const int self_idx = lua_gettop(L);

    create_map_preview_control(L, self_idx, parent_idx);
    set_map_preview_metatable(L, self_idx);

    return 1;
}

static int l_InternalCreateHistogram(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateHistogram: no UIControlRegistry");
    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateHistogram: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateHistogram: failed to create control");

    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    if (lua_istable(L, 2)) {
        auto* parent = check_control(L, 2);
        if (parent) ctrl->set_parent(parent);
    }

    create_lazyvar(L, 1, "Left");
    create_lazyvar(L, 1, "Top");
    create_lazyvar(L, 1, "Right");
    create_lazyvar(L, 1, "Bottom");
    create_lazyvar(L, 1, "Width");
    create_lazyvar(L, 1, "Height");
    create_lazyvar(L, 1, "Depth");

    spdlog::debug("InternalCreateHistogram: control #{}", id);
    return 0;
}

static int l_InternalCreateWorldMesh(lua_State* L) {
    auto* reg = get_ui_registry(L);
    if (!reg) return luaL_error(L, "InternalCreateWorldMesh: no UIControlRegistry");
    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateWorldMesh: arg 1 must be self table");

    u32 id = reg->create();
    auto* ctrl = reg->get(id);
    if (!ctrl) return luaL_error(L, "InternalCreateWorldMesh: failed to create control");

    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ctrl->set_lua_table_ref(ref);

    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, ctrl);
    lua_rawset(L, 1);

    spdlog::debug("InternalCreateWorldMesh: control #{}", id);
    return 0;
}

/// InternalCreateDiscoveryService(serviceClass) -> instance
static int l_InternalCreateDiscoveryService(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateDiscoveryService: arg 1 must be class table");

    // Create instance table
    lua_newtable(L);

    // Set class as metatable with __index
    lua_newtable(L); // mt
    lua_pushstring(L, "__index");
    lua_pushvalue(L, 1); // class
    lua_rawset(L, -3);   // mt.__index = class
    lua_setmetatable(L, -2); // setmetatable(instance, mt)

    // Set _c_object dummy
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, reinterpret_cast<void*>(static_cast<uintptr_t>(0x2)));
    lua_rawset(L, -3);

    spdlog::debug("InternalCreateDiscoveryService: created");
    return 1; // return instance
}

/// InternalCreateLobby(lobbyComClass, protocol, port, maxConns, name, uid, nat) -> instance
static int l_InternalCreateLobby(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "InternalCreateLobby: arg 1 must be class table");

    // Create instance table
    lua_newtable(L);

    // Set class as metatable with __index
    lua_newtable(L); // mt
    lua_pushstring(L, "__index");
    lua_pushvalue(L, 1); // class
    lua_rawset(L, -3);
    lua_setmetatable(L, -2);

    // _c_object dummy
    lua_pushstring(L, "_c_object");
    lua_pushlightuserdata(L, reinterpret_cast<void*>(static_cast<uintptr_t>(0x3)));
    lua_rawset(L, -3);

    spdlog::debug("InternalCreateLobby: created");
    return 1; // return instance
}

// --- UI bootstrap globals (M76) ---

/// GetFrame(head) -> root frame table (only head 0 supported)
static int l_GetFrame(lua_State* L) {
    int head = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (head != 0) { lua_pushnil(L); return 1; }
    lua_pushstring(L, "__osc_root_frame");
    lua_rawget(L, LUA_REGISTRYINDEX);
    return 1;
}

/// GetNumRootFrames() -> 1
static int l_GetNumRootFrames(lua_State* L) {
    lua_pushnumber(L, 1);
    return 1;
}

/// SetCursor(cursor) — store active cursor in registry
static int l_SetCursor(lua_State* L) {
    lua_pushstring(L, "__osc_active_cursor");
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_GetCursor(lua_State* L) {
    lua_pushstring(L, "__osc_active_cursor");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1)) {
        // Return a dummy table with Hide/Show if no cursor set
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, "Hide");
        lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
        lua_rawset(L, -3);
        lua_pushstring(L, "Show");
        lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
        lua_rawset(L, -3);
    }
    return 1;
}


/// Read {x, y, z} (array or vector table) at idx into x/z.
bool read_xz(lua_State* L, int idx, f32& x, f32& z) {
    if (!lua_istable(L, idx)) return false;
    lua_rawgeti(L, idx, 1);
    lua_rawgeti(L, idx, 3);
    bool ok = lua_isnumber(L, -2) && lua_isnumber(L, -1);
    if (ok) {
        x = static_cast<f32>(lua_tonumber(L, -2));
        z = static_cast<f32>(lua_tonumber(L, -1));
    }
    lua_pop(L, 2);
    return ok;
}


// ====================================================================
// Registration
// ====================================================================

struct MohoClassDef {
    const char* name;
    const MethodEntry* methods;
    const char*
        base; // Name of base class (array entry [1]), or nullptr
};

// clang-format off
static const MohoClassDef moho_classes[] = {
    // Base classes (no inheritance)
    {"entity_methods",          entity_methods,          nullptr},
    {"manipulator_methods",     manipulator_methods,     nullptr},
    {"weapon_methods",          weapon_methods,          nullptr},
    {"blip_methods",            blip_methods,            nullptr},
    {"aibrain_methods",         aibrain_methods,         nullptr},
    {"platoon_methods",         platoon_methods,         nullptr},
    {"navigator_methods",       navigator_methods,       nullptr},
    {"aipersonality_methods",   empty_methods,           nullptr},
    {"CAiAttackerImpl_methods", empty_methods,           nullptr},
    {"ScriptTask_Methods",      empty_methods,           nullptr},
    {"sound_methods",           empty_methods,           nullptr},
    {"CDamage",                 empty_methods,           nullptr},
    {"CDecalHandle",            decal_handle_methods,    nullptr},
    {"EconomyEvent",            economy_event_methods,   nullptr},
    {"EntityCategory",          entity_category_methods, nullptr},
    {"CPrefetchSet",            empty_methods,           nullptr},
    {"MotorFallDown",           empty_methods,           nullptr},
    {"PathDebugger_methods",    empty_methods,           nullptr},

    // Inherit from entity_methods
    {"unit_methods",            unit_methods,           "entity_methods"},
    // The UI's unit objects (no base: Moho's UserEntity adds no methods)
    {"user_unit_methods",       user_unit_methods,      nullptr},
    {"projectile_methods",      projectile_methods,     "entity_methods"},
    {"prop_methods",            prop_methods,           "entity_methods"},
    {"shield_methods",          shield_methods,         "entity_methods"},
    {"CollisionBeamEntity",     collision_beam_methods, "entity_methods"},

    // IEffect (no base)
    {"IEffect",                 ieffect_methods,         nullptr},

    // Manipulators (inherit from manipulator_methods)
    {"AimManipulator",          aim_manipulator_methods,        "manipulator_methods"},
    {"AnimationManipulator",    animation_manipulator_methods,  "manipulator_methods"},
    {"BuilderArmManipulator",   builder_arm_methods,            "manipulator_methods"},
    {"RotateManipulator",       rotate_manipulator_methods,     "manipulator_methods"},
    {"SlideManipulator",        slide_manipulator_methods,      "manipulator_methods"},
    {"SlaveManipulator",        empty_methods,                  "manipulator_methods"},
    {"ThrustManipulator",       thrust_manipulator_methods,     "manipulator_methods"},
    {"BoneEntityManipulator",   empty_methods,                  "manipulator_methods"},
    {"StorageManipulator",      empty_methods,                  "manipulator_methods"},
    {"FootPlantManipulator",    empty_methods,                  "manipulator_methods"},
    {"CollisionManipulator",    collision_manipulator_methods,   "manipulator_methods"},

    // UI classes. As in Moho, the controls derive from control_methods
    // ([1] = base, per FAF's engine annotations): globalInit's class
    // conversion (retail ConvertCClassToLuaClass, FAF's flattening
    // ConvertCClassToLuaSimplifiedClass) folds the base in, and both class
    // systems resolve Class(moho.bitmap_methods, Control) through the
    // hierarchy -- Control's Lua overrides win over the C base's.
    {"control_methods",         ui_control_methods, nullptr},
    {"group_methods",           ui_group_methods,  "control_methods"},
    {"frame_methods",           ui_frame_methods,  "control_methods"},
    {"bitmap_methods",          ui_bitmap_methods,  "control_methods"},
    {"border_methods",          ui_border_methods,  "control_methods"},
    {"cursor_methods",          ui_cursor_methods,  nullptr},
    {"discovery_service_methods", ui_discovery_methods, nullptr},
    {"dragger_methods",         ui_dragger_methods,  nullptr},
    {"edit_methods",            ui_edit_methods,  "control_methods"},
    {"histogram_methods",       ui_histogram_methods,  "control_methods"},
    {"item_list_methods",       ui_item_list_methods,  "control_methods"},
    {"lobby_methods",           ui_lobby_methods,  nullptr},
    {"mesh_methods",            empty_methods,  "control_methods"},
    {"movie_methods",           ui_movie_methods,  "control_methods"},
    {"ui_map_preview_methods",  ui_map_preview_methods, "control_methods"},
    {"scrollbar_methods",       ui_scrollbar_methods,  "control_methods"},
    {"text_methods",            ui_text_methods,  "control_methods"},
    {"UIWorldView",             ui_worldview_methods,  "control_methods"},
    {"camera_methods",          camera_methods,  nullptr},
    {"userDecal_methods",       empty_methods,  nullptr},
    {"WldUIProvider_methods",   ui_wlduiprovider_methods,  nullptr},
    {"world_mesh_methods",      ui_world_mesh_methods,  nullptr},

    {nullptr, nullptr, nullptr}, // sentinel
};
// clang-format on

void register_moho_bindings(LuaState& state, sim::SimState& sim) {
    lua_State* L = state.raw();

    // Store sim state pointer in registry
    lua_pushstring(L, "osc_sim_state");
    lua_pushlightuserdata(L, &sim);
    lua_rawset(L, LUA_REGISTRYINDEX);

    // Create the moho global table
    lua_newtable(L);

    // First pass: register all base classes (no inheritance)
    for (const auto* def = moho_classes; def->name != nullptr; def++) {
        if (def->base != nullptr) continue; // skip derived classes in first pass

        lua_pushstring(L, def->name);
        lua_newtable(L);

        for (const auto* m = def->methods; m->name != nullptr; m++) {
            lua_pushstring(L, m->name);
            lua_pushcfunction(L, m->func);
            lua_rawset(L, -3);
        }

        lua_rawset(L, -3); // moho[name] = class_table
    }

    // Second pass: register derived classes (with base reference as [1])
    for (const auto* def = moho_classes; def->name != nullptr; def++) {
        if (def->base == nullptr) continue; // skip base classes

        lua_pushstring(L, def->name);
        lua_newtable(L);

        // Set [1] = moho[base_name] for Flatten to find
        lua_pushstring(L, def->base);
        lua_rawget(L, -4); // get moho[base_name]
        lua_rawseti(L, -2, 1);

        // Add this class's own methods
        for (const auto* m = def->methods; m->name != nullptr; m++) {
            lua_pushstring(L, m->name);
            lua_pushcfunction(L, m->func);
            lua_rawset(L, -3);
        }

        lua_rawset(L, -3); // moho[name] = class_table
    }

    lua_setglobal(L, "moho"); // set the moho global

    spdlog::info("Registered moho bindings");
}

// ====================================================================
// Localization helpers: LOC / LOCF
// ====================================================================

static osc::core::Localization* get_loc(lua_State* L) {
    lua_pushstring(L, "__osc_loc_cache");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* loc = static_cast<osc::core::Localization*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return loc;
}

static int l_LOC(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    auto* loc = get_loc(L);
    if (loc) {
        const auto& result = loc->lookup(key);
        lua_pushstring(L, result.c_str());
    } else {
        lua_pushvalue(L, 1); // return key as-is
    }
    return 1;
}

static int l_LOCF(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    auto* loc = get_loc(L);
    if (!loc) { lua_pushvalue(L, 1); return 1; }

    int top = lua_gettop(L);
    int nargs = top - 1;
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(nargs));
    for (int i = 2; i <= top; ++i) {
        const char* s = lua_tostring(L, i);
        args.push_back(s ? s : "");
    }

    auto result = loc->format(key, args);
    lua_pushstring(L, result.c_str());
    return 1;
}

// ====================================================================
// Preferences helpers: GetPreference / SetPreference
// ====================================================================

static osc::core::Preferences* get_prefs(lua_State* L) {
    lua_pushstring(L, "__osc_preferences");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* p = static_cast<osc::core::Preferences*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return p;
}

/// GetPreference(key [, default]) -> a copy of the preference at the dotted
/// key (any Lua value: retail keeps whole profile tables), else `default`.
static int l_GetPreference(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    if (auto* prefs = get_prefs(L)) prefs->push(key, L);
    else lua_pushnil(L);
    if (lua_isnil(L, -1) && lua_gettop(L) >= 3) {
        lua_pop(L, 1);
        lua_pushvalue(L, 2);
    }
    return 1;
}

/// SetPreference(key, value) -- stores a copy; nil removes the key.
static int l_SetPreference(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    lua_settop(L, 2);
    if (auto* prefs = get_prefs(L)) prefs->set(key, L, 2);
    return 0;
}

/// SavePreferences() -- write Game.prefs (a no-op when preferences are kept
/// in memory, as in tests).
static int l_SavePreferences(lua_State* L) {
    if (auto* prefs = get_prefs(L)) prefs->save();
    return 0;
}

/// GetOptions(key) -> the current profile's option, or nil (retail's
/// Prefs.GetOption falls back to the option's default).
static int l_GetOptions(lua_State* L) {
    const char* key = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : "";
    if (auto* prefs = get_prefs(L)) prefs->push_option(key, L);
    else lua_pushnil(L);
    return 1;
}

// ====================================================================
// UI-side thread/coroutine system
// ====================================================================

static sim::ThreadManager* get_ui_threads(lua_State* L) {
    lua_pushstring(L, "__osc_ui_thread_manager");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* tm = static_cast<sim::ThreadManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return tm;
}

static int l_ui_ForkThread(lua_State* L) {
    auto* tm = get_ui_threads(L);
    if (!tm) { lua_pushnil(L); return 1; }
    return tm->fork_thread(L);
}

/// WaitSeconds(n): convert seconds to frame count, yield with frame count.
/// ThreadManager::resume_all() interprets yielded numbers as RELATIVE wait counts.
static constexpr f64 UI_FRAMES_PER_SECOND = 60.0;

static int l_ui_WaitSeconds(lua_State* L) {
    f64 seconds = luaL_checknumber(L, 1);
    if (seconds < 0.0) seconds = 0.0;
    u32 frames = static_cast<u32>(std::llround(seconds * UI_FRAMES_PER_SECOND));
    if (frames < 1) frames = 1;
    lua_pushnumber(L, static_cast<lua_Number>(frames));
    return lua_yield(L, 1);
}

/// WaitTicks(n): in UI context, 1 tick = 1 frame.
static int l_ui_WaitTicks(lua_State* L) {
    int ticks = static_cast<int>(luaL_checknumber(L, 1));
    if (ticks < 1) ticks = 1;
    lua_pushnumber(L, static_cast<lua_Number>(ticks));
    return lua_yield(L, 1);
}

// ====================================================================
// Selection↔Lua bridge (M137)
// ====================================================================


static osc::GameStateManager* get_game_state_mgr(lua_State* L) {
    lua_pushstring(L, "__osc_game_state_mgr");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* mgr = static_cast<osc::GameStateManager*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return mgr;
}

/// Push a unit table for the UI Lua state: a handle by id with EntityId and
/// Army fields, whose methods are UserUnit's (moho.user_unit_methods, M191
/// step 3), which read the UI's snapshot of the tick. Cached metatable:
/// __osc_ui_unit_mt.
void push_unit_for_ui(lua_State* L, sim::Entity* entity) {
    push_user_unit(L, entity->entity_id(), entity->army());
}

void push_user_unit(lua_State* L, u32 id, i32 army) {
    lua_newtable(L);
    int tbl = lua_gettop(L);

    // A handle by id, never a raw pointer: UI scripts keep these across
    // beats (avatars, idle lists, selections), and check_entity resolves the
    // id on every call. The sim generation rejects handles from an earlier
    // game, whose ids a new sim reuses.
    lua_pushstring(L, "_c_entity_id");
    lua_pushnumber(L, static_cast<lua_Number>(id));
    lua_rawset(L, tbl);
    lua_pushstring(L, "_c_sim_gen");
    lua_pushnumber(L, static_cast<lua_Number>(sim::SimState::sim_generation()));
    lua_rawset(L, tbl);

    // EntityId
    lua_pushstring(L, "EntityId");
    lua_pushnumber(L, static_cast<lua_Number>(id));
    lua_rawset(L, tbl);

    // Army (1-based for Lua)
    lua_pushstring(L, "Army");
    lua_pushnumber(L, static_cast<lua_Number>(army + 1));
    lua_rawset(L, tbl);

    // Set metatable: get or create cached __osc_ui_unit_mt
    lua_pushstring(L, "__osc_ui_unit_mt");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        // Build: { __index = moho.user_unit_methods }
        lua_newtable(L); // mt
        lua_pushstring(L, "__index");
        lua_pushstring(L, "moho");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_istable(L, -1)) {
            lua_pushstring(L, "user_unit_methods");
            lua_rawget(L, -2);
            lua_remove(L, -2); // remove moho table
        }
        lua_rawset(L, -3); // mt.__index = user_unit_methods (or nil if moho missing)

        // Cache it
        lua_pushstring(L, "__osc_ui_unit_mt");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_setmetatable(L, tbl);
}


void push_units_for_ui(lua_State* L, const std::vector<u32>& ids) {
    lua_newtable(L);
    int result = lua_gettop(L);
    auto* sim = get_sim(L);
    if (!sim) return;
    int idx = 1;
    for (u32 eid : ids) {
        auto* entity = sim->entity_registry().find(eid);
        if (entity && entity->is_unit() && !entity->destroyed()) {
            push_unit_for_ui(L, entity);
            lua_rawseti(L, result, idx++);
        }
    }
}


static int l_AddOnSelectionChangedCallback(lua_State* L) {
    if (!lua_isfunction(L, 1)) return 0;

    // Get or create the callbacks table at __osc_sel_changed_cbs
    lua_pushstring(L, "__osc_sel_changed_cbs");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1); // pop nil/non-table
        lua_newtable(L);
        lua_pushstring(L, "__osc_sel_changed_cbs");
        lua_pushvalue(L, -2); // dup table
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    // table is on top; append the function
    int idx = luaL_getn(L, -1) + 1; // Lua 5.0: no lua_objlen
    lua_pushvalue(L, 1); // dup function arg
    lua_rawseti(L, -2, idx);
    lua_pop(L, 1); // pop table
    return 0;
}

static int l_ValidateUnitsList(lua_State* L) {
    auto* sim = get_sim(L);
    lua_newtable(L);
    int result = lua_gettop(L);
    int out_idx = 1;

    if (!lua_istable(L, 1) || !sim) return 1;

    int n = luaL_getn(L, 1);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        auto* e = check_entity(L, lua_gettop(L));
        if (e && !e->destroyed()) {
            lua_rawseti(L, result, out_idx++);
            continue;
        }
        lua_pop(L, 1);
    }
    return 1;
}

// ====================================================================
// SimCallback UI→Sim bridge (M138a)
// ====================================================================

// Helper: get SimCallbackQueue from registry
sim::SimCallbackQueue* get_callback_queue(lua_State* L) {
    lua_pushstring(L, "__osc_sim_callback_queue");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* q = static_cast<sim::SimCallbackQueue*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return q;
}

static int l_GetFocusArmy(lua_State* L) {
    lua_pushstring(L, "__osc_focus_army");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnumber(L, -1)) {
        const int stored = static_cast<int>(lua_tonumber(L, -1));
        lua_pushnumber(L, stored >= 0 ? stored + 1 : -1);
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 1);
    lua_pushnumber(L, 1); // default army 1
    return 1;
}

/// The focus army's live units matching `pred`, in entity-id order.
/// Empty when there is no sim or the player is observing.
template <typename Pred>
static std::vector<sim::Entity*> focus_army_units(lua_State* L, Pred pred) {
    std::vector<sim::Entity*> out;
    auto* sim = get_sim(L);
    if (!sim) return out;
    int focus = 0;
    lua_pushstring(L, "__osc_focus_army");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnumber(L, -1)) focus = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    if (focus < 0) return out;
    sim->entity_registry().for_each_unit([&](sim::Entity& e) {
        if (!e.is_unit() || e.destroyed() || e.army() != focus) return;
        if (pred(static_cast<sim::Unit&>(e))) out.push_back(&e);
    });
    std::sort(out.begin(), out.end(), [](const sim::Entity* a, const sim::Entity* b) {
        return a->entity_id() < b->entity_id();
    });
    return out;
}

/// Push units as an array of UI unit objects; `nil_if_empty` pushes nil for
/// none (the idle lists: avatars.lua takes a table as "show the idle tab").
static void push_ui_unit_array(lua_State* L, const std::vector<sim::Entity*>& units,
                               bool nil_if_empty) {
    if (units.empty() && nil_if_empty) { lua_pushnil(L); return; }
    lua_newtable(L);
    int idx = 1;
    for (auto* e : units) {
        push_unit_for_ui(L, e);
        lua_rawseti(L, -2, idx++);
    }
}

void notify_focus_army_damage(lua_State* uiL, sim::SimState& sim) {
    // Per sim, told apart by generation: a new SimState can reuse the old
    // one's address, and its entity ids restart.
    static u32 last_generation = 0;
    static std::unordered_map<u32, f32> last_health;
    if (last_generation != sim::SimState::sim_generation()) {
        last_generation = sim::SimState::sim_generation();
        last_health.clear();
    }
    lua_pushstring(uiL, "__osc_focus_army");
    lua_rawget(uiL, LUA_REGISTRYINDEX);
    const int focus = lua_isnumber(uiL, -1) ? static_cast<int>(lua_tonumber(uiL, -1)) : -1;
    lua_pop(uiL, 1);

    std::vector<sim::Entity*> damaged;
    std::unordered_map<u32, f32> health;
    sim.entity_registry().for_each_unit([&](const sim::Entity& e) {
        if (focus < 0 || !e.is_unit() || e.destroyed() || e.army() != focus) return;
        health.emplace(e.entity_id(), e.health());
        auto it = last_health.find(e.entity_id());
        if (it != last_health.end() && e.health() < it->second)
            damaged.push_back(const_cast<sim::Entity*>(&e));
    });
    last_health = std::move(health);
    std::sort(damaged.begin(), damaged.end(), [](const sim::Entity* a, const sim::Entity* b) {
        return a->entity_id() < b->entity_id();
    });
    for (auto* e : damaged) {
        push_unit_for_ui(uiL, e);
        core::call_ui_callback(uiL, core::kGameMainModule, "OnFocusArmyUnitDamaged", 1);
    }
}

/// GetArmyAvatars() -> the focus army's commander units (the avatars the
/// game UI shows and zooms to at game start), or nil when it has none (an
/// observer has none): retail's gamemain and avatars.lua test for nil, as for
/// GetIdleEngineers.
static int l_GetArmyAvatars(lua_State* L) {
    push_ui_unit_array(
        L, focus_army_units(L, [](const sim::Unit& u) { return u.has_category("COMMAND"); }), true);
    return 1;
}

/// GetIdleEngineers() -> the focus army's idle engineers (commanders are
/// avatars, not engineers), or nil if there are none.
static int l_GetIdleEngineers(lua_State* L) {
    push_ui_unit_array(L, focus_army_units(L, [](const sim::Unit& u) {
        return u.has_category("ENGINEER") && !u.has_category("COMMAND") &&
               unit_is_idle(u);
    }), true);
    return 1;
}

/// GetIdleFactories() -> the focus army's idle factories, or nil.
static int l_GetIdleFactories(lua_State* L) {
    push_ui_unit_array(L, focus_army_units(L, [](const sim::Unit& u) {
        return u.has_category("FACTORY") && unit_is_idle(u);
    }), true);
    return 1;
}

/// Live units in the Lua array at stack index `idx` (UI unit objects).
static std::vector<sim::Unit*> ui_unit_list(lua_State* L, int idx) {
    std::vector<sim::Unit*> units;
    if (!lua_istable(L, idx)) return units;
    const int n = luaL_getn(L, idx);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        auto* e = check_entity(L, lua_gettop(L));
        if (e && e->is_unit() && !e->destroyed()) units.push_back(static_cast<sim::Unit*>(e));
        lua_pop(L, 1);
    }
    return units;
}

/// True when `units` is non-empty and every unit satisfies `pred`.
template <typename Pred>
static bool all_units(const std::vector<sim::Unit*>& units, Pred pred) {
    if (units.empty()) return false;
    for (auto* u : units)
        if (!pred(*u)) return false;
    return true;
}

// The orders panel's per-selection state queries (UserUnit lists).

/// GetFireState(units) -> the shared fire state (0 return fire, 1 hold
/// fire, 2 hold ground), -1 when the units differ.
static int l_GetFireState(lua_State* L) {
    const auto units = ui_unit_list(L, 1);
    int state = units.empty() ? 0 : units.front()->fire_state();
    for (auto* u : units)
        if (u->fire_state() != state) { state = -1; break; }
    lua_pushnumber(L, state);
    return 1;
}

/// GetScriptBit(units, bit) -> whether every unit has the toggle bit set.
static int l_GetScriptBit(lua_State* L) {
    const auto bit = static_cast<i32>(luaL_checknumber(L, 2));
    lua_pushboolean(L, all_units(ui_unit_list(L, 1), [&](const sim::Unit& u) {
        return u.get_script_bit(bit);
    }));
    return 1;
}

static int l_GetIsPaused(lua_State* L) {
    lua_pushboolean(L, all_units(ui_unit_list(L, 1),
                                 [](const sim::Unit& u) { return u.is_paused(); }));
    return 1;
}

static int l_GetIsAutoMode(lua_State* L) {
    lua_pushboolean(L, all_units(ui_unit_list(L, 1),
                                 [](const sim::Unit& u) { return u.auto_mode(); }));
    return 1;
}

/// GetIsSubmerged(units) -> -1 all submerged, 1 all surfaced, 0 mixed/none.
static int l_GetIsSubmerged(lua_State* L) {
    const auto units = ui_unit_list(L, 1);
    const auto sub = [](const sim::Unit& u) { return u.layer() == "Sub"; };
    int state = 0;
    if (all_units(units, sub)) state = -1;
    else if (all_units(units, [&](const sim::Unit& u) { return !sub(u); })) state = 1;
    lua_pushnumber(L, state);
    return 1;
}

/// GetIsAutoSurfaceMode(units) -> whether every unit surfaces by itself.
static int l_GetIsAutoSurfaceMode(lua_State* L) {
    lua_pushboolean(
        L, all_units(ui_unit_list(L, 1), [](const sim::Unit& u) { return u.auto_surface_mode(); }));
    return 1;
}

// The orders panel's unit settings. Each is a request the sim applies
// inside a tick, like an order: in multiplayer every peer applies it on the
// same tick. A toggle is resolved here, from the state the UI shows, to the
// value it sets, so every unit in a mixed selection ends up alike.

/// Queue `setting` = `value` (plus the script bit, if any) for the units in
/// the array at index 1.
static void queue_unit_setting(lua_State* L, const char* setting, sim::SimCallbackArg value,
                               std::optional<f64> bit = std::nullopt) {
    auto* queue = get_callback_queue(L);
    if (!queue) return;
    sim::SimCallbackEntry entry;
    entry.func_name = sim::kUnitSettingCallback;
    entry.args["Setting"] = std::string(setting);
    entry.args["Value"] = std::move(value);
    if (bit) entry.args["Bit"] = *bit;
    for (auto* u : ui_unit_list(L, 1)) entry.unit_ids.push_back(u->entity_id());
    if (!entry.unit_ids.empty()) queue->push(std::move(entry));
}

/// SetPaused(units, paused)
static int l_SetPaused(lua_State* L) {
    queue_unit_setting(L, "Paused", lua_toboolean(L, 2) != 0);
    return 0;
}

/// SetAutoMode(units, on) -- a factory's or silo's automatic building.
static int l_SetAutoMode(lua_State* L) {
    queue_unit_setting(L, "AutoMode", lua_toboolean(L, 2) != 0);
    return 0;
}

/// SetAutoSurfaceMode(units, on)
static int l_SetAutoSurfaceMode(lua_State* L) {
    queue_unit_setting(L, "AutoSurfaceMode", lua_toboolean(L, 2) != 0);
    return 0;
}

/// SetFireState(units, state): the orders panel passes 'ReturnFire',
/// 'HoldFire' or 'HoldGround'; 0, 1 or 2 is taken too.
static int l_SetFireState(lua_State* L) {
    f64 state = -1;
    if (lua_type(L, 2) == LUA_TNUMBER) {
        state = lua_tonumber(L, 2);
    } else if (lua_type(L, 2) == LUA_TSTRING) {
        const std::string name = lua_tostring(L, 2);
        if (name == "ReturnFire") state = 0;
        else if (name == "HoldFire") state = 1;
        else if (name == "HoldGround") state = 2;
    }
    if (state != 0 && state != 1 && state != 2) {
        return luaL_error(L, "SetFireState: unknown fire state");
    }
    queue_unit_setting(L, "FireState", state);
    return 0;
}

/// ToggleFireState(units, current) -> the next state after `current`
/// (GetFireState's -1 for a mixed selection starts over at return fire).
static int l_ToggleFireState(lua_State* L) {
    const int current = static_cast<int>(luaL_checknumber(L, 2));
    queue_unit_setting(L, "FireState", static_cast<f64>(current < 0 ? 0 : (current + 1) % 3));
    return 0;
}

/// ToggleScriptBit(units, bit, current): every unit's bit becomes `not
/// current`, where current is the state the button shows.
static int l_ToggleScriptBit(lua_State* L) {
    const auto bit = static_cast<i32>(luaL_checknumber(L, 2));
    if (bit < 0 || bit > 8) return luaL_error(L, "ToggleScriptBit: bit %d out of range", bit);
    queue_unit_setting(L, "ScriptBit", lua_toboolean(L, 3) == 0, static_cast<f64>(bit));
    return 0;
}

/// GetAssistingUnitsList(units) -> the units guarding/assisting any of them.
static int l_GetAssistingUnitsList(lua_State* L) {
    const auto targets = ui_unit_list(L, 1);
    std::unordered_set<u32> ids;
    for (auto* u : targets) ids.insert(u->entity_id());
    push_ui_unit_array(L, focus_army_units(L, [&](const sim::Unit& u) {
        const auto& q = u.command_queue();
        return !q.empty() && q.front().type == sim::CommandType::Guard &&
               ids.count(q.front().target_id) > 0;
    }), false);
    return 1;
}

// The session's extra select list: units the UI highlights besides the
// selection (construction.lua marks a hovered factory's queue owner). Kept
// as entity ids in the UI registry; nothing draws them yet.
static constexpr const char* kExtraSelectKey = "__osc_extra_select";

static void push_extra_select_table(lua_State* L) {
    lua_pushstring(L, kExtraSelectKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushstring(L, kExtraSelectKey);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

static int extra_select_set(lua_State* L, bool add) {
    auto* e = check_entity(L, 1);
    if (!e) return 0;
    push_extra_select_table(L);
    lua_pushnumber(L, e->entity_id());
    if (add) lua_pushboolean(L, 1);
    else lua_pushnil(L);
    lua_rawset(L, -3);
    lua_pop(L, 1);
    return 0;
}

static int l_AddToSessionExtraSelectList(lua_State* L) { return extra_select_set(L, true); }
static int l_RemoveFromSessionExtraSelectList(lua_State* L) { return extra_select_set(L, false); }
static int l_ClearSessionExtraSelectList(lua_State* L) {
    lua_pushstring(L, kExtraSelectKey);
    lua_newtable(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}

/// SetOverlayFilters(names) -- FA's active range-overlay filters
/// (multifunction.lua). Kept for the renderer, which shows the matching
/// intel range rings.
static int l_SetOverlayFilters(lua_State* L) {
    lua_newtable(L);
    int n = 1;
    if (lua_istable(L, 1)) {
        const int count = luaL_getn(L, 1);
        for (int i = 1; i <= count; ++i) {
            lua_rawgeti(L, 1, i);
            if (lua_type(L, -1) == LUA_TSTRING) lua_rawseti(L, -2, n++);
            else lua_pop(L, 1);
        }
    }
    lua_pushstring(L, core::kOverlayFiltersKey);
    lua_insert(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}

/// SetOverlayFilter(name, categories, colors, thicknesses...) defines one
/// filter's look; the renderer draws its own ring style, so this is kept
/// only as a known filter.
static int l_SetOverlayFilter(lua_State* /*L*/) { return 0; }

/// SessionGetLocalCommandSource() -> this client's command source (1-based).
/// Command sources are the players' clients; single player has one.
static int l_SessionGetLocalCommandSource(lua_State* L) {
    const auto& mp = mp_net_state();
    lua_pushnumber(L, mp.active() ? static_cast<lua_Number>(mp.local_source + 1) : 1);
    return 1;
}

/// SessionGetCommandSourceNames() -> player name per command source: the
/// human armies' nicknames in army order (AIs have no command source).
static int l_SessionGetCommandSourceNames(lua_State* L) {
    lua_newtable(L);
    auto* sim = get_sim(L);
    if (!sim) return 1;
    int idx = 1;
    for (size_t i = 0; i < sim->army_count(); ++i) {
        auto* brain = sim->army_at(i);
        if (!brain || !brain->is_human()) continue;
        lua_pushstring(L, brain->nickname().c_str());
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

/// AddConsoleOutputReciever(fn) -> handle (Moho's spelling). The console
/// echo (consoleecho.lua) registers here; console output is not forwarded
/// to receivers yet, so they are only kept until removed.
static int l_AddConsoleOutputReciever(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushstring(L, "__osc_console_receivers");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, "__osc_console_receivers");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_pushvalue(L, 1);
    int handle = luaL_ref(L, -2);
    lua_pop(L, 1);
    lua_pushnumber(L, handle);
    return 1;
}

static int l_RemoveConsoleOutputReciever(lua_State* L) {
    if (!lua_isnumber(L, 1)) return 0;
    lua_pushstring(L, "__osc_console_receivers");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1))
        luaL_unref(L, -1, static_cast<int>(lua_tonumber(L, 1)));
    lua_pop(L, 1);
    return 0;
}

/// SetFocusArmy(army) -- 1-based, -1 = observer. During a world session it
/// is a request the next sim beat applies (sync_beat), as Moho's session
/// does, so the sim and the UI's OnSync see the change together; with no
/// session there is nothing to wait for, so it applies at once.
static int l_SetFocusArmy(lua_State* L) {
    int army = static_cast<int>(luaL_checknumber(L, 1));
    lua_pushstring(L, core::kWorldUiActiveKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const bool in_session = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    lua_pushstring(L, in_session ? kFocusArmyRequestKey : "__osc_focus_army");
    lua_pushnumber(L, army > 0 ? army - 1 : army);
    lua_rawset(L, LUA_REGISTRYINDEX);
    spdlog::debug("UI SetFocusArmy: {}{}", army, in_session ? " (next beat)" : "");
    return 0;
}

// ====================================================================
// Command data + issuance globals (M138b)
// ====================================================================

/// GetUnitCommandData(units) → availableOrders, availableToggles, buildableCategories
/// Returns three tables based on the intersection of command caps across all units.
/// A unit blueprint's Economy.BuildableCategory strings ("BUILTBYCOMMANDER
/// UEF", ...), read from the blueprint store.
static std::vector<std::string> buildable_category_strings(lua_State* L,
                                                           const sim::Unit* u) {
    std::vector<std::string> out;
    if (!push_entity_blueprint(L, u)) return out;
    lua_pushstring(L, "Economy");
    lua_rawget(L, -2);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "BuildableCategory");
        lua_rawget(L, -2);
        if (lua_istable(L, -1)) {
            const int n = luaL_getn(L, -1);
            for (int i = 1; i <= n; ++i) {
                lua_rawgeti(L, -1, i);
                if (lua_type(L, -1) == LUA_TSTRING) out.emplace_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1); // BuildableCategory
    }
    lua_pop(L, 2); // Economy + blueprint
    return out;
}

/// Push the category of what a selection can build: per unit the union of
/// its BuildableCategory entries (each "A B" meaning A and B), across the
/// selection the intersection -- the options every selected unit has. A
/// selection that builds nothing gets a category no unit is in.
static void push_buildable_category(lua_State* L,
                                    const std::vector<std::vector<std::string>>& per_unit) {
    static const char* kCombine =
        "return function(lists)\n"
        "  local result\n"
        "  for i = 1, table.getn(lists) do\n"
        "    local u\n"
        "    for j = 1, table.getn(lists[i]) do\n"
        "      local c = ParseEntityCategory(lists[i][j])\n"
        "      if u then u = u + c else u = c end\n"
        "    end\n"
        "    if not u then return categories.OSC_BUILDS_NOTHING end\n"
        "    if result then result = result * u else result = u end\n"
        "  end\n"
        "  return result or categories.OSC_BUILDS_NOTHING\n"
        "end\n";
    lua_pushstring(L, "__osc_buildable_combine");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        if (luaL_loadbuffer(L, kCombine, std::strlen(kCombine), "=buildable") != 0 ||
            lua_pcall(L, 0, 1, 0) != 0) {
            spdlog::warn("buildable category helper: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
            lua_newtable(L);
            return;
        }
        lua_pushstring(L, "__osc_buildable_combine");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    lua_newtable(L);
    for (size_t i = 0; i < per_unit.size(); ++i) {
        lua_newtable(L);
        for (size_t j = 0; j < per_unit[i].size(); ++j) {
            lua_pushstring(L, per_unit[i][j].c_str());
            lua_rawseti(L, -2, static_cast<int>(j + 1));
        }
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    if (lua_pcall(L, 1, 1, 0) != 0) {
        spdlog::warn("buildable category: {}", lua_tostring(L, -1));
        lua_pop(L, 1);
        lua_newtable(L);
    }
}

static int l_GetUnitCommandData(lua_State* L) {
    auto* sim = get_sim(L);

    // Push the three return tables up front so their stack indices are stable.
    lua_newtable(L); // index: top-2  (orders)
    lua_newtable(L); // index: top-1  (toggles)
    lua_newtable(L); // index: top    (buildable, replaced below)

    if (!sim || !lua_istable(L, 1)) return 3;

    static const char* all_caps[] = {
        "RULEUCC_Move", "RULEUCC_Attack", "RULEUCC_Guard", "RULEUCC_Patrol",
        "RULEUCC_Stop", "RULEUCC_RetaliateToggle", "RULEUCC_Repair",
        "RULEUCC_Capture", "RULEUCC_Reclaim", "RULEUCC_Overcharge",
        "RULEUCC_Transport", "RULEUCC_Ferry", "RULEUCC_Sacrifice",
        "RULEUCC_Nuke", "RULEUCC_Tactical", "RULEUCC_Teleport",
        "RULEUCC_Dive", "RULEUCC_Pause", nullptr
    };

    bool first_unit = true;
    std::unordered_set<std::string> common_caps;
    std::vector<std::vector<std::string>> buildable;

    int n = luaL_getn(L, 1);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        auto* entity = check_entity(L, lua_gettop(L));
        if (!entity || !entity->is_unit() || entity->destroyed()) {
            lua_pop(L, 1); // unit table
            continue;
        }
        auto* unit = static_cast<sim::Unit*>(entity);
        buildable.push_back(buildable_category_strings(L, unit));

        if (first_unit) {
            for (const char** cap = all_caps; *cap; ++cap) {
                if (unit->has_command_cap(*cap)) {
                    common_caps.insert(*cap);
                }
            }
            first_unit = false;
        } else {
            // Intersect: remove caps the current unit doesn't have
            auto it = common_caps.begin();
            while (it != common_caps.end()) {
                if (!unit->has_command_cap(*it)) {
                    it = common_caps.erase(it);
                } else {
                    ++it;
                }
            }
        }
        lua_pop(L, 1); // unit table
    }

    // The 3 return tables are at stack positions (top-2), (top-1), (top).
    // lua_gettop(L) == orders_idx + 2.
    int orders_tbl   = lua_gettop(L) - 2;
    int toggles_tbl  = lua_gettop(L) - 1;

    // Fill orders table
    int oidx = 1;
    for (const auto& cap : common_caps) {
        lua_pushstring(L, cap.c_str());
        lua_rawseti(L, orders_tbl, oidx++);
    }

    // Toggle caps: these become toggle buttons in the UI
    static const char* toggle_caps[] = {
        "RULEUCC_Pause", "RULEUCC_RetaliateToggle", "RULEUCC_Dive", nullptr
    };
    int tidx = 1;
    for (const char** tc = toggle_caps; *tc; ++tc) {
        if (common_caps.count(*tc)) {
            lua_pushstring(L, *tc);
            lua_rawseti(L, toggles_tbl, tidx++);
        }
    }

    if (!buildable.empty()) {
        const int buildable_tbl = lua_gettop(L);
        push_buildable_category(L, buildable);
        lua_replace(L, buildable_tbl);
    }
    return 3;
}

/// GetUnitCommandDataOfUnit(unit) — single-unit variant for command mode
static int l_GetUnitCommandDataOfUnit(lua_State* L) {
    // Wrap the single unit into a 1-element array table and delegate
    // to the existing GetUnitCommandData implementation
    lua_newtable(L);
    lua_pushnumber(L, 1);
    lua_pushvalue(L, 1); // copy the unit table
    lua_rawset(L, -3);   // {[1] = unit}

    // Replace arg 1 with the wrapped table
    lua_replace(L, 1);
    return l_GetUnitCommandData(L);
}


// The UI's orders. Each is the local player's order, routed as human input
// like the input handler's clicks: a command on the sim's input that it
// applies inside its next tick, and that a networked match broadcasts.

/// Route `cmd` for `ids` as the local player's order.
void issue_player_order(lua_State* L, const std::vector<u32>& ids, const sim::UnitCommand& cmd,
                        bool clear) {
    auto* sim = get_sim(L);
    if (!sim || ids.empty()) return;
    sim->set_human_input_active(true);
    sim->route_player_command(ids, cmd, clear);
    sim->set_human_input_active(false);
}

static std::vector<u32> ui_unit_ids(lua_State* L, int idx) {
    std::vector<u32> ids;
    for (auto* u : ui_unit_list(L, idx)) ids.push_back(u->entity_id());
    return ids;
}


/// "UNITCOMMAND_Stop" or "Stop" -> "Stop".
std::string ui_command_name(const char* name) {
    static constexpr std::string_view kPrefix = "UNITCOMMAND_";
    std::string_view n(name);
    if (n.substr(0, kPrefix.size()) == kPrefix) n.remove_prefix(kPrefix.size());
    return std::string(n);
}

/// An order with no target: Stop, Dive, a silo build, or a Script order
/// whose task is an enhancement (the construction panel's). `data` is the
/// order's table.
void issue_targetless_order(lua_State* L, const std::vector<u32>& ids, const std::string& name,
                            int data, bool clear) {
    sim::UnitCommand cmd;
    if (name == "Stop") {
        cmd.type = sim::CommandType::Stop;
    } else if (name == "Dive") {
        cmd.type = sim::CommandType::Dive;
    } else if (name == "BuildSiloNuke" || name == "BuildSiloTactical") {
        // The orders panel's silo build: one missile more (its silo takes
        // it, not its queue, so `clear` does nothing).
        cmd.type = name == "BuildSiloNuke" ? sim::CommandType::SiloBuildNuke
                                           : sim::CommandType::SiloBuildTactical;
    } else if (name == "Script" && lua_istable(L, data)) {
        lua_pushstring(L, "TaskName");
        lua_gettable(L, data);
        const bool enhance = lua_type(L, -1) == LUA_TSTRING &&
                             std::string_view(lua_tostring(L, -1)) == "EnhanceTask";
        lua_pop(L, 1);
        lua_pushstring(L, "Enhancement");
        lua_gettable(L, data);
        if (enhance && lua_type(L, -1) == LUA_TSTRING) {
            cmd.type = sim::CommandType::Enhance;
            cmd.blueprint_id = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
        if (cmd.type != sim::CommandType::Enhance) {
            spdlog::warn("IssueCommand: unsupported Script order");
            return;
        }
    } else {
        spdlog::warn("IssueCommand: unsupported order '{}'", name);
        return;
    }
    issue_player_order(L, ids, cmd, clear);
}

/// IssueUnitCommand(units, command)
static int l_IssueUnitCommand(lua_State* L) {
    const std::string name = ui_command_name(luaL_checkstring(L, 2));
    issue_targetless_order(L, ui_unit_ids(L, 1), name, 3, true);
    return 0;
}

/// IssueUnitCommandToUnit(unit, command)
static int l_IssueUnitCommandToUnit(lua_State* L) {
    const std::string name = ui_command_name(luaL_checkstring(L, 2));
    auto* e = check_entity(L, 1);
    if (!e || !e->is_unit() || e->destroyed()) return 0;
    issue_targetless_order(L, {e->entity_id()}, name, 3, true);
    return 0;
}


/// IssueBuildMobile(units, position, blueprint)
static int l_IssueBuildMobile(lua_State* L) {
    if (!lua_istable(L, 2)) return 0;
    sim::UnitCommand cmd;
    cmd.type = sim::CommandType::BuildMobile;
    lua_rawgeti(L, 2, 1);
    lua_rawgeti(L, 2, 2);
    lua_rawgeti(L, 2, 3);
    cmd.target_pos = {static_cast<f32>(lua_tonumber(L, -3)), static_cast<f32>(lua_tonumber(L, -2)),
                      static_cast<f32>(lua_tonumber(L, -1))};
    lua_pop(L, 3);
    cmd.blueprint_id = luaL_checkstring(L, 3);
    issue_player_order(L, ui_unit_ids(L, 1), cmd, false);
    return 0;
}

// Forward declarations (defined later in file, needed by functions below)
static sim::Entity* extract_ui_entity(lua_State* L, int idx);

// ====================================================================
// Construction panel globals
// ====================================================================


/// GetAttachedUnitsList(units) — for each unit, collect its cargo/attached units.
static int l_GetAttachedUnitsList(lua_State* L) {
    auto* sim = get_sim(L);
    lua_newtable(L);
    int result = lua_gettop(L);
    int out_idx = 1;

    if (!sim || !lua_istable(L, 1)) return 1;

    auto& reg = sim->entity_registry();
    int n = luaL_getn(L, 1);
    for (int i = 1; i <= n; i++) {
        lua_rawgeti(L, 1, i);
        auto* entity = extract_ui_entity(L, lua_gettop(L));
        lua_pop(L, 1);

        if (!entity || !entity->is_unit() || entity->destroyed()) continue;
        auto* unit = static_cast<sim::Unit*>(entity);

        for (u32 cargo_id : unit->cargo_ids()) {
            auto* cargo = reg.find(cargo_id);
            if (cargo && !cargo->destroyed() && cargo->is_unit()) {
                push_unit_for_ui(L, cargo);
                lua_rawseti(L, result, out_idx++);
            }
        }
    }
    return 1;
}

/// ClearCommands(units) — clear command queues for all units in the table.
static int l_ClearCommands(lua_State* L) {
    sim::UnitCommand stop;
    stop.type = sim::CommandType::Stop;
    issue_player_order(L, ui_unit_ids(L, 1), stop, true);
    return 0;
}

// ====================================================================
// Build mode / command mode globals (M139)
// ====================================================================

/// ClearBuildTemplates() — called when exiting build mode; clears build ghost.
static int l_ClearBuildTemplates(lua_State* L) {
    auto* sim = get_sim(L);
    if (sim) sim->clear_build_ghost();
    return 0;
}

/// GetActiveBuildTemplate() — returns nil (no active template).
static int l_GetActiveBuildTemplate(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

/// SetActiveBuildTemplate(template) — stub; build templates not needed for basic build mode.
static int l_SetActiveBuildTemplate(lua_State* /*L*/) {
    return 0;
}

/// AddCommandFeedbackBlip(blipTable) — visual feedback stub; cosmetic only.
static int l_AddCommandFeedbackBlip(lua_State* /*L*/) {
    return 0;
}

/// GetUnitById(entityId) — retrieves a unit table by entity ID.
static int l_GetUnitById(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_pushnil(L); return 1; }

    u32 eid = static_cast<u32>(luaL_checknumber(L, 1));
    auto* entity = sim->entity_registry().find(eid);
    if (!entity || entity->destroyed()) {
        lua_pushnil(L);
        return 1;
    }

    push_unit_for_ui(L, entity);
    return 1;
}


/// IN_AddKeyMapTable(keymap) — register a key map table for hotkey dispatch.
static int l_IN_AddKeyMapTable(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "__osc_keymap_registry");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* reg = static_cast<osc::ui::KeyMapRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (reg) reg->add(L, 1);
    return 0;
}

/// IN_RemoveKeyMapTable(keymap) — unregister a previously added key map table.
static int l_IN_RemoveKeyMapTable(lua_State* L) {
    if (!lua_istable(L, 1)) return 0;
    lua_pushstring(L, "__osc_keymap_registry");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* reg = static_cast<osc::ui::KeyMapRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (reg) reg->remove_by_ref(L, 1);
    return 0;
}

/// EntityCategoryGetUnitList(category) — ui_L version (M140a)
/// Returns an array of blueprint IDs whose categories match the given category expression.
static int l_ui_EntityCategoryGetUnitList(lua_State* L) {
    lua_newtable(L);
    int result = lua_gettop(L);
    int out_idx = 1;

    auto* store = lua::LuaState::get_blueprint_store(L);
    if (!store || !lua_istable(L, 1)) return 1;

    auto entries = store->get_all(blueprints::BlueprintType::Unit);
    const osc::lua::CategoryMatcher category(L, 1);
    for (const auto* entry : entries) {
        store->push_lua_table(*entry, L);
        int bp_tbl = lua_gettop(L);

        std::unordered_set<std::string> bp_cats;
        sim::collect_blueprint_categories(L, bp_tbl, bp_cats);
        if (!bp_cats.empty() && category.matches(bp_cats)) {
            lua_pushnumber(L, out_idx++);
            lua_pushstring(L, entry->id.c_str());
            lua_rawset(L, result);
        }
        lua_pop(L, 1); // bp_table
    }
    return 1;
}

/// Helper: extract Entity* from a ui_L unit table (has _c_object lightuserdata).
/// Returns nullptr if table is missing or entity is invalid.
static sim::Entity* extract_ui_entity(lua_State* L, int idx) {
    if (idx < 0) idx = lua_gettop(L) + idx + 1;
    return check_entity(L, idx);
}

/// ui_L category filter helper. If keep_matches is true, keeps units matching
/// the category (FilterDown). If false, keeps non-matching (FilterOut).
/// The categories of a UI list item: a unit object, or a blueprint id
/// string (the construction panel filters lists of blueprint ids). False
/// for anything else, or a dead unit.
static bool ui_item_categories(lua_State* L, int idx,
                               std::unordered_set<std::string>& cats) {
    if (lua_type(L, idx) == LUA_TSTRING) {
        auto* store = lua::LuaState::get_blueprint_store(L);
        auto* entry = store ? store->find(lua_tostring(L, idx)) : nullptr;
        if (!entry) return false;
        store->push_lua_table(*entry, L);
        sim::collect_blueprint_categories(L, lua_gettop(L), cats);
        lua_pop(L, 1);
        return true;
    }
    if (!lua_istable(L, idx)) return false;
    auto* entity = extract_ui_entity(L, idx);
    if (!entity || !entity->is_unit() || entity->destroyed()) return false;
    cats = static_cast<sim::Unit*>(entity)->categories();
    return true;
}

static int ui_category_filter(lua_State* L, bool keep_matches) {
    lua_newtable(L);
    int result = lua_gettop(L);
    int out_idx = 1;

    if (!lua_istable(L, 1) || !lua_istable(L, 2)) return 1;

    const osc::lua::CategoryMatcher category(L, 1);
    for (int i = 1; ; i++) {
        lua_rawgeti(L, 2, i);
        if (lua_isnil(L, -1)) { lua_pop(L, 1); break; }

        const int item = lua_gettop(L);
        std::unordered_set<std::string> cats;
        if (!ui_item_categories(L, item, cats)) { lua_pop(L, 1); continue; }
        const bool matches = category.matches(cats);
        if (matches == keep_matches) {
            lua_pushnumber(L, out_idx++);
            lua_pushvalue(L, item);
            lua_rawset(L, result);
        }
        lua_pop(L, 1); // item
    }
    return 1;
}

/// EntityCategoryContains(category, unit) — check if a single unit matches (ui_L)
static int l_ui_EntityCategoryContains(lua_State* L) {
    std::unordered_set<std::string> cats;
    const bool matches = lua_istable(L, 1) && ui_item_categories(L, 2, cats) &&
                         osc::lua::categories_match(L, 1, cats);
    lua_pushboolean(L, matches ? 1 : 0);
    return 1;
}

/// EntityCategoryFilterDown(category, unitList) — keep matching units (ui_L)
static int l_ui_EntityCategoryFilterDown(lua_State* L) {
    return ui_category_filter(L, true);
}

/// EntityCategoryFilterOut(category, unitList) — keep non-matching units (ui_L)
static int l_ui_EntityCategoryFilterOut(lua_State* L) {
    return ui_category_filter(L, false);
}

/// GetBlueprintIconPath(bpId) — resolve a blueprint's icon DDS path (M140b)
static int l_GetBlueprintIconPath(lua_State* L) {
    const char* bp_id = luaL_checkstring(L, 1);
    auto* store = lua::LuaState::get_blueprint_store(L);
    if (store) {
        const auto* entry = store->find(bp_id);
        if (entry) {
            store->push_lua_table(*entry, L);
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "Display");
                lua_rawget(L, -2);
                if (lua_istable(L, -1)) {
                    lua_pushstring(L, "IconPath");
                    lua_rawget(L, -2);
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        std::string path(lua_tostring(L, -1));
                        lua_pop(L, 3); // IconPath, Display, bp table
                        lua_pushstring(L, path.c_str());
                        return 1;
                    }
                    lua_pop(L, 1); // nil IconPath
                }
                lua_pop(L, 1); // Display or nil
            }
            lua_pop(L, 1); // bp table
        }
    }
    // Default path convention
    std::string path = std::string("/textures/ui/common/icons/units/") + bp_id + "_icon.dds";
    lua_pushstring(L, path.c_str());
    return 1;
}

// ====================================================================
// Factory queue display bindings (M140c)
// ====================================================================

static lua::FactoryQueueDisplay* get_factory_queue(lua_State* L) {
    lua_pushstring(L, "__osc_factory_queue");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* fq = static_cast<lua::FactoryQueueDisplay*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return fq;
}

static int l_SetCurrentFactoryForQueueDisplay(lua_State* L) {
    auto* fq = get_factory_queue(L);
    auto* unit = check_unit(L, 1);
    if (fq && unit) {
        fq->set_current(L, unit);
    } else {
        lua_newtable(L);
    }
    return 1;
}

static int l_PeekCurrentFactoryForQueueDisplay(lua_State* L) {
    auto* fq = get_factory_queue(L);
    auto* unit = check_unit(L, 1);
    if (fq && unit) {
        fq->peek(L, unit);
    } else {
        lua_newtable(L);
    }
    return 1;
}

static int l_ClearCurrentFactoryForQueueDisplay(lua_State* L) {
    auto* fq = get_factory_queue(L);
    if (fq) fq->clear();
    return 0;
}

/// A change to the count at `index` of the queue display's factory's
/// queue: a request the sim applies in its next tick.
static int push_build_count_request(lua_State* L, const char* func_name) {
    auto* fq = get_factory_queue(L);
    auto* queue = get_callback_queue(L);
    if (!fq || !queue || fq->current_factory_id() == 0) return 0;
    sim::SimCallbackEntry entry;
    entry.func_name = func_name;
    entry.args["Index"] = static_cast<f64>(luaL_checknumber(L, 1));
    entry.args["Count"] = static_cast<f64>(luaL_optnumber(L, 2, 1));
    entry.unit_ids.push_back(fq->current_factory_id());
    queue->push(std::move(entry));
    return 0;
}

/// DecreaseBuildCountInQueue(index, count): the queue display's
/// right-click (fewer of one of the factory's queued units).
static int l_DecreaseBuildCountInQueue(lua_State* L) {
    return push_build_count_request(L, sim::kDecreaseBuildCountCallback);
}

/// IncreaseBuildCountInQueue(index, count): its left-click (more of one).
static int l_IncreaseBuildCountInQueue(lua_State* L) {
    return push_build_count_request(L, sim::kIncreaseBuildCountCallback);
}

/// GetOrderBitmapNames(bitmapId) → 8 return values
static int l_GetOrderBitmapNames(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    auto paths = lua::get_order_bitmap_paths(id);
    lua_pushstring(L, paths.up.c_str());
    lua_pushstring(L, paths.up_sel.c_str());
    lua_pushstring(L, paths.over.c_str());
    lua_pushstring(L, paths.over_sel.c_str());
    lua_pushstring(L, paths.dis.c_str());
    lua_pushstring(L, paths.dis_sel.c_str());
    lua_pushstring(L, paths.sound_click);
    lua_pushstring(L, paths.sound_rollover);
    return 8;
}


/// EnhancementCommon.GetEnhancements(entityId) → table of installed enhancements (M142c)
/// Returns: {Back="AdvancedEngineering", RCH="CoolingUpgrade", ...} (slot → enhName)
static int l_GetEnhancements(lua_State* L) {
    auto* sim = get_sim(L);
    u32 entity_id = static_cast<u32>(luaL_checknumber(L, 1));
    lua_newtable(L);

    if (!sim) return 1;
    auto* entity = sim->entity_registry().find(entity_id);
    if (!entity || !entity->is_unit()) return 1;
    auto* unit = static_cast<osc::sim::Unit*>(entity);

    for (const auto& [slot, name] : unit->enhancements()) {
        lua_pushstring(L, slot.c_str());
        lua_pushstring(L, name.c_str());
        lua_rawset(L, -3);
    }
    return 1;
}

/// StartCursorText(x, y, text, color, time, flash) — floating text near cursor
/// Used for brief notifications like "Invalid target" or build placement feedback.
static int l_StartCursorText(lua_State* L) {
    f32 x = static_cast<f32>(luaL_optnumber(L, 1, 0));
    f32 y = static_cast<f32>(luaL_optnumber(L, 2, 0));
    const char* text = luaL_optstring(L, 3, "");
    // Color is a table {r, g, b, a} or string
    f32 r = 1, g = 1, b = 1, a = 1;
    if (lua_istable(L, 4)) {
        lua_rawgeti(L, 4, 1); r = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_rawgeti(L, 4, 2); g = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_rawgeti(L, 4, 3); b = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
        lua_rawgeti(L, 4, 4); a = static_cast<f32>(lua_tonumber(L, -1)); lua_pop(L, 1);
    }
    f32 duration = static_cast<f32>(luaL_optnumber(L, 5, 2.0));
    // bool flash = lua_toboolean(L, 6) != 0;  // not used yet

    // Store cursor text in registry for overlay renderer to display
    lua_pushstring(L, "__osc_cursor_text");
    lua_newtable(L);
    lua_pushstring(L, "text"); lua_pushstring(L, text); lua_rawset(L, -3);
    lua_pushstring(L, "x"); lua_pushnumber(L, x); lua_rawset(L, -3);
    lua_pushstring(L, "y"); lua_pushnumber(L, y); lua_rawset(L, -3);
    lua_pushstring(L, "r"); lua_pushnumber(L, r); lua_rawset(L, -3);
    lua_pushstring(L, "g"); lua_pushnumber(L, g); lua_rawset(L, -3);
    lua_pushstring(L, "b"); lua_pushnumber(L, b); lua_rawset(L, -3);
    lua_pushstring(L, "a"); lua_pushnumber(L, a); lua_rawset(L, -3);
    lua_pushstring(L, "duration"); lua_pushnumber(L, duration); lua_rawset(L, -3);
    lua_pushstring(L, "time"); lua_pushnumber(L, 0); lua_rawset(L, -3);
    lua_rawset(L, LUA_REGISTRYINDEX);

    spdlog::debug("StartCursorText: '{}' at ({:.0f},{:.0f}) for {:.1f}s", text, x, y, duration);
    return 0;
}

static osc::lua::BeatFunctionRegistry* get_beat_registry(lua_State* L) {
    lua_pushstring(L, "__osc_beat_registry");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* reg = static_cast<osc::lua::BeatFunctionRegistry*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return reg;
}

/// AddBeatFunction(func, name) — register a per-frame callback
static int l_AddBeatFunction(lua_State* L) {
    auto* reg = get_beat_registry(L);
    if (!reg || !lua_isfunction(L, 1)) return 0;
    std::string name;
    if (lua_type(L, 2) == LUA_TSTRING) {
        name = lua_tostring(L, 2);
    }
    reg->add(L, 1, name);
    return 0;
}

/// RemoveBeatFunction(func_or_name) — unregister a per-frame callback
static int l_RemoveBeatFunction(lua_State* L) {
    auto* reg = get_beat_registry(L);
    if (!reg) return 0;
    if (lua_isfunction(L, 1)) {
        reg->remove(L, 1);
    } else if (lua_type(L, 1) == LUA_TSTRING) {
        reg->remove_by_name(lua_tostring(L, 1), L);
    }
    return 0;
}

// Engine state query bindings (M144c)

/// GetCurrentUIState() → "front-end" | "game" | "score" etc.
static int l_GetCurrentUIState(lua_State* L) {
    auto* mgr = get_game_state_mgr(L);
    if (mgr) {
        lua_pushstring(L, osc::game_state_to_string(mgr->current()));
    } else {
        lua_pushstring(L, "game");
    }
    return 1;
}

/// WorldIsLoading() → boolean
static int l_WorldIsLoading(lua_State* L) {
    auto* mgr = get_game_state_mgr(L);
    lua_pushboolean(L, mgr && mgr->current() == osc::GameState::LOADING ? 1 : 0);
    return 1;
}

/// SessionIsActive() -> boolean: a game session exists -- loading, playing or
/// at its score screen -- and hasn't been ended (Moho: an active world
/// session). The front end has none. (Retail's UI gates Quick Save, the
/// camera zoom and more on it; a fallback had answered false in game too.)
static int l_ui_SessionIsActive(lua_State* L) {
    auto* mgr = get_game_state_mgr(L);
    const auto state = mgr ? mgr->current() : osc::GameState::INIT;
    const bool in_session = state == osc::GameState::LOADING || state == osc::GameState::GAME ||
                            state == osc::GameState::SCORE;
    lua_pushboolean(L, in_session && !mgr->sim_stopped() ? 1 : 0);
    return 1;
}

/// WorldIsPlaying() -> boolean: the world is running (Moho: its frame action
/// is Playing), not loading or gone.
static int l_WorldIsPlaying(lua_State* L) {
    auto* mgr = get_game_state_mgr(L);
    lua_pushboolean(L, mgr && mgr->current() == osc::GameState::GAME ? 1 : 0);
    return 1;
}

/// GameTime() -> seconds of game time. Moho adds the time since the last
/// tick; this is the tick's.
static int l_GameTime(lua_State* L) {
    auto* sim = get_sim(L);
    lua_pushnumber(L, sim ? sim->game_time() : 0.0);
    return 1;
}

/// SessionIsPaused() -> boolean
static int l_SessionIsPaused(lua_State* L) {
    auto* mgr = get_game_state_mgr(L);
    lua_pushboolean(L, mgr && mgr->paused() ? 1 : 0);
    return 1;
}

/// MapBorderClear() / MapBorderAdd(mesh): the decorative meshes around the
/// map's edge (the skin's "imager" mesh; retail's UpdateWorldBorderState).
/// They are kept for the world view; nothing draws them yet.
static int l_MapBorderClear(lua_State* L) {
    lua_pushstring(L, "__osc_map_border_meshes");
    lua_newtable(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_MapBorderAdd(lua_State* L) {
    if (lua_type(L, 1) != LUA_TSTRING) return 0;
    lua_pushstring(L, "__osc_map_border_meshes");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        l_MapBorderClear(L);
        lua_pushstring(L, "__osc_map_border_meshes");
        lua_rawget(L, LUA_REGISTRYINDEX);
    }
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, luaL_getn(L, -2) + 1);
    lua_pop(L, 1);
    return 0;
}

/// IsNISMode() -> false: no in-game cinematic (a campaign's NIS) runs here.
static int l_IsNISMode(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

/// LaunchSinglePlayerSession(sessionConfig) — launch a game from lobby config.
/// Reads ScenarioFile from GameOptions or the legacy top-level field, stores
/// config in FrontEndData, signals main loop.
static int l_LaunchSinglePlayerSession(lua_State* L) {
    if (!lua_istable(L, 1)) {
        spdlog::warn("LaunchSinglePlayerSession: expected table arg");
        return 0;
    }

    // Read scenario file path. FA lobby configs carry this under GameOptions;
    // older tests and helpers may still pass a top-level ScenarioFile.
    std::string scenario;
    lua_pushstring(L, "GameOptions");
    lua_rawget(L, 1);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "ScenarioFile");
        lua_rawget(L, -2);
        if (lua_type(L, -1) == LUA_TSTRING) {
            scenario = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    lua_pushstring(L, "ScenarioFile");
    lua_rawget(L, 1);
    if (scenario.empty() && lua_type(L, -1) == LUA_TSTRING) {
        scenario = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    if (scenario.empty()) {
        spdlog::warn("LaunchSinglePlayerSession: no ScenarioFile in config");
        return 0;
    }

    // Normalize for reload/session code that still expects the legacy field.
    lua_pushstring(L, "ScenarioFile");
    lua_pushstring(L, scenario.c_str());
    lua_rawset(L, 1);

    // Store session config in FrontEndData for loader
    auto* fed = get_front_end_data(L);
    if (fed) fed->set(L, "sessionConfig", 1);

    // Store scenario path in registry for main loop
    lua_pushstring(L, "__osc_launch_scenario");
    lua_pushstring(L, scenario.c_str());
    lua_rawset(L, LUA_REGISTRYINDEX);

    // Signal main loop to transition FRONT_END -> LOADING -> GAME
    lua_pushstring(L, "__osc_launch_requested");
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    // A lobby game, not a replay a LaunchReplaySession asked for before it.
    lua_pushstring(L, "__osc_launch_replay");
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);

    spdlog::info("LaunchSinglePlayerSession: scenario={}", scenario);
    return 0;
}

/// StartFrontEndUI() — transition to FRONT_END, call main.lua:CreateUI()
static int l_StartFrontEndUI(lua_State* L) {
    auto* mgr = get_game_state_mgr(L);
    if (mgr) mgr->transition_to(osc::GameState::FRONT_END, L);

    // Call CreateUI() from main.lua
    lua_pushstring(L, "CreateUI");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != 0) {
            spdlog::warn("CreateUI error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
        spdlog::warn("StartFrontEndUI: CreateUI not found (main.lua not loaded?)");
    }
    return 0;
}

// ====================================================================
// Time query bindings (M145c)
// ====================================================================

/// GetGameTimeSeconds() → number (seconds since game start)
static int l_ui_GetGameTimeSeconds(lua_State* L) {
    auto* sim = get_sim(L);
    lua_pushnumber(L, sim ? sim->game_time() : 0.0);
    return 1;
}

/// GameTick() → integer (current sim tick)
static int l_ui_GameTick(lua_State* L) {
    auto* sim = get_sim(L);
    lua_pushnumber(L, sim ? sim->tick_count() : 0);
    return 1;
}

/// GetGameTime() → formatted string "MM:SS"
static int l_GetGameTime(lua_State* L) {
    auto* sim = get_sim(L);
    f64 t = sim ? sim->game_time() : 0.0;
    int minutes = static_cast<int>(t) / 60;
    int seconds = static_cast<int>(t) % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", minutes, seconds);
    lua_pushstring(L, buf);
    return 1;
}

/// GetSimRate() → number (sim ticks per second, typically 10)
static int l_GetSimRate(lua_State* L) {
    lua_pushnumber(L, 10.0);
    return 1;
}

/// CurrentTime() → number (wall-clock seconds for UI animations)
// The UI clock: seconds of UI frame time, advanced once per UI frame by
// advance_ui_clock(). CurrentTime() reads it, so UI scripts' timing (retail
// userInit's WaitSeconds polls CurrentTime) follows frames: real time in the
// window, a fixed step in headless pumps, which outrun the wall clock.
static constexpr const char* kUiClockKey = "__osc_ui_clock";

void advance_ui_clock(lua_State* L, double dt) {
    lua_pushstring(L, kUiClockKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const double now = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
    lua_pop(L, 1);
    lua_pushstring(L, kUiClockKey);
    lua_pushnumber(L, now + dt);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

static int l_CurrentTime(lua_State* L) {
    lua_pushstring(L, kUiClockKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnumber(L, -1)) return 1;
    lua_pop(L, 1);
    // No frames yet: time since start.
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    lua_pushnumber(L, duration<double>(steady_clock::now() - start).count());
    return 1;
}

/// GetSystemTimeSeconds() → number (same as CurrentTime)
static int l_GetSystemTimeSeconds(lua_State* L) {
    using namespace std::chrono;
    auto now = high_resolution_clock::now().time_since_epoch();
    double secs = duration<double>(now).count();
    lua_pushnumber(L, secs);
    return 1;
}

// ====================================================================
// SessionGetScenarioInfo (M145c2)
// ====================================================================

/// SessionGetScenarioInfo() → table with scenario metadata
static int l_ui_SessionGetScenarioInfo(lua_State* L) {
    auto* sim = get_sim(L);
    if (!sim) { lua_newtable(L); return 1; }

    lua_newtable(L);
    auto set_str = [&](const char* k, const char* v) {
        lua_pushstring(L, k); lua_pushstring(L, v); lua_rawset(L, -3);
    };
    auto set_num = [&](const char* k, f64 v) {
        lua_pushstring(L, k); lua_pushnumber(L, v); lua_rawset(L, -3);
    };

    // Read scenario path from registry
    lua_pushstring(L, "__osc_scenario_path");
    lua_rawget(L, LUA_REGISTRYINDEX);
    const char* path = lua_isnil(L, -1) ? "" : lua_tostring(L, -1);
    lua_pop(L, 1);

    set_str("name", path);
    set_str("file", path);

    // Map size from terrain
    auto* terrain = sim->terrain();
    if (terrain) {
        set_num("size_x", terrain->map_width());
        set_num("size_z", terrain->map_height());
    }

    // Armies subtable
    lua_pushstring(L, "Armies");
    lua_newtable(L);
    for (size_t i = 0; i < sim->army_count(); ++i) {
        auto* brain = sim->army_at(i);
        if (brain) {
            lua_newtable(L);
            lua_pushstring(L, "name");
            lua_pushstring(L, brain->name().c_str());
            lua_rawset(L, -3);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
    }
    lua_rawset(L, -3);

    return 1;
}

// ====================================================================
// Speed/pause control bindings (M145d)
// ====================================================================

/// SetGameSpeed(speed) — set sim speed multiplier (0.0-10.0)
static int l_SetGameSpeed(lua_State* L) {
    auto* mgr = get_game_state_mgr(L);
    if (mgr) {
        f64 speed = luaL_checknumber(L, 1);
        mgr->set_speed(speed);
        spdlog::debug("SetGameSpeed: {:.2f}", speed);
    }
    return 0;
}

/// GetGameSpeed() → number (current speed multiplier)
static int l_GetGameSpeed(lua_State* L) {
    auto* mgr = get_game_state_mgr(L);
    lua_pushnumber(L, mgr ? mgr->speed() : 1.0);
    return 1;
}

/// ConExecute(cmd) — execute a console command string
static int l_ConExecute(lua_State* L) {
    const char* cmd = luaL_checkstring(L, 1);
    std::string s(cmd);
    if (s.rfind("WLD_GameSpeed", 0) == 0) {
        auto* mgr = get_game_state_mgr(L);
        if (mgr) {
            f64 speed = 1.0;
            if (s.size() > 14) {
                try { speed = std::stod(s.substr(14)); }
                catch (...) { spdlog::warn("ConExecute: invalid speed in '{}'", cmd); }
            }
            mgr->set_speed(speed);
        }
    } else {
        spdlog::debug("ConExecute: '{}' (unhandled)", cmd);
    }
    return 0;
}

/// SessionRequestPause() — request the sim to pause
static int l_SessionRequestPause(lua_State* L) {
    auto* mgr = get_game_state_mgr(L);
    if (mgr) mgr->set_paused(true, L);
    return 0;
}

/// SessionResume() — resume the sim
static int l_SessionResume(lua_State* L) {
    auto* mgr = get_game_state_mgr(L);
    if (mgr) mgr->set_paused(false, L);
    return 0;
}

// ── Score screen data (M146b) ────────────────────────────────────────────────


/// IsObserver() → boolean (true if focus army is -1)
static int l_IsObserver(lua_State* L) {
    lua_pushstring(L, "__osc_focus_army");
    lua_rawget(L, LUA_REGISTRYINDEX);
    int army = lua_isnil(L, -1) ? 0 : static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    lua_pushboolean(L, army < 0 ? 1 : 0);
    return 1;
}

// ── Escape handler / HideGameUI (M146c) ──────────────────────────────────────

/// EscapeHandler() — called when ESC is pressed
static int l_EscapeHandler(lua_State* L) {
    lua_pushstring(L, "__osc_escape_handler");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != 0) {
            spdlog::warn("EscapeHandler error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
        spdlog::debug("EscapeHandler: no handler registered");
    }
    return 0;
}

/// SetEscapeHandler(func) — register the ESC key handler
static int l_SetEscapeHandler(lua_State* L) {
    if (!lua_isfunction(L, 1)) return 0;
    lua_pushstring(L, "__osc_escape_handler");
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}

/// HideGameUI(hide) — show/hide the game HUD panels
static int l_HideGameUI(lua_State* L) {
    bool hide = lua_toboolean(L, 1) != 0;
    lua_pushstring(L, "__osc_hide_game_ui");
    lua_pushboolean(L, hide ? 1 : 0);
    lua_rawset(L, LUA_REGISTRYINDEX);
    spdlog::debug("HideGameUI: {}", hide ? "hidden" : "visible");
    return 0;
}

static void push_sound_handle(lua_State* L, osc::audio::SoundHandle h) {
    if (h == osc::audio::INVALID_SOUND) lua_pushnil(L);
    else lua_pushnumber(L, static_cast<lua_Number>(h));
}

/// PlaySound(sound) -> handle, or nil when nothing plays (unknown cue, or
/// over its instance limits). UI sounds are 2D.
static int l_PlaySound(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    std::string bank, cue;
    if (!mgr || !sound_arg(L, 1, bank, cue)) {
        lua_pushnil(L);
        return 1;
    }
    push_sound_handle(L, mgr->play(bank, cue, nullptr));
    return 1;
}

/// StopSound(handle [, immediate]): fade out (the cue's fade or release
/// curve), or stop at once.
static int l_StopSound(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    if (mgr && lua_type(L, 1) == LUA_TNUMBER) {
        mgr->stop(static_cast<osc::audio::SoundHandle>(lua_tonumber(L, 1)), lua_toboolean(L, 2) != 0);
    }
    return 0;
}

/// PlayVoice(sound [, duck]) -> handle. With `duck`, the rest of the mix
/// dips while it speaks (the Duck variable, read by FA's RPC curves).
static int l_PlayVoice(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    std::string bank, cue;
    if (!mgr || !sound_arg(L, 1, bank, cue)) {
        lua_pushnil(L);
        return 1;
    }
    const auto h = mgr->play(bank, cue, nullptr);
    if (h != osc::audio::INVALID_SOUND && lua_toboolean(L, 2)) {
        static int ducking = 0; // voices ducking now (one sound engine per process)
        if (ducking++ == 0) mgr->set_global_variable("Duck", 1.0f);
        mgr->on_finished(h, [mgr] {
            if (--ducking == 0) mgr->set_global_variable("Duck", 0.0f);
        });
    }
    push_sound_handle(L, h);
    return 1;
}

/// StopAllSounds()
static int l_StopAllSounds(lua_State* L) {
    if (auto* mgr = get_sound_mgr(L)) mgr->stop_all();
    return 0;
}

/// SetVolume(category, volume 0..1): the player's volume for a category
/// (retail's options: Global, World, Interface, Music, VO).
static int l_SetVolume(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    if (mgr && lua_type(L, 1) == LUA_TSTRING)
        mgr->set_category_volume(lua_tostring(L, 1), static_cast<f32>(luaL_checknumber(L, 2)));
    return 0;
}

/// GetVolume(category) -> 0..1
static int l_GetVolume(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    lua_pushnumber(L, mgr && lua_type(L, 1) == LUA_TSTRING ? mgr->category_volume(lua_tostring(L, 1)) : 1.0);
    return 1;
}

/// PauseSound(bank, pause) / PauseVoice(bank, pause): no pause yet.
static int l_PauseSound(lua_State* /*L*/) { return 0; }
static int l_PauseVoice(lua_State* /*L*/) { return 0; }

/// EnableWorldSounds() / DisableWorldSounds(): the World category on or
/// off (retail silences it for movies and the score screen).
static int l_EnableWorldSounds(lua_State* L) {
    if (auto* mgr = get_sound_mgr(L)) mgr->set_world_enabled(true);
    return 0;
}
static int l_DisableWorldSounds(lua_State* L) {
    if (auto* mgr = get_sound_mgr(L)) mgr->set_world_enabled(false);
    return 0;
}

/// What a UI thread waits on in WaitFor(sound): done when the sound ends.
struct SoundWait : sim::Waitable {
    bool done = false;
    bool is_done() const override { return done; }
    bool is_cancelled() const override { return false; }
};

/// WaitFor(handle) in the UI state: suspend the calling thread until the
/// sound ends (retail's music thread waits out a fade this way).
static int l_ui_WaitFor(lua_State* L) {
    auto* mgr = get_sound_mgr(L);
    auto* threads = get_ui_threads(L);
    if (!mgr || !threads || lua_type(L, 1) != LUA_TNUMBER) return 0;
    const auto h = static_cast<osc::audio::SoundHandle>(lua_tonumber(L, 1));
    if (!mgr->is_playing(h)) return 0;
    // The thread manager reads the waitable only when it parks the thread
    // (right after this yield); the callback keeps it alive until the wake.
    auto wait = std::make_shared<SoundWait>();
    mgr->on_finished(h, [wait, threads] {
        wait->done = true;
        threads->wake(*wait, 0); // only the thread that waited, if it still lives
    });
    lua_pushlightuserdata(L, static_cast<sim::Waitable*>(wait.get()));
    return lua_yield(L, 1);
}

// ====================================================================
// FrontEndData cross-state store (M147c)
// ====================================================================

/// GetFrontEndData(key) -> value
static int l_GetFrontEndData(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    auto* fed = get_front_end_data(L);
    if (fed) {
        fed->get(L, key);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/// SetFrontEndData(key, value)
static int l_SetFrontEndData(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    auto* fed = get_front_end_data(L);
    if (fed) fed->set(L, key, 2);
    return 0;
}

// ====================================================================
// HasCommandLineArg (M147d)
// ====================================================================

/// HasCommandLineArg(arg) -> boolean
static int l_HasCommandLineArg(lua_State* L) {
    const char* arg = luaL_checkstring(L, 1);
    lua_pushstring(L, "__osc_cmdline_args");
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* args = static_cast<std::set<std::string>*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    lua_pushboolean(L, args && args->count(arg) > 0 ? 1 : 0);
    return 1;
}

// ====================================================================
// Profile system — Prefs table (M149a)
// ====================================================================

/// Prefs.GetFromCurrentProfile(key [, default]) -> the current profile's
/// field (retail keeps these in /lua/user/prefs.lua; this global mirrors it).
static int l_GetFromCurrentProfile(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    auto* prefs = get_prefs(L);
    const std::string profile = prefs ? prefs->current_profile_path() : std::string{};
    if (!prefs || profile.empty()) lua_pushnil(L);
    else prefs->push(profile + "." + key, L);
    if (lua_isnil(L, -1) && lua_gettop(L) >= 3) {
        lua_pop(L, 1);
        lua_pushvalue(L, 2);
    }
    return 1;
}

/// Prefs.SetToCurrentProfile(key, val)
static int l_SetToCurrentProfile(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    lua_settop(L, 2);
    auto* prefs = get_prefs(L);
    const std::string profile = prefs ? prefs->current_profile_path() : std::string{};
    if (prefs && !profile.empty()) prefs->set(profile + "." + key, L, 2);
    return 0;
}

// ====================================================================
// Skin selection stub (M149b)
// ====================================================================

/// SetCurrentSkin(skinName)
static int l_SetCurrentSkin(lua_State* L) {
    const char* skin = luaL_checkstring(L, 1);
    lua_pushstring(L, "__osc_current_skin");
    lua_pushstring(L, skin);
    lua_rawset(L, LUA_REGISTRYINDEX);
    spdlog::debug("SetCurrentSkin: {}", skin);
    return 0;
}

/// GetCurrentSkin() -> string
static int l_GetCurrentSkin(lua_State* L) {
    lua_pushstring(L, "__osc_current_skin");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "uef"); // default skin
    }
    return 1;
}

// ====================================================================
// Key binding display (M149c)
// ====================================================================

/// GetKeyBindings() -> table of {action=key} pairs (read-only)
static int l_GetKeyBindings(lua_State* L) {
    lua_newtable(L);

    auto set = [&](const char* action, const char* key) {
        lua_pushstring(L, action);
        lua_pushstring(L, key);
        lua_rawset(L, -3);
    };

    set("attack", "A");
    set("move", "M");
    set("stop", "S");
    set("patrol", "P");
    set("guard", "G");
    set("reclaim", "R");
    set("repair", "E");
    set("capture", "C");
    set("select_all_on_screen", "Ctrl+A");
    set("select_all", "Ctrl+Shift+A");
    set("toggle_pause", "Pause");

    lua_pushstring(L, "__osc_key_bindings");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, -6);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    return 1;
}

/// SetKeyBinding(action, key) -> boolean
static int l_SetKeyBinding(lua_State* L) {
    const char* action = luaL_checkstring(L, 1);
    const char* key = luaL_checkstring(L, 2);

    lua_pushstring(L, "__osc_key_bindings");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, "__osc_key_bindings");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }

    lua_pushstring(L, action);
    lua_pushstring(L, key);
    lua_rawset(L, -3);
    lua_pop(L, 1);

    lua_pushboolean(L, 1);
    return 1;
}

// ====================================================================
// Layout preference (M149d)
// ====================================================================

/// SetLayoutPreference(layout) — store preferred panel layout
static int l_SetLayoutPreference(lua_State* L) {
    const char* layout = luaL_checkstring(L, 1);
    lua_pushstring(L, "__osc_layout_pref");
    lua_pushstring(L, layout);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}

/// GetLayoutPreference() -> string ("bottom" default)
static int l_GetLayoutPreference(lua_State* L) {
    lua_pushstring(L, "__osc_layout_pref");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "bottom");
    }
    return 1;
}

// ── Exit/return (M146d) ───────────────────────────────────────────────────────

/// ExitGame() — return from score screen to front-end menu
static void clear_chat_history(lua_State* L);

/// SessionIsGameOver() -> whether the session has ended (the sim called
/// EndGame, or the score screen ended it).
static int l_SessionIsGameOver(lua_State* L) {
    auto* sim = get_sim(L);
    auto* mgr = get_game_state_mgr(L);
    lua_pushboolean(L, (sim && sim->game_ended()) || (mgr && mgr->sim_stopped()) ? 1 : 0);
    return 1;
}

/// SessionEndGame() -- the score screen ends the session: this client's sim
/// stops. It is a local UI action, so it leaves sim state alone; writing it
/// there would diverge from the other peers.
static int l_SessionEndGame(lua_State* L) {
    if (auto* mgr = get_game_state_mgr(L)) mgr->stop_sim();
    return 0;
}

static int l_ExitGame(lua_State* L) {
    // Leaving a game (the score screen's Continue): tear the session down
    // and return to the front end, as ReturnToLobby does.
    if (get_sim(L)) {
        clear_chat_history(L);
        lua_pushstring(L, "__osc_return_to_lobby");
        lua_pushboolean(L, 1);
        lua_rawset(L, LUA_REGISTRYINDEX);
        return 0;
    }
    auto* mgr = get_game_state_mgr(L);
    if (mgr) mgr->transition_to(osc::GameState::FRONT_END, L);
    auto* beat = get_beat_registry(L);
    if (beat) beat->clear(L);
    spdlog::info("ExitGame: returning to front-end");

    // Call CreateUI() to rebuild the main menu
    lua_pushstring(L, "CreateUI");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        if (lua_pcall(L, 0, 0, 0) != 0) {
            spdlog::warn("ExitGame CreateUI error: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    return 0;
}

/// ReturnToLobby() — score screen continue button signal.
static void clear_chat_history(lua_State* L) {
    lua_pushstring(L, "__osc_chat_history");
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

static int l_ReturnToLobby(lua_State* L) {
    clear_chat_history(L);
    lua_pushstring(L, "__osc_return_to_lobby");
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}

/// ExitApplication() — clean shutdown
static int l_ExitApplication(lua_State* L) {
    lua_pushstring(L, "__osc_exit_requested");
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    return 0;
}

// ── Chat stubs (M151a) ────────────────────────────────────────────────────────

/// RegisterChatFunc(func, name) — register a chat message handler.
/// In single-player, this receives system announcements.
static int l_RegisterChatFunc(lua_State* L) {
    if (!lua_isfunction(L, 1)) return 0;
    lua_pushstring(L, "__osc_chat_func");
    lua_pushvalue(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    spdlog::debug("RegisterChatFunc: handler registered");
    return 0;
}

static void append_chat_history(lua_State* L, int msg_idx) {
    if (msg_idx < 0) msg_idx = lua_gettop(L) + msg_idx + 1;
    lua_pushstring(L, "__osc_chat_history");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushstring(L, "__osc_chat_history");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }

    int next = luaL_getn(L, -1) + 1;
    lua_pushvalue(L, msg_idx);
    lua_rawseti(L, -2, next);
    lua_pop(L, 1);
}

static void dispatch_chat_message(lua_State* L, int msg_idx, const char* warning_prefix) {
    if (msg_idx < 0) msg_idx = lua_gettop(L) + msg_idx + 1;
    lua_pushstring(L, "__osc_chat_func");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, msg_idx);
        if (lua_pcall(L, 1, 0, 0) != 0) {
            spdlog::warn("{} error: {}", warning_prefix, lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
}

/// SessionSendChatMessage(clients, msgTable) — send a chat message.
/// In single-player, echoes to the registered chat function.
static int l_SessionSendChatMessage(lua_State* L) {
    if (!lua_istable(L, 2)) return 0;
    append_chat_history(L, 2);
    dispatch_chat_message(L, 2, "ChatFunc");
    return 0;
}

/// SendSystemMessage(text) — display a system announcement in chat.
/// Calls the registered chat function with sender = "System".
static int l_SendSystemMessage(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);

    lua_newtable(L);
    int msg_idx = lua_gettop(L);
    lua_pushstring(L, "from"); lua_pushstring(L, "System"); lua_rawset(L, msg_idx);
    lua_pushstring(L, "text"); lua_pushstring(L, text); lua_rawset(L, msg_idx);

    append_chat_history(L, msg_idx);
    dispatch_chat_message(L, msg_idx, "SendSystemMessage");
    lua_pop(L, 1);
    return 0;
}

/// GetChatHistory() -> array of chat message tables.
static int l_GetChatHistory(lua_State* L) {
    lua_pushstring(L, "__osc_chat_history");
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    return 1;
}

/// GetSessionClients() → table of connected players.
/// In single-player, returns just the local player.
static int l_GetSessionClients(lua_State* L) {
    lua_newtable(L);
    lua_newtable(L); // client 1
    lua_pushstring(L, "id"); lua_pushstring(L, "0"); lua_rawset(L, -3);
    lua_pushstring(L, "name"); lua_pushstring(L, "Player"); lua_rawset(L, -3);
    lua_pushstring(L, "army"); lua_pushnumber(L, 0); lua_rawset(L, -3);
    lua_rawseti(L, -2, 1);
    return 1;
}

static bool global_is_defined(lua_State* L, const char* name) {
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool defined = !lua_isnil(L, -1);
    lua_pop(L, 1);
    return defined;
}

void register_front_end_fallback_bindings(LuaState& state) {
    lua_State* L = state.raw();

    auto set_stub = [&](const char* name) {
        if (global_is_defined(L, name)) return;
        lua_pushstring(L, name);
        lua_pushcfunction(L, [](lua_State*) -> int { return 0; });
        lua_rawset(L, LUA_GLOBALSINDEX);
    };
    auto set_str = [&](const char* name, const char* val) {
        if (global_is_defined(L, name)) return;
        lua_pushstring(L, name);
        lua_pushstring(L, val);
        lua_rawset(L, LUA_GLOBALSINDEX);
    };
    auto set_bool_fn = [&](const char* name, bool val) {
        if (global_is_defined(L, name)) return;
        lua_pushstring(L, name);
        lua_pushcfunction(L, val ?
            +[](lua_State* call_L) -> int { lua_pushboolean(call_L, 1); return 1; } :
            +[](lua_State* call_L) -> int { lua_pushboolean(call_L, 0); return 1; });
        lua_rawset(L, LUA_GLOBALSINDEX);
    };
    auto set_nil_fn = [&](const char* name) {
        if (global_is_defined(L, name)) return;
        lua_pushstring(L, name);
        lua_pushcfunction(L, [](lua_State* call_L) -> int {
            lua_pushnil(call_L);
            return 1;
        });
        lua_rawset(L, LUA_GLOBALSINDEX);
    };

    set_stub("AudioSetLanguage");
    set_str("__language", "us");
    set_bool_fn("HasLocalizedVO", false);
    set_stub("ConExecute");
    set_stub("ConExecuteSave");
    set_stub("AddInputCapture");
    set_stub("RemoveInputCapture");
    set_bool_fn("AnyInputCapture", false);
    set_bool_fn("DebugFacilitiesEnabled", false);
    set_stub("ExitApplication");
    set_stub("PrefetchSession");
    set_stub("SetFocusArmy");
    set_nil_fn("GetFocusArmy");
    set_stub("ClearFrame");
    set_stub("GpgNetSend");
    set_bool_fn("HasCommandLineArg2", false);
    set_bool_fn("SessionIsActive", false);
    set_bool_fn("SessionIsMultiplayer", false);
    set_bool_fn("SessionIsObservingAllowed", false);
    set_bool_fn("SessionIsBeingRecorded", false);
    set_bool_fn("SessionCanRestart", false);
    set_nil_fn("SessionGetCommandSourceNames");
    set_nil_fn("SessionGetLocalCommandSource");
    set_nil_fn("GetMouseScreenPos");
    set_stub("SetOverlayFilter");
    set_stub("SetOverlayFilters");
    set_nil_fn("GetActiveBuildTemplate");
    set_nil_fn("GetHighlightCommand");
    set_nil_fn("GetInputCapture");
    set_stub("RestartSession");
    set_nil_fn("GetAntiAliasingOptions");
    set_nil_fn("GetResolution");
    set_stub("SetResolution");

    if (!global_is_defined(L, "__installedlanguages")) {
        lua_pushstring(L, "__installedlanguages");
        lua_newtable(L);
        lua_pushstring(L, "us");
        lua_rawseti(L, -2, 1);
        lua_rawset(L, LUA_GLOBALSINDEX);
    }
}

// Read an optional Lua port arg, clamped to [0, 65535] (casting an out-of-range
// double straight to u16 would be undefined behavior; these globals are
// script-callable, so a bad value must not trip UB).
static u16 lan_port_arg(lua_State* L, int idx, u16 dflt) {
    if (!lua_isnumber(L, idx)) return dflt;
    double p = lua_tonumber(L, idx);
    if (p < 0.0) p = 0.0;
    if (p > 65535.0) p = 65535.0;
    return static_cast<u16>(p);
}

// LanHost([port]) -> bool : start hosting a LAN game (default port 47624).
static int l_LanHost(lua_State* L) {
    if (mp_net_state().transport_ready) { lua_pushboolean(L, 0); return 1; }
    bool ok = mp_begin_host(lan_port_arg(L, 1, 47624));
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// LanJoin(ip[, port]) -> bool : connect to a LAN host at ip[:port].
static int l_LanJoin(lua_State* L) {
    if (mp_net_state().transport_ready) { lua_pushboolean(L, 0); return 1; }
    if (lua_type(L, 1) != LUA_TSTRING) { lua_pushboolean(L, 0); return 1; }
    std::string ip = lua_tostring(L, 1);
    size_t a = ip.find_first_not_of(" \t");
    size_t b = ip.find_last_not_of(" \t");
    ip = (a == std::string::npos) ? std::string() : ip.substr(a, b - a + 1);
    if (ip.empty()) { lua_pushboolean(L, 0); return 1; }
    bool ok = mp_begin_join(ip, lan_port_arg(L, 2, 47624));
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// LanNetStatus() -> string : short status for the LAN dialog to display.
static int l_LanNetStatus(lua_State* L) {
    auto& s = mp_net_state();
    const char* status;
    if (s.session) status = "in game";
    else if (!s.transport_ready) status = "idle";
    else if (s.role == MpNetState::Role::Host)
        status = "hosting: waiting for player";
    else status = "connecting";
    lua_pushstring(L, status);
    return 1;
}

void register_lan_ui_bindings(LuaState& state) {
    state.register_function("LanHost", l_LanHost);
    state.register_function("LanJoin", l_LanJoin);
    state.register_function("LanNetStatus", l_LanNetStatus);
}

void register_ui_bindings(LuaState& state, ui::UIControlRegistry& registry) {
    lua_State* L = state.raw();
    clear_chat_history(L);

    // Initialize FontMetricsProvider with VFS for real TrueType metrics
    auto* vfs = LuaState::get_vfs(L);
    if (vfs) ui::FontMetricsProvider::instance().set_vfs(vfs);

    // Store UIControlRegistry pointer in Lua registry
    lua_pushstring(L, "osc_ui_registry");
    lua_pushlightuserdata(L, &registry);
    lua_rawset(L, LUA_REGISTRYINDEX);

    // Register factory globals
    state.register_function("InternalCreateGroup", l_InternalCreateGroup);
    state.register_function("InternalCreateFrame", l_InternalCreateFrame);
    state.register_function("InternalCreateBitmap", l_InternalCreateBitmap);
    state.register_function("InternalCreateText", l_InternalCreateText);
    state.register_function("InternalCreateEdit", l_InternalCreateEdit);
    state.register_function("InternalCreateItemList", l_InternalCreateItemList);
    state.register_function("InternalCreateScrollbar", l_InternalCreateScrollbar);
    state.register_function("InternalCreateBorder", l_InternalCreateBorder);
    state.register_function("InternalCreateDragger", l_InternalCreateDragger);
    state.register_function("PostDragger", l_PostDragger);
    state.register_function("_c_CreateCursor", l_c_CreateCursor);
    state.register_function("InternalCreateMovie", l_InternalCreateMovie);
    state.register_function("InternalCreateMapPreview", l_InternalCreateMapPreview);
    state.register_function("InternalCreateHistogram", l_InternalCreateHistogram);
    state.register_function("InternalCreateWorldMesh", l_InternalCreateWorldMesh);
    state.register_function("InternalCreateDiscoveryService", l_InternalCreateDiscoveryService);
    state.register_function("InternalCreateLobby", l_InternalCreateLobby);
    state.register_function("GetTextureDimensions", l_GetTextureDimensions);
    state.register_function("GetFrame", l_GetFrame);
    state.register_function("GetNumRootFrames", l_GetNumRootFrames);
    state.register_function("SetCursor", l_SetCursor);
    state.register_function("GetCursor", l_GetCursor);

    // LAN multiplayer globals (LanHost/LanJoin/LanNetStatus)
    register_lan_ui_bindings(state);

    // Localization globals
    state.register_function("LOC", l_LOC);
    state.register_function("LOCF", l_LOCF);

    // Preference globals
    state.register_function("GetPreference", l_GetPreference);
    state.register_function("SetPreference", l_SetPreference);
    state.register_function("SavePreferences", l_SavePreferences);
    state.register_function("GetOptions", l_GetOptions);

    // UI thread/coroutine globals
    state.register_function("ForkThread", l_ui_ForkThread);
    state.register_function("WaitSeconds", l_ui_WaitSeconds);
    state.register_function("WaitTicks", l_ui_WaitTicks);

    // Selection↔Lua bridge globals (M137)
    state.register_function("AddOnSelectionChangedCallback", l_AddOnSelectionChangedCallback);
    state.register_function("ValidateUnitsList", l_ValidateUnitsList);
    state.register_function("SetFocusArmy", l_SetFocusArmy);
    state.register_function("GetFocusArmy", l_GetFocusArmy);
    state.register_function("GetArmyAvatars", l_GetArmyAvatars);
    state.register_function("SessionGetLocalCommandSource", l_SessionGetLocalCommandSource);
    state.register_function("GetFireState", l_GetFireState);
    state.register_function("SetOverlayFilters", l_SetOverlayFilters);
    state.register_function("SetOverlayFilter", l_SetOverlayFilter);
    state.register_function("AddToSessionExtraSelectList", l_AddToSessionExtraSelectList);
    state.register_function("RemoveFromSessionExtraSelectList", l_RemoveFromSessionExtraSelectList);
    state.register_function("ClearSessionExtraSelectList", l_ClearSessionExtraSelectList);
    state.register_function("GetScriptBit", l_GetScriptBit);
    state.register_function("GetIsPaused", l_GetIsPaused);
    state.register_function("GetIsAutoMode", l_GetIsAutoMode);
    state.register_function("GetIsSubmerged", l_GetIsSubmerged);
    state.register_function("GetIsAutoSurfaceMode", l_GetIsAutoSurfaceMode);
    state.register_function("SetPaused", l_SetPaused);
    state.register_function("SetAutoMode", l_SetAutoMode);
    state.register_function("SetAutoSurfaceMode", l_SetAutoSurfaceMode);
    state.register_function("SetFireState", l_SetFireState);
    state.register_function("ToggleFireState", l_ToggleFireState);
    state.register_function("ToggleScriptBit", l_ToggleScriptBit);
    state.register_function("GetAssistingUnitsList", l_GetAssistingUnitsList);
    state.register_function("SessionGetCommandSourceNames", l_SessionGetCommandSourceNames);
    state.register_function("GetIdleEngineers", l_GetIdleEngineers);
    state.register_function("GetIdleFactories", l_GetIdleFactories);
    state.register_function("AddConsoleOutputReciever", l_AddConsoleOutputReciever);
    state.register_function("RemoveConsoleOutputReciever", l_RemoveConsoleOutputReciever);

    // SimCallback UI→Sim bridge (M138a)

    // Command data + issuance globals (M138b)
    state.register_function("GetUnitCommandData",          l_GetUnitCommandData);
    state.register_function("GetUnitCommandDataOfUnit", l_GetUnitCommandDataOfUnit);
    state.register_function("IssueUnitCommand",            l_IssueUnitCommand);
    state.register_function("IssueUnitCommandToUnit", l_IssueUnitCommandToUnit);
    state.register_function("IssueBuildMobile", l_IssueBuildMobile);
    state.register_function("GetAttachedUnitsList",        l_GetAttachedUnitsList);
    state.register_function("ClearCommands",               l_ClearCommands);

    // Build mode / command mode globals (M139)
    state.register_function("ClearBuildTemplates",      l_ClearBuildTemplates);
    state.register_function("GetActiveBuildTemplate",   l_GetActiveBuildTemplate);
    state.register_function("SetActiveBuildTemplate",   l_SetActiveBuildTemplate);
    state.register_function("AddCommandFeedbackBlip",   l_AddCommandFeedbackBlip);
    state.register_function("GetUnitById", l_GetUnitById);
    state.register_function("IN_AddKeyMapTable",        l_IN_AddKeyMapTable);
    state.register_function("IN_RemoveKeyMapTable", l_IN_RemoveKeyMapTable);

    // Blueprint query globals (M140)
    state.register_function("EntityCategoryGetUnitList", l_ui_EntityCategoryGetUnitList);
    state.register_function("GetBlueprintIconPath",      l_GetBlueprintIconPath);

    // Category filter globals for ui_L (Phase 1 — construction panel needs these)
    state.register_function("EntityCategoryFilterOut",  l_ui_EntityCategoryFilterOut);
    state.register_function("EntityCategoryFilterDown", l_ui_EntityCategoryFilterDown);
    state.register_function("EntityCategoryContains",   l_ui_EntityCategoryContains);

    // Factory queue display globals (M140c)
    state.register_function("SetCurrentFactoryForQueueDisplay",   l_SetCurrentFactoryForQueueDisplay);
    state.register_function("PeekCurrentFactoryForQueueDisplay",  l_PeekCurrentFactoryForQueueDisplay);
    state.register_function("ClearCurrentFactoryForQueueDisplay", l_ClearCurrentFactoryForQueueDisplay);
    state.register_function("DecreaseBuildCountInQueue",          l_DecreaseBuildCountInQueue);
    state.register_function("IncreaseBuildCountInQueue", l_IncreaseBuildCountInQueue);

    // Order bitmap helpers (M141a)
    state.register_function("GetOrderBitmapNames", l_GetOrderBitmapNames);

    // Unit rollover info (M142a)

    // EnhancementCommon table (M142c)
    {
        lua_pushstring(L, "EnhancementCommon");
        lua_newtable(L);
        lua_pushstring(L, "GetEnhancements");
        lua_pushcfunction(L, l_GetEnhancements);
        lua_rawset(L, -3);          // table["GetEnhancements"] = cfunc
        lua_rawset(L, LUA_GLOBALSINDEX);  // _G["EnhancementCommon"] = table
    }

    // Tooltip/cursor text (M143a)
    state.register_function("StartCursorText", l_StartCursorText);

    // Beat functions (M145b)
    state.register_function("AddBeatFunction", l_AddBeatFunction);
    state.register_function("RemoveBeatFunction", l_RemoveBeatFunction);

    // Time queries (M145c)
    state.register_function("GetGameTimeSeconds", l_ui_GetGameTimeSeconds);
    state.register_function("GameTick", l_ui_GameTick);
    state.register_function("GetGameTime", l_GetGameTime);
    state.register_function("GetSimRate", l_GetSimRate);
    state.register_function("CurrentTime", l_CurrentTime);
    state.register_function("GetSystemTimeSeconds", l_GetSystemTimeSeconds);

    // Scenario info (M145c2)
    state.register_function("SessionGetScenarioInfo", l_ui_SessionGetScenarioInfo);

    // Speed/pause control (M145d)
    state.register_function("SetGameSpeed", l_SetGameSpeed);
    state.register_function("GetGameSpeed", l_GetGameSpeed);
    state.register_function("ConExecute", l_ConExecute);
    state.register_function("SessionRequestPause", l_SessionRequestPause);
    state.register_function("SessionResume", l_SessionResume);

    // Score screen data (M146b)
    state.register_function("IsObserver", l_IsObserver);

    // Escape handler / HideGameUI (M146c)
    state.register_function("EscapeHandler", l_EscapeHandler);
    state.register_function("SetEscapeHandler", l_SetEscapeHandler);
    state.register_function("HideGameUI", l_HideGameUI);

    // Exit/return (M146d)
    state.register_function("ExitGame", l_ExitGame);
    state.register_function("SessionIsGameOver", l_SessionIsGameOver);
    state.register_function("SessionEndGame", l_SessionEndGame);
    state.register_function("ReturnToLobby", l_ReturnToLobby);
    state.register_function("ExitApplication", l_ExitApplication);

    // FrontEndData cross-state store (M147c)
    state.register_function("GetFrontEndData", l_GetFrontEndData);
    state.register_function("SetFrontEndData", l_SetFrontEndData);

    // HasCommandLineArg (M147d)
    state.register_function("HasCommandLineArg", l_HasCommandLineArg);

    // GetCommandLineArg(name, count) → array of count values after the named arg, or nil
    state.register_function("GetCommandLineArg", [](lua_State* L) -> int {
        lua_pushnil(L); // no FA-style command line args in our engine
        return 1;
    });

    // MATH_Lerp(t, t0, t1, v0, v1) → v0 + (v1-v0) * (t-t0) / (t1-t0)
    state.register_function("MATH_Lerp", [](lua_State* L) -> int {
        double t  = luaL_checknumber(L, 1);
        double t0 = luaL_checknumber(L, 2);
        double t1 = luaL_checknumber(L, 3);
        double v0 = luaL_checknumber(L, 4);
        double v1 = luaL_checknumber(L, 5);
        double denom = t1 - t0;
        double result = (denom != 0.0) ? v0 + (v1 - v0) * (t - t0) / denom : v0;
        lua_pushnumber(L, result);
        return 1;
    });

    // Map preview stub (M148d)
    state.register_function("MapPreview", l_MapPreview);

    // Sound(table) — identity constructor used by FA for sound descriptors
    state.register_function("Sound", [](lua_State* L) -> int {
        lua_pushvalue(L, 1);
        return 1;
    });

    // Audio globals (M147b)
    state.register_function("PlaySound", l_PlaySound);
    state.register_function("StopSound", l_StopSound);
    state.register_function("PlayVoice", l_PlayVoice);
    state.register_function("PauseSound", l_PauseSound);
    state.register_function("PauseVoice", l_PauseVoice);
    state.register_function("EnableWorldSounds", l_EnableWorldSounds);
    state.register_function("DisableWorldSounds", l_DisableWorldSounds);
    state.register_function("StopAllSounds", l_StopAllSounds);
    state.register_function("SetVolume", l_SetVolume);
    state.register_function("GetVolume", l_GetVolume);
    state.register_function("WaitFor", l_ui_WaitFor);
    state.register_function("AudioSetLanguage", [](lua_State*) -> int { return 0; });

    // Prefs table (M149a)
    {
        lua_pushstring(L, "Prefs");
        lua_newtable(L);
        lua_pushstring(L, "GetFromCurrentProfile");
        lua_pushcfunction(L, l_GetFromCurrentProfile);
        lua_rawset(L, -3);
        lua_pushstring(L, "SetToCurrentProfile");
        lua_pushcfunction(L, l_SetToCurrentProfile);
        lua_rawset(L, -3);
        lua_rawset(L, LUA_GLOBALSINDEX);
    }

    // UIUtil table — skin and layout (M149b, M149d)
    {
        // Create or get existing UIUtil table
        lua_pushstring(L, "UIUtil");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
        }
        lua_pushstring(L, "SetCurrentSkin");
        lua_pushcfunction(L, l_SetCurrentSkin);
        lua_rawset(L, -3);
        lua_pushstring(L, "GetCurrentSkin");
        lua_pushcfunction(L, l_GetCurrentSkin);
        lua_rawset(L, -3);
        lua_pushstring(L, "SetLayoutPreference");
        lua_pushcfunction(L, l_SetLayoutPreference);
        lua_rawset(L, -3);
        lua_pushstring(L, "GetLayoutPreference");
        lua_pushcfunction(L, l_GetLayoutPreference);
        lua_rawset(L, -3);
        lua_pushstring(L, "UIUtil");
        lua_pushvalue(L, -2);
        lua_rawset(L, LUA_GLOBALSINDEX);
        lua_pop(L, 1); // pop the table
    }

    // Key bindings (M149c)
    state.register_function("GetKeyBindings", l_GetKeyBindings);
    state.register_function("SetKeyBinding", l_SetKeyBinding);

    // Chat stubs (M151a-b)
    state.register_function("RegisterChatFunc", l_RegisterChatFunc);
    state.register_function("SessionSendChatMessage", l_SessionSendChatMessage);
    state.register_function("SendSystemMessage", l_SendSystemMessage);
    state.register_function("GetChatHistory", l_GetChatHistory);
    state.register_function("GetSessionClients", l_GetSessionClients);

    // Engine state queries (M144c)
    state.register_function("GetCurrentUIState", l_GetCurrentUIState);
    state.register_function("WorldIsLoading", l_WorldIsLoading);
    state.register_function("SessionIsActive", l_ui_SessionIsActive);
    state.register_function("WorldIsPlaying", l_WorldIsPlaying);
    state.register_function("GameTime", l_GameTime);
    state.register_function("SessionIsPaused", l_SessionIsPaused);
    state.register_function("IsNISMode", l_IsNISMode);
    state.register_function("MapBorderClear", l_MapBorderClear);
    state.register_function("MapBorderAdd", l_MapBorderAdd);
    state.register_function("LaunchSinglePlayerSession", l_LaunchSinglePlayerSession);
    state.register_function("StartFrontEndUI", l_StartFrontEndUI);

    // Cache the LazyVar.Create function in registry for fast access.
    // We import /lua/lazyvar.lua and grab its Create function.
    int top = lua_gettop(L);
    lua_pushstring(L, "import");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, "/lua/lazyvar.lua");
        if (lua_pcall(L, 1, 1, 0) == 0 && lua_istable(L, -1)) {
            lua_pushstring(L, "Create");
            lua_rawget(L, -2);
            if (lua_isfunction(L, -1)) {
                lua_pushstring(L, "__osc_lazyvar_create");
                lua_pushvalue(L, -2);
                lua_rawset(L, LUA_REGISTRYINDEX);
                spdlog::info("Cached LazyVar.Create in registry");
            }
            lua_pop(L, 1); // Create function
        } else {
            if (lua_isstring(L, -1))
                spdlog::warn("LazyVar import failed: {}", lua_tostring(L, -1));
        }
    }
    lua_settop(L, top);

    // Create the root frame (GetFrame(0)) — a UIControl with frame_methods metatable
    {
        u32 id = registry.create();
        auto* root_ctrl = registry.get(id);
        if (root_ctrl) {
            // Create a Lua table for the root frame
            lua_newtable(L);

            // Set _c_object
            lua_pushstring(L, "_c_object");
            lua_pushlightuserdata(L, root_ctrl);
            lua_rawset(L, -3);

            // Set metatable to moho.frame_methods (which inherits control_methods)
            lua_getglobal(L, "moho");
            if (lua_istable(L, -1)) {
                lua_pushstring(L, "frame_methods");
                lua_rawget(L, -2);
                if (lua_istable(L, -1)) {
                    // Create metatable with __index = frame_methods
                    lua_newtable(L); // mt
                    lua_pushstring(L, "__index");
                    lua_pushvalue(L, -3); // frame_methods
                    lua_rawset(L, -3);    // mt.__index = frame_methods
                    lua_setmetatable(L, -4); // setmetatable(frame_table, mt)
                }
                lua_pop(L, 1); // frame_methods
            }
            lua_pop(L, 1); // moho

            // Store Lua table ref on the control
            lua_pushvalue(L, -1);
            int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            root_ctrl->set_lua_table_ref(ref);
            root_ctrl->set_name("RootFrame");

            // Create LazyVars on the frame table
            create_lazyvar(L, -1, "Left");
            create_lazyvar(L, -1, "Top");
            create_lazyvar(L, -1, "Right");
            create_lazyvar(L, -1, "Bottom");
            create_lazyvar(L, -1, "Width");
            create_lazyvar(L, -1, "Height");
            create_lazyvar(L, -1, "Depth");

            // Store in registry as __osc_root_frame
            lua_pushstring(L, "__osc_root_frame");
            lua_pushvalue(L, -2);
            lua_rawset(L, LUA_REGISTRYINDEX);

            lua_pop(L, 1); // pop frame table

            spdlog::info("Created root frame (GetFrame(0)) as UIControl #{}", id);
        }
    }

    spdlog::info("Registered UI bindings ({} controls available)",
                 registry.count());
}
} // namespace osc::lua
