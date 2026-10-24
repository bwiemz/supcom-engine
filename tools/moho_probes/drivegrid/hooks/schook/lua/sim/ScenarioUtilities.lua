-- Drive grid probe: every ground unit's drive, the same legs for each. The
-- land units (ids ???l####, not air, able to move) go in batches, each on a
-- lane of its own on the flat ground at x 584-704, z 70-120 of Seton's Clutch:
--   1. made side-on to the lane, driven 40 down it (the turn at the start);
--   2. back 30 up it (a half turn);
--   3. to a point 3 behind it (backing up, within BackUpDistance);
--   4. 6 on (a short hop, below top speed).
-- Each tick it logs every unit's position, heading and Moving state; once
-- each, the blueprint fields its drive reads. drive_diff.py compares two logs.
-- Positions and orientations print with 9 significant digits, enough to give a
-- float back bit for bit: two logs' lines agree exactly when the floats do.

local X0, X1 = 584, 704 -- the lanes' span: each rises under 0.3 along its length
local ZS = 117 -- where each starts
local BATCH = 24
local LEG_TICKS = 300 -- a leg given up on after this many ticks
local REST_TICKS = 3 -- a leg ends once the unit is still this many ticks

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function yaw(u)
    local q = u:GetOrientation() -- x, y, z, w
    return math.deg(math.atan2(2 * (q[4] * q[2] + q[1] * q[3]), 1 - 2 * (q[2] * q[2] + q[1] * q[1])))
end

local function num(v)
    return type(v) == 'number' and v or -1
end

local function ground_units()
    local ids = {}
    for id, bp in __blueprints do
        local phys = type(id) == 'string' and bp.BlueprintId == id and bp.Physics
        if phys and string.find(id, '^%a%a[lL]%d%d%d%d$') and num(phys.MaxSpeed) > 0 and
            phys.MotionType ~= 'RULEUMT_Air' and phys.MotionType ~= 'RULEUMT_None' then
            table.insert(ids, id)
        end
    end
    table.sort(ids)
    return ids
end

local function traits(id)
    local bp = __blueprints[id]
    local p, f = bp.Physics, bp.Footprint or {}
    probe('dgbp %s mt=%s spd=%.3f acc=%.3f brk=%.3f rev=%.3f turn=%.3f rad=%.3f ros=%s rost=%.3f bud=%.3f ' ..
          'sx=%.3f sz=%.3f fx=%.3f fz=%.3f', id, tostring(p.MotionType), num(p.MaxSpeed),
        num(p.MaxAcceleration), num(p.MaxBrake), num(p.MaxSpeedReverse), num(p.TurnRate),
        num(p.TurnRadius), tostring(p.RotateOnSpot), num(p.RotateOnSpotThreshold),
        num(p.BackUpDistance), num(bp.SizeX), num(bp.SizeZ), num(f.SizeX), num(f.SizeZ))
end

local function width(id)
    local bp = __blueprints[id]
    local f = bp.Footprint or {}
    return math.ceil(math.max(4, num(bp.SizeX), num(bp.SizeZ), num(f.SizeX), num(f.SizeZ))) + 3
end

-- One unit's legs, each logged every tick until it ends.
local function drive(b, id, u, x, done)
    -- down the lane (-z), back up it (+z), 3 behind (-z, facing +z), 6 on (+z)
    local legs = {{1, ZS - 40}, {2, ZS - 10}, {3, ZS - 13}, {4, ZS - 7}}
    for _, leg in legs do
        local n, z = leg[1], leg[2]
        IssueMove({u}, {x, GetTerrainHeight(x, z), z})
        local still, last = 0, nil
        for t = 0, LEG_TICKS do
            if u:IsDead() then break end
            local p = u:GetPosition()
            local moving = u:IsUnitState('Moving')
            local q = u:GetOrientation()
            probe('dg b=%d %s L%d t=%d %.9g %.9g %.2f %s q=%.9g,%.9g,%.9g,%.9g', b, id, n, t, p[1], p[3],
                yaw(u), moving and 'm' or '-', q[1], q[2], q[3], q[4])
            if last and p[1] == last[1] and p[3] == last[2] then still = still + 1 else still = 0 end
            last = {p[1], p[3]} -- a copy: the original hands back one table, updated in place
            if t > 2 and not moving and table.getn(u:GetCommandQueue()) == 0 and still >= REST_TICKS then
                break
            end
            WaitTicks(1)
        end
    end
    done[id] = true
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
    local ids = ground_units()
    probe('dg units=%d', table.getn(ids))
    for _, id in ids do traits(id) end
    local b, i = 0, 1
    while i <= table.getn(ids) do
        b = b + 1
        -- this batch: as many as fit across the span, at most BATCH
        local batch, x = {}, X0
        while i <= table.getn(ids) and table.getn(batch) < BATCH and x + width(ids[i]) <= X1 do
            local w = width(ids[i])
            table.insert(batch, {ids[i], x + w / 2})
            x = x + w
            i = i + 1
        end
        local units, done = {}, {}
        for _, e in batch do
            local id, lx = e[1], e[2]
            -- the lane's ground, for flatness: its lowest and highest point
            local lo, hi = 1e9, -1e9
            for z = ZS - 45, ZS + 2 do
                local h = GetTerrainHeight(lx, z)
                lo, hi = math.min(lo, h), math.max(hi, h)
            end
            probe('dglane b=%d %s x=%.2f rise=%.3f', b, id, lx, hi - lo)
            units[id] = CreateUnitHPR(id, me, lx, GetTerrainHeight(lx, ZS), ZS, 0, math.pi / 2, 0)
        end
        WaitTicks(10)
        probe('dg batch=%d start tick=%d', b, GetGameTick())
        for _, e in batch do
            local u = units[e[1]]
            if u and not u:IsDead() then ForkThread(drive, b, e[1], u, e[2], done)
            else done[e[1]] = true end
        end
        local waited = 0
        while waited < 4 * (LEG_TICKS + 10) do
            local all = true
            for _, e in batch do if not done[e[1]] then all = false end end
            if all then break end
            WaitTicks(5)
            waited = waited + 5
        end
        probe('dg batch=%d end tick=%d', b, GetGameTick())
        for _, u in units do if not u:IsDead() then u:Destroy() end end
        WaitTicks(10)
    end
    probe('dg done')
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
