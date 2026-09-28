# M217d: the console's session commands

M217c made retail's hotkeys reach the console, but the commands behind most
of them act on the game session and were missing:
- `StartCommandMode order RULEUCC_Move` and its kin (22 bindings: move,
  attack, patrol, guard, repair, reclaim, ferry, nukes...);
- `UI_SelectByCategory` (16: select the engineers, the idle engineer, the
  commander, everything on screen, the nearest factory...);
- `IssueCommand Stop` (S).

Each printed `Unknown console command`.

## Moho

From faf-re `CConCommand.cpp` and `CCommandLuaFunctionRegistrations.cpp`.

- **`CON_StartCommandMode mode name`** (with no session, "no session"; with
  fewer than three tokens, nothing):
  - It reads the active mode from `commandmode.lua`'s
    `GetCommandMode()`: `{mode, payload}`.
  - If the same mode is active with the same `payload.name`, compared
    case-insensitively, it ends it (`EndCommandMode()`).
  - Otherwise, if a selected unit has the command cap named by `name`
    (`ERuleBPUnitCommandCaps`), it starts it:
    `StartCommandMode(mode, {name = name})`.
- **`UI_SelectByCategory [+add] [+nearest] [+idle] [+inview] [+goto]
  [+excludeengineers] expression`**, through `SelectUnitsByCategory`:
  - The candidates are the units in the camera's frustum (`+inview`), else
    all.
  - The expression is parsed once: spaces intersect, commas unite.
  - The result starts from the selection with `+add`, else empty.
  - Each candidate must be selectable, of the focus army, idle if `+idle`
    (not busy, nothing queued), and in the category. With
    `+excludeengineers` it must be neither `ENGINEER` nor `COMMAND`.
  - `+nearest` keeps the one nearest the cursor. Otherwise every unit
    passing is added, except one being upgraded.
  - `+goto` frames the result with the camera: one unit's box, or the box
    of them all (an idle one cycles between framings).
  - The result becomes the selection.
- **`CON_IssueCommand Stop|Pause|Dive|SiloBuildTactical|SiloBuildNuke`** issues
  that order to the selection. Stop, Pause and Dive clear the queues
  (`ISSUE_Command(..., true)`), and Stop and Pause set the selection again.
  The silo builds don't clear.

## The engine

`lua::register_session_console_commands` adds the three to the console.
They read the input handler's selection, the sim's units and the renderer's
camera, as the UI's other bindings do.
- The cap check is `Unit::has_command_cap`.
- The category is `parse_category_list`, the blueprint list parser, which
  has Moho's syntax.
- The frustum is the camera's view-projection.
- The cursor is the world view's pick under the mouse, as
  `GetMouseWorldPos`.

## Not done

- `+goto` recentres the camera on the result (as `UIZoomTo` does); it
  doesn't fit the box's zoom or cycle the idle framing.
- `IssueCommand Pause`: the engine has no pause order.
- `UI_TrackUnit` needs camera tracking.

## Tests

- `--session-command-test` (gate), in a game with FA's interface, with two
  engineers, a tank and an enemy engineer added:
  - selections by category: intersection, union, `ALLUNITS`, never the
    enemy's;
  - `+excludeengineers`, `+add`, `+nearest`, `+idle`, an unselectable unit;
  - `StartCommandMode`'s start, toggle and cap refusal;
  - Ctrl-B, M and S through the key map.
