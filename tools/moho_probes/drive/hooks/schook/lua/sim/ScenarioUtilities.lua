-- Drive probe: one Striker driven 40 north, then 30 east (a quarter turn),
-- then 30 back west (a half turn), its position and orientation logged every
-- tick. Run the original with /synclog to have its motion ticks beside these.

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function drive(u, leg, x, z)
    IssueMove({u}, {x, GetTerrainHeight(x, z), z})
    for t = 0, 200 do
        local p = u:GetPosition()
        local q = u:GetOrientation()
        probe('drive %s t=%d at=%.3f,%.3f,%.3f q=%.4f,%.4f,%.4f,%.4f moving=%s', leg, t, p[1], p[2],
            p[3], q[1], q[2], q[3], q[4], tostring(u:IsUnitState('Moving')))
        if t > 2 and not u:IsUnitState('Moving') and table.getn(u:GetCommandQueue()) == 0 then
            return
        end
        WaitTicks(1)
    end
end

function ProbeMain()
    WaitTicks(80)
    local armies = ListArmies()
    local me = armies[1]
    for _, other in armies do
        if other ~= me then
            SetAlliance(me, other, 'Neutral')
            SetAlliance(other, me, 'Neutral')
        end
    end
    local u = CreateUnitHPR('uel0201', me, 300, GetTerrainHeight(300, 790), 790, 0, 0, 0)
    WaitTicks(5)
    probe('drive start tick=%d', GetGameTick())
    drive(u, 'north', 300, 750)
    drive(u, 'east', 330, 750)
    drive(u, 'west', 300, 750)
    probe('drive done')
end

local probe_started = false
local probe_create_initial_army_group = CreateInitialArmyGroup
function CreateInitialArmyGroup(strArmy, createCommander)
    if not probe_started then
        probe_started = true
        ForkThread(ProbeMain)
    end
    return probe_create_initial_army_group(strArmy, createCommander)
end
