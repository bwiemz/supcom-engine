-- The sim's probes (see README.md). Once the commanders have warped in, the
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

-- Support commanders (REBUILDER), on dry ground at x 270-330, z 790. One
-- guards a T1 power generator that is then destroyed: is it built again where it stood (faf-re's
-- LookForStructureRebuilder)? faf-re's guard task has a REBUILDER not bind
-- a live target, so: does one guarding a building factory help it, and does
-- one guarding a tank follow it?
function ProbeRebuild(army)
    local pgen = spawn('ueb1101', army, 270, 790)
    local scu = spawn('uel0301', army, 270, 802)
    -- A control: a T1 engineer, no REBUILDER, guarding another
    local pgen2 = spawn('ueb1101', army, 250, 790)
    local eng = spawn('uel0105', army, 250, 798)
    WaitTicks(10)
    IssueGuard({scu}, pgen)
    IssueGuard({eng}, pgen2)
    WaitTicks(60)
    local p = pgen:GetPosition()
    local s = scu:GetPosition()
    probe('rebuild guarding scu-at=%.1f,%.1f pgen-at=%.1f,%.1f queue=%d engineer-queue=%d', s[1], s[3],
        p[1], p[3], table.getn(scu:GetCommandQueue() or {}), table.getn(eng:GetCommandQueue() or {}))
    pgen:Kill()
    pgen2:Kill()
    local waited = 0
    for _, at in {30, 150, 400, 800} do
        WaitTicks(at - waited)
        waited = at
        local found, fraction = 0, -1
        for _, u in GetUnitsInRect(Rect(p[1] - 1, p[3] - 1, p[1] + 1, p[3] + 1)) or {} do
            if not u:IsDead() and u:GetUnitId() == 'ueb1101' then
                found = found + 1
                fraction = u:GetFractionComplete()
            end
        end
        s = scu:GetPosition()
        probe('rebuild after-kill tick=%d pgens=%d fraction=%.2f scu-at=%.1f,%.1f building=%s queue=%d engineer-queue=%d',
            at, found, fraction, s[1], s[3], tostring(scu:IsUnitState('Building')),
            table.getn(scu:GetCommandQueue() or {}), table.getn(eng:GetCommandQueue() or {}))
    end
end

function ProbeScuAssist(army)
    local factory = spawn('ueb0101', army, 300, 790)
    WaitTicks(5)
    IssueBuildFactory({factory}, 'uel0201', 3)
    local scu = spawn('uel0301', army, 300, 808)
    IssueGuard({scu}, factory)
    -- A control: a T1 engineer guarding the same factory
    local eng = spawn('uel0105', army, 306, 808)
    IssueGuard({eng}, factory)
    local waited = 0
    for _, at in {60, 150, 300} do
        WaitTicks(at - waited)
        waited = at
        local focus = scu:GetFocusUnit()
        local s = scu:GetPosition()
        local efocus = eng:GetFocusUnit()
        probe('scu-assist tick=%d building=%s guarding=%s focus=%s engineer-focus=%s factory-progress=%.2f scu-at=%.1f,%.1f',
            at, tostring(scu:IsUnitState('Building')), tostring(scu:IsUnitState('Guarding')),
            focus and focus:GetUnitId() or 'none', efocus and efocus:GetUnitId() or 'none',
            factory:GetWorkProgress(), s[1], s[3])
    end
end

function ProbeScuFollow(army)
    local tank = spawn('uel0201', army, 330, 790)
    local scu = spawn('uel0301', army, 324, 790)
    IssueGuard({scu}, tank)
    WaitTicks(10)
    IssueMove({tank}, {330, GetTerrainHeight(330, 860), 860})
    local waited = 0
    for _, at in {100, 250} do
        WaitTicks(at - waited)
        waited = at
        local t, s = tank:GetPosition(), scu:GetPosition()
        probe('scu-follow tick=%d tank-at=%.1f,%.1f scu-at=%.1f,%.1f apart=%.1f', at, t[1], t[3], s[1],
            s[3], VDist2(t[1], t[3], s[1], s[3]))
    end
