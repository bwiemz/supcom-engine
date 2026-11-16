#pragma once

// --mod <uid or path>: the mods of a game the command line starts (--map),
// for modders to test a mod without the lobby.
//
// A path (a mod's folder, or a .zip or .scd) is mounted at /mods/<name>,
// where retail's init file mounts each entry of the player's mods folder,
// before the init file's own mounts. The list is then resolved by the
// data's own /lua/mods.lua, in a Lua state of its own, as the mod manager
// and the lobby use it: the mods each requires join it, a missing
// requirement or a conflict refuses it, and GetGameMods orders the game
// mods (retail by uid, FAF by its before/after lists). The game mods are
// the game's GameMods (sim::GameSetup::mods); the UI-only ones stand in for
// the player's selection when the game's UI runs SessionInit.lua.

#include "core/result.hpp"
#include "core/types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace osc::vfs {
class VirtualFileSystem;
}

namespace osc::lua {

/// One --mod: an installed mod's uid, or a mod on disk.
struct ModArg {
    std::string given;       ///< as the command line gave it
    fs::path path;           ///< the mod on disk; empty for a uid
    std::string mount_point; ///< where the mod on disk mounts; empty for a uid
};

/// --mod's argument: a mod on disk when `given` names a file or folder
/// there, else a uid.
ModArg mod_arg(std::string_view given);

/// Where retail's init file mounts a mods folder entry so named: /mods/ and
/// its name in lower case, less a .scd or .zip extension (mount_contents).
std::string mod_mount_point(const fs::path& path);

/// What is wrong with `mods` as a command line's (empty: nothing): a mod on
/// disk must be a folder or an archive, and two can't share a mount point.
std::string check_mod_args(const std::vector<ModArg>& mods);

/// The mods a command line's game plays with.
struct CommandLineMods {
    /// Its GameMods, as sim::lua_to_bytes writes it (sim::GameSetup::mods):
    /// mods.lua's GetGameMods of the selection.
    std::string game;
    std::vector<std::string> game_uids; ///< the game mods' uids, in its order
    std::vector<std::string> ui_uids;   ///< the UI-only mods' uids, sorted
    /// The mods the named ones require, which joined them, sorted.
    std::vector<std::string> required;
};

/// Resolve `mods` with `vfs`'s /lua/mods.lua (its mods on disk mounted):
/// each uid matched among the installed mods (exactly, else ignoring
/// case), each mod on disk to the mods under its mount point. An unknown
/// uid fails, its message listing the installed mods' names and uids.
Result<CommandLineMods> resolve_command_line_mods(vfs::VirtualFileSystem& vfs,
                                                  const std::vector<ModArg>& mods);

} // namespace osc::lua
