#pragma once

#include "core/types.hpp"
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace osc::lua {

enum class SmokeCategory {
    MissingGlobal,
    MissingMethod,
    PcallError,
    WrongReturn, // Deferred: detection requires per-function return type knowledge.
                 // Category exists for manual tagging during triage.
};

struct SmokeReportEntry {
    SmokeCategory category;
    std::string name;
    std::string first_location;
    u32 count;
};

class SmokeTestHarness {
public:
    void record(SmokeCategory category, const std::string& name,
                const std::string& location);

    std::vector<SmokeReportEntry> generate_report() const;
    u32 total_count() const;

    void print_report() const;

    /// Install __index metamethod on the globals table that logs missing accesses.
    /// The harness pointer is stored in the Lua registry as "__osc_smoke_harness".
    void install_global_interceptor(lua_State* L);

private:
    struct EntryKey {
        SmokeCategory category;
        std::string name;
        bool operator==(const EntryKey& o) const {
            return category == o.category && name == o.name;
        }
    };
    struct EntryKeyHash {
        size_t operator()(const EntryKey& k) const {
            return std::hash<std::string>()(k.name) ^
                   (std::hash<int>()(static_cast<int>(k.category)) << 16);
        }
    };
    struct EntryData {
        std::string first_location;
        u32 count = 0;
    };
    std::unordered_map<EntryKey, EntryData, EntryKeyHash> entries_;
};

} // namespace osc::lua
