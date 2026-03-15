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
    std::string phase;
    u32 count;
};

class SmokeTestHarness {
public:
    ~SmokeTestHarness() { deactivate(); }

    void activate() { s_active_ = this; }
    void deactivate() { if (s_active_ == this) s_active_ = nullptr; }
    static SmokeTestHarness* active_instance() { return s_active_; }

    static constexpr u32 MAX_ISSUES_PER_PHASE = 500;

    void record(SmokeCategory category, const std::string& name,
                const std::string& location);

    void set_phase(const std::string& phase) { current_phase_ = phase; }

    std::vector<SmokeReportEntry> generate_report() const;
    u32 total_count() const;

    void print_report(bool group_by_phase = false) const;
    void write_report_to_file(const std::string& path,
                              bool group_by_phase = true) const;

    /// Install __index metamethod on the globals table that logs missing accesses.
    /// The harness pointer is stored in the Lua registry as "__osc_smoke_harness".
    void install_global_interceptor(lua_State* L);

    /// Install __index fallback on a cached metatable to log missing method calls.
    /// registry_key: e.g., "__osc_proj_mt". type_name: e.g., "Projectile".
    void install_method_interceptor(lua_State* L, const char* registry_key,
                                     const char* type_name);

    /// Install method interceptors on all known __osc_*_mt metatables.
    void install_all_method_interceptors(lua_State* L);

    /// Install lua_atpanic handler that logs instead of aborting.
    void install_panic_handler(lua_State* L);

    /// Execute a Lua string, recording any pcall errors in the harness.
    bool do_string_logged(lua_State* L, const char* code);

    /// Execute a Lua file (via VFS buffer), recording any pcall errors.
    bool do_buffer_logged(lua_State* L, const char* buffer, size_t len,
                          const char* name);

private:
    struct EntryKey {
        SmokeCategory category;
        std::string name;
        std::string phase;
        bool operator==(const EntryKey& o) const {
            return category == o.category && name == o.name && phase == o.phase;
        }
    };
    struct EntryKeyHash {
        size_t operator()(const EntryKey& k) const {
            auto h1 = std::hash<std::string>()(k.name);
            auto h2 = std::hash<int>()(static_cast<int>(k.category));
            auto h3 = std::hash<std::string>()(k.phase);
            return h1 ^ (h2 << 16) ^ (h3 << 1);
        }
    };
    struct EntryData {
        std::string first_location;
        u32 count = 0;
    };
    std::unordered_map<EntryKey, EntryData, EntryKeyHash> entries_;
    std::string current_phase_;
    std::unordered_map<std::string, u32> phase_unique_counts_;

    static SmokeTestHarness* s_active_;
};

} // namespace osc::lua
