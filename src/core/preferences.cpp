#include "core/preferences.hpp"

#include "core/lua_copy.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unordered_set>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::core {

namespace {

std::vector<std::string_view> split_path(std::string_view key) {
    std::vector<std::string_view> segs;
    while (!key.empty()) {
        const size_t dot = key.find('.');
        const std::string_view seg = key.substr(0, dot);
        if (!seg.empty()) segs.push_back(seg);
        if (dot == std::string_view::npos) break;
        key.remove_prefix(dot + 1);
    }
    return segs;
}

/// Push the table key a path segment names: a number for digits, else a string.
void push_segment(lua_State* L, std::string_view seg) {
    const bool digits = std::all_of(seg.begin(), seg.end(),
                                    [](char c) { return c >= '0' && c <= '9'; });
    if (digits) {
        lua_pushnumber(L, std::strtod(std::string(seg).c_str(), nullptr));
    } else {
        lua_pushlstring(L, seg.data(), seg.size());
    }
}

bool is_identifier(std::string_view s) {
    static const std::unordered_set<std::string_view> kKeywords = {
        "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in",
        "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
    if (s.empty() || (s[0] >= '0' && s[0] <= '9')) return false;
    const bool chars_ok = std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    });
    return chars_ok && !kKeywords.count(s);
}

bool is_plain(lua_State* L, int idx) {
    const int t = lua_type(L, idx);
    return t == LUA_TNUMBER || t == LUA_TSTRING || t == LUA_TBOOLEAN || t == LUA_TTABLE;
}

/// Serializes plain Lua data as Lua source.
class Writer {
public:
    explicit Writer(lua_State* L) : L_(L) {}

    std::string out;

    /// Write the root table's entries as `name = value` statements.
    void root(int idx) {
        for (const Key& k : sorted_keys(idx)) {
            if (k.type != LUA_TSTRING || !is_identifier(k.str)) {
                spdlog::warn("Preferences: top-level key '{}' is not a name; not saved", k.str);
                continue;
            }
            push_key(k);
            lua_rawget(L_, idx);
            if (is_plain(L_, -1)) {
                out += k.str;
                out += " = ";
                write_value(lua_gettop(L_), 0);
                out += '\n';
            }
            lua_pop(L_, 1);
        }
    }

private:
    struct Key {
        int type = LUA_TNIL;
        lua_Number num = 0;
        std::string str;
        bool boolean = false;
    };

    std::vector<Key> sorted_keys(int idx) {
        std::vector<Key> keys;
        lua_pushnil(L_);
        while (lua_next(L_, idx) != 0) {
            Key k;
            k.type = lua_type(L_, -2);
            if (k.type == LUA_TNUMBER) k.num = lua_tonumber(L_, -2);
            else if (k.type == LUA_TSTRING) k.str.assign(lua_tostring(L_, -2), lua_strlen(L_, -2));
            else if (k.type == LUA_TBOOLEAN) k.boolean = lua_toboolean(L_, -2) != 0;
            if (k.type == LUA_TNUMBER || k.type == LUA_TSTRING || k.type == LUA_TBOOLEAN)
                keys.push_back(std::move(k));
            lua_pop(L_, 1);
        }
        // Numbers, then strings, then booleans: arrays read in order.
        auto rank = [](int t) { return t == LUA_TNUMBER ? 0 : t == LUA_TSTRING ? 1 : 2; };
        std::sort(keys.begin(), keys.end(), [&](const Key& a, const Key& b) {
            if (a.type != b.type) return rank(a.type) < rank(b.type);
            if (a.type == LUA_TNUMBER) return a.num < b.num;
            if (a.type == LUA_TSTRING) return a.str < b.str;
            return !a.boolean && b.boolean;
        });
        return keys;
    }

    void push_key(const Key& k) {
        if (k.type == LUA_TNUMBER) lua_pushnumber(L_, k.num);
        else if (k.type == LUA_TSTRING) lua_pushlstring(L_, k.str.data(), k.str.size());
        else lua_pushboolean(L_, k.boolean ? 1 : 0);
    }

