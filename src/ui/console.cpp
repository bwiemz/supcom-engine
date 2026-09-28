#include "ui/console.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

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

namespace {

bool same_no_case(const std::string& a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

/// The command's operator and its value (TConVar's `op` and `rhs`).
const std::string* arg(const std::vector<std::string>& args, size_t i) {
    return i < args.size() ? &args[i] : nullptr;
}

// atoi and atof, as Moho parses (junk is 0), by strtol and strtof
int to_int(const std::string* s) {
    return s ? static_cast<int>(std::strtol(s->c_str(), nullptr, 10)) : 0;
}

float to_float(const std::string* s) {
    return s ? std::strtof(s->c_str(), nullptr) : 0.0f;
}

} // namespace

bool convar_bool(const std::vector<std::string>& args, bool value, bool& shown) {
    shown = false;
    const std::string* op = arg(args, 1);
    const std::string* rhs = arg(args, 2);
    if (!op) return !value; // no argument: toggled
    if (*op == "=" && rhs) return to_int(rhs) != 0;
    if (same_no_case(*op, "on") || same_no_case(*op, "true")) return true;
    if (same_no_case(*op, "off") || same_no_case(*op, "false")) return false;
    if (same_no_case(*op, "show")) {
        shown = true;
        return value;
    }
    if (same_no_case(*op, "tog")) return !value;
    return to_int(op) != 0;
}

int convar_int(const std::vector<std::string>& args, int value, bool& shown) {
    shown = false;
    const std::string* op = arg(args, 1);
    const std::string* rhs = arg(args, 2);
    if (!op) {
        shown = true;
        return value;
    }
    if (rhs) {
        const int v = to_int(rhs);
        if (*op == "=") return v;
        if (*op == "+=") return value + v;
        if (*op == "-=") return value - v;
        if (*op == "*=") return value * v;
        if (*op == "/=") return v == 0 ? value : value / v;
        if (*op == "%=") return v == 0 ? value : value % v;
        if (*op == "&=") return value & v;
        if (*op == "|=") return value | v;
        if (*op == "^=") return value ^ v;
    }
    if (same_no_case(*op, "on") || same_no_case(*op, "true")) return 1;
    if (same_no_case(*op, "off") || same_no_case(*op, "false")) return 0;
    if (same_no_case(*op, "tog")) return value == 0 ? 1 : 0;
    return to_int(op);
}

float convar_float(const std::vector<std::string>& args, float value, bool& shown) {
    shown = false;
    const std::string* op = arg(args, 1);
    const std::string* rhs = arg(args, 2);
    if (!op) {
        shown = true;
        return value;
    }
    if (rhs) {
        const float v = to_float(rhs);
        if (*op == "=") return v;
        if (*op == "+=") return value + v;
        if (*op == "-=") return value - v;
        if (*op == "*=") return value * v;
        if (*op == "/=") return v == 0.0f ? value : value / v;
    }
    return to_float(op);
}

void add_bool_var(Console& console, const std::string& name, std::function<bool(lua_State*)> get,
                  std::function<void(lua_State*, bool)> set) {
    console.add(name, [name, get = std::move(get),
                       set = std::move(set)](lua_State* L, const std::vector<std::string>& args) {
        bool shown = false;
        const bool v = convar_bool(args, get(L), shown);
        if (shown) {
            spdlog::info("bool {} is {}", name, v ? "on" : "off");
            return;
        }
        set(L, v);
        if (args.size() < 2) spdlog::info("toggled {} is now {}", name, v ? "on" : "off");
    });
}

void add_int_var(Console& console, const std::string& name, std::function<int(lua_State*)> get,
                 std::function<void(lua_State*, int)> set) {
    console.add(name, [name, get = std::move(get),
                       set = std::move(set)](lua_State* L, const std::vector<std::string>& args) {
        bool shown = false;
        const int v = convar_int(args, get(L), shown);
        if (shown) spdlog::info("int {} == {}", name, v);
        else set(L, v);
    });
}

void add_float_var(Console& console, const std::string& name, std::function<float(lua_State*)> get,
                   std::function<void(lua_State*, float)> set) {
    console.add(name, [name, get = std::move(get),
                       set = std::move(set)](lua_State* L, const std::vector<std::string>& args) {
        bool shown = false;
        const float v = convar_float(args, get(L), shown);
        if (shown) spdlog::info("float {} == {:.4f}", name, v);
        else set(L, v);
    });
}

} // namespace osc::ui
