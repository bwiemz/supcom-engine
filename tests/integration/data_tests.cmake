# Membership of the data-backed integration modes (see CMakeLists.txt).
#
# Retail FA 3599 (Steam) on Seton's Clutch, Linux. Moving a mode from
# retail-gap to gate is part of closing the gap it exercises (roadmap Phase B).
#   2026-09-22 baseline: 44 gate / 55 retail-gap (+ lobby-flow-test).
#   2026-09-23 (M184/M185): 66 gate / 33 retail-gap. Per-army ACU lookup,
#     the retail AI API batch, reachability-based CanPathTo and Moho build
#     placement let the AI, combat, platoon and smoke modes pass.
#   2026-09-23 (M186): 85 gate + lobby-flow-test / 14 retail-gap. UI modes run
#     against the UI Lua state, which boots through retail userInit.lua; the
#     front end reaches a hosted skirmish lobby.
#
#   Then 95 gate: the sim modes stopped assuming FAF entity ids and FAF
#     data (shield.Army / ShieldType are FAF script fields), teleports and
#     economy events follow Moho, and unit:CanBuild reads category names
#     rather than the category table's numeric keys.
#   Then 99 gate / 0 retail-gap: the engine reads blueprints from the store,
#     not self.Blueprint (enhancements, unit sounds); GiveStorage persists;
#     finished or paused animations hold their pose; EnableIntel only
#     enables intel the unit has (retail SetupIntel had cloaked every unit).
#   M187: gameui-test -- the engine drives retail's in-game UI
#     (uimain.StartGameUI, the provider's CreateGameInterface ->
#     gamemain.CreateUI) and the sim -> UI sync channel.
#   M188: audio-data-test -- every retail cue resolves through the XACT
#     data (sound banks, wave banks by internal name, global settings).
#   M189: victory-test -- retail's victory.lua decides a game end to end.
#   M190: interp-test -- the world is drawn between sim ticks (offscreen
#     renderer, four frames per tick: a walking ACU moves every frame).

set(OSC_DATA_TESTS_GATE
    adjacency-test ai-test anim-render-test anim-test armor-test audio-test
    beam-test bitmap-test blend-test bone-test border-render-test build-test
    canpath-test capture-test chain-test cmd-test collision-test combat-test
    construction-test controls-test cursor-render-test damage-test decal-test
    decalsplat-test deposit-test drag-render-test draw-test dualstate-test
    economy-test edit-render-test edit-test emitter-test enhance-test
    gameui-test
    enhance-wreck-test fire-test flags-test font-test fow-test full-smoke-test
    input-test intel-overlay-test intel-test interp-test itemlist-render-test
    jammer-test
    layercap-test los-test lowstub-test manip-test massstub-test massstub2-test
    massstub3-test massstub4-test medstub-test move-test normal-test
    onframe-test path-test phase2-test phase3-test phase4-test phase5-test
    platoon-test profile-test projectile-test prop-test reclaim-test
    repair-test scale-test scissor-test scrollbar-render-test shadow-test
    shield-render-test shield-test silo-test smoke-test spatial-test
    specular-test stall-test stats-test stress-test stub-test teamcolor-test
    terrain-normal-test terrain-tex-test text-test threat-test
    tiled-render-test toggle-test transport-silo-test transport-test ui-test
    uiboot-test uirender-test unitsound-test upgrade-test vet-adj-render-test
    vet-test vfx-render-test victory-test wreck-test
)

set(OSC_DATA_TESTS_RETAIL_GAP
)

# Front-end flows that must boot without --map.
set(OSC_DATA_TESTS_NO_MAP_GATE
    audio-data-test lobby-flow-test
)
set(OSC_DATA_TESTS_NO_MAP_RETAIL_GAP
)
