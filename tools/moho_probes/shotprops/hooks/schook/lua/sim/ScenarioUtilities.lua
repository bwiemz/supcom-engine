
-- Shots and props probe: does a shot stop on a prop in its way? Five lanes on
-- the plain at x 645-675, z 90-130, each a level shell from a Striker at 645
-- toward 675, 0.3 up, with ARMY_2's power generator at 673 behind whatever
-- stands at 660: nothing, a rock, a tree group, a single tree, a tank's
-- wreck. The shell is the engine's collide test's (`__osc_shoot`), so both
-- games fire the same one. Each lane logs the shell's first impact (its type,
-- and whether it struck the prop or the wall), then the prop's and the wall's
-- health, and whether the tree group broke up.

local function probe(fmt, ...)
    LOG('PROBE ' .. string.format(fmt, unpack(arg)))
end

local function ground(x, z) return GetTerrainHeight(x, z) end

local function spawn(bp, army, x, z)
    return CreateUnitHPR(bp, army, x, ground(x, z), z, 0, 0, 0)
end

local function shoot(from, x, y, z, up, rec)
    local at = from:GetPosition()
    local p = from:CreateProjectile('/projectiles/tdfgauss01/tdfgauss01_proj.bp', 0, up, 0,
                                    x - at[1], y - (at[2] + up), z - at[3])
    p:SetBallisticAcceleration(0)
    p:SetLifetime(5)
    local impact = p.OnImpact
    p.OnImpact = function(s, type, target)
        if not rec.type then
            rec.type, rec.target, rec.at, rec.tick = type, target, s:GetPosition(), GetGameTick()
        end
        return impact(s, type, target)
    end
    return p
end

local function health(e)
    if not e or e:BeenDestroyed() then return 'gone' end
    return string.format('%.1f', e:GetHealth())
end

function ProbeShotProps(me, other, third)
    local lanes = {
        {name = 'none', z = 90},
        {name = 'rock', z = 100, bp = '/env/evergreen/props/rocks/fieldstone03_prop.bp'},
        {name = 'treegroup', z = 110, bp = '/env/evergreen/props/trees/groups/pine06_big_groupa_prop.bp'},
        {name = 'tree', z = 120, bp = '/env/evergreen/props/trees/pine06_big_prop.bp'},
        {name = 'wreck', z = 130},
    }
    -- The map's own props on the lanes go first, so only the lane's prop is in a shell's way
    local cleared = 0
    for _, e in GetReclaimablesInRect(Rect(640, 85, 680, 135)) or {} do
        if IsProp(e) and not e:BeenDestroyed() then
            e:Destroy()
            cleared = cleared + 1
        end
    end
    probe('shotprops cleared %d props', cleared)
    WaitTicks(2)
    for _, l in lanes do
        l.wall = spawn('ueb1101', other, 673, l.z)
        if l.bp then
            l.prop = CreatePropHPR(l.bp, 660, ground(660, l.z), l.z, 0, 0, 0)
        elseif l.name == 'wreck' then
            local tank = spawn('uel0201', third, 660, l.z)
            l.prop = tank:CreateWreckageProp(0)
            tank:Destroy()
        end
        probe('shotprops made %s prop=%s health=%s wall=%s', l.name, tostring(l.prop ~= nil),
            health(l.prop), health(l.wall))
    end
    WaitTicks(5)
    for _, l in lanes do
        l.shooter = spawn('uel0201', me, 645, l.z)
        l.rec = {}
    end
    WaitTicks(2)
    for _, l in lanes do
        shoot(l.shooter, 675, ground(675, l.z) + 0.3, l.z, 0.3, l.rec)
    end
    WaitTicks(40)
    for _, l in lanes do
        local r = l.rec
        local what = 'nothing'
        if r.target then
            if r.target == l.prop then what = 'the prop'
            elseif r.target == l.wall then what = 'the target'
            else what = 'something else' end
        end
        probe('shotprops %s impact=%s on=%s at=%s tick=%s prop=%s target=%s', l.name,
            tostring(r.type), what, r.at and string.format('%.2f,%.2f,%.2f', r.at[1], r.at[2], r.at[3]) or '-',
            tostring(r.tick), health(l.prop), health(l.wall))
    end
end

function ProbeMain()
    WaitTicks(80)
    local armies = ListArmies()
    local me, other, third = armies[1], armies[2], armies[3]
    -- The shooters' army and the targets' are enemies; every other pair is neutral
    for _, a in armies do
        for _, b in armies do
            if a ~= b then SetAlliance(a, b, 'Neutral') end
        end
    end
    SetAlliance(me, other, 'Enemy')
    SetAlliance(other, me, 'Enemy')
    ProbeShotProps(me, other, third)
    probe('shotprops done')
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
