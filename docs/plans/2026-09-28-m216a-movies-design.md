# M216a: movies, their sounds, and FA's splash screens

The engine could open a movie but never show one. A Movie control decoded
one frame per rendered frame whatever the movie's rate, never uploaded
it, and never drew it. It never finished (`OnFinished` never ran), so
nothing that waits on a movie could go on. `GetNumFrames` returned 0.
`MovieWidth` and `MovieHeight`, which size a movie in retail's layout,
stayed unset. The sounds retail plays with a movie never started:
`PlaySound(sound, true)` played at once, and `SoundIsPrepared` and
`StartSound` did not exist. The front end skipped FA's splash screens and
went straight to the main menu.

## Moho

From faf-re (`UiRuntimeTypes.cpp`, `CMovie.h`, `StartupHelpers.cpp`,
`CUserSoundManager.cpp`, `IUIManager.cpp`, `CScApp.cpp`) and retail's
`movie.lua` and `splash.lua`:

- **`CMovie`** plays a Sofdec (`.sfd`, MPEG-1) movie on Sofdec's own clock.
  `OpenMovie` reads the header's frame count (`mwsffrm_AnalyTotalFrm`,
  the SofdecStream block in the second or third 2 KiB block) and rate,
  opens the player paused, and uploads the first frame. `PlayMovie`
  unpauses, `Stop` pauses, and `StartMoviePlaybackFromName` starts over.
  `HasPlaybackFinished` is Sofdec's play-end status. `UpdatePlaybackFrame`
  uploads the frame due.
- **`CMauiMovie::Frame(dt)`**:
  - if the control is not playing, it stops its frame updates and runs
    `OnFinished`;
  - otherwise it runs `OnFrame(dt)`;
  - if stopped, it stops its frame updates and runs `OnStopped`;
  - at the movie's end it starts over when looping, else it stops playing
    and runs `OnFinished`;
  - otherwise it shows the frame due, and runs `OnSubtitle` when the
    subtitle changes.
- **`Play`** sets the control playing and wanting frame updates, unpauses
  the movie, and clears `stopped`. With no movie it only wants frame
  updates, so the next frame runs `OnFinished`.
- **`Stop`** marks the control stopped and pauses the movie.
- **`LoadFile`** (`InternalSet`) replaces the movie and publishes its size
  to `MovieWidth` and `MovieHeight`. On failure it drops the movie and
  stops playing.
- **`GetNumFrames` and `GetFrameRate`** read the movie. Moho has no guard
  for a control without one.
- **Drawing:** `DoRender` draws the movie's texture over the control, in
  its vertex alpha, only while it plays.
- **Sounds:** `PlaySound(params, prepareOnly)`,
  `PlayVoice(params, duck, prepareOnly)` and `PlayTutorialVO` hand XACT a
  preload-only play: the cue is prepared, not started. `SoundIsPrepared`
  is true once the cue is no longer preparing (for no cue, true).
  `StartSound` plays it. `movie.lua`'s `Set` prepares its sound and
  voice, waits until the movie is loaded and both are prepared, then
  calls `OnLoaded`. `Play` starts the sounds.
- **The splash:** Moho's default start is `UI_StartSplashScreens`, which
  runs `uimain.lua`'s `StartSplashScreen` (with no `/map`, `/replay` or
  `/load`, or when one fails). The splash captures input and plays the
  THQ, GPG and NVIDIA logos and FA's intro in turn. A click or Escape skips
  to the intro, then leaves. With the `movie.nologo` preference it goes
  straight on. It leaves through `EngineStartFrontEndUI`, which is
  `UI_StartFrontEnd`: it clears the input capture and the dragger,
  rebuilds the root frames, and runs `StartFrontEndUI` (the main menu) in
  the same Lua state.

## The engine

- **`video::MoviePlayer`** is `CMovie`: it holds the header's frame count
  and a clock. It opens paused on its first frame. While running, the
  clock advances by each frame's `dt`, and it decodes up to the frame due
  (frame n from n / rate), converting only the last to RGBA. It decodes at
  most 8 frames a call, so a movie that decodes slower than it plays drops
  frames and still ends on time. It has finished once the clock passes its
  last frame, or its stream ends first. The decoder now takes the file
  instead of copying it (FA's intro is 177 MB). Its frames are opaque:
  pl_mpeg's RGBA conversion never writes alpha, and the buffer had started
  at 0.
- **Movie controls** hold a player and Moho's flags (`playing`, `looping`,
  `stopped`). `UIDispatch::update_controls` runs `CMauiMovie::Frame` for
  them in place of the plain `OnFrame`. It looks the callbacks up through
  the class, as `RunScript` does. The bindings follow `CMauiMovie`.
  `InternalSet` sets `MovieWidth` and `MovieHeight`. `GetNumFrames` and
  `GetFrameRate` read 0 without a movie.
- **`renderer::MovieTextures`** gives each loaded movie an RGBA texture.
  A new frame is copied to a per-frame-slot staging buffer as the UI is
  built, then into the texture by the frame's command buffer. A texture
  replaced or dropped is freed once no frame in flight can use it. The UI
  renderer draws a playing movie's texture over the control, in its alpha.
  `render_ui_only` now builds the UI before its render pass, so the copy
  can be recorded, and runs the UI on the frame's measured (or fixed)
  step instead of 1/60.
- **Prepared sounds:** `SoundManager::prepare` creates the instance and
  applies the cue's limits, but schedules nothing. `start` begins it. A
  stop ends a prepared sound at once. `PlaySound`, `PlayVoice` (whose duck
  starts with it, as Moho's does) and `PlayTutorialVO` take
  `prepareOnly`. `SoundIsPrepared` is true: the engine reads a cue's waves
  when it prepares it. `StartSound` starts it.
- **The splash:** a player's front end (a window, not a test or capture)
  starts with `StartSplashScreen`. Test runs keep the main menu, and a test
  can ask for the splash. `EngineStartFrontEndUI` clears the input
  capture, empties the root frame, and runs `StartFrontEndUI`, followed by
  the engine's LAN dialog. A game that returns to the front end runs
  `StartFrontEndUI` too, as Moho's does, in place of `main.lua`'s
  `CreateUI`.

- **Escape no longer quits.** `Renderer::poll_events` closed the window
  on Escape, a debugging aid from M35. Moho has no such key: Escape skips
  the splash, closes dialogs, and opens the escape menu. The game quits
  through retail's Exit (`ExitApplication`), or by closing the window.
- The main menu's background movie (`main_menu.sfd`, looping) now shows
  behind it.

## Not done

- Subtitles (`OnSubtitle`): FA's movies carry none; the campaign draws its
  own.
- `OnMinimized` pausing a movie.
- `/nomovie`.
- The key map names Escape `Escape`, where retail's `keyNames.lua` says
  `Esc`. So retail's `escape` key action (`uimain.EscapeHandler`) is still
  reached through the engine's own handler (M217).

## Tests

- `--movie-test` (gate):
  - the header's frame count and rate;
  - playback on its clock: the frame shown at a time, a stop that pauses,
    `OnFinished` once at the end, a loop that starts over;
  - `Play` with no movie finishing on the next frame;
  - the prepared sounds;
  - the movie drawn: the frame on screen is the decoded frame due;
  - the splash, from the front end's start: the THQ logo plays with its
    sound, ends, and the GPG logo follows; Escape skips to the intro and
    Escape again leaves to the main menu, with the capture gone.
- Unit tests: the Sofdec header, and the prepared sounds.
