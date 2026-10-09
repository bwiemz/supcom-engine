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
    adjacency-test ai-test aim-test air-attack-run-test air-auto-engage-test air-staging-test air-turn-test anim-render-test attack-move-test anim-test arc-test area-test armor-test
    army-colors-test audio-test
    beam-render-test beam-test beam-weapon-test binding-tail-test bitmap-test blend-test bone-test border-render-test build-test
    camera-test
    camera-moves-test window-test options-test canpath-test capture-test colors-test carrier-land-test carrier-test chain-test change-army-test charge-test cmd-test collide-test crowd-test collision-test combat-test
    construction-test controls-test counter-intel-test decal-render-test cursor-render-test damage-test decal-test
    death-test decalsplat-test defence-test destroyed-handle-test drive-test deposit-test drag-render-test draw-test
    dualstate-test
    economy-test edit-render-test edit-test effect-intel-test emitter-test enhance-test
    script-order-test
    gameui-test impact-test influence-test issue-handles-test
    enhance-wreck-test factory-assist-test factory-rally-test ferry-test fire-test flags-test formation-test font-test fow-test guard-engage-test
    feedback-render-test focus-army-test footfall-test full-smoke-test ghost-render-test hull-facing-test
    input-test intel-field-test intel-overlay-test intel-test interp-test itemlist-render-test
    jammer-blip-test jammer-test keyboard-test keymap-test
    layercap-test lighting-test los-test map-parse-test wave-test material-test bloom-test build-shader-test effect-mesh-test skinning-test prop-material-test clipped-shadow-test shadow-map-test lowstub-test manip-test massstub-test massstub2-test missile-test
    massstub3-test massstub4-test medstub-test meshless-test move-test naval-depth-test need-unpack-test normal-test notify-upgrade-test
    onframe-test particle-render-test path-test phase2-test phase3-test placement-layers-test phase4-test phase5-test
    patrol-test persist-test platoon-test prebuilt-test profile-test projectile-test prop-test range-render-test range-test reclaim-test refract-render-test
    runtime-decal-test terrain-normal-render-test
    repair-test resource-icon-render-test right-click-test scale-test scissor-test scrollbar-render-test selection-render-test session-command-test shadow-test
    mesh-capacity-test shield-render-test shield-test silo-test sky-test smoke-test spatial-test strata-test
    specular-test stall-test stats-test steer-test strategic-icon-test stress-test stub-test targeting-test
    teamcolor-test
    terrain-normal-test terrain-tex-test text-test threat-test trail-render-test
    tiled-render-test toggle-test transport-drop-test transport-pickup-test transport-silo-test transport-slots-test transport-test ui-test
    uiboot-test uirender-test unit-cap-test unit-hooks-test unit-intel-test unitsound-test upgrade-test vet-adj-render-test
    vet-test vfx-render-test victory-test water-reflection-test water-render-test weapon-test wreck-test
    yaw-only-test
)

set(OSC_DATA_TESTS_RETAIL_GAP
)

# Front-end flows that must boot without --map.
set(OSC_DATA_TESTS_NO_MAP_GATE
    audio-data-test edit-text-test lan-screen-test lobby-flow-test movie-test
)
set(OSC_DATA_TESTS_NO_MAP_RETAIL_GAP
)
