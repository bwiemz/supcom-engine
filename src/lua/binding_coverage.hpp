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
using lua_CFunction = int (*)(lua_State*);

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
    /// Globals bound only to a stand-in answering a constant (or nothing):
    /// neither missing nor covered, reported apart.
    std::vector<std::pair<std::string, std::string>> stand_in_globals;
    size_t globals_referenced = 0;
    size_t methods_referenced = 0;
    /// Referenced globals a real binding provides.
    size_t globals_covered = 0;
};

/// Referenced names that are neither defined in Lua nor registered.
/// Methods are matched by name only (no class attribution). A global that
/// is not registered but in `stand_in_globals` goes to the report's
/// stand-ins, not its gaps.
CoverageReport compute_coverage(const LuaReferences& refs,
                                const std::set<std::string>& registered_globals,
                                const std::set<std::string>& registered_methods,
                                const std::set<std::string>& stand_in_globals = {});

/// Add every global name and every engine-provided method name visible in L.
void collect_registered(lua_State* L, std::set<std::string>& globals,
                        std::set<std::string>& methods);

/// Add the globals of L still bound to a blueprint-phase stand-in: they are
/// missing there, whatever another state binds
void collect_stand_ins(lua_State* L, std::set<std::string>& globals);

/// Whether `f` answers a constant, or nothing, whatever it is asked: a
/// lua_stubs:: helper (noop, return_nil, return_false, ...) or one of the
/// user state's stand-ins (is_user_stand_in).
bool is_constant_stand_in(lua_CFunction f);

/// Add the globals of L bound to a constant stand-in.
void collect_constant_stand_ins(lua_State* L, std::set<std::string>& globals);

/// Baseline entries are "G Name" / "M Name" lines; '#' starts a comment.
/// (A names-only list, one name a line, reads the same way.)
std::set<std::string> parse_baseline(std::string_view text);

/// Where a gap no script calls is reported: Moho's user state has the name.
inline constexpr const char* kMohoUserState = "Moho's user state";

/// Add to `report`, as missing globals, the names of `moho_user` (the
/// functions Moho's user state defines) that `ui_globals` lacks: the engine
/// is to define them whether or not a game script calls one. A name the
/// scripts call and the engine lacks is already there, at its call site.
/// One the UI state binds to a stand-in (`ui_stand_ins`) goes to the
/// report's stand-ins instead.
void add_moho_user_gaps(CoverageReport& report, const std::set<std::string>& moho_user,
                        const std::set<std::string>& ui_globals,
                        const std::set<std::string>& ui_stand_ins = {});

/// Render a report as baseline lines (sorted "G ..." then "M ...").
std::string format_baseline(const CoverageReport& report);

struct BaselineDelta {
    std::vector<std::string> new_gaps;    ///< missing now, not in baseline
    std::vector<std::string> closed_gaps; ///< in baseline, no longer missing
};
BaselineDelta diff_against_baseline(const CoverageReport& report,
                                    const std::set<std::string>& baseline);

/// `opensupcom --map <scenario> --binding-coverage <report> [--binding-baseline <file>]
/// [--binding-moho-user <file>]`: scan the game's scripts in the VFS, compare
/// against the sim and UI states' registered bindings -- and, given Moho's
/// user-state functions, the UI state's globals against them -- write the
/// report (baseline format plus a first location per name), and -- with a
/// baseline -- fail (return 1) on any gap not listed there. Returns the
/// process exit code.
int run_coverage_report(lua_State* sim_L, lua_State* ui_L, const vfs::VirtualFileSystem& vfs,
                        const std::string& report_path, const std::string& baseline_path,
                        const std::string& moho_user_path = {});

} // namespace osc::lua::coverage
