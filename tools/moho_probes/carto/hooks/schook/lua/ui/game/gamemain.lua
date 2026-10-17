-- Probe: the cartographic view (the design's P8). Past the opening's
-- camera, the main view is reset to the whole map (CameraReset) and a frame
-- dumped; then the main view is made cartographic (retail's
-- ToggleMainCartographicView) and a frame dumped again. The original game
-- writes them with its dump_Frame console command into PROBE_DUMP_DIR
-- (SCFrame_<time>_<n>.bmp); the engine, which has no dump_Frame, is
-- captured by its --screenshot once the log says it's done (frame 1500).
-- The original's dump_Frame writes into this folder, which it doesn't make:
-- make it first (mkdir /tmp/carto_dump), or edit the path. /tmp is shared
-- with Steam's runtime, so Z:\tmp reaches it.
local PROBE_DUMP_DIR = 'Z:\\tmp\\carto_dump'

local probe_first_update = OnFirstUpdate
function OnFirstUpdate()
    local r = probe_first_update()
    ForkThread(function()
        WaitSeconds(6)
        GetCamera('WorldCamera'):Reset()
        WaitSeconds(3)
        LOG(string.format('PROBE carto world ui-tick=%d version=%s', GameTick(), GetVersion()))
        ConExecute('dump_Frame ' .. PROBE_DUMP_DIR)
        WaitSeconds(1)
        import('/lua/ui/game/worldview.lua').ToggleMainCartographicView()
        WaitSeconds(3)
        local wv = import('/lua/ui/game/worldview.lua')
        LOG(string.format('PROBE carto map ui-tick=%d cartographic=%s', GameTick(),
            tostring(wv.viewLeft and wv.viewLeft:IsCartographic())))
        ConExecute('dump_Frame ' .. PROBE_DUMP_DIR)
        WaitSeconds(1)
        LOG('PROBE carto done')
    end)
    return r
end
