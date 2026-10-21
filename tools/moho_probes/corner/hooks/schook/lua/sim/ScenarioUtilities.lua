-- Corner probe: a Striker at the end of a wall two cells thick, inside its
-- width, ordered to a goal beyond it whose straight line clips the wall's
-- end: does it slide out, stop, or rock in place?
local CX, CZ = 640, 110

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function scene(name, ux, uz, gx, gz)
    local walls = {}
    -- the wall: cells x CX..CX+2, z CZ..CZ+16
    for z = 0, 15 do
        for x = 0, 1 do
            local wx, wz = CX + x + 0.5, CZ + z + 0.5
            table.insert(walls, CreateUnitHPR('ueb5101', ArmyBrains[1]:GetArmyIndex(), wx,
                GetTerrainHeight(wx, wz), wz, 0, 0, 0))
        end
    end
    local x, z = CX + ux, CZ + uz
    local u = CreateUnitHPR('uel0201', ArmyBrains[1]:GetArmyIndex(), x, GetTerrainHeight(x, z), z, 0,
        math.atan2(gx - ux, gz - uz), 0)
    WaitTicks(3)
    probe('corner %s start tick=%d id=%d', name, GetGameTick(), u:GetEntityId())
    IssueMove({u}, {CX + gx, GetTerrainHeight(CX + gx, CZ + gz), CZ + gz})
    for t = 0, 250 do
        local p = u:GetPosition()
        probe('corner %s t=%d at=%.4f,%.4f moving=%s queue=%d', name, t, p[1] - CX, p[3] - CZ,
            tostring(u:IsUnitState('Moving')), table.getn(u:GetCommandQueue()))
        if t > 5 and table.getn(u:GetCommandQueue()) == 0 then break end
        WaitTicks(1)
    end
    u:Destroy()
    for _, w in walls do w:Destroy() end
    WaitTicks(5)
end

function ProbeMain()
    WaitTicks(80)
    local armies = ListArmies()
    for _, other in armies do
        if other ~= armies[1] then
            SetAlliance(armies[1], other, 'Neutral')
            SetAlliance(other, armies[1], 'Neutral')
        end
    end
    -- the engine's stuck tank, relative to the wall's end: (1.77, -0.002) to (30.5, 2.5)
    scene('a', 1.77, -0.002, 30.5, 2.5)
    scene('b', 1.77, -0.3, 30.5, 2.5)
    scene('c', 1.5, -0.4, 30.5, 6.5)
    scene('d', 1.0, -0.5, 30.5, 2.5)
    probe('corner done')
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
