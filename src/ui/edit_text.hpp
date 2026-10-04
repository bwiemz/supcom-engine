#pragma once

#include "core/types.hpp"

#include <string>

namespace osc::ui {

/// An Edit's text and its caret, a count of characters from the start (the
/// text is UTF-8), as Moho's CMauiEdit edits them.
struct EditText {
    std::string text;
    i32 caret = 0;

    /// Insert one character at the caret, unless the text holds `max_chars`
    /// (0: no limit); whether it went in
    bool insert(u32 codepoint, i32 max_chars);
    bool erase_before();
    bool erase_after();
    void left();
    void right();
    void home() { caret = 0; }
    void end();
};

i32 utf8_length(const std::string& s);

} // namespace osc::ui
