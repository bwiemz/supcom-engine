-- Starting-layer probe: the layer a unit is made on, for the motion types the
-- drown probe didn't ask about. Subs (SurfacingSub) in deep water and one in
-- water too shallow for a sub, a frigate (Water) in deep water, and aircraft
-- (a T1 scout, and two experimentals) on dry ground. Each is logged as made
-- and after a while: its layer, height and health. Each scene is on its own
-- rows, 8 apart from z=300, at the first spot as deep as it wants.

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function depth_at(x, z)
    return GetSurfaceHeight(x, z) - GetTerrainHeight(x, z)
end

-- The first spot on rows z0 .. z0 + 6 (x from 400 to 900) whose water is
-- between `lo` and `hi` deep
local function spot(lo, hi, z0)
    for z = z0, z0 + 6 do
        local x = 400
        while x <= 900 do
            local d = depth_at(x, z)
            if d >= lo and d <= hi then return x, z end
            x = x + 0.5
        end
    end
    return nil
end

function ProbeLayers(army)
    local wants = {
        {'sub-uef', 'ues0203', 8, 30}, {'sub-aeon', 'uas0203', 8, 30}, {'sub-cybran', 'urs0203', 8, 30},
        {'sub-sera', 'xss0201', 8, 30}, {'sub-shallow', 'ues0203', 0.8, 1.2}, {'frigate', 'ues0103', 8, 30},
        {'scout', 'uea0101', 0, 0}, {'soulripper', 'ura0401', 0, 0}, {'czar', 'uaa0310', 0, 0},
    }
    local scenes = {}
    for i, w in wants do
        local x, z = spot(w[3], w[4], 300 + (i - 1) * 8)
        if x then
            table.insert(scenes, {name = w[1], bp = w[2], x = x, z = z})
        else
            probe('layers no spot %.1f-%.1f deep for %s', w[3], w[4], w[1])
        end
    end
    for _, s in scenes do
        s.unit = CreateUnitHPR(s.bp, army, s.x, GetTerrainHeight(s.x, s.z), s.z, 0, 0, 0)
        probe('layers made %s %s at=%.1f,%.1f depth=%.2f ground=%.2f', s.name, s.bp, s.x, s.z,
            depth_at(s.x, s.z), GetTerrainHeight(s.x, s.z))
    end
    for _, t in {0, 1, 5, 20, 60} do
        for _, s in scenes do
            local u = s.unit
            if u and not u:IsDead() and not u:BeenDestroyed() then
                local p = u:GetPosition()
                probe('layers t=%d %s layer=%s y=%.2f health=%.1f/%.1f', t, s.name, u:GetCurrentLayer(),
                    p[2], u:GetHealth(), u:GetMaxHealth())
            else
                probe('layers t=%d %s gone', t, s.name)
            end
        end
        if t < 60 then WaitTicks(({[0] = 2, [1] = 5, [5] = 16, [20] = 41})[t]) end
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
    ProbeLayers(me)
    probe('layers done')
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
