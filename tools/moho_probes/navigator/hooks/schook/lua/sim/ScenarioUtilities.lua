-- One Striker at a time to a point past a wall with a 4-wide gap (the choke
-- scene's wall, no crowd). Every tick: its position and its navigator's
-- current target (GetCurrentTargetPos: the path navigator's target cell
-- centre) and status. Which cells does it aim at on the way?
local function probe(fmt, ...) LOG('PROBE ' .. string.format(fmt, unpack(arg))) end
local function spawn(bp, army, x, z) return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, 0, 0) end

local STARTS = {{730, 154}, {742, 158}, {700, 168}}
local TICKS = 160

function ProbeTarget(army)
    for x = 650, 789 do
        if x < 718 or x >= 722 then
            CreateUnitHPR('ueb5101', army, x + 0.5, GetTerrainHeight(x + 0.5, 175.5), 175.5, 0, 0, 0)
        end
    end
    for s, start in STARTS do
        local u = spawn('uel0201', army, start[1], start[2])
        WaitTicks(1)
        IssueMove({u}, {720, GetTerrainHeight(720, 200), 200})
        local last = ''
        for tick = 1, TICKS do
            WaitTicks(1)
            if u:IsDead() then break end
            local p = u:GetPosition()
            local nav = u:GetNavigator()
            local tgt = 'nil'
            if nav and nav.GetCurrentTargetPos then
                local t = nav:GetCurrentTargetPos()
                if t then tgt = string.format('%.1f,%.1f', t[1], t[3]) end
            end
            local status = (nav and nav.GetStatus) and tostring(nav:GetStatus()) or '?'
            probe('target s=%d t=%d pos=%.2f,%.2f tgt=%s st=%s', s, tick, p[1], p[3], tgt, status)
        end
        u:Destroy()
        WaitTicks(5)
    end
    probe('target-all-done')
end

function ProbeMain()
    WaitTicks(80)
    local armies = ListArmies()
    local me = armies[1]
    for _, other in armies do
        if other ~= me then SetAlliance(me, other, 'Neutral') SetAlliance(other, me, 'Neutral') end
    end
    ForkThread(ProbeTarget, me)
end

local started = false
local orig = CreateInitialArmyGroup
function CreateInitialArmyGroup(a, b)
    if not started then started = true ForkThread(ProbeMain) end
    return orig(a, b)
end
