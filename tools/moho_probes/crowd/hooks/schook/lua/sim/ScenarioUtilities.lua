-- Trace the crowd at a gap: 24 Strikers to one point past a wall with a
-- 4-wide gap (tools/moho_probes' choke scene), in several variants whose
-- spawn points are jittered. Every 2 ticks it logs each unit's position and
-- whether it still has its order; at the end, who never crossed.
local function probe(fmt, ...) LOG('PROBE ' .. string.format(fmt, unpack(arg))) end
local function spawn(bp, army, x, z) return CreateUnitHPR(bp, army, x, GetTerrainHeight(x, z), z, 0, 0, 0) end

local VARIANTS = 6
local TICKS = 400

function ProbeTrace(army)
    for x = 650, 789 do
        if x < 718 or x >= 722 then
            CreateUnitHPR('ueb5101', army, x + 0.5, GetTerrainHeight(x + 0.5, 175.5), 175.5, 0, 0, 0)
        end
    end
    for v = 0, VARIANTS - 1 do
        local k = 0
        local function jitter()
            if v == 0 then return 0 end
            k = k + 1
            return math.sin(v * 7.13 + k * 1.7) * 0.8
        end
        local crowd = {}
        for i = 0, 7 do
            for j = 0, 2 do
                table.insert(crowd, spawn('uel0201', army, 706 + 4 * i + jitter(), 150 + 4 * j + jitter()))
            end
        end
        WaitTicks(1)
        IssueMove(crowd, {720, GetTerrainHeight(720, 200), 200})
        local crossed = {}
        for tick = 1, TICKS do
            WaitTicks(1)
            local row = ''
            for i, u in crowd do
                if not u:IsDead() then
                    local p = u:GetPosition()
                    if p[3] >= 176 then crossed[i] = true end
                    if math.mod(tick, 2) == 0 then
                        row = row .. string.format('%.2f,%.2f,%d;', p[1], p[3], table.getn(u:GetCommandQueue() or {}))
                    end
                end
            end
            if math.mod(tick, 2) == 0 then probe('tr v=%d t=%d %s', v, tick, row) end
        end
        local short = 0
        for i, u in crowd do
            if not crossed[i] then
                short = short + 1
                local p = u:GetPosition()
                probe('tr-short v=%d i=%d x=%.2f z=%.2f q=%d', v, i, p[1], p[3], table.getn(u:GetCommandQueue() or {}))
            end
        end
        probe('tr-done v=%d short=%d', v, short)
        for _, u in crowd do if not u:IsDead() then u:Destroy() end end
        WaitTicks(10)
    end
    probe('tr-all-done')
end

function ProbeMain()
    WaitTicks(80)
    local armies = ListArmies()
    local me = armies[1]
    for _, other in armies do
        if other ~= me then SetAlliance(me, other, 'Neutral') SetAlliance(other, me, 'Neutral') end
    end
    ForkThread(ProbeTrace, me)
end

local started = false
local orig = CreateInitialArmyGroup
function CreateInitialArmyGroup(a, b)
    if not started then started = true ForkThread(ProbeMain) end
    return orig(a, b)
end
