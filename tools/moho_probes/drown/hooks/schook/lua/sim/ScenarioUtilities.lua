-- Drowning probe: what happens to a unit made in water its motion type can't
-- be on. Land tanks (UEL0201, RULEUMT_Land) at five depths, and in 4 deep an
-- amphibious tank (URL0203), a hover tank (UEL0203) and an engineer
-- (UEL0105, AmphibiousFloating). Each is logged every WaitTicks(5), which
-- is 4 ticks (both games resume a thread n - 1 ticks on), so the log's t is
-- 5 a step and t=125 is the 100th tick: its layer and health, until it is
-- gone. Each scene is on its own rows, 8 apart from
-- z=300, at the first spot as deep as it wants: a scan that both games run
-- on the same heights.

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function depth_at(x, z)
    return GetSurfaceHeight(x, z) - GetTerrainHeight(x, z)
end

-- The first spot on rows z0 .. z0 + 6 (x from 540 to 760) whose water is
-- `want` deep, within 0.1
local function spot(want, z0)
    for z = z0, z0 + 6 do
        local x = 540
        while x <= 760 do
            if math.abs(depth_at(x, z) - want) <= 0.1 then return x, z end
            x = x + 0.5
        end
    end
    return nil
end

function ProbeDrown(army)
    local scenes = {}
    local wants = {
        {'tank-0.2', 'uel0201', 0.2}, {'tank-0.6', 'uel0201', 0.6}, {'tank-1.2', 'uel0201', 1.2},
        {'tank-2.5', 'uel0201', 2.5}, {'tank-4', 'uel0201', 4}, {'amphibious-4', 'url0203', 4},
        {'hover-4', 'uel0203', 4}, {'engineer-4', 'uel0105', 4},
    }
    for i, w in wants do
        -- Each on its own rows, 8 apart, so none touches another
        local x, z = spot(w[3], 300 + (i - 1) * 8)
        if x then
            table.insert(scenes, {name = w[1], bp = w[2], x = x, z = z})
        else
            probe('drown no spot %.1f deep for %s', w[3], w[1])
        end
    end
    for _, s in scenes do
        s.unit = CreateUnitHPR(s.bp, army, s.x, GetTerrainHeight(s.x, s.z), s.z, 0, 0, 0)
        probe('drown made %s %s at=%.1f,%.1f depth=%.2f', s.name, s.bp, s.x, s.z, depth_at(s.x, s.z))
    end
    for t = 0, 150, 5 do
        for _, s in scenes do
            local u = s.unit
            if u and not u:IsDead() and not u:BeenDestroyed() then
                probe('drown t=%d %s layer=%s health=%.1f/%.1f', t, s.name, u:GetCurrentLayer(),
                    u:GetHealth(), u:GetMaxHealth())
            elseif not s.gone then
                s.gone = t
                probe('drown t=%d %s gone', t, s.name)
            end
        end
        WaitTicks(5)
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
    ProbeDrown(me)
    probe('drown done')
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