    void write_number(lua_Number v) {
        char buf[64];
        if (std::isnan(v)) {
            out += "0/0";
        } else if (std::isinf(v)) {
            out += v > 0 ? "1/0" : "-1/0";
        } else if (v == 0 && std::signbit(v)) {
            // Neither an integer cast nor a "-0" literal keeps the sign: Lua
            // folds -0 into its constant table, which dedupes it with 0.
            out += "1/(-1/0)";
        } else if (v == std::floor(v) && std::fabs(v) < 9007199254740992.0) {
            std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(v));
            out += buf;
        } else {
            std::snprintf(buf, sizeof buf, "%.17g", v);
            out += buf;
        }
    }

    void write_string(std::string_view s) {
        out += '"';
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            if (c == '"' || c == '\\') {
                out += '\\';
                out += ch;
            } else if (c == '\n') {
                out += "\\n";
            } else if (c < 32 || c == 127) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\%03u", c); // 3 digits: never ambiguous
                out += buf;
            } else {
                out += ch;
            }
        }
        out += '"';
    }

    void write_key(const Key& k) {
        if (k.type == LUA_TSTRING && is_identifier(k.str)) {
            out += k.str;
            return;
        }
        out += '[';
        if (k.type == LUA_TNUMBER) write_number(k.num);
        else if (k.type == LUA_TSTRING) write_string(k.str);
        else out += k.boolean ? "true" : "false";
        out += ']';
    }

    void write_value(int idx, int indent) {
        switch (lua_type(L_, idx)) {
        case LUA_TNUMBER: write_number(lua_tonumber(L_, idx)); return;
        case LUA_TBOOLEAN: out += lua_toboolean(L_, idx) ? "true" : "false"; return;
        case LUA_TSTRING: write_string({lua_tostring(L_, idx), lua_strlen(L_, idx)}); return;
        case LUA_TTABLE: write_table(idx, indent); return;
        default: out += "nil"; return;
        }
    }

    void write_table(int idx, int indent) {
        const void* id = lua_topointer(L_, idx);
        if (!open_.insert(id).second) { // a cycle: Lua source cannot say it
            out += "nil";
            return;
        }
        if (!lua_checkstack(L_, 4)) {
            out += "{}";
            open_.erase(id);
            return;
        }
        const std::string pad(static_cast<size_t>(indent + 1) * 4, ' ');
        out += "{\n";
        for (const Key& k : sorted_keys(idx)) {
            push_key(k);
            lua_rawget(L_, idx);
            if (is_plain(L_, -1)) {
                out += pad;
                write_key(k);
                out += " = ";
                write_value(lua_gettop(L_), indent + 1);
                out += ",\n";
            }
            lua_pop(L_, 1);
        }
        out.append(static_cast<size_t>(indent) * 4, ' ');
        out += '}';
        open_.erase(id);
    }

    lua_State* L_;
    std::unordered_set<const void*> open_; // tables being written (cycle guard)
};

} // namespace

Preferences::Preferences() : L_(lua_open()) {
    lua_newtable(L_);
    root_ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
}

Preferences::~Preferences() {
    if (L_) lua_close(L_);
}

