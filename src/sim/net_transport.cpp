#include "sim/net_transport.hpp"

namespace osc::sim {

bool extract_wire_frames(std::vector<u8>& buf, std::vector<std::vector<u8>>& out) {
    size_t off = 0;
    bool ok = true;
    while (buf.size() - off >= 4) {
        const u32 len = static_cast<u32>(buf[off]) | (static_cast<u32>(buf[off + 1]) << 8) |
                        (static_cast<u32>(buf[off + 2]) << 16) |
                        (static_cast<u32>(buf[off + 3]) << 24);
        if (len > kMaxWireMessage) {
            ok = false;
            break;
        }
        if (buf.size() - off - 4 < len) break; // incomplete
        out.emplace_back(buf.begin() + static_cast<long>(off + 4),
                         buf.begin() + static_cast<long>(off + 4 + len));
        off += 4 + len;
    }
    if (off > 0) buf.erase(buf.begin(), buf.begin() + static_cast<long>(off));
    return ok;
}

} // namespace osc::sim
