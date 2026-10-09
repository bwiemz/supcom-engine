# M215c: FA's strategic icons

The engine drew strategic icons as procedural shapes (a circle, a square,
a triangle by category) in the army's colour, and only past a fixed 250
of zoom. FA draws each unit's own icon, from retail's
`/textures/ui/common/game/strategicicons/` (1249 files), by rules Moho's
`CWldSession::RenderStrategicIcons` and `RenderUnitIcon` keep (faf-re), and
shades them with `primbatcher.fx`'s `StrategicIconPS`. Retail's files are
read at run time, never copied.

## The rules

- **Which texture.** A blueprint's `StrategicIconName` names four textures
  under the icon directory (`_rest`, `_over`, `_selected`, `_selectedover`;
  an absolute name is its own base). A selected unit draws `_selected`,
  anything else `_rest`. A blip not seen since it was detected draws one of
  `/lua/ui/game/strategicIcons.lua`'s `GenericIcons`: `Structure` for an
  immobile blueprint, else `Land`, `Naval` or `Air`. Moho loads a
  blueprint's icons with the blueprint.
- **How it's shaded.** Point-sampled at the texture's own size, centred on
  the unit's floored screen position. Where a texel is near mid-grey
  (`|rgb − 0.5|² < 0.25`), it takes the tint; everything else (the outline,
  the selected ring) keeps its own colour; alpha is the texture's. The tint
  is the army's colour, or GameColors' `UnidentifiedColor` for a
  never-seen blip.
- **When.** A unit drawn as itself shows its icon once the camera is out
  past its mesh's `IconFadeInZoom` (130 for most of retail's; Moho's default
  0; capped at 0.89 of the farthest zoom). A blip, having no mesh, shows its
  icon at any zoom. A unit being built has none.
- **Order.** Ground icons, then air (`Air.CanFly`), then high-priority
  (`StrategicIconSortPriority`, a byte, under 65), then the selected, so a
  selected unit is on top.
- **Badges.** A stunned unit's `StunnedIcons.StunnedRest` over its icon.

Moho compares the fade-in distance with the camera target's depth. faf-re's
recovered comparison in `RenderStrategicIcons` reads inverted; its economy
readout pass states the rule plainly (`depth >= IconFadeInZoom`: "past the
mesh's icon-fade-in depth the unit is a strategic icon"), and so does FA on
screen. So a mesh without `IconFadeInZoom` (Moho's default 0) is an icon at
any zoom; the render tests' plate meshes say 130, as retail's do.

## The engine

- `StrategicIconRenderer` draws the textures: a per-blueprint cache of the
  icon fields (from `__blueprints`), the generic and stunned icons from
  strategicIcons.lua (retail's paths if the import fails), a draw group per
  texture, and `preload` for the world's blueprints when the scene is built.
  The procedural atlas stays for the C++ HUD's selection panel.
- The UI fragment shader takes a negative alpha as "StrategicIconPS".
- `GPUTexture` knows its size.
- The render-state dump lists icons in draw order, each with its texture.
- 250 of zoom still hides the meshes.

## Tests

`--strategic-icon-test` (gate), SCMP_009, ARMY_1's units (senses off
but a radar) and, on that radar, ARMY_2's:

1. The tank, air scout and support commander show their blueprints'
   `_rest` icons, at the textures' sizes, in ARMY_1's colour.
2. The selected tank shows `_selected`.
3. A power generator being built shows none.
4. Draw order: ground, air, high-priority, selected.
5. A stunned tank's badge right after its icon.
6. Inside the tank's icon, some pixels are exactly its army's colour (the
   grey texels), some not (the outline): FA's shading, not a multiply.
7. Never-seen blips: the generic land, structure and air icons, in
   `UnidentifiedColor`.
8. A tank seen once, on radar: its own icon, in ARMY_2's colour.
9. Zoomed in inside `IconFadeInZoom`: the tank has no icon; the blip keeps
   its.

`--unit-intel-test` now looks from 110, inside the fade-in, where only
blips have icons.

## Left for later

- The `_over` textures (the hovered unit: the engine doesn't track it yet)
  and the paused badge.
- Blinking when a friendly unit is hit; the underlay texture; carriers'
  and transported units' icons; projectile icons; formation ghosts.
- Icons on the minimap (it still draws dots).
  *Done (2026-10-09): the minimap is a world view of its own, and Moho's
  CUIWorldView::Render draws its icons through the same
  RenderStrategicIcons, with its camera fully zoomed out: every unit's icon
  at its texture's size, in UI points, under the same intel and selection.
  `--strategic-icon-test` Tests 12-15.*
- Meshes by their LODs' cutoffs rather than the engine's 250.
