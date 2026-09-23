#include "lua/binding_coverage.hpp"

#include "vfs/virtual_file_system.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua::coverage {

namespace {

enum class TokKind { Name, Punct, Value, End };

struct Token {
    TokKind kind = TokKind::End;
    std::string text; // identifier or punctuation
    int line = 0;
};

bool is_name_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}
bool is_name_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

const std::set<std::string>& keywords() {
    static const std::set<std::string> k{
        "and",   "break", "continue", "do",     "else", "elseif", "end",
        "false", "for",   "function", "if",     "in",   "local",  "nil",
        "not",   "or",    "repeat",   "return", "then", "true",   "until",
        "while"};
    return k;
}

/// Tokenizer for FA's LuaPlus dialect. Strings, numbers and long strings
/// collapse to Value tokens; all comment forms are skipped.
class Lexer {
public:
    explicit Lexer(std::string_view src) : s_(src) {}

    std::vector<Token> run() {
        std::vector<Token> out;
        while (true) {
            Token t = next();
            if (t.kind == TokKind::End) break;
            out.push_back(std::move(t));
        }
        return out;
    }

private:
    std::string_view s_;
    size_t i_ = 0;
    int line_ = 1;

    char peek(size_t ahead = 0) const {
        return i_ + ahead < s_.size() ? s_[i_ + ahead] : '\0';
    }
    void advance() {
        if (i_ < s_.size() && s_[i_] == '\n') ++line_;
        ++i_;
    }
    void skip_line() {
        while (i_ < s_.size() && s_[i_] != '\n') advance();
    }
    /// At "[[": skip a (nestable, Lua 5.0) long bracket.
    void skip_long_bracket() {
        int depth = 0;
        while (i_ < s_.size()) {
            if (peek() == '[' && peek(1) == '[') {
                ++depth;
                advance();
                advance();
            } else if (peek() == ']' && peek(1) == ']') {
                --depth;
                advance();
                advance();
                if (depth == 0) return;
            } else {
                advance();
            }
        }
    }

    Token next() {
        while (i_ < s_.size()) {
            const char c = peek();
            if (std::isspace(static_cast<unsigned char>(c))) {
                advance();
            } else if (c == '-' && peek(1) == '-') {
                advance();
                advance();
                if (peek() == '[' && peek(1) == '[') {
                    skip_long_bracket();
                } else {
                    skip_line();
                }
            } else if (c == '#') {
                skip_line(); // LuaPlus line comment
            } else {
                break;
            }
        }
        Token t;
        t.line = line_;
        if (i_ >= s_.size()) return t;

        const char c = peek();
        if (is_name_start(c)) {
            t.kind = TokKind::Name;
            while (is_name_char(peek())) {
                t.text += peek();
                advance();
            }
            return t;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            t.kind = TokKind::Value;
            while (is_name_char(peek()) || peek() == '.') advance();
            return t;
        }
        if (c == '"' || c == '\'') {
            t.kind = TokKind::Value;
            advance();
            while (i_ < s_.size() && peek() != c && peek() != '\n') {
                if (peek() == '\\') advance();
                advance();
            }
            advance(); // closing quote
            return t;
        }
        if (c == '[' && peek(1) == '[') {
            t.kind = TokKind::Value;
            skip_long_bracket();
            return t;
        }
        t.kind = TokKind::Punct;
        // Two-character operators that must not be read as '=' / '.'.
        const char n = peek(1);
        if ((c == '=' && n == '=') || (c == '~' && n == '=') || (c == '!' && n == '=') ||
            (c == '<' && n == '=') || (c == '>' && n == '=') || (c == '.' && n == '.')) {
            t.text = std::string{c, n};
            advance();
            advance();
            if (t.text == ".." && peek() == '.') {
                t.text = "...";
                advance();
            }
            return t;
        }
        t.text = std::string(1, c);
        advance();
        return t;
    }
};

bool is_punct(const Token& t, const char* p) {
    return t.kind == TokKind::Punct && t.text == p;
}
bool is_keyword(const Token& t, const char* k) {
    return t.kind == TokKind::Name && t.text == k;
}
/// A call opens with '(', a table constructor or a string literal.
bool opens_call(const Token& t) {
    return is_punct(t, "(") || is_punct(t, "{") || t.kind == TokKind::Value;
}

void record(std::map<std::string, std::string>& into, const std::string& name,
            std::string_view file, int line) {
    into.emplace(name, std::string(file) + ":" + std::to_string(line));
}

} // namespace

