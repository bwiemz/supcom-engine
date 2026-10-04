#include "ui/edit_text.hpp"

#include <algorithm>

namespace osc::ui {

namespace {

size_t byte_at(const std::string& s, i32 chars) {
    size_t i = 0;
    for (i32 n = 0; n < chars && i < s.size(); ++n) {
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) {
            ++i;
        }
    }
    return i;
}

std::string encode(u32 cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

} // namespace

i32 utf8_length(const std::string& s) {
    i32 n = 0;
    for (char c : s) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
            ++n;
        }
    }
    return n;
}

bool EditText::insert(u32 codepoint, i32 max_chars) {
    const i32 length = utf8_length(text);
    if (max_chars > 0 && length >= max_chars) {
        return false;
    }
    caret = std::clamp(caret, 0, length);
    text.insert(byte_at(text, caret), encode(codepoint));
    ++caret;
    return true;
}

bool EditText::erase_before() {
    caret = std::clamp(caret, 0, utf8_length(text));
    if (caret == 0) {
        return false;
    }
    const size_t from = byte_at(text, caret - 1);
    text.erase(from, byte_at(text, caret) - from);
    --caret;
    return true;
}

bool EditText::erase_after() {
    caret = std::clamp(caret, 0, utf8_length(text));
    if (caret >= utf8_length(text)) {
        return false;
    }
    const size_t from = byte_at(text, caret);
    text.erase(from, byte_at(text, caret + 1) - from);
    return true;
}

void EditText::left() {
    caret = std::max(0, std::min(caret, utf8_length(text)) - 1);
}

void EditText::right() {
    caret = std::min(caret + 1, utf8_length(text));
}

void EditText::end() {
    caret = utf8_length(text);
}

} // namespace osc::ui
