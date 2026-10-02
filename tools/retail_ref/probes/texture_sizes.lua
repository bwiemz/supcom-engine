-- retail_ref probe: the size retail gives each texture (--arg textures=a,b,...,
-- paths as UIUtil.UIFile takes them), by GetTextureDimensions and as a
-- Bitmap's BitmapWidth/BitmapHeight and Width/Height:
--   retail_ref.py --probe probes/texture_sizes.lua --arg textures=/icons/units/UEL0001_icon.dds
function Probe(log, args)
    local UIUtil = import('/lua/ui/uiutil.lua')
    local Bitmap = import('/lua/maui/bitmap.lua').Bitmap
    for path in string.gfind(args.textures or '', '[^,]+') do
        local file = UIUtil.UIFile(path)
        local w, h = GetTextureDimensions(file)
        local b = Bitmap(GetFrame(0), file)
        WaitSeconds(0.1)
        log(path .. ' dimensions ' .. tostring(w) .. 'x' .. tostring(h) ..
            ' bitmap ' .. tostring(b.BitmapWidth()) .. 'x' .. tostring(b.BitmapHeight()) ..
            ' size ' .. tostring(b.Width()) .. 'x' .. tostring(b.Height()))
        b:Destroy()
    end
end
