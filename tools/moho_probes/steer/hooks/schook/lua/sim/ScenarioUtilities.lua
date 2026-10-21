-- Steer probe: the engine's steer test 3 asked of the original. A Pillar and
-- an engineer of one army drive head-on, half a cell apart sideways, about
-- (640, 110) on SCMP_009: 24 each side of it (the engine's Moho-pathing case),
-- then 40 (its grid case). Both are logged every tick: who stops, who strays.

local CX, CZ = 640, 110

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function spawn(bp, army, dx, dz, heading)
    local x, z = CX + dx, CZ + dz
    return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, heading, 0)
end

local function move(u, dx, dz)
    local x, z = CX + dx, CZ + dz
    IssueMove({u}, {x, GetTerrainHeight(x, z), z})
end

local function headon(army, case, d)
    local eng = spawn('uel0105', army, d, 0.5, -math.pi / 2)
    local big = spawn('uel0202', army, -d, 0, math.pi / 2)
    WaitTicks(2)
    move(big, d, 0)
    move(eng, -d, 0.5)
    for t = 0, 600 do
        local b, e = big:GetPosition(), eng:GetPosition()
        probe('steer %s t=%d big=%.3f,%.3f eng=%.3f,%.3f moving=%s,%s', case, t, b[1] - CX,
            b[3] - CZ, e[1] - CX, e[3] - CZ, tostring(big:IsUnitState('Moving')),
            tostring(eng:IsUnitState('Moving')))
        if t > 5 and table.getn(big:GetCommandQueue()) == 0 and
            table.getn(eng:GetCommandQueue()) == 0 then
            break
        end
        WaitTicks(1)
    end
    big:Destroy()
    eng:Destroy()
    WaitTicks(5)
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
    headon(me, 'near', 24)
    headon(me, 'far', 40)
    probe('steer done')
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
