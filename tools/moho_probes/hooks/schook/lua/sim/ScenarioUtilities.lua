-- The sim's probes (see README.md). Once the commanders have warped in, four
-- scenes play at once in separate corners of Seton's Clutch (SCMP_009), each
-- logging `PROBE <scene> ...` lines. Written for Lua 5.0, as Moho runs it.

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function spawn(bp, army, x, z, heading)
    return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, heading or 0, 0)
end

-- How far GetUnitsAroundPoint reaches: a T1 power generator (2x2) at
-- (120, 920), asked for from points about its centre. Retail's
-- EngineerTryRepair asks with a radius of 1 before each build. Each point is
-- asked of GetNumUnitsAroundPoint too.
function ProbeUnitsAroundPoint(army)
    local brain = GetArmyBrain(army)
    local pgen = spawn('ueb1101', army, 120, 920)
    WaitTicks(2)
    local p = pgen:GetPosition()
    local function ask(r, dx, dy, dz)
        local at = {p[1] + dx, p[2] + (dy or 0), p[3] + (dz or 0)}
        local found = brain:GetUnitsAroundPoint(categories.ueb1101, at, r, 'Ally') or {}
        local num = brain:GetNumUnitsAroundPoint(categories.ueb1101, at, r, 'Ally')
        probe('units-around-point radius=%g offset=(%g,%g,%g) found=%d num=%s', r, dx, dy or 0,
            dz or 0, table.getn(found), tostring(num))
    end
    -- Along x, by whole units
    for d = 0, 8 do ask(1, d) end
    ask(0, 0)
    ask(0, 3)
    -- The edge: strictly within, or within
    for _, d in {0.5, 0.9, 0.99, 1.0, 1.01} do ask(1, d) end
    for _, d in {1.9, 1.99, 2.0, 2.01} do ask(2, d) end
    for _, d in {4.9, 4.99, 5.0, 5.01, 5.1} do ask(5, d) end
    -- Height: 0.6 off in x and 0.9 up is 1.08 away in 3D, 0.6 across the map
    ask(1, 0.6, 0.5)
    ask(1, 0.6, 0.9)
    -- A circle or a square: 0.75 along both x and z is 1.06 away
    ask(1, 0.6, 0, 0.6)
    ask(1, 0.75, 0, 0.75)
end

-- The strait at z 520: banks too steep, by the engine's reading of Moho's
-- MobileCheck, for a hover's or an amphibious unit's footprint, which then
-- drive some 500 units round by land. A hover, an amphibious and a land tank
-- are sent across it.
function ProbeShore(army)
    local units = {}
    for _, row in {{'hover', 'uel0203', 516}, {'amphibious', 'url0203', 520}, {'land', 'uel0201', 524}} do
        local u = spawn(row[2], army, 160, row[3], math.pi / 2)
        IssueMove({u}, {392, GetTerrainHeight(392, row[3]), row[3]})
        units[row[1]] = {unit = u, goal_z = row[3], layers = {}}
    end
    for tick = 1, 1400 do
        WaitTicks(1)
        for name, s in units do
            if not s.unit:IsDead() then
                s.layers[s.unit:GetCurrentLayer()] = true
                local p = s.unit:GetPosition()
                if not s.arrived and VDist2(p[1], p[3], 392, s.goal_z) < 3 then s.arrived = tick end
            end
        end
    end
    for name, s in units do
        local layers = ''
        for layer in s.layers do layers = layers .. layer .. ' ' end
        probe('shore %s arrived=%s layers=%s', name, tostring(s.arrived), layers)
    end
end

-- A wall of 1x1 segments along z 175 from x 650 to 790, a gap 4 wide at
-- x 718-722; 24 Strikers sent through it to one point, and a Monkeylord
-- (Amphibious3x3, which ignores structures) straight at the wall.
function ProbeChoke(army)
    for x = 650, 789 do
        if x < 718 or x >= 722 then
            CreateUnitHPR('ueb5101', army, x + 0.5, GetTerrainHeight(x + 0.5, 175.5), 175.5, 0, 0, 0)
        end
    end
    local crowd = {}
    for i = 0, 7 do
        for j = 0, 2 do
            table.insert(crowd, spawn('uel0201', army, 706 + 4 * i, 150 + 4 * j))
        end
    end
    IssueMove(crowd, {720, GetTerrainHeight(720, 200), 200})
    local big = spawn('url0402', army, 680, 138)
    IssueMove({big}, {680, GetTerrainHeight(680, 205), 205})
    local crossed, last_z = {}, {}
    local function track(key, u)
        if u:IsDead() then return end
        local p = u:GetPosition()
        if last_z[key] and last_z[key] < 175 and p[3] >= 175 and not crossed[key] then crossed[key] = p[1] end
        last_z[key] = p[3]
    end
    for tick = 1, 1500 do
        WaitTicks(1)
        for i, u in crowd do track(i, u) end
        track('big', big)
    end
    local through, round, clipped, stranded = 0, 0, 0, 0
    for i in crowd do
        local x = crossed[i]
        if not x then stranded = stranded + 1
        elseif x >= 717.75 and x <= 722.25 then through = through + 1
        elseif x < 650 or x > 790 then round = round + 1
        else clipped = clipped + 1 end
    end
    probe('choke strikers through=%d round=%d through-wall-cells=%d stopped-short=%d', through, round, clipped, stranded)
    probe('choke monkeylord crossed-at-x=%s', tostring(crossed.big))
end

-- A FormAttack and a plain attack on a point: six Strikers each. Moho's
-- FormAttack sends each unit to its slot about the target; a plain attack
-- sends each straight at it.
function ProbeFormAttack(army)
    local formed, plain = {}, {}
    for i = 0, 5 do
        table.insert(formed, spawn('uel0201', army, 100 + 3 * i, 780))
        table.insert(plain, spawn('uel0201', army, 200 + 3 * i, 780))
    end
    IssueFormAttack(formed, {108, GetTerrainHeight(108, 840), 840}, 'AttackFormation', 0)
    IssueAttack(plain, {208, GetTerrainHeight(208, 840), 840})
    local waited = 0
    for _, at in {60, 120, 200} do
        WaitTicks(at - waited)
        waited = at
        for name, group in {formed = formed, plain = plain} do
            local xs, zs = '', ''
            local lo, hi = 1e9, -1e9
            for _, u in group do
                local p = u:GetPosition()
                xs = xs .. string.format('%.0f ', p[1])
                zs = zs .. string.format('%.0f ', p[3])
                lo, hi = math.min(lo, p[1]), math.max(hi, p[1])
            end
            probe('formattack %s tick=%d spread-x=%.1f x=[%s] z=[%s]', name, at, hi - lo, xs, zs)
        end
    end
end

function ProbeMain()
    WaitTicks(80) -- after the commanders' warp-in
    local armies = ListArmies()
    local me = armies[1]
    for _, other in armies do
        -- Both ways, whichever way SetAlliance sets it
        if other ~= me then
            SetAlliance(me, other, 'Neutral')
            SetAlliance(other, me, 'Neutral')
        end
    end
    probe('start armies=%d', table.getn(armies))
    ForkThread(ProbeUnitsAroundPoint, me)
    ForkThread(ProbeShore, me)
    ForkThread(ProbeChoke, me)
    ForkThread(ProbeFormAttack, me)
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