void scan_lua_source(std::string_view source, std::string_view file,
                     LuaReferences& refs) {
    const auto toks = Lexer(source).run();
    const auto& kw = keywords();
    auto at = [&](size_t i) -> const Token& {
        static const Token end{};
        return i < toks.size() ? toks[i] : end;
    };

    for (size_t i = 0; i < toks.size(); ++i) {
        const Token& t = toks[i];
        if (t.kind != TokKind::Name) continue;

        // Definitions -----------------------------------------------------
        if (t.text == "function") {
            // function a.b.c:Name(  -- the last name of the chain is defined
            size_t j = i + 1;
            std::string last;
            while (at(j).kind == TokKind::Name) {
                last = at(j).text;
                if (is_punct(at(j + 1), ".") || is_punct(at(j + 1), ":")) {
                    j += 2;
                } else {
                    ++j;
                    break;
                }
            }
            if (!last.empty()) refs.defined.insert(last);
            // Parameters are defined too: `function(cb) cb() end`.
            if (is_punct(at(j), "(")) {
                for (++j; j < toks.size() && !is_punct(at(j), ")"); ++j) {
                    if (at(j).kind == TokKind::Name) refs.defined.insert(at(j).text);
                }
            }
            continue;
        }
        if (t.text == "for") {
            // for k, v in ... / for i = ...: loop variables are locals
            for (size_t j = i + 1; at(j).kind == TokKind::Name && !kw.count(at(j).text);
                 j += 2) {
                refs.defined.insert(at(j).text);
                if (!is_punct(at(j + 1), ",")) break;
            }
            continue;
        }
        if (t.text == "local") {
            size_t j = i + 1;
            if (is_keyword(at(j), "function")) continue; // handled above
            while (at(j).kind == TokKind::Name && !kw.count(at(j).text)) {
                refs.defined.insert(at(j).text);
                if (!is_punct(at(j + 1), ",")) break;
                j += 2;
            }
            continue;
        }
        if (kw.count(t.text)) continue;
        if (is_punct(at(i + 1), "=")) {
            refs.defined.insert(t.text); // global/field assignment
            continue;
        }

        // Calls -------------------------------------------------------------
        const Token& prev = i > 0 ? toks[i - 1] : at(toks.size());
        if (!opens_call(at(i + 1))) continue;
        if (is_punct(prev, ":")) {
            record(refs.method_calls, t.text, file, t.line);
        } else if (!is_punct(prev, ".") && !is_keyword(prev, "function")) {
            record(refs.global_calls, t.text, file, t.line);
        }
    }
}

CoverageReport compute_coverage(const LuaReferences& refs,
                                const std::set<std::string>& registered_globals,
                                const std::set<std::string>& registered_methods) {
    CoverageReport report;
    report.globals_referenced = refs.global_calls.size();
    report.methods_referenced = refs.method_calls.size();
    for (const auto& [name, where] : refs.global_calls) {
        if (!refs.defined.count(name) && !registered_globals.count(name)) {
            report.missing_globals.emplace_back(name, where);
        }
    }
    for (const auto& [name, where] : refs.method_calls) {
        if (!refs.defined.count(name) && !registered_methods.count(name)) {
            report.missing_methods.emplace_back(name, where);
        }
    }
    return report; // std::map iteration: already sorted by name
}

void collect_registered(lua_State* L, std::set<std::string>& globals,
                        std::set<std::string>& methods) {
    auto add_string_keys = [&](int table, std::set<std::string>& into) {
        lua_pushnil(L);
        while (lua_next(L, table) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) into.insert(lua_tostring(L, -2));
            lua_pop(L, 1);
        }
    };

    // Globals, read raw (config.lua makes unknown global reads error).
    add_string_keys(LUA_GLOBALSINDEX, globals);

    // moho.<class>_methods tables hold the engine's methods.
    lua_pushstring(L, "moho");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        const int moho = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, moho) != 0) {
            if (lua_istable(L, -1)) add_string_keys(lua_gettop(L), methods);
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);

    // Engine metatables cached in the registry (__osc_*_mt and friends),
    // including their __index method tables.
    lua_pushnil(L);
    while (lua_next(L, LUA_REGISTRYINDEX) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING && lua_istable(L, -1)) {
            const std::string key = lua_tostring(L, -2);
            if (key.rfind("__osc_", 0) == 0 || key.rfind("osc_", 0) == 0 ||
                key.rfind("__dummy", 0) == 0) {
                const int mt = lua_gettop(L);
                add_string_keys(mt, methods);
                lua_pushstring(L, "__index");
                lua_rawget(L, mt);
                if (lua_istable(L, -1)) add_string_keys(lua_gettop(L), methods);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
}

std::set<std::string> parse_baseline(std::string_view text) {
    std::set<std::string> out;
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        if (auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        size_t start = 0;
        while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start])))
            ++start;
        if (start < line.size()) out.insert(line.substr(start));
    }
    return out;
}

