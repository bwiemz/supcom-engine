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
