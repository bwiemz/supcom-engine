# M206w: script orders, Moho's way

## Why

Retail gives some orders as Lua classes, run by the engine as a unit's
task: a **script order** (`UNITCOMMAND_Script`) names its class with
`TaskName`, and the engine runs `/lua/sim/tasks/<TaskName>.lua`. Retail has
two:

- `EnhanceTask`: the construction panel's ACU and SACU upgrades. The task
  stops the unit, starts the work (`OnWorkBegin`), advances
  `WorkProgress` from the resources the unit is given each tick, and ends it
  (`OnWorkEnd`), or fails it (`OnWorkFail`) when cancelled.
- `TargetLocation`: a unit's ability with a target point. The Eye of
  Rhianne (`RemoteViewing.lua`) offers it through `Sync.Abilities`, and its
  `OnTargetLocation` opens the remote view.

The engine turns the construction panel's `EnhanceTask` into an
enhancement order of its own (C++ progress) and drops every other script
order ("unsupported Script order"): the Eye of Rhianne can't scry.
`IssueScript` is a no-op, and retail's AI enhances its ACU with it
(`platoon.lua` `EnhanceAI`: `IssueScript({unit}, {TaskName =
'EnhanceTask', Enhancement = ...})`), so AI commanders never upgrade.
`moho.ScriptTask_Methods` is empty, so FAF's and mods' tasks can't run.
(`IssueEnhancement` is the engine's own global, not retail's.)

The rules come from faf-re (`moho/script/CUnitScriptTask.cpp`,
`moho/ai/IAiCommandDispatchImpl.cpp`, `moho/task/CTaskThread.cpp`) and the
retail scripts (`ScriptTask.lua`, `EnhanceTask.lua`, `TargetLocation.lua`,
`RemoteViewing.lua`, `construction.lua`, `orders.lua`).

## The rules

- A script order carries its Lua table (`mArgs`): the UI's
  `IssueCommand('UNITCOMMAND_Script', args)` or the sim's
  `IssueScript(units, args)`. `TaskName` names the class; the rest is the
  task's data (`Enhancement`, `Location`, ...).
