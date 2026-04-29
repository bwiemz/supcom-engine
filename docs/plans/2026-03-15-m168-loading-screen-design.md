# M168: Loading Screen — Design Spec

**Goal:** FA-compatible loading screen with video playback, animated text, and cycling tips. FA's `LoadDialog()` runs unmodified. The reload sequence is chunked so the render loop stays alive during loading.

**Dependencies:** pl_mpeg (single-header MPEG-1 decoder, public domain)

---

## 1. Architecture

Three subsystems work together:

1. **Chunked reload** — `execute_reload_sequence()` split into a 5-stage state machine. The main loop executes one stage per frame, pumping UI frames between stages.
2. **Movie UIControl** — New UI control type backed by pl_mpeg. Decodes MPEG-1 video frames to a Vulkan texture each render tick.
3. **FA Lua path** — FA's `LoadDialog()` in `gamemain.lua:391-438` runs unmodified. `WorldIsLoading()`, `WldUIProvider`, tips thread, and `Pulse` effect all work.

### Loading Flow

```
1. Player clicks "Launch" in lobby
2. __osc_launch_requested registry flag set
3. Main loop detects flag, transitions GameState → LOADING
4. Explicit C++ call to WldUIProvider:StartLoadingDialog()
5. LoadDialog() creates Movie + text + forks tip thread
6. Main loop enters chunked reload mode:
   while (reload_state != DONE) {
       glfwPollEvents();      // prevent OS "Not Responding"
       advance_reload();      // execute one chunk
       pump_ui_frames();      // resume UI coroutines, fire OnBeat
       render_ui_only();      // render Movie + text (no sim required)
   }
7. WorldIsLoading() returns false → tip thread exits
8. Transition LOADING → GAME (without calling SetupUI — see Section 2)
9. Explicit C++ call to WldUIProvider:StopLoadingDialog()
10. First game frame renders with full 3D scene
```

---

## 2. Chunked Reload

### State Machine

```cpp
enum class ReloadStage : u8 {
    CLEAR_SCENE,       // GPU fence, clear_scene(), destroy old sim/Lua
    INIT_LUA,          // Create Lua VM, init sequence, rebind blueprints
    LOAD_SCENARIO,     // Create SimState, load scenario, register callbacks
    BOOT_SIM,          // boot_sim(), start_session() (AI setup)
    BUILD_SCENE,       // build_scene(), reset camera, transition to GAME
    DONE
};
```

### advance_reload()

```cpp
// Returns true if more stages remain
bool advance_reload(ReloadStage& stage, /* ...existing reload params... */) {
    switch (stage) {
    case ReloadStage::CLEAR_SCENE:
        // Steps 1-3: GPU fence, destroy old sim_state, destroy old Lua VM
        if (renderer) renderer->clear_scene();
        sim_state.reset();
        sim_lua_state.reset();
        stage = ReloadStage::INIT_LUA;
        return true;

    case ReloadStage::INIT_LUA:
        // Steps 4-6: Create Lua VM, run init, rebind + reload blueprints
        sim_lua_state = std::make_unique<LuaState>();
        loader.execute_init(*sim_lua_state, vfs);
        store.rebind(sim_lua_state->raw());
        loader.load_blueprints(*sim_lua_state, vfs, store);
        stage = ReloadStage::LOAD_SCENARIO;
        return true;

    case ReloadStage::LOAD_SCENARIO:
        // Steps 7-11: SimState, scenario, armies, callbacks, GSM pointer
        sim_state = std::make_unique<SimState>(...);
        // ... load scenario, add armies, register SimCallbacks
        stage = ReloadStage::BOOT_SIM;
        return true;

    case ReloadStage::BOOT_SIM:
        // Steps 12-13: boot_sim (moho+sim bindings, simInit.lua), start_session
        sim_loader.boot_sim(*sim_lua_state, vfs, *sim_state);
        session_manager.start_session(*sim_lua_state, ...);
        stage = ReloadStage::BUILD_SCENE;
        return true;

    case ReloadStage::BUILD_SCENE:
        // Steps 14-19: Renderer scene, camera, UI state
        if (renderer) renderer->build_scene(*sim_state, ...);
        // ... reset camera, update UI state, clear selection
        stage = ReloadStage::DONE;
        return false;

    case ReloadStage::DONE:
        return false;
    }
    return false;
}
```

