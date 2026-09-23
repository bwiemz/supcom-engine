# Membership of the data-backed integration modes (see CMakeLists.txt).
#
# Retail FA 3599 (Steam) on Seton's Clutch, Linux. Moving a mode from
# retail-gap to gate is part of closing the gap it exercises (roadmap Phase B).
#   2026-09-22 baseline: 44 gate / 55 retail-gap (+ lobby-flow-test).
#   2026-09-23 (M184/M185): 66 gate / 33 retail-gap. Per-army ACU lookup,
#     the retail AI API batch, reachability-based CanPathTo and Moho build
#     placement let the AI, combat, platoon and smoke modes pass.
#
# Why the remaining retail-gap modes fail (first failure per mode):
#   - UI tests look for UI factories (InternalCreateBitmap, GetFrame, ...) in
#     the sim Lua state; seven *-render/input modes then crash in test code
#     on an unchecked lua_rawget.
#   - Retail-only engine API not yet bound, and units are not instances of
#     their blueprint script classes (roadmap M185), so unit-script behaviour
#     the sim tests expect is missing.

set(OSC_DATA_TESTS_GATE
    adjacency-test ai-test audio-test beam-test blend-test bone-test build-test
    canpath-test chain-test collision-test combat-test construction-test
    damage-test decal-test decalsplat-test deposit-test draw-test dualstate-test
    economy-test emitter-test enhance-wreck-test fire-test flags-test fow-test
    full-smoke-test intel-overlay-test intel-test los-test lowstub-test
    manip-test massstub3-test massstub4-test medstub-test move-test normal-test
    path-test phase2-test phase3-test phase4-test phase5-test platoon-test
    profile-test projectile-test prop-test reclaim-test scale-test shadow-test
    shield-render-test silo-test smoke-test spatial-test specular-test
    stall-test stats-test stress-test teamcolor-test terrain-normal-test
    terrain-tex-test threat-test toggle-test transport-silo-test transport-test
    vet-adj-render-test vet-test vfx-render-test wreck-test
)

set(OSC_DATA_TESTS_RETAIL_GAP
    anim-render-test anim-test armor-test bitmap-test border-render-test
    capture-test cmd-test controls-test cursor-render-test drag-render-test
    edit-render-test edit-test enhance-test font-test input-test
    itemlist-render-test jammer-test layercap-test massstub-test massstub2-test
    onframe-test repair-test scissor-test scrollbar-render-test shield-test
    stub-test text-test tiled-render-test ui-test uiboot-test uirender-test
    unitsound-test upgrade-test
)

# Front-end flows that must boot without --map.
set(OSC_DATA_TESTS_NO_MAP_RETAIL_GAP
    lobby-flow-test
)
