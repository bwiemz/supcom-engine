-- Stall-start probe: a T1 land factory given one Striker, logged every tick
-- from the order for 40 ticks, in four economies in turn on Seton's Clutch:
-- - "ml": energy drained and a Monkeylord's upkeep (250 a second, energy
--   only) already starving the army when the order comes;
-- - "midbuild": the same upkeep, the store refilled to 300 at the order, so
--   the stall begins while the factory builds;
-- - "dual": energy drained and two other factories already building, so
--   only requests wanting both resources share the stall;
-- - "solo": energy ample and mass drained, the factory the only consumer.
-- Each row logs the factory's work progress, its unit's fraction, its
-- GetResourceConsumed and consumption, the other consumer's
-- GetResourceConsumed, and the army's energy and mass (stored / income /
-- requested / usage).

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function spawn(bp, army, x, z, heading)
    return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, heading or 0, 0)
end

local function resource(brain, kind)
    return string.format('%.2f/%.6f/%.6f/%.6f', brain:GetEconomyStored(kind), brain:GetEconomyIncome(kind),
        brain:GetEconomyRequested(kind), brain:GetEconomyUsage(kind))
end

local function drain(brain, kind)
    brain:TakeResource(kind, brain:GetEconomyStored(kind))
end

local function remove(factory)
    local unit = factory:GetFocusUnit()
    if unit then unit:Destroy() end
    factory:Destroy()
end

function ProbeStart(army, scene, x, z, other, at_order)
    local brain = GetArmyBrain(army)
    local factory = spawn('ueb0101', army, x, z)
    WaitTicks(5)
    if at_order then at_order(brain) end
    IssueBuildFactory({factory}, 'uel0201', 1)
    for t = 0, 40 do
        local unit = factory:GetFocusUnit()
        probe('stall %s t=%d prog=%.6f frac=%.6f eff=%.6f use=%.4f/%.4f other=%.6f energy=%s mass=%s', scene, t,
            factory:GetWorkProgress(), unit and unit:GetFractionComplete() or -1, factory:GetResourceConsumed(),
            factory:GetConsumptionPerSecondEnergy(), factory:GetConsumptionPerSecondMass(),
            other and other:GetResourceConsumed() or -1, resource(brain, 'ENERGY'), resource(brain, 'MASS'))
        WaitTicks(1)
    end
    remove(factory)
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

    drain(brain, 'ENERGY')
    local monkeylord = spawn('url0402', me, 300, 720)
    WaitTicks(5)
    ProbeStart(me, 'ml', 300, 790, monkeylord)
    ProbeStart(me, 'midbuild', 260, 790, monkeylord, function(b)
        drain(b, 'ENERGY')
        b:GiveResource('ENERGY', 300)
    end)
    monkeylord:Destroy()

    local helpers = {spawn('ueb0101', me, 300, 790), spawn('ueb0101', me, 260, 790)}
    WaitTicks(5)
    IssueBuildFactory(helpers, 'uel0201', 2)
    WaitTicks(20)
    ProbeStart(me, 'dual', 340, 790, helpers[1], function(b) drain(b, 'ENERGY') end)
    for _, helper in helpers do remove(helper) end

    brain:GiveResource('ENERGY', 4000)
    ProbeStart(me, 'solo', 300, 790, nil, function(b) drain(b, 'MASS') end)
    probe('stall done')
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
