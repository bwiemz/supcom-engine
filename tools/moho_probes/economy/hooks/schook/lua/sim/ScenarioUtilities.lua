-- Economy probe: a T1 land factory building three Strikers with a T1
-- engineer guarding it, three times in turn on Seton's Clutch:
-- - "alone": the army's starting economy, nothing else drawing on it;
-- - "stalled": energy drained, and a Monkeylord's upkeep (250 a second)
--   starving every consumer;
-- - "ample": storage and stock enough for everything.
-- Each logs the factory's progress, the engineer's focus and the army's
-- energy and mass (stored / income / requested) at fixed ticks.

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function spawn(bp, army, x, z, heading)
    return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, heading or 0, 0)
end

local function economy(brain)
    return string.format('energy=%.1f/%.2f/%.2f mass=%.1f/%.2f/%.2f',
        brain:GetEconomyStored('ENERGY'), brain:GetEconomyIncome('ENERGY'),
        brain:GetEconomyRequested('ENERGY'), brain:GetEconomyStored('MASS'),
        brain:GetEconomyIncome('MASS'), brain:GetEconomyRequested('MASS'))
end

function ProbeFactory(army, label, x, z)
    local brain = GetArmyBrain(army)
    local factory = spawn('ueb0101', army, x, z)
    WaitTicks(5)
    IssueBuildFactory({factory}, 'uel0201', 3)
    local eng = spawn('uel0105', army, x + 6, z + 18)
    IssueGuard({eng}, factory)
    local waited = 0
    for _, at in {30, 60, 100, 150, 200, 300} do
        WaitTicks(at - waited)
        waited = at
        local efocus = eng:GetFocusUnit()
        probe('econ %s tick=%d progress=%.3f engineer-focus=%s factory-use=%.2f %s', label, at,
            factory:GetWorkProgress(), efocus and efocus:GetUnitId() or 'none',
            factory:GetConsumptionPerSecondEnergy() or 0, economy(brain))
    end
    factory:Destroy()
    eng:Destroy()
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
    probe('econ start %s', economy(brain))
    ProbeFactory(me, 'alone', 300, 790)

    -- Stalled: the store emptied, and a Monkeylord's upkeep
    brain:TakeResource('ENERGY', brain:GetEconomyStored('ENERGY'))
    local monkeylord = spawn('url0402', me, 300, 720)
    WaitTicks(5)
    probe('econ stall-start monkeylord-use=%.2f %s',
        monkeylord:GetConsumptionPerSecondEnergy() or 0, economy(brain))
    ProbeFactory(me, 'stalled', 260, 790)
    monkeylord:Destroy()

    brain:GiveStorage('ENERGY', 100000)
    brain:GiveStorage('MASS', 100000)
    brain:GiveResource('ENERGY', 100000)
    brain:GiveResource('MASS', 100000)
    WaitTicks(5)
    probe('econ ample-start %s', economy(brain))
    ProbeFactory(me, 'ample', 340, 790)
    probe('econ done')
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
