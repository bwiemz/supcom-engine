-- Warp probe: where Unit::Warp puts a unit (faf-re: GetStartingLayer with no
-- preferred layer, then CalcSpawnElevation, and the navigator's move
-- aborted). Each scene's unit is made, then warped; logged before, at once
-- after, and after a while: its layer and height (and for the mover, its
-- position and orders).

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function depth_at(x, z)
    return GetSurfaceHeight(x, z) - GetTerrainHeight(x, z)
end

-- The first spot on rows z0 .. z0 + 6 (x from x0 to x1) whose water is
-- between `lo` and `hi` deep (dry ground is 0 deep). On SCMP_009
-- the ground east of x 650 is dry, the sea west of x 450 deep.
local function spot(lo, hi, z0, x0, x1)
    for z = z0, z0 + 6 do
        local x = x0
        while x <= x1 do
            local d = depth_at(x, z)
            if d >= lo and d <= hi then return x, z end
            x = x + 0.5
        end
    end
    return nil
end

local function show(tag, s)
    local u = s.unit
    if u and not u:IsDead() and not u:BeenDestroyed() then
        local p = u:GetPosition()
        probe('warp %s %s layer=%s x=%.2f y=%.2f z=%.2f health=%.1f orders=%d', tag, s.name,
            u:GetCurrentLayer(), p[1], p[2], p[3], u:GetHealth(), table.getn(u:GetCommandQueue()))
    else
        probe('warp %s %s gone', tag, s.name)
    end
end

function ProbeWarp(army)
    -- name, blueprint, where it's made (depth range), where it's warped to
    -- (depth range), the height it's warped to (relative: ground, surface)
    local wants = {
        {'tank-high', 'uel0201', -1, 0, -1, 0, 'ground', 10},
        {'tank-low', 'uel0201', -1, 0, -1, 0, 'ground', -3},
        {'tank-deep', 'uel0201', -1, 0, 8, 30, 'surface', 0},
        {'amphib-deep', 'url0203', -1, 0, 8, 30, 'surface', 0},
        {'hover-deep', 'uel0203', -1, 0, 8, 30, 'zero', 0},
        {'frigate', 'ues0103', 8, 30, 8, 30, 'zero', 0},
        {'scout', 'uea0101', -1, 0, -1, 0, 'zero', 0},
        {'czar', 'uaa0310', -1, 0, -1, 0, 'zero', 0},
    }
    local scenes = {}
    for i, w in wants do
        local region = function(lo) return lo < 0 and 650 or 100, lo < 0 and 1000 or 450 end
        local x0, x1 = region(w[3])
        local tx0, tx1 = region(w[5])
        local x, z = spot(w[3], w[4], 100 + (i - 1) * 12, x0, x1)
        local tx, tz = spot(w[5], w[6], 100 + (i - 1) * 12, tx0, tx1)
        if x and tx and x == tx and z == tz then tx = tx + 12 end
        if x and tx then
            table.insert(scenes, {name = w[1], bp = w[2], x = x, z = z, tx = tx, tz = tz, rel = w[7], dy = w[8]})
        else
            probe('warp no spot for %s', w[1])
        end
    end
    for _, s in scenes do
        s.unit = CreateUnitHPR(s.bp, army, s.x, GetTerrainHeight(s.x, s.z), s.z, 0, 0, 0)
    end
    WaitTicks(11)
    for _, s in scenes do show('before', s) end
    for _, s in scenes do
        local y = s.dy
        if s.rel == 'ground' then y = GetTerrainHeight(s.tx, s.tz) + s.dy
        elseif s.rel == 'surface' then y = GetSurfaceHeight(s.tx, s.tz) + s.dy end
        probe('warp to %s at=%.2f,%.2f asked-y=%.2f ground=%.2f surface=%.2f', s.name, s.tx, s.tz, y,
            GetTerrainHeight(s.tx, s.tz), GetSurfaceHeight(s.tx, s.tz))
        Warp(s.unit, Vector(s.tx, y, s.tz))
        show('t=0', s)
    end
    for _, t in {1, 5, 20} do
        WaitTicks(({[1] = 2, [5] = 5, [20] = 16})[t])
        for _, s in scenes do show('t=' .. t, s) end
    end

    -- A tank on its way: warped 20 aside mid-move. Does it go on to its
    -- goal, and when does it get there?
    local x, z = spot(-1, 0, 230, 680, 760)
    local gx, gz = spot(-1, 0, 230, 760, 800)
    if x and gx then
        local mover = {name = 'mover', unit = CreateUnitHPR('uel0201', army, x, GetTerrainHeight(x, z), z, 0, 0, 0)}
        WaitTicks(6)
        IssueMove({mover.unit}, Vector(gx, GetTerrainHeight(gx, gz), gz))
        probe('warp mover goal=%.2f,%.2f', gx, gz)
        WaitTicks(21)
        show('mover-before', mover)
        local p = mover.unit:GetPosition()
        Warp(mover.unit, Vector(p[1] + 20, GetTerrainHeight(p[1] + 20, p[3]), p[3]))
        show('mover-t=0', mover)
        for i = 1, 30 do
            WaitTicks(11)
            show('mover-t=' .. (i * 10), mover)
            if table.getn(mover.unit:GetCommandQueue()) == 0 then break end
        end
    else
        probe('warp no spot for mover')
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
    ProbeWarp(me)
    probe('warp done')
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
