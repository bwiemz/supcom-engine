#include "blueprints/blueprint_store.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace osc::blueprints {

const char* blueprint_type_name(BlueprintType type) {
    switch (type) {
    case BlueprintType::Unit: return "Unit";
    case BlueprintType::Projectile: return "Projectile";
    case BlueprintType::Prop: return "Prop";
    case BlueprintType::Mesh: return "Mesh";
    case BlueprintType::Beam: return "Beam";
    case BlueprintType::Emitter: return "Emitter";
    case BlueprintType::TrailEmitter: return "TrailEmitter";
    }
    return "Unknown";
}

BlueprintStore::BlueprintStore(lua_State* L) : L_(L) {}

BlueprintStore::~BlueprintStore() {
    // Release all Lua references.
    // Skip if Lua state was already destroyed (e.g. after sim reload/teardown
    // where rebind() was not called before the old state was freed).
    if (!L_) return;
    for (auto& [id, entry] : blueprints_) {
        if (entry.lua_ref != -1) {
            luaL_unref(L_, LUA_REGISTRYINDEX, entry.lua_ref);
        }
    }
}

namespace {

/// Moho fills in blueprint fields that scripts read unconditionally. A unit
/// without a Footprint uses its SizeX/SizeZ, rounded, at least 1 (FAF engine
/// notes, EntityBlueprint.lua). 165 retail units have none, and StructureUnit
/// reads bp.Footprint.SizeX when it flattens its skirt.
/// t[key] = {Min = 0, Max = 0} unless t already has it.
void default_min_max(lua_State* L, int t, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, t);
    const bool present = !lua_isnil(L, -1);
    lua_pop(L, 1);
    if (present) return;
    lua_pushstring(L, key);
    lua_newtable(L);
    lua_pushstring(L, "Min");
    lua_pushnumber(L, 0);
    lua_rawset(L, -3);
    lua_pushstring(L, "Max");
    lua_pushnumber(L, 0);
    lua_rawset(L, -3);
    lua_rawset(L, t);
}

void apply_unit_defaults(lua_State* L, int bp) {
    // Intel always exists, with its range pairs: Unit.IntelWatchThread
    // iterates Intel.JamRadius / SpoofRadius on every intel-powered unit.
    lua_pushstring(L, "Intel");
    lua_rawget(L, bp);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "Intel");
        lua_newtable(L);
        lua_rawset(L, bp);
        lua_pushstring(L, "Intel");
        lua_rawget(L, bp);
    }
    const int intel = lua_gettop(L);
    default_min_max(L, intel, "JamRadius");
    default_min_max(L, intel, "SpoofRadius");
    lua_pop(L, 1);

    // A footprint the .bp leaves unsized, whole or per axis, takes the unit's
    // own size, rounded and at least 1, as Moho's does (FAF's loader
    // emulates the same rule). Retail's GetSkirtRect and GetFootPrintSize
    // read Footprint.SizeX/SizeZ unguarded; 205 retail units omit them, some
    // with a Footprint table that holds only MinWaterDepth (the UEF TMD).
    lua_pushstring(L, "Footprint");
    lua_rawget(L, bp);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "Footprint");
        lua_newtable(L);
        lua_rawset(L, bp);
        lua_pushstring(L, "Footprint");
        lua_rawget(L, bp);
    }
    const int footprint = lua_gettop(L);
    for (const char* axis : {"SizeX", "SizeZ"}) {
        lua_pushstring(L, axis);
        lua_rawget(L, footprint);
        const bool sized = lua_isnumber(L, -1) != 0;
        lua_pop(L, 1);
        if (sized) continue;
        lua_pushstring(L, axis);
        lua_rawget(L, bp); // the unit's own SizeX / SizeZ
        const double size = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 1.0;
        lua_pop(L, 1);
        lua_pushstring(L, axis);
        lua_pushnumber(L, std::max(1.0, std::floor(size + 0.5)));
        lua_rawset(L, footprint);
    }

    // A skirt the .bp leaves out is the footprint (at least 1, as FAF's
    // loader reads a missing one), with no offset. Mobile units have none,
    // and the AI's AIBuildAdjacency reads Physics.SkirtSizeX unguarded on
    // whatever it builds beside, a commander included.
    lua_pushstring(L, "Physics");
    lua_rawget(L, bp);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "Physics");
        lua_newtable(L);
        lua_rawset(L, bp);
        lua_pushstring(L, "Physics");
        lua_rawget(L, bp);
    }
    const int physics = lua_gettop(L);
    for (const auto& [skirt, offset, foot] :
         {std::array<const char*, 3>{"SkirtSizeX", "SkirtOffsetX", "SizeX"},
          std::array<const char*, 3>{"SkirtSizeZ", "SkirtOffsetZ", "SizeZ"}}) {
        lua_pushstring(L, skirt);
        lua_rawget(L, physics);
        const bool sized = lua_isnumber(L, -1) != 0;
        lua_pop(L, 1);
        if (!sized) {
            lua_pushstring(L, foot);
            lua_rawget(L, footprint);
            const double size = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 1.0;
            lua_pop(L, 1);
            lua_pushstring(L, skirt);
            lua_pushnumber(L, std::max(1.0, size));
            lua_rawset(L, physics);
        }
        lua_pushstring(L, offset);
        lua_rawget(L, physics);
        const bool placed = lua_isnumber(L, -1) != 0;
        lua_pop(L, 1);
        if (!placed) {
            lua_pushstring(L, offset);
            lua_pushnumber(L, 0);
            lua_rawset(L, physics);
        }
    }
    lua_pop(L, 2); // Physics, Footprint
}

