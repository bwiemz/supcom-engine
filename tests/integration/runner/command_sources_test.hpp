#pragma once

namespace osc::app {
struct Engine;
}

namespace osc::test {

/// --command-sources-test: a launch's player and its allied AI under the
/// CommonArmy option Union. Each army takes its own player's commands
/// (Moho's mValidCommandSources); on FAF's data its union army shares the
/// ally's control, its MergeArmyPair hands the player's control, units
/// (its commander too: FAF's exe gives one) and focus to the army merged
/// into, and a dropped player's army is abandoned to its
/// brain (AbandonedByPlayer). On retail's, the option is unknown, and the
/// dropped player's army is defeated by retail's AbandonedByPlayer.
void run_command_sources_test(app::Engine& e);

} // namespace osc::test
