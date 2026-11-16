#pragma once

// opensupcom --check-mods and --list-mods: what a modder can learn of a
// broken mod before a game does. Moho says little. A mod_info.lua that fails
// is one warning in the log (mods.lua's LoadModInfo), and the mod leaves the
// mod manager; of two mods with one uid the mod manager keeps one; a hook of
// a path no script has never runs, silently; a hook that doesn't compile, or
// follows a script ending in `return`, breaks the script it hooks, as the
// script and its hooks are one chunk (doscript); a ShaderName the renderer
// lacks draws as Unit. The checks read the mods as the engine reads them, in
// a Lua state of their own, without a window or a GPU.

#include <map>
#include <string>
#include <vector>

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::lua {
struct InitConfig;
}

namespace osc::app::mods {

enum class Severity { Warning, Error };

/// One thing a check found.
struct Finding {
    Severity severity = Severity::Error;
    std::string check; ///< the check's name: "mod_info", "hook_target"...
    std::string file;  ///< the VFS file it is about
    int line = 0;      ///< its line in `file`, from 1; 0 for the file as a whole
    std::string message;
};

/// A mod: its mod_info.lua as mods.lua's LoadModInfo reads it.
struct ModInfo {
    std::string info_file; ///< as DiskFindFiles names it: /mods/x/mod_info.lua
    std::string location;  ///< its folder, /mods/x
    bool readable = false; ///< the file ran
    std::string uid;       ///< "" when it isn't a string (LoadModInfo's default: info_file)
    std::string name;
    std::string version; ///< as written; "" when not given
    std::string hookdir = "/hook";
    bool ui_only = false;
    bool enabled = true; ///< `enabled = false`: never loaded
    bool exclusive = false;
    std::vector<std::string> required;                 ///< `requires`
    std::vector<std::string> conflicting;              ///< `conflicts`
    std::map<std::string, std::string> required_names; ///< `requiresNames`
};

/// A mod and what the checks found in it.
struct ModReport {
    ModInfo mod;
    std::vector<Finding> findings;
    int errors() const;
    int warnings() const;
};

struct Report {
    std::vector<ModReport> mods;        ///< by location
    std::vector<std::string> unmatched; ///< names given that no mod answers to
    /// Each unmatched name counts as an error.
    int errors() const;
    int warnings() const;
};

/// Each mod_info.lua under /mods, read as the mod manager reads it, by
/// location; a report's findings are reading's (a file that doesn't run).
std::vector<ModReport> read_mods(const vfs::VirtualFileSystem& vfs);

/// Check the mods `names` names, or every mod under /mods when it names none.
/// A name is a uid, a mod's folder or its mod_info.lua (VFS paths, or a
/// folder above mods: "/mods" names them all), or its name. Each mod's
/// mod_info.lua (it runs; a uid, unique; a whole-number version; a hookdir
/// from '/'; what it requires installed and conflicts with not, by the data's
/// mods.lua GetDependencies where it has one), its hooks (of files there are,
/// that compile with them), its blueprints (they compile and run; their
/// ShaderNames have a technique; the files their meshes name are there) and
/// its sounds folder's banks (they parse).
Report check_mods(const vfs::VirtualFileSystem& vfs, const std::vector<std::string>& names);

/// A report as text: per mod, "file:line: error: message" lines.
std::string report_text(const Report& report);
/// ...as JSON.
std::string report_json(const Report& report);
/// read_mods' mods, a line each.
std::string list_text(const std::vector<ModReport>& mods);
std::string list_json(const std::vector<ModReport>& mods);

/// The command (--check-mods [names...] or --list-mods, either --json): the
/// data mounted from `config` as a game mounts it, a name that is a folder or
/// archive on disk mounted first, at /mods/<its name> as retail's init file
/// mounts the player's mods; the report on stdout. Exits 0 with no errors, 1
/// with one, 2 when it can't check (no game, or its init file fails).
int run_mod_command(const lua::InitConfig& config, std::vector<std::string> names, bool list,
                    bool json);

} // namespace osc::app::mods
