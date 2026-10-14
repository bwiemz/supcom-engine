-- Engineer build-start probe: a T1 engineer told to build a power generator
-- behind it, logged every tick from the order with its heading, its build
-- turret's direction and its open animation, and each build callback as it
-- runs (OnPrepareArmToBuild, the open-animation wait, OnStartBuild,
-- OnStopBuilderTracking; BuildManipulatorSetEnabled too, which neither game
-- called), and where the frame appears.
-- Ample resources, so the economy plays no part.

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local order_tick = 0
local function tick()
    if type(GetGameTick) == 'function' then return GetGameTick() - order_tick end
    return -1
end

local function state(u)
    local s = ''
    for _, name in {'Building', 'Busy', 'Upgrading', 'Repairing', 'Moving'} do
        if u:IsUnitState(name) then s = s .. name:sub(1, 2) end
    end
    return s == '' and '-' or s
end

local function anim_fraction(u)
    local m = u.BuildingOpenAnimManip
    if m and type(m.GetAnimationFraction) == 'function' then return m:GetAnimationFraction() end
    return -1
end

local function turret(u)
    local x, y, z = u:GetBoneDirection('Turret')
    return x or 0, z or 0
end

-- Each callback, logged as it runs, on this engineer only
local function watch(eng, name)
    local old = eng[name]
    if type(old) ~= 'function' then
        probe('eng-build no %s', name)
        return
    end
    eng[name] = function(self, a, b, c)
        probe('eng-build event %s tick=%d', name, tick())
        local r = old(self, a, b, c)
        probe('eng-build event %s-done tick=%d', name, tick())
        return r
    end
end

local frame_logged = false

function ProbeEngineerStart(army)
    local eng = CreateUnitHPR('uel0105', army, 260, GetTerrainHeight(260, 790), 790, 0, 0, 0)
    WaitTicks(5)
    for _, name in {'OnPrepareArmToBuild', 'WaitForBuildAnimation', 'OnStartBuild',
                    'OnStopBuilderTracking', 'BuildManipulatorSetEnabled'} do
        watch(eng, name)
    end
    local tp = eng:GetPosition('Turret')
    local ep = eng:GetPosition()
    probe('eng-build engineer at=%.3f,%.3f,%.3f turret-bone at=%.3f,%.3f,%.3f', ep[1], ep[2], ep[3],
        tp[1], tp[2], tp[3])
    if type(GetGameTick) == 'function' then order_tick = GetGameTick() end
    IssueBuildMobile({eng}, {262, GetTerrainHeight(262, 784), 784}, 'ueb1101', {})
    for t = 0, 20 do
        local focus = eng:GetFocusUnit()
        local tx, tz = turret(eng)
        probe('eng-build t=%d tick=%d state=%s progress=%.4f consumed=%.3f focus=%s heading=%.4f turret=%.3f,%.3f anim=%.4f',
            t, tick(), state(eng), eng:GetWorkProgress(), eng:GetResourceConsumed() or -1,
            focus and focus:GetUnitId() or 'none', eng:GetHeading() or -99, tx, tz, anim_fraction(eng))
        if focus and not frame_logged then
            frame_logged = true
            local fp = focus:GetPosition()
            probe('eng-build frame at=%.3f,%.3f,%.3f', fp[1], fp[2], fp[3])
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
    local brain = GetArmyBrain(me)
    brain:GiveStorage('MASS', 100000)
    brain:GiveResource('MASS', 100000)
    brain:GiveResource('ENERGY', 100000)
    WaitTicks(2)
    ProbeEngineerStart(me)
    probe('eng-build done')
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
