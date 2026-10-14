-- Factory timing probe: a T1 land factory given three Strikers, logged every
-- tick through its first build's start and its first unit's roll-off, and a
-- T1 engineer told to build a power generator, logged every tick from the
-- order. Ample resources, so the economy plays no part.

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function spawn(bp, army, x, z, heading)
    return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, heading or 0, 0)
end

local function state(u)
    local s = ''
    for _, name in {'Building', 'Busy', 'Upgrading', 'Repairing', 'Moving'} do
        if u:IsUnitState(name) then s = s .. name:sub(1, 2) end
    end
    return s == '' and '-' or s
end

function ProbeFactoryTiming(army)
    local factory = spawn('ueb0101', army, 300, 790)
    WaitTicks(5)
    IssueBuildFactory({factory}, 'uel0201', 3)
    for t = 0, 160 do
        if t <= 12 or (t >= 130 and t <= 160) then
            local focus = factory:GetFocusUnit()
            probe('timing factory t=%d state=%s progress=%.4f consumed=%.3f focus=%s focus-frac=%.4f',
                t, state(factory), factory:GetWorkProgress(), factory:GetResourceConsumed() or -1,
                focus and focus:GetUnitId() or 'none', focus and focus:GetFractionComplete() or -1)
        end
        WaitTicks(1)
    end
end

function ProbeEngineerTiming(army)
    local eng = spawn('uel0105', army, 260, 790)
    WaitTicks(5)
    IssueBuildMobile({eng}, {262, GetTerrainHeight(262, 784), 784}, 'ueb1101', {})
    for t = 0, 40 do
        local focus = eng:GetFocusUnit()
        local p = eng:GetPosition()
        probe('timing engineer t=%d state=%s progress=%.4f consumed=%.3f focus=%s focus-frac=%.4f at=%.2f,%.2f',
            t, state(eng), eng:GetWorkProgress(), eng:GetResourceConsumed() or -1,
            focus and focus:GetUnitId() or 'none', focus and focus:GetFractionComplete() or -1, p[1], p[3])
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
    local brain = GetArmyBrain(me)
    brain:GiveStorage('MASS', 100000)
    brain:GiveResource('MASS', 100000)
    brain:GiveResource('ENERGY', 100000)
    WaitTicks(2)
    ForkThread(ProbeEngineerTiming, me)
    ProbeFactoryTiming(me)
    probe('timing done')
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