end

-- Tree breakup: 12 Strikers driven north through a forest (x 128-192,
-- z 640-704); its tree groups and single trees counted before and after.
function ProbeForest(army)
    local function count()
        local groups, singles = 0, 0
        for _, e in GetReclaimablesInRect(Rect(128, 640, 192, 704)) or {} do
            if not IsUnit(e) then
                local id = e:GetBlueprint().BlueprintId or ''
                if string.find(id, 'group') then groups = groups + 1
                elseif string.find(id, 'trees') then singles = singles + 1 end
            end
        end
        return groups, singles
    end
    local g, t = count()
    probe('forest tick=0 groups=%d singles=%d', g, t)
    local tanks = {}
    for i = 0, 11 do
        local x = 132 + 5 * i
        local u = spawn('uel0201', army, x, 628)
        IssueMove({u}, {x, GetTerrainHeight(x, 716), 716})
        table.insert(tanks, u)
    end
    local waited = 0
    for _, at in {100, 200, 400} do
        WaitTicks(at - waited)
        waited = at
        g, t = count()
        local north = 0
        for _, u in tanks do
            if not u:IsDead() and u:GetPosition()[3] > 704 then north = north + 1 end
        end
        probe('forest tick=%d groups=%d singles=%d tanks-through=%d', at, g, t, north)
    end
end

-- GetUnitsInRect's order: three Strikers made in one cell (x 700-704,
-- z 120-124), then the first warped away and back. Moho lists a cell's units
-- the last linked first.
function ProbeRectOrder(army)
    local made = {}
    for i, at in {{702, 122}, {702.3, 122.2}, {701.8, 121.8}} do
        local u = spawn('uel0201', army, at[1], at[2])
        u.ProbeIndex = i
        made[i] = u
    end
    local function order(label)
        local t = {}
        for _, u in GetUnitsInRect(Rect(700, 120, 704, 124)) or {} do
            table.insert(t, tostring(u.ProbeIndex or 'other'))
        end
        probe('rect-order %s [%s]', label, table.concat(t, ' '))
    end
    order('made')
    WaitTicks(3)
    local home = made[1]:GetPosition()
    Warp(made[1], {760, GetTerrainHeight(760, 122), 122})
    WaitTicks(3)
    order('away')
    Warp(made[1], home)
    WaitTicks(3)
    order('back')
end

local function extents(e)
    local x = e:GetCollisionExtents()
    if not x then return 'none' end
    return string.format('%.3f %.3f %.3f %.3f', x.Min.x, x.Min.z, x.Max.x, x.Max.z)
end

-- Map props' orientation and boxes: the tree groups in Rect(116, 650, 132,
-- 690), read before the forest drive reaches them.
function ProbePropOrient()
    for _, e in GetReclaimablesInRect(Rect(116, 650, 132, 690)) or {} do
        if not IsUnit(e) and string.find(e:GetBlueprint().BlueprintId or '', 'group') then
            local p, q = e:GetPosition(), e:GetOrientation()
            probe('prop-orient %.3f %.3f q=%.4f %.4f %.4f %.4f box=%s', p[1], p[3], q[1], q[2], q[3], q[4],
                extents(e))
        end
    end
end

-- Units' boxes: a Seraphim artillery (xsb2302, CollisionOffsetZ -1.5) at
-- four headings, its box offset turned with it.
function ProbeUnitBox(army)
    for i, h in {0, math.pi / 2, math.pi, 1} do
        local u = spawn('xsb2302', army, 580 + 20 * i, 300, h)
        local o = u:GetOrientation()
        probe('unit-box %d q=%.4f %.4f %.4f %.4f box=%s', i, o[1], o[2], o[3], o[4], extents(u))
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
    ProbePropOrient()
    ForkThread(ProbeForest, me)
    ForkThread(ProbeRectOrder, me)
    ForkThread(ProbeUnitBox, me)
    ForkThread(ProbeRebuild, me)
    ForkThread(ProbeScuAssist, me)
    ForkThread(ProbeScuFollow, me)
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
