#include "lua/order_helpers.hpp"

namespace osc::lua {

OrderBitmapPaths get_order_bitmap_paths(const std::string& id) {
    OrderBitmapPaths p;
    std::string base = "/textures/ui/common/game/orders/" + id + "_btn_";
    p.up       = base + "up.dds";
    p.up_sel   = base + "up_sel.dds";
    p.over     = base + "over.dds";
    p.over_sel = base + "over_sel.dds";
    p.dis      = base + "dis.dds";
    p.dis_sel  = base + "dis_sel.dds";
    return p;
}

} // namespace osc::lua
