#include "blueprints/blueprint_store.hpp"

#include <algorithm>
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
    // Release all Lua references
    for (auto& [id, entry] : blueprints_) {
        if (entry.lua_ref != -1) {
            luaL_unref(L_, LUA_REGISTRYINDEX, entry.lua_ref);
        }
    }
}

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

} // namespace osc::blueprints