### LOADING → GAME Transition (Double-SetupUI Fix)

**Problem:** `GameStateManager::transition_to()` calls `SetupUI()` on every transition. If called for both LOADING and GAME, the loading screen UI gets torn down prematurely.

**Solution:** The LOADING → GAME transition must NOT call `SetupUI()`. Instead:
1. Enter LOADING: call `transition_to(LOADING, uiL)` — fires `SetupUI()` which is needed for the loading screen
2. Run chunked reload
3. Enter GAME: call `transition_to(GAME, nullptr)` — passing nullptr for `ui_L` skips `SetupUI()`
4. Explicitly call `WldUIProvider:StopLoadingDialog()` from C++ to trigger FA's fade-out
5. Later, when the game UI needs setup, call `StartGameUI()` explicitly

This avoids the double-`SetupUI()` bug while keeping FA's loading screen lifecycle intact.

### Event Polling During Stages

Each `advance_reload()` call blocks for its stage duration (up to ~500ms for INIT_LUA). To prevent OS "Not Responding" marking, the chunked reload loop calls `glfwPollEvents()` before each stage. This processes window messages even if the stage blocks.

### render_ui_only()

New renderer method that draws **only** the UI layer using the swapchain render pass:

```cpp
void Renderer::render_ui_only(lua_State* L, ui::UIControlRegistry* ui_registry) {
    // 1. Acquire swapchain image, begin command buffer
    // 2. Begin render_pass_ (swapchain framebuffer, NOT scene_render_pass_)
    //    - Clear color: black (0, 0, 0, 1)
    //    - No depth attachment needed
    // 3. Bind ui_pipeline_
    // 4. UIRenderer::update() — walks control tree, builds 2D quads
    // 5. MovieControl::record_upload() — copy staging→VkImage in this cmd buf
    // 6. UIRenderer::render() — draw all 2D quads
    // 7. End render pass
    // 8. Submit command buffer, present swapchain image
}
```

**Critical render pass detail:** This method uses `render_pass_` (swapchain framebuffer), NOT `scene_render_pass_`. No `scene_color_image_`, no bloom passes, no bloom descriptor binds. The `ui_pipeline_` is already built against `render_pass_`, so it works directly. After `clear_scene()` destroys scene-specific resources, `render_ui_only()` remains safe because it only touches `render_pass_`, `ui_pipeline_`, and the UIRenderer — none of which are scene-dependent.

---

## 3. Video Playback

### SFD Container Format

`.sfd` files (CRI Sofdec) are a **proprietary multiplexed container** with interleaved video (MPEG-1) and audio (ADX) chunks. The MPEG-1 video data is NOT a contiguous byte range — it is split across CRI-proprietary chunk boundaries.

**Primary approach: Pre-converted MPEG-1 files.** The engine looks for `.mpg` files alongside the `.sfd` files in the VFS. For example, if FA's `LoadDialog()` requests `/movies/FMV_UEF_Load.sfd`, the engine first checks for `/movies/FMV_UEF_Load.mpg`. Pre-conversion can be done with standard tools (ffmpeg: `ffmpeg -i input.sfd -c:v mpeg1video -an output.mpg`).

**Secondary approach: SFD demuxer.** If no `.mpg` fallback exists, attempt to demux the `.sfd`:
1. Read CRI Sofdec header (magic `CRID` or `SFD\x00`)
2. Parse chunk table to locate video stream packets
3. Reassemble video packets into a contiguous MPEG-1 byte buffer
4. Pass to pl_mpeg