/// Moho reads a prop's reclaim values as numbers, so an empty string (162
/// of retail's prop blueprints write ReclaimEnergyMax = '', and the default
/// wreck too) is 0. Prop.lua's GetReclaimCosts does arithmetic with them.
void apply_prop_defaults(lua_State* L, int bp) {
    lua_pushstring(L, "Economy");
    lua_rawget(L, bp);
    if (lua_istable(L, -1)) {
        const int economy = lua_gettop(L);
        for (const char* field : {"ReclaimMassMax", "ReclaimEnergyMax", "ReclaimTime",
                                  "ReclaimMassTimeMultiplier", "ReclaimEnergyTimeMultiplier"}) {
            lua_pushstring(L, field);
            lua_rawget(L, economy);
            const int type = lua_type(L, -1);
            // A string that reads as a number (lua_tonumber) keeps its value.
            const lua_Number value = type == LUA_TSTRING ? lua_tonumber(L, -1) : 0;
            lua_pop(L, 1);
            if (type != LUA_TSTRING) continue;
            lua_pushstring(L, field);
            lua_pushnumber(L, value);
            lua_rawset(L, economy);
        }
    }
    lua_pop(L, 1);
}

} // namespace

void BlueprintStore::register_blueprint(lua_State* L, BlueprintType type,
                                          int stack_index) {
    // Read BlueprintId from the table.
    // Copy to std::string immediately — lua_tostring pointers are only
    // valid while the value is on the stack.
    std::string id;
    lua_pushstring(L, "BlueprintId");
    lua_gettable(L, stack_index);
    if (lua_isstring(L, -1) && lua_strlen(L, -1) > 0) {
        id = lua_tostring(L, -1);
    }
    lua_pop(L, 1);

    if (id.empty()) {
        // Try to read Source field as fallback for ID derivation
        lua_pushstring(L, "Source");
        lua_gettable(L, stack_index);
        if (lua_isstring(L, -1)) {
            id = lua_tostring(L, -1);
        }
        lua_pop(L, 1);
    }

    if (id.empty()) {
        spdlog::debug("Blueprint with no BlueprintId or Source, skipping");
        return;
    }

    // Read Source for logging
    std::string source;
    lua_pushstring(L, "Source");
    lua_gettable(L, stack_index);
    if (lua_isstring(L, -1)) {
        source = lua_tostring(L, -1);
    }
    lua_pop(L, 1);
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (type == BlueprintType::Unit || type == BlueprintType::Prop) {
        const int bp = stack_index > 0 ? stack_index : lua_gettop(L) + stack_index + 1;
        if (type == BlueprintType::Unit) apply_unit_defaults(L, bp);
        else apply_prop_defaults(L, bp);
    }

    // Create a Lua registry reference for the table
    lua_pushvalue(L, stack_index);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    BlueprintEntry entry;
    entry.type = type;
    entry.id = id;
    entry.source = std::move(source);
    entry.lua_ref = ref;

    // If duplicate, release old reference
    auto it = blueprints_.find(id);
    if (it != blueprints_.end()) {
        if (it->second.lua_ref != -1) {
            luaL_unref(L_, LUA_REGISTRYINDEX, it->second.lua_ref);
        }
    }

    blueprints_[id] = std::move(entry);
}

