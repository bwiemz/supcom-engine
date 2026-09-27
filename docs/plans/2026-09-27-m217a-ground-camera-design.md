# M217a: the camera looks at the ground, and clicks land on it

Two parts of the view assumed the world lay at height 0.
- **The camera orbited a point at height 0.** SCMP_009's ground at the
  first army's start is 18.7 up. From the nearest zoom (30) the eye sat 4
  over the grass, and the start was out of view below it. On higher maps
  the camera went into the hills.
- **Clicks met the plane y = 0.** Every pick in `InputHandler` (select,
  drag-select, orders, command-mode clicks, the build ghost) intersected
  the cursor's ray with y = 0. On high ground that lies well beyond the
  ground the cursor is over: on SCMP_009, a click landed about 15 units
  short of it, toward the far side of the screen. Only the UI's
  `GetMouseWorldPos` refined its answer against the terrain.

## Moho

`CameraImpl` (faf-re) keeps the camera's focus (`mTargetLocation`, then
`mOffset`) on the ground: its height is the heightfield's elevation at the
focus, or the water's if that is higher. The eye sits back from the focus
along the view's axis. The minimap outline, the audio listener and the
particle billboards all derive from that view.

## The engine

- **The camera has a focus height** (`target_y`). The renderer sets it to
  the ground under the target (the heightmap, or the water's surface over
  it) after each poll of the camera's input, so this frame's clicks see a
  pan's new ground, and again when it draws, after any script's move. The renderer keeps a copy of the scene's heightmap, as
  the sim's terrain is replaced on a reload. The eye, the view, the shadow
  map's box, the particles' billboards, the audio listener and the minimap
  outline all use it.
- **`Camera::pick_ground`** follows the cursor's ray to where it first
  meets the ground: `Heightmap::intersect` (Moho's
  `CHeightField::Intersection`), or the water's surface where the ray meets
  that first. Off the map it meets the level of the focus.
- **`InputHandler::world_at`** resolves every click and drag through it,
  and so does `WorldView::GetMouseWorldPos`. The camera's Lua
  `GetFocusPosition` reports the surface height too (it had the seabed's
  over water).

## Tests

- **Unit** (`[camera]`, `[shadow]`):
  - The focus lifts the eye by its height, and is the screen's middle.
  - A pixel picks the ground point it shows, where the plane y = 0 lies
    more than 10 away.
  - A plateau above the focus is picked on it.
  - Over water, the surface is picked (with the focus neither the
    surface's level nor the ground's).
  - Off the map, the focus's level is picked.
  - The shadow map centres on a raised focus.
- **`--camera-test`** (gate), on SCMP_009:
  1. The focus sits on the ground at the ACU (18.68).
  2. A click on the pixel of a ground point lands on it (0.00 off).
  3. From the nearest zoom, the start is in view (18 meshes drawn).
  4. Over the sea, the focus is on the water's surface.
  5. Moved over the sea and polled, the focus is there before a frame is
     drawn.
- `--meshless-test` looks from the nearest zoom again.
- **`--lighting-test`** now draws flat ground of its own. Once the camera
  looked at the ground, its view of SCMP_009 took in slopes, trees and
  their shadows, and no stretch of that map is flat and clear enough.
  On its own ground it matches FA's formula to 0.002, and its checks
  tighten from 0.05 to 0.02.
