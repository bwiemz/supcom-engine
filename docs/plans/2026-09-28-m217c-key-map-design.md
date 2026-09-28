# M217c: retail's key map, and the console its actions run through

Retail's hotkeys never fired in the engine.

- **Actions.** `IN_AddKeyMapTable` took only Lua functions, but retail's key
  mappings bind each key to a console command:
  `{action = 'UI_Lua import("/lua/ui/game/tabs.lua").TogglePause()'}`. The
  engine dropped every one of them.
- **Names.** The engine named keys its own way (`Escape`, `Up`, `Numpad0`,
  `LBracket`), where retail's `keyNames.lua` says `Esc`, `UpArrow`, `Num0`,
  `LeftBracket`.
- **Loading.** Moho loads the default mappings itself when its UI starts,
  so the front end has them too. The engine had none until a lobby
  launched.
- **Routing.** The engine sent a key a focused control ignored on to the
  key map. Moho never does.
- **Console.** `ConExecute` understood one command, and read
  `WLD_GameSpeed n` as a speed multiplier. Retail uses the console for game
  speed (`WLD_IncreaseSimRate`), and `SetGameSpeed(0)` (cutscenes) means
  normal speed, not a stopped sim.
- **Hardcoded keys.** The window loop had its own P, +, − and Escape. They
  fired over chat and dialogs, and doubled retail's own bindings.

## Moho

From faf-re: `UiRuntimeTypes.cpp` (CUIKeyHandler, around lines 27690-28430),
`CConCommand.cpp`, `CWldSession.h`, `Sim.h`, `IUIManager.cpp`.

- **Key names:** `in_keyNames[256]`, indexed by Windows virtual-key code.
  They start as `Unknown%02X` and are filled from `keyNames.lua`'s
  `keyNames` (hex strings to names).
- **`IN_ParseKeyModifiers`** splits `Ctrl-Shift-A` on `-`. The last token is
  the key, found case-insensitively in the names (-1 if unknown). The rest
  are modifiers, case-insensitive: Shift `0x80000000`, Ctrl `0x40000000`,
  Alt `0x20000000`. Anything else is warned about.
- **The key map** is one map from chord to action string, plus a set of
  chords that fire on auto-repeat. `IN_AddKeyMapTable(t)` sets
  `t[key].action` (and `keyRepeat`), and a later table overwrites an earlier
  one. `IN_RemoveKeyMapTable(t)` erases `t`'s chords. At start,
  `IN_InitKeyHandler` loads the names and `keymapper.GetKeyMappings()`.
- **`CUIKeyHandler::OnKeyDown`** sits below the controls' event mapper. It
  sees only the keys a focused control skipped, or those with no focus and
  no capture. If a control has focus, it does nothing. Otherwise:
  - the chord is the raw code (virtual key) with the modifier bits;
  - an auto-repeat of a chord not marked `keyRepeat` is dropped;
  - a bound chord runs its action through `CON_Execute`;
  - an unbound Enter calls `chat.lua`'s `ActivateChat(modifiers)`, in a
    game only;
  - `~` toggles the console.
- **`CON_Execute`** parses commands:
  - whitespace separates tokens;
  - a token that starts with `"` runs to the next `"` (with `\"` and `\\`),
    while a quote mid-token is a character;
  - `#` or `//` ends the line, and `;` ends a command;
  - names are found case-insensitively; an unknown one prints `Unknown
    console command`.
- **`UI_Lua`** joins its tokens with spaces and runs them in the UI state.
  `UI_MakeSelectionSet`/`UI_ApplySelectionSet name` call `selection.lua`.
  `UI_RotateSkin`/`UI_RotateLayout [dir]` call `uiutil.lua` (default `+`).
  `UI_ToggleGamePanels` calls `gamemain.lua`'s `HideGameUI()`.
- **Game speed** is an integer sim rate from −10 to +50; the sim runs at
  10^(rate/10) times normal.
  - `WLD_GameSpeed n`, `WLD_IncreaseSimRate`, `WLD_DecreaseSimRate` and
    `WLD_ResetSimRate` set it.
  - `SetGameSpeed(n)` sets it, and `GetGameSpeed()` reads it.

## The engine

- **`ui::Console`:** the parser and a case-insensitive command registry.
  `lua::register_console_commands` adds the commands above. `ConExecute`
  and `ConExecuteSave` run lines through it. It lives with the app, beside
  the key map, and is published to each UI state.
- **`ui::KeyMapRegistry`** is Moho's key handler: key names, chord parsing,
  one action map, and repeat chords. The boot loads `keyNames.lua` and the
  default mappings once, after the first UI state.
- **`UIDispatch`:** a key goes to the focused control, else the capture,
  else the key map, as the order above gives.
- **`GameStateManager`** holds the sim rate. The speed is derived from it.
- **The window loop's P, +, − and Escape are gone.** Retail's key map
  covers them: Pause, NumPlus, NumMinus, NumStar and Esc.

## Not done (M217d)

`StartCommandMode`, `UI_SelectByCategory`, `UI_TrackUnit` and `IssueCommand`
need the session's selection, command modes and camera. Until then they
print `Unknown console command`, as a command Moho lacks would. The `~`
console is also left out.

## Tests

- Unit tests:
  - the parser: quotes, escapes, `;`, `#`, `//`;
  - the registry: case, unknown commands;
  - chord parsing: names, modifiers in any order and any case, unknown
    names;
  - add, remove, `keyRepeat`;
  - routing: focus and capture keep keys from the map, repeats drop,
    actions run;
  - the sim rate.
- `--keymap-test` (gate), in a game with FA's interface:
  - retail's names and default mappings are loaded at boot;
  - Esc runs `uimain.EscapeHandler`;
  - NumPlus, NumMinus and NumStar step and reset the rate;
  - Pause pauses the game;
  - Enter opens chat;
  - a focused edit box keeps its keys from the map.
