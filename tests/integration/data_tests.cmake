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
# Why the remaining retail-gap modes fail (first failure per mode):
#   - Build/target helpers: capture/repair/upgrade cannot find the structure
#     they just built; layercap/massstub/massstub2 find no enemy unit.
#   - Engine gaps: anim-test (animated bone matrices), unitsound-test, and the
#     armor/cmd/enhance/jammer/shield/stub modes (not yet triaged).

set(OSC_DATA_TESTS_GATE
    adjacency-test ai-test anim-render-test audio-test beam-test bitmap-test
    blend-test bone-test border-render-test build-test canpath-test chain-test
    collision-test combat-test construction-test controls-test
    cursor-render-test damage-test decal-test decalsplat-test deposit-test
    drag-render-test draw-test dualstate-test economy-test edit-render-test
    edit-test emitter-test enhance-wreck-test fire-test flags-test font-test
    fow-test full-smoke-test input-test intel-overlay-test intel-test
    itemlist-render-test los-test lowstub-test manip-test massstub3-test
    massstub4-test medstub-test move-test normal-test onframe-test path-test
    phase2-test phase3-test phase4-test phase5-test platoon-test profile-test
    projectile-test prop-test reclaim-test scale-test scissor-test
    scrollbar-render-test shadow-test shield-render-test silo-test smoke-test
    spatial-test specular-test stall-test stats-test stress-test teamcolor-test
    terrain-normal-test terrain-tex-test text-test threat-test tiled-render-test
    toggle-test transport-silo-test transport-test ui-test uiboot-test
    uirender-test vet-adj-render-test vet-test vfx-render-test wreck-test
)

set(OSC_DATA_TESTS_RETAIL_GAP
    anim-test armor-test capture-test cmd-test enhance-test jammer-test
    layercap-test massstub-test massstub2-test repair-test shield-test stub-test
    unitsound-test upgrade-test
)

# Front-end flows that must boot without --map.
set(OSC_DATA_TESTS_NO_MAP_GATE
    lobby-flow-test
)
set(OSC_DATA_TESTS_NO_MAP_RETAIL_GAP
)