The demuxer is best-effort — if parsing fails at any point, fall back to solid dark background.

**Implementation:** `src/video/sfd_demuxer.hpp` — `SfdDemuxer` class:
```cpp
class SfdDemuxer {
public:
    bool open(const u8* data, size_t size);
    const u8* video_data() const;   // reassembled MPEG-1 stream
    size_t video_size() const;
};
```

### pl_mpeg Integration

- **Location:** `third_party/pl_mpeg.h` (single header, ~5000 lines, public domain)
- **Implementation file:** `src/video/video_decoder.cpp` (`#define PL_MPEG_IMPLEMENTATION`)
- **API wrapper:** `VideoDecoder` class:

```cpp
class VideoDecoder {
public:
    bool open(const u8* data, size_t size);  // MPEG-1 data
    bool decode_next_frame();                 // decode one frame
    const u8* rgb_data() const;              // current frame RGB888
    u32 width() const;
    u32 height() const;
    f64 framerate() const;
    void set_loop(bool loop);
    void rewind();
};
```

### Movie UIControl

New UI control type registered in the control factory:

**Lua API:**
```lua
local movie = Movie(parent, '/movies/FMV_UEF_Load.sfd')
movie:Loop(true)
movie:Play()
movie:Stop()
movie.Width:Set(GetFrame(0).Width())
movie.Height:Set(GetFrame(0).Height())
```

**C++ backing (`MovieControl`):**
- Inherits from `UIControl`
- Owns a `VideoDecoder` instance
- Owns a dynamic VkImage + VkImageView (resized on first frame decode)
- Owns a **persistent staging buffer** (pre-allocated for max frame dimensions)
- Each UI update tick:
  1. `decoder.decode_next_frame()` → RGB data in CPU memory
  2. `memcpy` RGB data to persistent staging buffer (already CPU-mapped)
  3. During `record_upload(VkCommandBuffer cmd)`: record `vkCmdCopyBufferToImage` + layout transition into the **frame command buffer** (not a separate one-shot submission)
  4. UIRenderer draws control as textured quad (like Bitmap, but dynamic texture)

**Per-frame upload pattern (GPU stall fix):** Unlike `TextureCache::upload_rgba()` which creates a one-shot command buffer with `vkQueueWaitIdle`, the MovieControl uses a **persistent CPU-mapped staging buffer** and records the copy into the main frame's command buffer. This avoids per-frame GPU synchronization stalls. The staging buffer is allocated once at video open time, sized for `width * height * 3` (RGB888), or `width * height * 4` if converted to RGBA.

**Fallback:** If video file not found or decode fails, MovieControl renders as a solid dark rectangle (same as a Bitmap with no texture). The text overlays still display correctly.

---

## 4. FA Lua Compatibility

### Already Implemented
- `WorldIsLoading()` — returns true when `GameState == LOADING`
- `UIUtil.CreateText(parent, text, size, font)` — text control creation
- `ForkThread(fn, ...)` / `WaitSeconds(n)` — UI coroutine system
- `GetFrame(0)` — root UI frame
- `LOC(key)` — localization
- `Random(min, max)` — random integers
- `LayoutHelpers` — pure Lua module, imports via VFS (manipulates Width/Height/Left/Top LazyVars)

### New for M168

**Movie control (Section 3 above):**
- `Movie(parent, path)` constructor
- `Movie:Play()`, `Movie:Loop(bool)`, `Movie:Stop()`

**GetCursor() global:**
FA's `StartLoadingDialog` (gamemain.lua:450) calls `GetCursor():Hide()` as its first statement, and `StopLoadingDialog` (line 492) calls `GetCursor():Show()`. Need:
```cpp
static int l_GetCursor(lua_State* L) {
    // Return a table with Hide() and Show() methods
    lua_newtable(L);
    lua_pushstring(L, "Hide");
    lua_pushcfunction(L, [](lua_State* L) -> int {
        // Set cursor visibility to false (glfwSetInputMode GLFW_CURSOR_HIDDEN)
        return 0;
    });
    lua_rawset(L, -3);
    lua_pushstring(L, "Show");
    lua_pushcfunction(L, [](lua_State* L) -> int {
        // Set cursor visibility to true (glfwSetInputMode GLFW_CURSOR_NORMAL)
        return 0;
    });
    lua_rawset(L, -3);
    return 1;
}
```