const BlueprintEntry* BlueprintStore::find(std::string_view id) const {
    std::string key(id);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto it = blueprints_.find(key);
    return (it != blueprints_.end()) ? &it->second : nullptr;
}

std::vector<const BlueprintEntry*> BlueprintStore::get_all(
    BlueprintType type) const {
    std::vector<const BlueprintEntry*> result;
    for (const auto& [id, entry] : blueprints_) {
        if (entry.type == type) {
            result.push_back(&entry);
        }
    }
    return result;
}

size_t BlueprintStore::count(BlueprintType type) const {
    size_t c = 0;
    for (const auto& [id, entry] : blueprints_) {
        if (entry.type == type) c++;
    }
    return c;
}

void BlueprintStore::push_lua_table(const BlueprintEntry& entry) const {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, entry.lua_ref);
}

void BlueprintStore::push_lua_table(const BlueprintEntry& entry, lua_State* L) const {
    lua_rawgeti(L, LUA_REGISTRYINDEX, entry.lua_ref);
}

std::optional<std::string> BlueprintStore::get_string_field(
    const BlueprintEntry& entry, const char* field) const {
    return get_string_field(entry, field, L_);
}

std::optional<std::string> BlueprintStore::get_string_field(
    const BlueprintEntry& entry, const char* field, lua_State* L) const {
    push_lua_table(entry, L);
    lua_pushstring(L, field);
    lua_gettable(L, -2);
    std::optional<std::string> result;
    if (lua_isstring(L, -1)) {
        result = lua_tostring(L, -1);
    }
    lua_pop(L, 2); // pop value and table
    return result;
}

std::optional<double> BlueprintStore::get_number_field(
    const BlueprintEntry& entry, const char* field) const {
    return get_number_field(entry, field, L_);
}

std::optional<double> BlueprintStore::get_number_field(
    const BlueprintEntry& entry, const char* field, lua_State* L) const {
    push_lua_table(entry, L);
    lua_pushstring(L, field);
    lua_gettable(L, -2);
    std::optional<double> result;
    if (lua_isnumber(L, -1)) {
        result = lua_tonumber(L, -1);
    }
    lua_pop(L, 2);
    return result;
}

void BlueprintStore::log_statistics() const {
    spdlog::info("Blueprint loading complete:");
    spdlog::info("  Units:        {}",
                 count(BlueprintType::Unit));
    spdlog::info("  Projectiles:  {}",
                 count(BlueprintType::Projectile));
    spdlog::info("  Meshes:       {}",
                 count(BlueprintType::Mesh));
    spdlog::info("  Props:        {}",
                 count(BlueprintType::Prop));
    spdlog::info("  Emitters:     {}",
                 count(BlueprintType::Emitter));
    spdlog::info("  Beams:        {}",
                 count(BlueprintType::Beam));
    spdlog::info("  Trails:       {}",
                 count(BlueprintType::TrailEmitter));
    spdlog::info("  Total:        {}", total_count());
}

void BlueprintStore::expose_to_lua(lua_State* L) const {
    // Build a Lua table: __blueprints[bp_id] = bp_table
    lua_newtable(L);
    for (const auto& [id, entry] : blueprints_) {
        lua_pushstring(L, id.c_str());
        lua_rawgeti(L, LUA_REGISTRYINDEX, entry.lua_ref);
        lua_settable(L, -3);
    }
    // Set as global __blueprints (use rawset to bypass strict mode)
    lua_pushstring(L, "__blueprints");
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_GLOBALSINDEX);
    lua_pop(L, 1); // pop table
    spdlog::info("Exposed {} blueprints as __blueprints global", blueprints_.size());
}

void BlueprintStore::rebind(lua_State* new_L) {
    L_ = new_L;
    // Clear all lua refs — the old Lua state is destroyed, so we must NOT
    // call luaL_unref. Just reset to -1 (LUA_NOREF equivalent).
    for (auto& [id, entry] : blueprints_) {
        entry.lua_ref = -1;
    }
    spdlog::info("BlueprintStore rebound to new Lua state ({} blueprints, refs cleared)",
                 blueprints_.size());
}

} // namespace osc::blueprints
