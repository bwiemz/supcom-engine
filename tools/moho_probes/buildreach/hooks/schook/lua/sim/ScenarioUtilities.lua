-- Build reach probe: where a builder out of reach of its site stops, and
-- where it starts to build. An engineer 9 west of a point defence's site (the
-- engine's range test 9), 10 north of a power generator's, and 12 west of a
-- land factory's, each logged every tick until it builds and for a while
-- after.

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function spawn(bp, army, x, z)
    return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, 0, 0)
end

local function at(x, z)
    return {x, GetTerrainHeight(x, z), z}
end

local function case(army, name, ux, uz, bp, sx, sz)
    local u = spawn('uel0105', army, ux, uz)
    WaitTicks(2)
    IssueBuildMobile({u}, at(sx, sz), bp, {})
    local started = nil
    for t = 0, 300 do
        local p = u:GetPosition()
        local building = u:IsUnitState('Building')
        if building and not started then
            started = t
            probe('reach %s start t=%d at=%.3f,%.3f from-site=%.3f', name, t, p[1], p[3],
                math.sqrt((p[1] - sx) * (p[1] - sx) + (p[3] - sz) * (p[3] - sz)))
        end
        if started and t == started + 20 then
            probe('reach %s rest t=%d at=%.3f,%.3f from-site=%.3f', name, t, p[1], p[3],
                math.sqrt((p[1] - sx) * (p[1] - sx) + (p[3] - sz) * (p[3] - sz)))
            break
        end
        WaitTicks(1)
    end
    if not started then probe('reach %s never-built', name) end
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
    local brain = GetArmyBrain(me)
    brain:GiveStorage('MASS', 100000)
    brain:GiveResource('MASS', 100000)
    brain:GiveResource('ENERGY', 100000)
    ForkThread(case, me, 'pd', 600.5, 110.5, 'ueb2101', 609.5, 110.5)
    ForkThread(case, me, 'pgen', 640.5, 100.5, 'ueb1101', 640.5, 110.5)
    case(me, 'factory', 668.5, 140.5, 'ueb0101', 680.5, 140.5)
    WaitTicks(40)
    probe('reach done')
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
