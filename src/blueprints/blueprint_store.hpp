#pragma once

#include "core/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace osc::blueprints {

enum class BlueprintType {
    Unit,
    Projectile,
    Prop,
    Mesh,
    Beam,
    Emitter,
    TrailEmitter,
};

const char* blueprint_type_name(BlueprintType type);

struct BlueprintEntry {
    BlueprintType type;
    std::string id;     ///< Lowercase blueprint ID (e.g., "uel0001")
    std::string source; ///< Source file path
    int lua_ref = -1;   ///< Lua registry reference to the blueprint table
};

/// Central registry of all loaded blueprints.
/// Stores blueprints as Lua table references — does NOT parse into C++ structs.
class BlueprintStore {
public:
    explicit BlueprintStore(lua_State* L);
    ~BlueprintStore();

    /// Called by Register*Blueprint C functions.
    /// Reads BlueprintId from the table at stack_index and stores it.
    void register_blueprint(lua_State* L, BlueprintType type, int stack_index);

    /// Find a blueprint by ID.
    const BlueprintEntry* find(std::string_view id) const;

    /// Get all blueprints of a given type.
    std::vector<const BlueprintEntry*> get_all(BlueprintType type) const;

    /// Count blueprints of a given type.
    size_t count(BlueprintType type) const;

    /// Total number of blueprints.
    size_t total_count() const { return blueprints_.size(); }

    /// Push the Lua table for a blueprint onto the stack.
    /// Uses the main lua_State (L_). For coroutine safety, use the overload
    /// that takes an explicit lua_State*.
    void push_lua_table(const BlueprintEntry& entry) const;

    /// Push the Lua table for a blueprint onto a specific lua_State's stack.
    /// Use this when called from a coroutine context.
    void push_lua_table(const BlueprintEntry& entry, lua_State* L) const;

    /// Read a string field from a blueprint's Lua table (uses main L_).
    std::optional<std::string> get_string_field(
        const BlueprintEntry& entry, const char* field) const;
    /// Read a string field — coroutine-safe overload.
    std::optional<std::string> get_string_field(
        const BlueprintEntry& entry, const char* field, lua_State* L) const;

    /// Read a number field from a blueprint's Lua table (uses main L_).
    std::optional<double> get_number_field(
        const BlueprintEntry& entry, const char* field) const;
    /// Read a number field — coroutine-safe overload.
    std::optional<double> get_number_field(
        const BlueprintEntry& entry, const char* field, lua_State* L) const;

    /// Log statistics about loaded blueprints.
    void log_statistics() const;

    /// Create the __blueprints Lua global table mapping bp IDs → bp tables.
    /// Must be called after all blueprints are registered.
    void expose_to_lua(lua_State* L) const;

private:
    lua_State* L_;
    std::unordered_map<std::string, BlueprintEntry> blueprints_;
};

} // namespace osc::blueprints