std::string format_baseline(const CoverageReport& report) {
    std::string out;
    for (const auto& [name, where] : report.missing_globals) out += "G " + name + "\n";
    for (const auto& [name, where] : report.missing_methods) out += "M " + name + "\n";
    return out;
}

BaselineDelta diff_against_baseline(const CoverageReport& report,
                                    const std::set<std::string>& baseline) {
    std::set<std::string> now;
    for (const auto& [name, where] : report.missing_globals) now.insert("G " + name);
    for (const auto& [name, where] : report.missing_methods) now.insert("M " + name);
    BaselineDelta delta;
    std::set_difference(now.begin(), now.end(), baseline.begin(), baseline.end(),
                        std::back_inserter(delta.new_gaps));
    std::set_difference(baseline.begin(), baseline.end(), now.begin(), now.end(),
                        std::back_inserter(delta.closed_gaps));
    return delta;
}

int run_coverage_report(lua_State* sim_L, lua_State* ui_L,
                        const vfs::VirtualFileSystem& vfs,
                        const std::string& report_path,
                        const std::string& baseline_path) {
    // Game script roots. Maps and mods are excluded: their scripts are user
    // content, and this report measures the engine against the game.
    static constexpr const char* kRoots[] = {"/lua",         "/schook", "/units",
                                             "/projectiles", "/effects", "/props",
                                             "/env"};
    LuaReferences refs;
    size_t files = 0;
    for (const char* root : kRoots) {
        for (const auto& path : vfs.find_files(root, "*.lua")) {
            auto data = vfs.read_file(path);
            if (!data) continue;
            scan_lua_source(std::string_view(data->data(), data->size()), path, refs);
            ++files;
        }
    }

    // Some engine objects are built on first use (e.g. the AI personality
    // from brain:GetPersonality()); create them so their methods count.
    if (sim_L) {
        const int top = lua_gettop(sim_L);
        const char* materialize =
            "local b = rawget(_G, 'ArmyBrains') and ArmyBrains[1]\n"
            "if b and b.GetPersonality then pcall(b.GetPersonality, b) end\n";
        if (luaL_loadbuffer(sim_L, materialize, std::strlen(materialize),
                            "=coverage") == 0) {
            lua_pcall(sim_L, 0, 0, 0);
        }
        lua_settop(sim_L, top);
    }

    std::set<std::string> globals;
    std::set<std::string> methods;
    if (sim_L) collect_registered(sim_L, globals, methods);
    if (ui_L) collect_registered(ui_L, globals, methods);

    const CoverageReport report = compute_coverage(refs, globals, methods);
    spdlog::info("Binding coverage: {} script files; {} global and {} method names "
                 "called; {} globals and {} methods missing",
                 files, report.globals_referenced, report.methods_referenced,
                 report.missing_globals.size(), report.missing_methods.size());

    std::ofstream out(report_path);
    if (!out) {
        spdlog::error("Binding coverage: cannot write {}", report_path);
        return 1;
    }
    out << "# Engine bindings called by the game's scripts but not provided.\n"
           "# Format: <G|M> <name>   # first call site. Generated by\n"
           "# opensupcom --binding-coverage; G = global function, M = method.\n";
    for (const auto& [name, where] : report.missing_globals)
        out << "G " << name << "   # " << where << "\n";
    for (const auto& [name, where] : report.missing_methods)
        out << "M " << name << "   # " << where << "\n";

    if (baseline_path.empty()) return 0;
    std::ifstream bin(baseline_path);
    if (!bin) {
        spdlog::error("Binding coverage: cannot read baseline {}", baseline_path);
        return 1;
    }
    std::ostringstream text;
    text << bin.rdbuf();
    const auto delta = diff_against_baseline(report, parse_baseline(text.str()));
    for (const auto& gap : delta.closed_gaps) {
        spdlog::info("Binding coverage: closed since baseline: {} (remove it from "
                     "the baseline)", gap);
    }
    for (const auto& gap : delta.new_gaps) {
        spdlog::error("Binding coverage: NEW gap not in baseline: {}", gap);
    }
    return delta.new_gaps.empty() ? 0 : 1;
}

} // namespace osc::lua::coverage
