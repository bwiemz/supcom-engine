-- Blueprint probe: every unit blueprint as the sim's scripts see it, one
-- `PROBE bp <id> <path>=<value>` line a field, arrays 0-based. The original's
-- own view is its /synclog beat 0 (see README.md); bp_diff.py compares them.

local function dump(id, prefix, t, depth)
    if depth > 8 then return end
    for k, v in t do
        local key
        if type(k) == 'number' then
            key = prefix .. '[' .. (k - 1) .. ']'
        else
            key = prefix == '' and tostring(k) or prefix .. '.' .. tostring(k)
        end
        if type(v) == 'table' then
            dump(id, key, v, depth + 1)
        elseif type(v) ~= 'function' and type(v) ~= 'userdata' then
            LOG('PROBE bp ' .. id .. ' ' .. key .. '=' .. tostring(v))
        end
    end
end

local function BlueprintsMain()
    for id, bp in __blueprints do
        if type(id) == 'string' and bp.BlueprintId == id and bp.Defense and bp.Physics then
            dump(id, '', bp, 0)
        end
    end
    LOG('PROBE bp done')
end

local bp_started = false
local bp_create_initial_army_group = CreateInitialArmyGroup
function CreateInitialArmyGroup(strArmy, createCommander)
    if not bp_started then
        bp_started = true
        BlueprintsMain()
    end
    return bp_create_initial_army_group(strArmy, createCommander)
end
