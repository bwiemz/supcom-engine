-- Keeps the original's /synclog files: without sim_KeepAllLogFiles it deletes
-- each beat's file unless that beat went out of sync, so a single-player game
-- keeps none. Harmless without /synclog, and the engine ignores the command.

local synclog_create_ui = CreateUI
function CreateUI(isReplay)
    ConExecute('sim_KeepAllLogFiles true')
    return synclog_create_ui(isReplay)
end
