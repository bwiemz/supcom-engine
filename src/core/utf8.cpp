#include "core/utf8.hpp"

namespace osc {

u32 next_codepoint(std::string_view text, size_t& i) {
    const u32 lead = static_cast<u8>(text[i]);
    size_t extra = 0;
    u32 cp = lead;
    if (lead >= 0xC2 && lead < 0xE0) {
        extra = 1;
        cp = lead & 0x1F;
    } else if (lead >= 0xE0 && lead < 0xF0) {
        extra = 2;
        cp = lead & 0x0F;
    } else if (lead >= 0xF0 && lead < 0xF5) {
        extra = 3;
        cp = lead & 0x07;
    }
    if (extra == 0 || i + extra >= text.size()) {
        ++i;
        return lead;
    }
    for (size_t k = 1; k <= extra; ++k) {
        const u32 b = static_cast<u8>(text[i + k]);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return lead;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    i += extra + 1;
    return cp;
}

} // namespace osc
