-- Crowd-gap probe: test_moho_pathing's "crowd of twenty through one gap"
-- with Strikers. A wall two cells thick down x = CX..CX+2, open at
-- z = CZ..CZ+6; twenty Strikers 5 x 4 west of it, each sent to its own
-- place east of it. Which arrive, which stop short, where.
local CX, CZ = 640, 110

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
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
    local army = ArmyBrains[1]:GetArmyIndex()
    for z = -40, 45 do
        if z < 0 or z >= 6 then
            for x = 0, 1 do
                local wx, wz = CX + x + 0.5, CZ + z + 0.5
                CreateUnitHPR('ueb5101', army, wx, GetTerrainHeight(wx, wz), wz, 0, 0, 0)
            end
        end
    end
    local tanks, goals = {}, {}
    for i = 0, 19 do
        local col, row = math.mod(i, 5), math.floor(i / 5)
        local x, z = CX - 29.5 + col * 3, CZ - 9.5 + row * 6
        local u = CreateUnitHPR('uel0201', army, x, GetTerrainHeight(x, z), z, 0, math.pi / 2, 0)
        table.insert(tanks, u)
        goals[i + 1] = {CX + 30.5 + col * 3, CZ - 9.5 + row * 6}
    end
    WaitTicks(3)
    probe('crowdgap start tick=%d', GetGameTick())
    for i, u in tanks do
        local g = goals[i]
        IssueMove({u}, {g[1], GetTerrainHeight(g[1], g[2]), g[2]})
    end
    for t = 0, 900 do
        if math.mod(t, 50) == 0 or t == 900 then
            local arrived, idle_short = 0, 0
            for i, u in tanks do
                local p = u:GetPosition()
                local q = table.getn(u:GetCommandQueue())
                if q == 0 and p[1] > CX + 25 then arrived = arrived + 1 end
                if q == 0 and p[1] <= CX + 25 then idle_short = idle_short + 1 end
            end
            probe('crowdgap t=%d arrived=%d stopped_short=%d', t, arrived, idle_short)
        end
        WaitTicks(1)
    end
    for i, u in tanks do
        local p = u:GetPosition()
        probe('crowdgap end %d at=%.2f,%.2f queue=%d', i, p[1] - CX, p[3] - CZ,
            table.getn(u:GetCommandQueue()))
    end
    probe('crowdgap done')
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