**Pulse effect helper:**
FA's `effecthelpers.lua:Pulse(control, duration, min_alpha, max_alpha)` forks a thread that tweens `control:SetAlpha()` between min and max in a sine wave. This is pure Lua — if FA's `effecthelpers.lua` imports cleanly, use it as-is. If not, a simplified fallback:

```lua
function Pulse(control, speed, minAlpha, maxAlpha)
    ForkThread(function()
        local t = 0
        while not control:IsDestroyed() do
            t = t + speed * 0.1
            local alpha = minAlpha + (maxAlpha - minAlpha) * (0.5 + 0.5 * math.sin(t))
            control:SetAlpha(alpha)
            WaitSeconds(0.1)
        end
    end)
end
```

**WldUIProvider wiring:**
FA's `gamemain.lua` creates a `WldUIProvider` object with `StartLoadingDialog` / `StopLoadingDialog` methods. Rather than relying on `SetupUI()` to trigger these automatically, the C++ loading flow calls them explicitly:

1. Before chunked reload: call `WldUIProvider:StartLoadingDialog()` from C++
2. After chunked reload: call `WldUIProvider:StopLoadingDialog()` from C++

This avoids coupling to the `SetupUI()` callback chain and prevents double-firing.

---

## 5. File Layout

```
third_party/
    pl_mpeg.h                    # Single-header MPEG-1 decoder (new)
src/video/
    sfd_demuxer.hpp              # CRI Sofdec demuxer (new)
    sfd_demuxer.cpp              # (new)
    video_decoder.hpp            # pl_mpeg wrapper (new)
    video_decoder.cpp            # PL_MPEG_IMPLEMENTATION here (new)
src/ui/
    movie_control.hpp            # Movie UIControl (new)
    movie_control.cpp            # (new)
src/renderer/
    renderer.hpp                 # Add render_ui_only() declaration
    renderer.cpp                 # Add render_ui_only() implementation
src/main.cpp                     # Chunked reload state machine
```

---

## 6. Known Gaps & Degraded Behavior

- **Audio in .sfd files** — ADX audio track decoding is deferred. Videos play silently.
- **Multiple faction videos** — FA selects faction-specific videos. M168 loads whichever file `LoadDialog()` requests. If the file isn't found, solid black fallback.
- **Progress percentage** — FA doesn't show a progress bar, just tips. We match that.
- **OnFrame dispatch for fade-out** — FA's `StopLoadingDialog` uses `SetNeedsFrameUpdate(true)` + `OnFrame` callback on a Bitmap for fade-out animation. The engine has `SetNeedsFrameUpdate` registered but may not dispatch `OnFrame` callbacks to controls each frame. If `OnFrame` dispatch is not implemented, the loading screen disappears instantly on GAME transition instead of fading. This is acceptable degraded behavior — the game is playable regardless. Adding `OnFrame` dispatch is a follow-up enhancement.
- **SFD demuxer reliability** — The CRI Sofdec format is proprietary and undocumented. The demuxer is best-effort. Pre-converted `.mpg` files are the reliable path.

---

## 7. Testing

- `--loading-test` flag: Triggers a reload sequence and verifies:
  - Movie control creates without crash
  - `WorldIsLoading()` returns true during load, false after
  - At least 3 UI frames rendered during chunked reload
  - GameState transitions FRONT_END → LOADING → GAME
- Unit test: `VideoDecoder` opens a test MPEG-1 file, decodes first frame, verifies dimensions
- Fallback test: Missing video path → MovieControl renders dark background, no crash
