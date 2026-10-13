-- Probe: is the commander selected at the start? (retail's OnFirstUpdate
-- selects it once, 3 s in; the warp keeps it unselectable for 2.1 s.)
local probe_select_units = SelectUnits
function SelectUnits(units)
    local r = probe_select_units(units)
    local n = table.getn(GetSelectedUnits() or {})
    LOG(string.format('PROBE startup-select ui-tick=%d asked=%d selected=%d', GameTick(),
        table.getn(units or {}), n))
    return r
end

-- And the race's two clocks: the UI's tick (the session's beats) against the
-- wall clock (CurrentTime, which WaitSeconds keeps), from OnFirstUpdate on.
local probe_first_update = OnFirstUpdate
function OnFirstUpdate()
    local t0 = CurrentTime()
    LOG(string.format('PROBE startup-clock first-update ui-tick=%d', GameTick()))
    ForkThread(function()
        for i = 1, 8 do
            WaitSeconds(0.5)
            LOG(string.format('PROBE startup-clock t=%.2f ui-tick=%d', CurrentTime() - t0, GameTick()))
        end
    end)
    return probe_first_update()
end
