#pragma once

#include "core/types.hpp"

#include <string>
#include <unordered_map>

struct lua_State;

namespace osc::sim {

class ArmorDefinition {
public:
    /// Parse from Lua global "armordefinition" table (array of arrays).
    /// Each entry: [1] = armor type name, [2..n] = "DamageType multiplier".
    void load_from_lua(lua_State* L);

    /// Get damage multiplier for armor_type x damage_type.
    /// Returns 1.0 if no specific entry (damage passes through unmodified).
    f32 get_multiplier(const std::string& armor_type,
                       const std::string& damage_type) const;

    /// Override a specific multiplier at runtime (AlterArmor).
    void set_multiplier(const std::string& armor_type,
                        const std::string& damage_type, f32 mult);

    /// How many armor types were loaded (for logging).
    size_t size() const { return table_.size(); }

private:
    // armor_type -> (damage_type -> multiplier)
    std::unordered_map<std::string,
        std::unordered_map<std::string, f32>> table_;
};

} // namespace osc::sim
