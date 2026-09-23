# Membership of the data-backed integration modes (see CMakeLists.txt).
#
# Baseline 2026-09-22, retail FA 3599 (Steam) on Seton's Clutch, Linux:
# 44 gate / 55 retail-gap (+ lobby-flow-test). Moving a mode from
# retail-gap to gate is part of closing the gap it exercises (roadmap Phase B).
#
# Why retail-gap modes fail today (first failure per mode):
#   - Tests address entities by fixed id (#1 = ACU). Retail creates map props
#     and resource deposits first, so the ACU is #5299 -> "no entity 1".
#   - UI tests look for UI factories (InternalCreateBitmap, GetFrame, ...) in
#     the sim Lua state; seven *-render/input modes then crash in test code
#     on an unchecked lua_rawget.
#   - Retail-only engine API not yet bound (platoon:GetFactionIndex,
#     unit:GetHealth, aibrain:GetNoRushTicks, ...) kills AI threads.

set(OSC_DATA_TESTS_GATE
    audio-test beam-test blend-test build-test chain-test collision-test
    construction-test damage-test decal-test decalsplat-test deposit-test
    draw-test dualstate-test economy-test emitter-test enhance-wreck-test
    fire-test intel-overlay-test lowstub-test massstub4-test medstub-test
    move-test normal-test phase2-test phase3-test phase4-test phase5-test
    profile-test projectile-test prop-test reclaim-test scale-test
    shield-render-test smoke-test spatial-test specular-test stall-test
    stress-test teamcolor-test terrain-normal-test terrain-tex-test
    transport-silo-test vet-adj-render-test vfx-render-test
)

set(OSC_DATA_TESTS_RETAIL_GAP
    adjacency-test ai-test anim-render-test anim-test armor-test bitmap-test
    bone-test border-render-test canpath-test capture-test cmd-test combat-test
    controls-test cursor-render-test drag-render-test edit-render-test
    edit-test enhance-test flags-test font-test fow-test full-smoke-test
    input-test intel-test itemlist-render-test jammer-test layercap-test
    los-test manip-test massstub-test massstub2-test massstub3-test
    onframe-test path-test platoon-test repair-test scissor-test
    scrollbar-render-test shadow-test shield-test silo-test stats-test
    stub-test text-test threat-test tiled-render-test toggle-test
    transport-test ui-test uiboot-test uirender-test unitsound-test
    upgrade-test vet-test wreck-test
)

# Front-end flows that must boot without --map.
set(OSC_DATA_TESTS_NO_MAP_RETAIL_GAP
    lobby-flow-test
)
