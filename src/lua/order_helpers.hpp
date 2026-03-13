#pragma once
#include <string>

namespace osc::lua {

struct OrderBitmapPaths {
    std::string up, up_sel, over, over_sel, dis, dis_sel;
    const char* sound_click = "UI_Action_MouseDown";
    const char* sound_rollover = "UI_Action_Rollover";
};

OrderBitmapPaths get_order_bitmap_paths(const std::string& bitmap_id);

} // namespace osc::lua
