#pragma once

namespace osc::lua {

// Front-end "LAN Game" button + IP/Host/Join dialog, run on ui_L after the menu
// CreateUI(). Entirely pcall-guarded: any FA-UI API mismatch logs a warning and
// leaves the menu usable rather than crashing. Calls the LanHost/LanJoin/
// LanNetStatus engine globals (registered via register_ui_bindings).
inline constexpr const char* kLanDialogLua = R"LUA(
local ok, err = pcall(function()
    if rawget(_G, '__osc_lan_dialog_built') then return end
    rawset(_G, '__osc_lan_dialog_built', true)
    local root = GetFrame(0)
    if not root then return end
    local Edit = import('/lua/maui/edit.lua').Edit

    -- The "LAN Game" launcher button, placed top-left of the menu.
    local lanBtn = UIUtil.CreateButtonStd(root, '/widgets/small', 'LAN Game', 16)
    LayoutHelpers.AtLeftTopIn(lanBtn, root, 40, 40)

    -- The dialog group (hidden until the button is clicked).
    local dlg = Group(root)
    dlg.Width:Set(360); dlg.Height:Set(220)
    LayoutHelpers.AtCenterIn(dlg, root)
    local bg = Bitmap(dlg)
    bg:SetSolidColor('dd101018'); LayoutHelpers.FillParent(bg, dlg)
    local title = UIUtil.CreateText(dlg, 'LAN Game', 20, UIUtil.bodyFont)
    LayoutHelpers.AtTopIn(title, dlg, 12); LayoutHelpers.AtHorizontalCenterIn(title, dlg)
    local ipEdit = Edit(dlg)
    ipEdit.Width:Set(240); ipEdit.Height:Set(28)
    LayoutHelpers.AtTopIn(ipEdit, dlg, 60); LayoutHelpers.AtHorizontalCenterIn(ipEdit, dlg)
    if ipEdit.SetText then ipEdit:SetText('127.0.0.1') end
    local status = UIUtil.CreateText(dlg, 'idle', 14, UIUtil.bodyFont)
    LayoutHelpers.AtBottomIn(status, dlg, 12); LayoutHelpers.AtHorizontalCenterIn(status, dlg)

    local hostBtn = UIUtil.CreateButtonStd(dlg, '/widgets/small', 'Host', 14)
    LayoutHelpers.AtTopIn(hostBtn, dlg, 110); LayoutHelpers.AtLeftIn(hostBtn, dlg, 40)
    local joinBtn = UIUtil.CreateButtonStd(dlg, '/widgets/small', 'Join', 14)
    LayoutHelpers.AtTopIn(joinBtn, dlg, 110); LayoutHelpers.AtRightIn(joinBtn, dlg, 40)
    local closeBtn = UIUtil.CreateButtonStd(dlg, '/widgets/small', 'Close', 14)
    LayoutHelpers.AtBottomIn(closeBtn, dlg, 40); LayoutHelpers.AtHorizontalCenterIn(closeBtn, dlg)

    dlg:Hide()
    lanBtn.OnClick = function() dlg:Show() end
    closeBtn.OnClick = function() dlg:Hide() end
    hostBtn.OnClick = function()
        if LanHost() then status:SetText('hosting: waiting for player')
        else status:SetText('host failed') end
    end
    joinBtn.OnClick = function()
        local ip = (ipEdit.GetText and ipEdit:GetText()) or '127.0.0.1'
        if LanJoin(ip) then status:SetText('connecting to ' .. ip)
        else status:SetText('join failed') end
    end
    -- Refresh the status line each beat while the dialog is up.
    dlg.OnFrame = function(self, delta)
        if self:IsHidden() then return end
        status:SetText(LanNetStatus())
    end
end)
if not ok then LOG('[lan-ui] dialog build failed: ' .. tostring(err)) end
)LUA";

} // namespace osc::lua
