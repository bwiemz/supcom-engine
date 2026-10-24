-- Guard-assist probe: a T1 engineer ordered to guard a T1 land factory that
-- builds Strikers, logged every tick from the order for 330 ticks: where it
-- stands, how far from the factory's skirt and from the unit being built, its
-- unit states, its focus, its and the factory's progress, and what the army
-- requests. Its start, stop, repair callbacks are logged as they run, on this
-- engineer alone. The economy is stalled as in `economy`, whose engineer
-- takes up the help and drops it again.
-- Two scenes, in turn, each cleaned up after:
-- - "far": economy's geometry, the engineer 18 south of the factory, out of
--   reach, so it walks up;
-- - "near": the engineer already within reach of the factory's skirt, west of
--   it and off its roll-off side.

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function spawn(bp, army, x, z, heading)
    return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, heading or 0, 0)
end

local STATES = {'Moving', 'Guarding', 'Building', 'Repairing', 'Reclaiming', 'Busy', 'GuardBusy',
                'AssistMoving', 'AssistingCommander', 'PathFinding'}

local function state(u)
    local s = nil
    for _, name in STATES do
        if u:IsUnitState(name) then s = s and (s .. '+' .. name) or name end
    end
    return s or 'idle'
end

-- The distance from a point to the rectangle the factory's skirt covers
local function skirt_gap(factory, p)
    local bp = factory:GetBlueprint()
    local physics, footprint = bp.Physics, bp.Footprint
    local fp = factory:GetPosition()
    local x0 = fp[1] - footprint.SizeX / 2 + (physics.SkirtOffsetX or 0)
    local z0 = fp[3] - footprint.SizeZ / 2 + (physics.SkirtOffsetZ or 0)
    local dx = math.max(x0 - p[1], 0, p[1] - (x0 + (physics.SkirtSizeX or footprint.SizeX)))
    local dz = math.max(z0 - p[3], 0, p[3] - (z0 + (physics.SkirtSizeZ or footprint.SizeZ)))
    return math.sqrt(dx * dx + dz * dz)
end

local function distance(a, b)
    local dx, dz = a[1] - b[1], a[3] - b[3]
    return math.sqrt(dx * dx + dz * dz)
end

function ProbeAssist(army, scene, fx, fz, ex, ez)
    local brain = GetArmyBrain(army)
    local seen, names, count = {}, {}, 0
    local function name_of(u)
        if not u then return 'none' end
        local id = u:GetEntityId()
        if not names[id] then
            count = count + 1
            names[id] = u:GetUnitId() .. '#' .. count
            table.insert(seen, u)
        end
        return names[id]
    end

    local factory = spawn('ueb0101', army, fx, fz)
    WaitTicks(5)
    IssueBuildFactory({factory}, 'uel0201', 2)
    local eng = spawn('uel0105', army, ex, ez)

    local t = 0
    local function watch(callback)
        local old = eng[callback]
        if type(old) == 'function' then
            eng[callback] = function(self, a, b, c)
                probe('assist %s event %s t=%d', scene, callback, t)
                return old(self, a, b, c)
            end
        end
    end
    for _, callback in {'OnStartBuild', 'OnStopBuild', 'OnStartRepair', 'OnStopRepair'} do
        watch(callback)
    end

    IssueGuard({eng}, factory)
    for step = 0, 330 do
        t = step
        local at = eng:GetPosition()
        local target = factory:GetFocusUnit()
        local focus = eng:GetFocusUnit()
        probe('assist %s t=%d at=%.2f,%.2f gap=%.2f ugap=%.2f state=%s focus=%s eprog=%.3f fprog=%.3f ftarget=%s req=%.2f/%.2f',
            scene, t, at[1], at[3], skirt_gap(factory, at), target and distance(at, target:GetPosition()) or -1,
            state(eng), name_of(focus), eng:GetWorkProgress(), factory:GetWorkProgress(), name_of(target),
            brain:GetEconomyRequested('ENERGY'), brain:GetEconomyRequested('MASS'))
        WaitTicks(1)
    end

    for _, u in seen do
        if not u:BeenDestroyed() then u:Destroy() end
    end
    eng:Destroy()
    factory:Destroy()
    WaitTicks(5)
end

function ProbeMain()
    WaitTicks(80) -- after the commanders' warp-in
    local armies = ListArmies()
    local me = armies[1]
    for _, other in armies do
        if other ~= me then
            SetAlliance(me, other, 'Neutral')
            SetAlliance(other, me, 'Neutral')
        end
    end
    local brain = GetArmyBrain(me)

    -- Stalled: the store emptied, and a Monkeylord's upkeep
    brain:TakeResource('ENERGY', brain:GetEconomyStored('ENERGY'))
    local monkeylord = spawn('url0402', me, 300, 720)
    WaitTicks(5)

    ProbeAssist(me, 'far', 300, 790, 306, 808)
    ProbeAssist(me, 'near', 340, 790, 332, 790)
    monkeylord:Destroy()
    probe('assist done')
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
