-- Build-bot probe: the Cybran build bot (ura0001, BuildRate 0) helping
-- builds and repairs, logged every tick from the order: the builder's state
-- and progress, the work's fraction and health, and the first bot's state,
-- focus and progress. The bots' build, repair and effect callbacks are
-- logged as they run, with the order a start names. Only the scene's own
-- bots are watched: the other armies' Cybran builders spawn bots too.
-- Ample resources, so the economy plays no part. Eight scenes, in turn, each
-- cleaned up after:
-- - "cybran": a Cybran T1 engineer builds a power generator; its scripts
--   spawn the bot and order it to guard the frame (SpawnBuildBots);
-- - "frame": a UEF T1 engineer builds a power generator; a bot the probe
--   made is ordered to guard the frame once it stands;
-- - "builder": as "frame", the bot ordered to guard the engineer;
-- - "control": as "builder", a UEF T1 engineer in the bot's place;
-- - "repair": a bot ordered to repair a power generator at half health;
-- - "reclaim": a bot ordered to reclaim a power generator;
-- - "guarded": a bot ordered to guard a power generator at half health;
-- - "silo": a bot ordered to guard a tactical missile launcher building a
--   missile.

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function spawn(bp, army, x, z, heading)
    return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, heading or 0, 0)
end

local STATES = {'Moving', 'Guarding', 'Building', 'Repairing', 'Reclaiming', 'Busy', 'GuardBusy',
                'AssistMoving', 'Attached'}

local function state(u)
    if not u or u:BeenDestroyed() then return 'gone' end
    local s = nil
    for _, name in STATES do
        if u:IsUnitState(name) then s = s and (s .. '+' .. name) or name end
    end
    return s or 'idle'
end

local CALLBACKS = {'OnStartBuild', 'OnStopBuild', 'OnFailedToBuild', 'OnStartRepair', 'OnStopRepair',
                   'OnStartReclaim', 'OnStopReclaim', 'CreateBuildEffects', 'StopBuildingEffects'}

local scene, t, bots, builder_of_scene = '', 0, {}, nil

-- Each callback, logged as it runs, on the bots alone
local function watch(bot)
    table.insert(bots, bot)
    local index = table.getn(bots)
    for _, callback in CALLBACKS do
        local name = callback
        local old = bot[name]
        if type(old) == 'function' then
            bot[name] = function(self, a, b, c)
                local order = (name == 'OnStartBuild' or name == 'CreateBuildEffects') and tostring(b) or '-'
                probe('bots %s event %s t=%d bot=%d order=%s', scene, name, t, index, order)
                return old(self, a, b, c)
            end
        end
    end
end

local EffectUtil = import('/lua/EffectUtilities.lua')
local spawn_build_bots = EffectUtil.SpawnBuildBots
EffectUtil.SpawnBuildBots = function(builder, unitBeingBuilt, numBots, bag)
    local spawned = spawn_build_bots(builder, unitBeingBuilt, numBots, bag)
    if builder == builder_of_scene then
        for _, bot in spawned do watch(bot) end
    end
    return spawned
end

local function alive()
    local n = 0
    for _, bot in bots do
        if not bot:BeenDestroyed() then n = n + 1 end
    end
    return n
end

local function log_step(builder, work)
    local bot = bots[1]
    local live = bot and not bot:BeenDestroyed()
    local focus = live and bot:GetFocusUnit()
    local at = live and bot:GetPosition() or {0, 0, 0}
    local whole = work and not work:BeenDestroyed()
    probe('bots %s t=%d estate=%s eprog=%.3f frac=%.3f hp=%.1f bots=%d state=%s focus=%s bprog=%.3f at=%.2f,%.2f',
        scene, t, builder and state(builder) or 'none', builder and not builder:BeenDestroyed() and builder:GetWorkProgress() or 0,
        whole and work:GetFractionComplete() or -1, whole and work:GetHealth() or -1, alive(),
        bot and state(bot) or 'none', focus and focus:GetUnitId() or 'none',
        live and bot:GetWorkProgress() or 0, at[1], at[3])
end

local function clean(units)
    for _, u in units do
        if u and not u:BeenDestroyed() then u:Destroy() end
    end
    for _, bot in bots do
        if not bot:BeenDestroyed() then bot:Destroy() end
    end
    bots = {}
    builder_of_scene = nil
    WaitTicks(5)
end

-- An engineer builds a power generator south-east of it; `helper(engineer, frame)`
-- orders the help (nil: the scripts' own bots), with the frame once it stands.
local function ProbeBuild(army, name, engineer_bp, generator_bp, x, z, steps, helper)
    scene = name
    local eng = spawn(engineer_bp, army, x, z)
    builder_of_scene = eng
    WaitTicks(5)
    local bot = nil
    if helper then
        bot = spawn(name == 'control' and 'uel0105' or 'ura0001', army, x - 4, z)
        watch(bot)
        WaitTicks(5)
    end
    IssueBuildMobile({eng}, {x + 2, GetTerrainHeight(x + 2, z + 6), z + 6}, generator_bp, {})
    local frame, ordered = nil, false
    for step = 0, steps do
        t = step
        frame = frame or eng:GetFocusUnit()
        if helper and not ordered and (frame or helper == 'builder') then
            ordered = true
            probe('bots %s event Ordered t=%d bot=1 order=%s', scene, t, helper)
            if helper == 'frame' then IssueGuard({bot}, frame) else IssueGuard({bot}, eng) end
        end
        log_step(eng, frame)
        WaitTicks(1)
    end
    clean({eng, frame})
end

-- A bot repairs (`repair`), reclaims or guards (`guarded`) a standing power
-- generator, or guards a missile launcher building a missile (`silo`)
local function ProbeStanding(army, name, x, z, steps)
    scene = name
    local work = spawn(name == 'silo' and 'ueb2108' or 'ueb1101', army, x + 2, z + 6)
    WaitTicks(5)
    if name == 'repair' or name == 'guarded' then work:SetHealth(work, work:GetMaxHealth() / 2) end
    if name == 'silo' then IssueSiloBuildTactical({work}) end
    local bot = spawn('ura0001', army, x - 4, z)
    watch(bot)
    WaitTicks(5)
    if name == 'repair' then
        IssueRepair({bot}, work)
    elseif name == 'reclaim' then
        IssueReclaim({bot}, work)
    else
        IssueGuard({bot}, work)
    end
    for step = 0, steps do
        t = step
        log_step(name == 'silo' and work or nil, work)
        WaitTicks(1)
    end
    clean({work})
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
    brain:GiveStorage('MASS', 100000)
    brain:GiveStorage('ENERGY', 100000)
    brain:GiveResource('MASS', 100000)
    brain:GiveResource('ENERGY', 100000)
    WaitTicks(2)

    ProbeBuild(me, 'cybran', 'url0105', 'urb1101', 300, 790, 360, nil)
    ProbeBuild(me, 'frame', 'uel0105', 'ueb1101', 300, 790, 360, 'frame')
    ProbeBuild(me, 'builder', 'uel0105', 'ueb1101', 300, 790, 360, 'builder')
    ProbeBuild(me, 'control', 'uel0105', 'ueb1101', 300, 790, 360, 'builder')
    ProbeStanding(me, 'repair', 300, 790, 120)
    ProbeStanding(me, 'reclaim', 300, 790, 120)
    ProbeStanding(me, 'guarded', 300, 790, 120)
    ProbeStanding(me, 'silo', 300, 790, 120)
    probe('bots done')
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