- When the order reaches the front of a unit's queue, the unit's command
  dispatch makes a `CUnitScriptTask`:
  - its class: `import('/lua/sim/tasks/<TaskName>.lua')[TaskName]`; with no
    `TaskName`, or no such class, `ScriptTask` itself, logging `Can't find
    task %s, using ScriptTask directly`;
  - its Lua object: an instance of that class (`moho.ScriptTask_Methods` is
    the class's C base), then `OnCreate(args)`.
- Each tick the task runs `TaskTick` (its class's, or its current state's:
  retail's tasks are state machines, `ChangeState(self, self.Stopping)`),
  and its return schedules it as every Moho task's does
  ([[moho-task-timing]]):
  - `-1` (Done): the task ends, and the unit's next order starts in the
    same tick;
  - `0` (Repeat): again in the same tick;
  - `1` (Wait): next tick; `n > 1`: `n - 1` ticks later;
  - `-2` (Suspend): asleep until something wakes it (the order changing);
    `-3` (Abort): the unit's tasks end; `-4` (Delay): after the other
    tasks this tick -- the engine runs it next tick, as neither retail
    task returns it.
  - An error in `TaskTick` is a warning, and the task ends (`-1`).
- A change to the order (its cancel, the queue cleared) sends the task back
  to its first state and ticks it at once (`OnEvent`).
- The task ending -- done, cancelled, its unit gone -- runs `OnDestroy`.
- `moho.ScriptTask_Methods`: `GetUnit()`, `SetAIResult(result)` (the
  order's AI result: 0 Unknown, 1 Success, 2 Fail, 3 Ignored).
- Globals for tasks: `LUnitMove(task, target)`,
  `LUnitMoveNear(task, target, range)` (the task's unit moves to an AI
  target).

### How the UI gives them
- The construction panel: `IssueCommand("UNITCOMMAND_Script", {TaskName =
  'EnhanceTask', Enhancement = ...}, true)`.
- An ability button (`orders.lua` `AbilityButtonBehavior`, for units in
  `categories.ABILITYBUTTON` whose `UnitData[id].Abilities` has one active):
  command mode `order` with `{name = 'RULEUCC_Script', AbilityName, TaskName}`;
  the world click then issues the script order to the selection. The task
  reads the click as `commandData.Location` (`TargetLocation.OnCreate`).
  faf-re's `CUnitCommand` takes its `mArgs` from the issue data's Lua
  object; where `Location` joins it isn't in the decompiled code, so the
  engine adds the click's position as `Location` to the mode's table.

## The design

- `CommandType::Script`; `UnitCommand::script_args` holds the order's table
  as `lua::lua_to_bytes` wrote it. The command codec carries it (replay
  format 11), so lockstep peers and replays run the same task.
- Issuing:
  - the sim's `IssueScript(units, args)` queues it on each unit, after
    what it has (as every `Issue*` does);
  - the UI's `IssueCommand('UNITCOMMAND_Script', args, clear)` issues it as
    a player's order (`EnhanceTask` is no longer turned into the engine's
    own enhancement order);
  - a world click in a `RULEUCC_Script` command mode issues it with the
    mode's table and the click's position as `Location`, to the selected
    units in `categories.ABILITYBUTTON` (the category the orders panel
    shows abilities for; the Eye has no `RULEUCC_Script` cap to filter
    by). The app builds the table (`CommandMode::script_args_at`), since
    the renderer's click code has no Lua.
- Running (`Unit::order_script`), with the unit's one running task
  (`ScriptTask`: the Lua object's ref, the order it runs, the ticks it
  waits, whether it is suspended, its AI result):
  - at the front of the queue with no task of its own: the class is
    resolved (`/lua/sim/tasks/<TaskName>.lua`, else `ScriptTask`), the
    object made as every script object is (`push_new_script_object`), and
    `OnCreate(args)` run;
  - each tick after its wait, `TaskTick` (looked up through the object, so
    a state's own runs); its status:
    - Done (`-1`) or Abort (`-3`): the order ends (`OnDestroy`), and the
      next one starts in the same tick;
    - Repeat (`0`, or no `TaskTick`): again at once, up to 64 times a tick
      (then next tick, with a warning: Moho would spin);
    - Wait (`1`), Delay (`-4`): next tick; `n > 1`: `n - 1` ticks on;
    - Suspend (`-2`): no more ticks until the order is gone;
    - an error, or a non-number: a warning, and Done.
  - the order gone from the front -- cleared, replaced, taken by a new
    order -- or the unit dying or destroyed: `OnDestroy`, then the object
    goes.
- `moho.ScriptTask_Methods`: `GetUnit()` and `SetAIResult(result)`. The
  object holds its unit's Lua object, so `GetUnit()` still answers in
  `OnDestroy` while the unit is being destroyed, as Moho's task reaches
  its unit then.
- The engine's own `CommandType::Enhance` stays for its own
  `IssueEnhancement` global (tests use it); retail never gives it.
- Not done: `LUnitMove`/`LUnitMoveNear` (no retail script calls them).

## Tests
- unit: a Lua task's statuses in ticks (Wait, `n`, Repeat, Done, Suspend,
  Abort); `OnCreate` gets the order's table, `OnDestroy` runs when it ends
  and when the order is cleared; a missing class falls back to
  `ScriptTask`; an error ends it; `GetUnit`/`SetAIResult`; the codec
  round-trips the table (and a version 10 replay has none); the
  construction panel's `IssueCommand` gives a script order;
- data: an ACU enhances through retail's `EnhanceTask`, as the
  construction panel and retail's AI order it; the Eye of Rhianne's
  `TargetLocation` reaches its `OnTargetLocation`; the enhance test and
  the determinism and replay gates hold.
