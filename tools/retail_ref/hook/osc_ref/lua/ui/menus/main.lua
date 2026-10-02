-- retail_ref: retail's front end clicked through its buttons by their labels,
-- as the engine's --click does, then the probes run; each step is logged as
-- "OSC-REF: ..." for tools/retail_ref/retail_ref.py, and the game exits.
local config = import('/lua/osc_ref_config.lua')
local UIUtil = import('/lua/ui/uiutil.lua')

-- Buttons by their label as shown, the last made of each
local buttons = {}
local create_button = UIUtil.CreateButtonStd
UIUtil.CreateButtonStd = function(parent, filename, label, a, b, c, d, e, f)
    local button = create_button(parent, filename, label, a, b, c, d, e, f)
    if label then
        buttons[LOC(label)] = button
    end
    return button
end

local function finish()
    LOG('OSC-REF: ready')
    -- The front end's log reaches its file only as its buffer fills
    for i = 1, 400 do
        LOG('OSC-REF: pad ' .. i)
    end
    WaitSeconds(config.hold)
    ExitApplication()
end

local function click(label)
    local waited = 0
    while not buttons[label] and waited < 30 do
        WaitSeconds(0.5)
        waited = waited + 0.5
    end
    local button = buttons[label]
    if not button then
        LOG('OSC-REF: failed: no button ' .. label)
        return false
    end
    -- Its menu animates in; its click is its own once it has
    WaitSeconds(3)
    local ok, err = pcall(function() button.OnClick(button) end)
    if not ok then
        LOG('OSC-REF: failed: ' .. label .. ': ' .. tostring(err))
        return false
    end
    LOG('OSC-REF: clicked ' .. label)
    return true
end

local function probe(name)
    -- In a thread of its own, so it may wait; a script error ends it, and
    -- the log has the error
    local done = false
    ForkThread(function()
        local module = import('/lua/' .. name .. '.lua')
        module.Probe(function(text) LOG('OSC-REF: probe ' .. tostring(text)) end, config.args)
        done = true
    end)
    local waited = 0
    while not done and waited < 60 do
        WaitSeconds(0.1)
        waited = waited + 0.1
    end
    if not done then
        LOG('OSC-REF: failed: probe ' .. name .. ' did not finish')
    end
    return done
end

local osc_CreateUI = CreateUI
function CreateUI()
    osc_CreateUI()
    LOG('OSC-REF: main menu')
    ForkThread(function()
        WaitSeconds(3)
        for _, label in config.clicks do
            if not click(label) then
                return finish()
            end
        end
        WaitSeconds(config.settle)
        for _, name in config.probes do
            if not probe(name) then
                return finish()
            end
        end
        finish()
    end)
end