bool Preferences::load(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::string src{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const std::string chunk_name = "@" + path.string();
    if (luaL_loadbuffer(L_, src.data(), src.size(), chunk_name.c_str()) != 0) {
        spdlog::warn("Preferences: cannot parse {}: {}", path.string(), lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    // The chunk runs with an empty table as its globals: its assignments
    // are the preferences, and it can call nothing.
    lua_newtable(L_);
    lua_pushvalue(L_, -1);
    lua_insert(L_, -3); // env, chunk, env
    lua_setfenv(L_, -2);
    if (lua_pcall(L_, 0, 0, 0) != 0) {
        spdlog::warn("Preferences: cannot load {}: {}", path.string(), lua_tostring(L_, -1));
        lua_pop(L_, 2); // error, env
        return false;
    }
    luaL_unref(L_, LUA_REGISTRYINDEX, root_ref_);
    root_ref_ = luaL_ref(L_, LUA_REGISTRYINDEX); // pops env
    spdlog::info("Preferences: loaded {}", path.string());
    return true;
}

bool Preferences::save(const std::filesystem::path& path) const {
    Writer writer(L_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, root_ref_);
    writer.root(lua_gettop(L_));
    lua_pop(L_, 1);

    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    // Write a sibling, then rename over: a crash mid-write keeps the old file.
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            spdlog::warn("Preferences: cannot write {}", tmp.string());
            return false;
        }
        out << writer.out;
        if (!out.flush()) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        spdlog::warn("Preferences: cannot replace {}: {}", path.string(), ec.message());
        return false;
    }
    return true;
}

void Preferences::walk(std::string_view key) const {
    lua_rawgeti(L_, LUA_REGISTRYINDEX, root_ref_);
    for (const auto seg : split_path(key)) {
        if (!lua_istable(L_, -1)) {
            lua_pop(L_, 1);
            lua_pushnil(L_);
            return;
        }
        push_segment(L_, seg);
        lua_rawget(L_, -2);
        lua_remove(L_, -2);
    }
}

void Preferences::assign(std::string_view key) {
    const auto segs = split_path(key);
    const int value = lua_gettop(L_);
    if (segs.empty()) { // the root itself is not replaceable
        lua_settop(L_, value - 1);
        return;
    }
    lua_rawgeti(L_, LUA_REGISTRYINDEX, root_ref_);
    for (size_t i = 0; i + 1 < segs.size(); ++i) {
        push_segment(L_, segs[i]);
        lua_rawget(L_, -2);
        if (!lua_istable(L_, -1)) {
            lua_pop(L_, 1);
            lua_newtable(L_);
            push_segment(L_, segs[i]);
            lua_pushvalue(L_, -2);
            lua_rawset(L_, -4); // parent[seg] = the new table
        }
        lua_remove(L_, -2); // parent
    }
    push_segment(L_, segs.back());
    lua_pushvalue(L_, value);
    lua_rawset(L_, -3);
    lua_settop(L_, value - 1);
}

void Preferences::push(std::string_view key, lua_State* L) const {
    walk(key);
    copy_lua_value(L_, -1, L);
    lua_pop(L_, 1);
}

void Preferences::set(std::string_view key, lua_State* L, int idx) {
    if (idx < 0) idx = lua_gettop(L) + idx + 1;
    copy_lua_value(L, idx, L_);
    assign(key);
}

std::string Preferences::current_profile_path() const {
    walk("profile.current");
    const bool numeric = lua_type(L_, -1) == LUA_TNUMBER;
    const lua_Number current = numeric ? lua_tonumber(L_, -1) : 0;
    lua_pop(L_, 1);
    if (!numeric || current != std::floor(current)) return {};
    std::string path = "profile.profiles." + std::to_string(static_cast<long long>(current));
    walk(path);
    const bool exists = lua_istable(L_, -1);
    lua_pop(L_, 1);
    return exists ? path : std::string{};
}

void Preferences::push_option(std::string_view key, lua_State* L) const {
    std::string path = current_profile_path();
    if (path.empty()) {
        lua_pushnil(L);
        return;
    }
    path += ".options";
    if (!key.empty()) {
        path += '.';
        path += key;
    }
    push(path, L);
}

bool Preferences::ensure_profile(std::string_view name) {
    if (!current_profile_path().empty()) return false;
    walk("profile.profiles.1");
    const bool first_exists = lua_istable(L_, -1);
    lua_pop(L_, 1);
    if (!first_exists) {
        lua_newtable(L_); // profiles
        lua_newtable(L_); // profiles[1]
        lua_pushstring(L_, "Name");
        lua_pushlstring(L_, name.data(), name.size());
        lua_rawset(L_, -3);
        lua_rawseti(L_, -2, 1);
        assign("profile.profiles");
    }
    lua_pushnumber(L_, 1);
    assign("profile.current");
    return true;
}

std::string Preferences::get_string(std::string_view key, const std::string& def) const {
    walk(key);
    std::string v = lua_type(L_, -1) == LUA_TSTRING
                        ? std::string(lua_tostring(L_, -1), lua_strlen(L_, -1))
                        : def;
    lua_pop(L_, 1);
    return v;
}

float Preferences::get_float(std::string_view key, float def) const {
    walk(key);
    const float v = lua_type(L_, -1) == LUA_TNUMBER ? static_cast<float>(lua_tonumber(L_, -1)) : def;
    lua_pop(L_, 1);
    return v;
}

bool Preferences::get_bool(std::string_view key, bool def) const {
    walk(key);
    const bool v = lua_type(L_, -1) == LUA_TBOOLEAN ? lua_toboolean(L_, -1) != 0 : def;
    lua_pop(L_, 1);
    return v;
}

int Preferences::get_int(std::string_view key, int def) const {
    walk(key);
    const int v = lua_type(L_, -1) == LUA_TNUMBER ? static_cast<int>(lua_tonumber(L_, -1)) : def;
    lua_pop(L_, 1);
    return v;
}

void Preferences::set_string(std::string_view key, const std::string& val) {
    lua_pushlstring(L_, val.data(), val.size());
    assign(key);
}

void Preferences::set_float(std::string_view key, float val) {
    lua_pushnumber(L_, val);
    assign(key);
}

void Preferences::set_bool(std::string_view key, bool val) {
    lua_pushboolean(L_, val ? 1 : 0);
    assign(key);
}

void Preferences::set_int(std::string_view key, int val) {
    lua_pushnumber(L_, val);
    assign(key);
}

} // namespace osc::core
