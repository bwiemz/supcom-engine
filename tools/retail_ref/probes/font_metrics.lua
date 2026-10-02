-- retail_ref probe: fonts as retail measures them: an ItemList's row height
-- and string widths, a Text's ascent, descent, external leading, advance and
-- height, an Edit's font height; with no font set, then in each family at
-- each size (--arg families=Arial,Zeroes Three --arg sizes=10,14,16)
function Probe(log, args)
    local ItemList = import('/lua/maui/itemlist.lua').ItemList
    local Text = import('/lua/maui/text.lua').Text
    local Edit = import('/lua/maui/edit.lua').Edit
    local texts = { 'W', 'iiiiiiiiii', 'MMMMMMMMMM', 'The quick brown fox', '0123456789' }
    local function widths(c)
        local out = {}
        for _, t in texts do
            table.insert(out, tostring(c:GetStringAdvance(t)))
        end
        return table.concat(out, ' ')
    end
    local function measure(family, size)
        local name = family and (family .. ' ' .. size) or 'none'
        local list = ItemList(GetFrame(0))
        local text = Text(GetFrame(0))
        local edit = Edit(GetFrame(0))
        edit.Width:Set(300)
        if family then
            list:SetFont(family, size)
            text:SetFont(family, size)
            edit:SetFont(family, size)
        end
        text:SetText('The quick brown fox')
        WaitSeconds(0.1)
        log('list ' .. name .. ' row ' .. tostring(list:GetRowHeight()) .. ' widths ' .. widths(list))
        log('text ' .. name .. ' ascent ' .. tostring(text.FontAscent()) ..
            ' descent ' .. tostring(text.FontDescent()) ..
            ' leading ' .. tostring(text.FontExternalLeading()) ..
            ' advance ' .. tostring(text.TextAdvance()) .. ' height ' .. tostring(text.Height()))
        log('edit ' .. name .. ' height ' .. tostring(edit:GetFontHeight()) .. ' widths ' .. widths(edit))
        list:Destroy()
        text:Destroy()
        edit:Destroy()
    end
    measure()
    for family in string.gfind(args.families or 'Arial,Zeroes Three', '[^,]+') do
        for size in string.gfind(args.sizes or '10,12,14,16,18', '[^,]+') do
            measure(family, tonumber(size))
        end
    end
end
