#pragma once

// Binding-coverage report (roadmap M184): which engine functions and methods
// do the game's Lua scripts call that this engine does not provide?
//
// Referenced names come from a static scan of the scripts; registered names
// from the booted engine's Lua states at runtime (globals, moho.* method
// tables, the engine's cached __osc_* metatables), so every registration
// style counts. The difference is ratcheted against a names-only baseline.

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct lua_State;

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::lua::coverage {

/// What a set of Lua sources calls and defines.
struct LuaReferences {
    /// `obj:Name(...)` calls -> first location ("file:line").
    std::map<std::string, std::string> method_calls;
    /// Bare `Name(...)` calls -> first location.
    std::map<std::string, std::string> global_calls;
    /// Names the scripts define themselves (functions, fields, locals, globals
    /// assigned); calls to these are not engine gaps.
    std::set<std::string> defined;
};

/// Scan one Lua source in FA's LuaPlus dialect (`#` and `--` comments,
/// strings, long brackets) and merge its references into `refs`.
void scan_lua_source(std::string_view source, std::string_view file,
                     LuaReferences& refs);

struct CoverageReport {
    /// (name, first location), sorted by name.
    std::vector<std::pair<std::string, std::string>> missing_globals;
    std::vector<std::pair<std::string, std::string>> missing_methods;
    size_t globals_referenced = 0;
    size_t methods_referenced = 0;
};

/// Referenced names that are neither defined in Lua nor registered.
/// Methods are matched by name only (no class attribution).
CoverageReport compute_coverage(const LuaReferences& refs,
                                const std::set<std::string>& registered_globals,
                                const std::set<std::string>& registered_methods);

/// Add every global name and every engine-provided method name visible in L.
void collect_registered(lua_State* L, std::set<std::string>& globals,
                        std::set<std::string>& methods);

/// Baseline entries are "G Name" / "M Name" lines; '#' starts a comment.
std::set<std::string> parse_baseline(std::string_view text);

/// Render a report as baseline lines (sorted "G ..." then "M ...").
std::string format_baseline(const CoverageReport& report);

struct BaselineDelta {
    std::vector<std::string> new_gaps;    ///< missing now, not in baseline
    std::vector<std::string> closed_gaps; ///< in baseline, no longer missing
};
BaselineDelta diff_against_baseline(const CoverageReport& report,
                                    const std::set<std::string>& baseline);

/// `opensupcom --map <scenario> --binding-coverage <report> [--binding-baseline <file>]`:
/// scan the game's scripts in the VFS, compare against the sim and UI states'
/// registered bindings, write the report (baseline format plus a first
/// location per name), and -- with a baseline -- fail (return 1) on any gap
/// not listed there. Returns the process exit code.
int run_coverage_report(lua_State* sim_L, lua_State* ui_L,
                        const vfs::VirtualFileSystem& vfs,
                        const std::string& report_path,
                        const std::string& baseline_path);

} // namespace osc::lua::coverage
