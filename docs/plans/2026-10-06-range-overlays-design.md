# Range overlays (roadmap item 7)

FA's range rings: weapon, intel, counter-intel, shield and build ranges drawn
on the ground for the player's units. The engine drew intel rings only, for
selected units, as screen-space lines; `SetOverlayFilter` was a stub, so the
profiles retail's UI registers (`/lua/ui/game/rangeoverlayparams.lua`, via
`multifunction.lua`) were dropped.

Source: faf-re `RangeRenderer.cpp`, `RangeExtractor.cpp` and the extractors,
`UserUnit::FindWeaponBy/GetIntelRanges/GetMaxCounterIntel`,
`cfunc_SetOverlayFilterL`, `CameraImpl::CacheCameraFrustumUnits`,
`WRenViewport::Render`; retail `range.fx` and `frame.fx`.

## What Moho does

**Profiles.** `SetOverlayFilter(name, categories, normal, selected, rollover,
inner0, inner1, outer0, outer1)` stores one profile by name (a sorted map):
its category set, three colours (`SCR_DecodeColor`, AARRGGBB with the alpha
the glow) and two thickness pairs, {zoomed in, zoomed out}. The name picks
the extractor; retail registers 12: AllMilitary, DirectFire, IndirectFire,
AntiAir, AntiNavy, Defense, Miscellaneous, AllIntel, Radar, Sonar, Omni,
CounterIntel. `SetOverlayFilters(names)` sets the session's active filters,
which each frame become the visible profiles, in the order given.

**Extractors**, each `Extract(unit)` (a live unit) and `Range(blueprint,
centre)` (build preview), giving `{x, z, inner, outer}`:

- weapon (Direct/Indirect/AntiAir/AntiNavy): over the blueprint's weapons of
  that `RangeCategory`, the largest runtime MaxRadius and the smallest
  MinRadius (`Range`: EffectiveRadius if positive, else MaxRadius).
- AllMilitary: an OVERLAYMISC unit's AI radius, else every weapon's. No
  `Range`.
- Defense: half the shield's size, else Countermeasure weapons.
- Miscellaneous: `AI.StagingPlatformScanRadius`, else `AI.GuardScanRadius`.
- Radar / Sonar / Omni / AllIntel (largest of the three): the unit's intel
  radii, whether on or off, unless the intel toggle (script bit 3) is off.
- CounterIntel: the largest stealth/cloak field and jam/spoof radius, unless
  jamming (bit 2) or stealth (bit 5) is toggled off.

**Passes** (`RangeRenderer::Render`, after the terrain and its decals, before
the meshes; gated on `ren_Ranges`):

1. `range_RenderBuild`: while placing a structure, each profile but the two
   combined ones whose categories hold the blueprint, at the cursor, in its
   normal colour.
2. Each visible profile: the focus army's units in view (alive, not being
   built), in its categories, in its normal colour, one batch.
3. `range_RenderSelected`: each profile but the combined ones, the focus
   army's selected units, in its selected colour.
4. `range_RenderHighlighted`: the hovered unit, if the focus army's, each
   profile, in its rollover colour.
5. The focus army's no-rush zone while its timer runs: grey 0.2, thickness
   {0.1, 1} inside and {1, 2} outside.

Retail's UI turns 1, 3 and 4 on from the player's prefs (default on);
`range_Fill` is off.

**A batch** (`RenderRingBatch`) draws each ring twice as a stencil volume, an
annular cylinder from 5 below the map's lowest point to its highest
(range.fx `Cast`: z-fail, front faces increment, back faces decrement, bits
0-6, only where bit 7 is clear):

- the fill, `[inner + ti, outer - to]` (inner 0 stays 0), then `RangeMask`
  marks bit 7 wherever the count is non-zero;
- the edges, `[inner, inner + ti]` and `[outer - to, outer]`, counted only
  outside every fill; `RangeFill` (if on) tints the fills white at 1/8,
  `RangeBurn` writes the colour, glow in alpha, where an edge counted; the
  stencil is cleared.

So rings of one batch merge: an edge inside another ring's fill is not drawn.
The thickness `t = (far * coeff * span - near) * zoom / max_zoom + near`,
with `coeff` the `range_Inner/OuterThicknessCoeff` convar (1/1024) and `span`
the playable rect's larger side.

## The engine

- `renderer/range_overlays.{hpp,cpp}` (no Vulkan): profiles, filters and
  convars; the extractors over a `RangeUnit`/`RangeBlueprint`; the passes as
  a list of batches; the ring bands and thicknesses; the ring volume's
  geometry. Unit-tested.
- `renderer/range_renderer.{hpp,cpp}`: the stencil passes. The depth buffer
  gains a stencil (D32S8, else D24S8; without one, no rings).
- The snapshot carries what the extractors read off a live unit: weapon
  ranges by blueprint index, intel radii on or off, script bits; each army's
  start and the no-rush radius.
- `SetOverlayFilter`/`SetOverlayFilters` in the UI state, the `range_*` and
  `ren_Ranges` convars. With FA's UI the old screen-space intel rings go;
  the C++ HUD keeps them.

One difference: Moho refreshes its in-view unit list every half second; the
engine tests each frame.
