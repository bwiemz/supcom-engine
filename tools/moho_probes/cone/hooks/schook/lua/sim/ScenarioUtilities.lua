-- Cone probe: pairs of units meeting at several angles, to read from
-- /synclog which of Moho's collision resolutions each meeting takes.
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

-- a: {bp, army, x, z, tx, tz}; b likewise. Headings face the goals.
local function scene(name, a, b, ticks)
    local function mk(s)
        return spawn(s[1], s[2], s[3], s[4], math.atan2(s[5] - s[3], s[6] - s[4]))
    end
    local ua, ub = mk(a), mk(b)
    probe('cone %s start tick=%d a=%d b=%d', name, GetGameTick(), ua:GetEntityId(), ub:GetEntityId())
    WaitTicks(2)
    move(ua, a[5], a[6])
    move(ub, b[5], b[6])
    -- Each tick, both units' places (copied: GetPosition hands back one table, updated in place).
    for t = 1, ticks do
        WaitTicks(1)
        local pa, pb = ua:GetPosition(), ub:GetPosition()
        probe('cone %s t=%d %.3f %.3f %.3f %.3f', name, t, pa[1], pa[3], pb[1], pb[3])
    end
    probe('cone %s end tick=%d', name, GetGameTick())
    ua:Destroy()
    ub:Destroy()
    WaitTicks(5)
end

function ProbeMain()
    WaitTicks(80)
    local armies = ListArmies()
    local me, them = armies[1], armies[2]
    for _, other in armies do
        if other ~= me then
            SetAlliance(me, other, 'Neutral')
            SetAlliance(other, me, 'Neutral')
        end
    end
    SetAlliance(them, me, 'Neutral')
    -- overtaking: a Striker behind an engineer, one army, then two
    scene('overtake1', {'uel0201', me, 0, -20, 0, 30}, {'uel0105', me, 0.3, -12, 0.3, 30}, 150)
    scene('overtake2', {'uel0201', me, 0, -20, 0, 30}, {'uel0105', them, 0.3, -12, 0.3, 30}, 150)
    -- crossing at right angles, two armies, then one
    scene('cross90a', {'uel0201', me, -20, 0, 20, 0}, {'uel0201', them, 0, -20, 0, 20}, 150)
    scene('cross90b', {'uel0201', me, -20, 0, 20, 0}, {'uel0201', me, 0, -20, 0, 20}, 150)
    -- the other coming in obliquely from ahead (60) and from behind (120)
    scene('cross60', {'uel0201', me, -20, 0, 20, 0}, {'uel0201', them, 17, -10, -17, 10}, 150)
    scene('cross120', {'uel0201', me, -20, 0, 20, 0}, {'uel0201', them, -10, -17, 10, 17}, 150)
    -- head-on: two Strikers of two armies, and engineer vs Striker
    scene('headon1', {'uel0201', me, -20, 0, 20, 0}, {'uel0201', them, 20, 0.5, -20, 0.5}, 150)
    scene('headon2', {'uel0105', me, -20, 0, 20, 0}, {'uel0201', them, 20, 0.5, -20, 0.5}, 150)
    probe('cone done')
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
