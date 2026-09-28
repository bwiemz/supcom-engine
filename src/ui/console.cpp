#include "ui/console.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>

namespace osc::ui {

namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t';
}

} // namespace

void parse_console_command(std::string_view line, std::vector<std::string>& tokens,
                           std::string& remainder) {
    tokens.clear();
    remainder.clear();
    // A copy: escapes are taken out of it as the scan meets them.
    std::string text(line);
    bool in_quotes = false;
    std::ptrdiff_t start = -1;
    size_t end = text.size();
    const auto push = [&](size_t from, size_t to) {
        tokens.emplace_back(text.substr(from, to - from));
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_quotes) {
            if (c == '"') {
                push(static_cast<size_t>(start), i);
                start = -1;
                in_quotes = false;
            } else if (c == '\\' && i + 1 < text.size() &&
                       (text[i + 1] == '"' || text[i + 1] == '\\')) {
                text.erase(i, 1); // the escaped character stays, as itself
            }
            continue;
        }
        if (c == '#') {
            end = i;
            break;
        }
        if (c == ';') {
            end = i;
            remainder = text.substr(i + 1);
            break;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            end = i;
            break;
        }
        if (is_space(c)) {
            if (start != -1) {
                push(static_cast<size_t>(start), i);
                start = -1;
            }
            continue;
        }
        if (start == -1) {
            start = static_cast<std::ptrdiff_t>(i);
            if (c == '"') {
                in_quotes = true;
                start = static_cast<std::ptrdiff_t>(i + 1);
            } else if (c == '\\' && i + 1 < text.size() && text[i + 1] == '"') {
                text.erase(i, 1); // a leading \" is a quote, not a quoted token
            }
        }
    }
    // The last token (an unclosed quote runs to the end).
    if (start != -1 && end >= static_cast<size_t>(start)) push(static_cast<size_t>(start), end);
}

bool Console::NoCase::operator()(std::string_view a, std::string_view b) const {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) <
               std::tolower(static_cast<unsigned char>(y));
    });
}

void Console::add(std::string name, Handler handler) {
    commands_.insert_or_assign(std::move(name), std::move(handler));
}

bool Console::has(std::string_view name) const {
    return commands_.find(name) != commands_.end();
}

void Console::execute(lua_State* L, std::string_view line) {
    std::string pending(line);
    std::vector<std::string> tokens;
    std::string remainder;
    while (!pending.empty()) {
        parse_console_command(pending, tokens, remainder);
        if (!tokens.empty()) {
            if (const auto it = commands_.find(tokens.front()); it != commands_.end())
                it->second(L, tokens);
            else spdlog::info("Unknown console command \"{}\"", tokens.front());
        }
        if (tokens.empty() && remainder == pending) break;
        pending = remainder;
    }
}

} // namespace osc::ui
