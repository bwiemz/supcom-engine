#pragma once

// Moho's sim console (faf-re sim/CSimConCommand.cpp, sim/Sim.cpp,
// sim/SimDebugCommandRegistrations.cpp): the commands the user's console sends
// into the sim through the command stream (DoSimCommand), each with the
// cursor's place, the focus army and the selection. Sim::ExecuteDebugCommand
// runs them; one not allowed without cheats runs only with cheats on.

#include "core/types.hpp"
#include "sim/entity.hpp"

#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace osc::sim {

class SimState;

/// The sim's console variables that act on the game (Moho's TSimConVar
/// instances, one set per sim). Saved with the game.
struct SimConVars {
    /// NoDamage: no damage to anything (Entity::AdjustHealth refuses a loss).
    bool no_damage = false;
    /// AI_RunOpponentAI: whether the opponent AI runs (CAiBrain::IsOpponentAIRunning,
    /// with /noai). Moho lets any player set it, cheats or not.
    bool run_opponent_ai = true;
};

/// What a sim command runs with: its tokens (the name first), where the cursor
/// was, the focus army (or -1) and the selected units, as Moho's handlers take
/// them.
struct SimConContext {
    SimState& sim;
    lua_State* L;
    const std::vector<std::string>& args;
    Vector3 at;
    i32 focus_army;
    const std::vector<EntityId>& selected;
};

/// One of Moho's sim commands: its name, whether it runs without cheats on
/// (CSimConCommand::mAllowWithoutCheats), and its handler.
struct SimConCommand {
    const char* name;
    bool allow_without_cheats;
    void (*run)(SimConContext& ctx);
};

/// The sim command of that name, in any case (Moho's registry compares with
/// STR_CompareNoCase); null if none.
const SimConCommand* find_sim_command(std::string_view name);

/// Every sim command, for the console's aliases (each CConAlias "<name>" runs
/// "DoSimCommand <name> ...").
const std::vector<SimConCommand>& sim_commands();

/// Moho's ParseOneSimCommand: the first command of `text` as tokens, split at
/// whitespace; `"` quotes (a `\` escapes inside them), `;` ends the command
/// (what follows goes to `rest`), `#` the line.
void parse_sim_command(std::string_view text, std::vector<std::string>& tokens, std::string& rest);

/// Moho's UnparseSimCommand: `tokens` as one line, a token with whitespace, `;`
/// or `#` (or none at all) quoted, its `\` and `"` escaped.
std::string unparse_sim_command(const std::vector<std::string>& tokens);

/// Sim::ExecuteDebugCommand and TryParseSimCommand: each `;`-separated command
/// of `text` in turn. An unknown one is reported; one that needs cheats ends
/// the line when they are off, and is echoed with its source when they are on.
void execute_debug_command(SimState& sim, lua_State* L, std::string_view text, const Vector3& at,
                           i32 focus_army, const std::vector<EntityId>& selected);

/// Lua's base library loaders: dofile and require run a file from this
/// machine's own disk, the others make code of data. Whether the function at
/// `idx` is one, by name or under another (`x = dofile`).
bool is_base_loader(lua_State* L, const std::string& name, int idx);

} // namespace osc::sim
