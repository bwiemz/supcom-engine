// opensupcom --check-mods and --list-mods (see mod_check.hpp).

#include "app/mod_check.hpp"

#include "audio/xact/sound_bank.hpp"
#include "audio/xwb_parser.hpp"
#include "blueprints/blueprint_store.hpp"
#include "blueprints/moho_reflected_fields.hpp"
#include "core/types.hpp"
#include "lua/blueprint_bindings.hpp"
#include "lua/engine_bindings.hpp"
#include "lua/init_loader.hpp"
#include "lua/lua_state.hpp"
#include "lua/script_loader.hpp"
#include "renderer/mesh_cache.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"
#include "vfs/zip_mount.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::app::mods {

namespace {

using vfs::VirtualFileSystem;

constexpr const char* kDefaultHookDir = "/hook";

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Dirname, as the binding: the path up to its last separator.
std::string dirname(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string extension(const std::string& path) {
    const size_t slash = path.rfind('/');
    const size_t dot = path.rfind('.');
    return dot == std::string::npos || (slash != std::string::npos && dot < slash)
               ? std::string()
               : lower(path.substr(dot));
}

std::span<const u8> bytes_of(const std::vector<char>& data) {
    return {reinterpret_cast<const u8*>(data.data()), data.size()};
}

/// The edit distance between two names (a misspelt hook's nearest file).
size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t above = row[j];
            const size_t change = a[i - 1] == b[j - 1] ? 0 : 1;
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + change});
            diagonal = above;
        }
    }
    return row[b.size()];
}

/// A Lua error message, "<chunk>:<line>: <text>", as its line and text when
/// its place is in `chunk` (Lua shortens a long name to its end: "...x.lua");
/// else line 0 and the whole message.
struct ErrorAt {
    int line = 0;
    std::string text;
};

ErrorAt split_error(const std::string& message, std::string_view chunk) {
    for (size_t colon = message.find(':'); colon != std::string::npos;
         colon = message.find(':', colon + 1)) {
        size_t end = colon + 1;
        while (end < message.size() && std::isdigit(static_cast<unsigned char>(message[end])))
            ++end;
        if (end == colon + 1 || end >= message.size() || message[end] != ':') continue;
        const std::string_view id(message.data(), colon);
        const bool here = id == chunk || (id.starts_with("...") && chunk.ends_with(id.substr(3)));
        if (!here) break;
        size_t text = end + 1;
        if (text < message.size() && message[text] == ' ') ++text;
        return {std::stoi(message.substr(colon + 1, end - colon - 1)), message.substr(text)};
    }
    return {0, message};
}

/// A script's chunk as doscript builds it, a file after another, knowing
/// which lines are whose (an error's line is the chunk's).
class Chunk {
public:
    void add(const std::string& file, const std::vector<char>& data) {
        const size_t before = text_.size();
        lua::append_script_file(text_, data);
        const auto lines = static_cast<int>(
            std::count(text_.begin() + static_cast<std::ptrdiff_t>(before), text_.end(), '\n'));
        if (lines > 0) {
            parts_.push_back({file, next_line_, lines});
            next_line_ += lines;
        }
    }
    const std::string& text() const { return text_; }
    /// The file the chunk's line `line` is in, and its line there (a line
    /// past the end is the last file's last).
    std::pair<std::string, int> at(int line) const {
        for (const Part& p : parts_) {
            if (line >= p.first && line < p.first + p.lines) return {p.file, line - p.first + 1};
        }
        if (parts_.empty()) return {{}, 0};
        return {parts_.back().file, parts_.back().lines};
    }
    /// The file of its last lines ("" when it has none).
    std::string last_file() const { return parts_.empty() ? std::string() : parts_.back().file; }

private:
    struct Part {
        std::string file;
        int first;
        int lines;
    };
    std::string text_;
    std::vector<Part> parts_;
    int next_line_ = 1;
};

/// Compile (not run) `text` as the chunk "@<name>": "" or Lua's message.
std::string compile_error(lua_State* L, const std::string& text, const std::string& name) {
    const std::string chunk_name = "@" + name;
    if (luaL_loadbuffer(L, text.data(), text.size(), chunk_name.c_str()) == 0) {
        lua_pop(L, 1);
        return {};
    }
    std::string message = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return message.empty() ? std::string("(no message)") : message;
}

/// Whether `text` ends in a `return` (or `break`): nothing may follow it.
bool ends_in_return(lua_State* L, const std::string& text, const std::string& name) {
    return compile_error(L, text, name).empty() &&
           !compile_error(L, text + "do end\n", name).empty();
}

void push_field(lua_State* L, int t, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, t);
}

/// t[key] when it is a string.
std::optional<std::string> string_at(lua_State* L, int t, const char* key) {
    push_field(L, t, key);
    std::optional<std::string> out;
    if (lua_type(L, -1) == LUA_TSTRING) out.emplace(lua_tostring(L, -1), lua_strlen(L, -1));
    lua_pop(L, 1);
    return out;
}

void set_string(lua_State* L, int t, const char* key, const std::string& value) {
    lua_pushstring(L, key);
    lua_pushlstring(L, value.data(), value.size());
    lua_rawset(L, t);
}

void set_bool(lua_State* L, int t, const char* key, bool value) {
    lua_pushstring(L, key);
    lua_pushboolean(L, value ? 1 : 0);
    lua_rawset(L, t);
}

/// The string values of the table on top of the stack, sorted; `others`
/// counts its values that aren't strings.
std::vector<std::string> string_values(lua_State* L, int& others) {
    std::vector<std::string> out;
    const int t = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, t) != 0) {
        if (lua_type(L, -1) == LUA_TSTRING) out.emplace_back(lua_tostring(L, -1));
        else ++others;
        lua_pop(L, 1);
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// The string keys of t[key] when it is a table (a set, as GetDependencies
/// answers), sorted.
std::vector<std::string> set_keys(lua_State* L, int t, const char* key) {
    std::vector<std::string> out;
    push_field(L, t, key);
    if (lua_istable(L, -1)) {
        const int set = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, set) != 0) {
            lua_pop(L, 1);
            if (lua_type(L, -1) == LUA_TSTRING) out.emplace_back(lua_tostring(L, -1));
        }
    }
    lua_pop(L, 1);
    std::sort(out.begin(), out.end());
    return out;
}

/// The environment a mod's .bp runs in: a constructor records each blueprint
/// the file makes in `found` ({kind, bp}), the rest is the state's globals,
/// as a blueprint's file sees them in the game. FAF's .bp files may set a
/// function over all blueprints (SetModBlueprintFunction): kept out.
constexpr const char* kBlueprintEnv = R"lua(
return function(found, globals)
    local env = {}
    for _, kind in ipairs({ 'Unit', 'Prop', 'Projectile', 'Mesh', 'TrailEmitter', 'Emitter', 'Beam' }) do
        local k = kind
        env[k .. 'Blueprint'] = function(bp)
            table.insert(found, { kind = k, bp = bp })
            return bp
        end
    end
    env.SetModBlueprintFunction = function(f) return f end
    return setmetatable(env, { __index = globals })
end
)lua";

/// A mesh LOD's files. Of those it leaves out, only a mesh is missed: the
/// engine then takes <source prefix>_lod<i>.scm (RMeshBlueprintLOD::Init),
/// and the textures' stand-ins are optional.
constexpr const char* kLodFiles[] = {"MeshName",     "AlbedoName", "NormalsName",
                                     "SpecularName", "LookupName", "SecondaryName"};

/// The texture fields of the effect blueprints, as the renderer reads them.
struct EffectTexture {
    const char* kind;
    const char* field;
};
constexpr EffectTexture kEffectTextures[] = {
    {"Emitter", "Texture"},
    {"Emitter", "TextureName"},
    {"Emitter", "RampTexture"},
    {"Emitter", "RampTextureName"},
    {"TrailEmitter", "RepeatTexture"},
    {"TrailEmitter", "RampTexture"},
    {"Beam", "TextureName"},
};

class Checker {
public:
    explicit Checker(const VirtualFileSystem& vfs) : vfs_(vfs) {
        // The game's blueprint state, as load_blueprints makes it: import,
        // and the globals a blueprint's file sees (footprints.lua's
        // SpecFootprints is the store's)
        lua::register_init_bindings(state_);
        state_.set_vfs(const_cast<VirtualFileSystem*>(&vfs));
        state_.set_blueprint_store(&store_);
        lua::register_blueprint_bindings(state_);
        lua::register_blueprint_store_bindings(state_);
        if (auto ran = lua::run_system_scripts(state_, vfs); !ran)
            spdlog::warn("check-mods: {}", ran.error().message);
        // A player may check a mod before trusting it, and the checker runs
        // its mod_info.lua and blueprints: they get no file access here
        // (the game's scripts reach the data through the VFS, never io).
        lua_State* L = state_.raw();
        for (const char* name : {"io", "dofile", "loadfile"}) {
            lua_pushstring(L, name);
            lua_pushnil(L);
            lua_rawset(L, LUA_GLOBALSINDEX);
        }
    }
    ~Checker() {
        if (dependencies_ != LUA_NOREF) luaL_unref(state_.raw(), LUA_REGISTRYINDEX, dependencies_);
        if (blueprint_env_ != LUA_NOREF)
            luaL_unref(state_.raw(), LUA_REGISTRYINDEX, blueprint_env_);
    }
    Checker(const Checker&) = delete;
    Checker& operator=(const Checker&) = delete;

    std::vector<ModReport> read_all() {
        std::vector<std::string> files = vfs_.find_files("/mods", "*mod_info.lua");
        std::sort(files.begin(), files.end());
        std::vector<ModReport> mods;
        mods.reserve(files.size());
        for (const std::string& file : files) mods.push_back(read(file));
        std::stable_sort(mods.begin(), mods.end(), [](const ModReport& a, const ModReport& b) {
            return a.mod.location < b.mod.location;
        });
        return mods;
    }

    /// Each uid more than one enabled mod has.
    static void check_duplicates(std::vector<ModReport>& mods) {
        std::map<std::string, std::vector<size_t>> by_uid;
        for (size_t i = 0; i < mods.size(); ++i) {
            const ModInfo& m = mods[i].mod;
            if (m.readable && m.enabled && !m.uid.empty()) by_uid[m.uid].push_back(i);
        }
        for (const auto& [uid, which] : by_uid) {
            if (which.size() < 2) continue;
            for (const size_t i : which) {
                std::string others;
                for (const size_t j : which) {
                    if (j == i) continue;
                    others += (others.empty() ? "" : ", ") + mods[j].mod.info_file;
                }
                add(mods[i], Severity::Error, "duplicate_uid", mods[i].mod.info_file, 0,
                    fmt::format("uid {} is also {}'s: the mod manager keeps only one mod of a uid",
                                uid, others));
            }
        }
    }

    void check_dependencies(ModReport& r, const std::vector<ModReport>& all) {
        const ModInfo& m = r.mod;
        if (!m.readable || m.uid.empty()) return;
        std::map<std::string, const ModInfo*> installed;
        for (const ModReport& other : all) {
            if (other.mod.readable && other.mod.enabled && !other.mod.uid.empty())
                installed.emplace(other.mod.uid, &other.mod);
        }
        std::vector<std::string> missing;
        std::vector<std::string> conflicts;
        if (!data_dependencies(m, missing, conflicts)) {
            // No GetDependencies in the data's mods.lua: retail's rule, by hand
            for (const std::string& uid : m.required)
                if (!installed.contains(uid)) missing.push_back(uid);
            std::set<std::string> found;
            for (const std::string& uid : m.conflicting)
                if (installed.contains(uid)) found.insert(uid);
            for (const auto& [uid, other] : installed) {
                if (std::find(other->conflicting.begin(), other->conflicting.end(), m.uid) !=
                    other->conflicting.end())
                    found.insert(uid);
            }
            conflicts.assign(found.begin(), found.end());
        }
        for (const std::string& uid : missing) {
            const auto named = m.required_names.find(uid);
            const std::string label =
                named == m.required_names.end() ? uid : uid + " (" + named->second + ")";
            add(r, Severity::Warning, "requires", m.info_file, 0,
                "requires " + label + ", which isn't installed: the mod manager won't enable it");
        }
        for (const std::string& uid : conflicts) {
            if (uid == m.uid) continue;
            const ModInfo* other = installed.contains(uid) ? installed.at(uid) : nullptr;
            // FAF's GetDependencies makes an exclusive mod conflict with
            // every other: only those a list names are news
            if (m.exclusive && !declared_conflict(m, uid, other)) continue;
            add(r, Severity::Warning, "conflicts", m.info_file, 0,
                "conflicts with " + uid + (other ? " (" + other->name + ")" : "") +
                    ", which is installed: the mod manager won't enable the two together");
        }
    }

    void check_hooks(ModReport& r) {
        const ModInfo& m = r.mod;
        const std::string hookdir = m.hookdir.empty() ? kDefaultHookDir : m.hookdir;
        const std::string dir = VirtualFileSystem::normalize(m.location + hookdir);
        lua_State* L = state_.raw();
        set_active_mods(&m);
        std::vector<std::string> hooks = vfs_.find_files(dir, "*");
        std::sort(hooks.begin(), hooks.end());
        for (const std::string& hook : hooks) {
            const std::string target = hook.substr(dir.size());
            const std::string ext = extension(target);
            if (ext != ".lua" && ext != ".bp") {
                add(r, Severity::Warning, "hook_not_script", hook, 0,
                    "isn't a script: hooks join scripts (.lua, .bp) as they run, so " + target +
                        " is never changed by it");
                continue;
            }
            if (!vfs_.file_exists(target)) {
                // Another mod's file is there only when that mod is
                // installed: a warning, as a requirement missing is
                if (target.starts_with("/mods/") && !target.starts_with(m.location + "/")) {
                    add(r, Severity::Warning, "hook_target", hook, 0,
                        "hooks " + target +
                            ", another mod's script, which isn't installed: it never runs");
                    continue;
                }
                const std::string near = nearest_file(target);
                add(r, Severity::Error, "hook_target", hook, 0,
                    "hooks " + target + ", which no script is, so it never runs" +
                        (near.empty() ? std::string() : " (did you mean " + near + "?)"));
                continue;
            }
            check_hook_chunk(r, L, target, hook);
        }
        set_active_mods(nullptr);
    }

    void check_blueprints(ModReport& r) {
        const ModInfo& m = r.mod;
        lua_State* L = state_.raw();
        set_active_mods(&m);
        // Blueprints.lua loads every .bp under the mod's folder, those in its
        // hook folder too: they are blueprints of their own as well
        const std::string hooks =
            VirtualFileSystem::normalize(m.location +
                                         (m.hookdir.empty() ? kDefaultHookDir : m.hookdir)) +
            "/";
        std::vector<std::string> files = vfs_.find_files(m.location, "*.bp");
        std::sort(files.begin(), files.end());
        for (const std::string& file : files) {
            const auto data = vfs_.read_file(file);
            if (!data) continue;
            const std::string skipped = file.starts_with(hooks)
                                            ? " (the game loads a .bp in the hook folder as a "
                                              "blueprint of its own too, and skips it)"
                                            : " (the game skips it)";
            Chunk chunk;
            chunk.add(file, *data);
            if (const std::string error = compile_error(L, chunk.text(), file); !error.empty()) {
                const ErrorAt at = split_error(error, file);
                add(r, Severity::Error, "blueprint_compile", file, at.line,
                    "doesn't compile: " + at.text + skipped);
                continue;
            }
            const int top = lua_gettop(L);
            lua_newtable(L);
            const int found = lua_gettop(L);
            if (!push_blueprint_env(found)) {
                lua_settop(L, top);
                continue;
            }
            const int env = lua_gettop(L);
            if (auto ran = lua::run_vfs_script(L, file, env); !ran) {
                const ErrorAt at = split_error(ran.error().message, file);
                add(r, Severity::Error, "blueprint_run", file, at.line,
                    "fails as it runs: " + at.text + skipped);
                lua_settop(L, top);
                continue;
            }
            for (int i = 1;; ++i) {
                lua_rawgeti(L, found, i);
                if (!lua_istable(L, -1)) {
                    lua_pop(L, 1);
                    break;
                }
                const int entry = lua_gettop(L);
                const std::string kind = string_at(L, entry, "kind").value_or("");
                push_field(L, entry, "bp");
                if (lua_istable(L, -1)) check_blueprint(r, file, kind, lua_gettop(L));
                lua_settop(L, entry - 1);
            }
            lua_settop(L, top);
        }
        set_active_mods(nullptr);
    }

    void check_sounds(ModReport& r) {
        const std::string dir = r.mod.location + "/sounds";
        std::vector<std::string> banks = vfs_.find_files(dir, "*.xsb");
        std::vector<std::string> waves = vfs_.find_files(dir, "*.xwb");
        std::sort(banks.begin(), banks.end());
        std::sort(waves.begin(), waves.end());
        for (const std::string& file : banks) {
            const auto data = vfs_.read_file(file);
            if (!data) continue;
            if (auto bank = audio::xact::SoundBank::parse(bytes_of(*data)); !bank)
                add(r, Severity::Error, "sound_bank", file, 0,
                    "doesn't parse as an XACT sound bank: " + bank.error().message);
        }
        for (const std::string& file : waves) {
            const auto data = vfs_.read_file(file);
            if (!data) continue;
            if (auto header = audio::XwbParser::check_header(bytes_of(*data)); !header)
                add(r, Severity::Error, "wave_bank", file, 0,
                    "doesn't parse as an XACT wave bank: " + header.error().message);
        }
    }

private:
    const VirtualFileSystem& vfs_;
    lua::LuaState state_;
    blueprints::BlueprintStore store_{state_.raw()};
    int dependencies_ = LUA_NOREF; ///< the data's GetDependencies
    bool dependencies_loaded_ = false;
    int blueprint_env_ = LUA_NOREF; ///< kBlueprintEnv's maker

    static void add(ModReport& r, Severity severity, const char* check, const std::string& file,
                    int line, std::string message) {
        r.findings.push_back({severity, check, file, line, std::move(message)});
    }

    /// L's __active_mods: `mod` alone, or none.
    void set_active_mods(const ModInfo* mod) {
        lua_State* L = state_.raw();
        lua_pushstring(L, "__active_mods");
        lua_newtable(L);
        if (mod) {
            lua_newtable(L);
            const int entry = lua_gettop(L);
            set_string(L, entry, "location", mod->location);
            set_string(L, entry, "hookdir", mod->hookdir);
            set_string(L, entry, "uid", mod->uid);
            lua_rawseti(L, -2, 1);
        }
        lua_rawset(L, LUA_GLOBALSINDEX);
    }

    /// The mod_info.lua `file`, run as LoadModInfo runs it: in a table of
    /// defaults (doscript(filename, env)).
    ModReport read(const std::string& file) {
        ModReport r;
        ModInfo& m = r.mod;
        m.info_file = file;
        m.location = dirname(file);
        m.name = file;
        lua_State* L = state_.raw();
        const int top = lua_gettop(L);
        set_active_mods(nullptr);
        lua_newtable(L);
        const int env = lua_gettop(L);
        set_string(L, env, "location", m.location);
        set_string(L, env, "name", file);
        set_string(L, env, "description", "<LOC uimod_0006>(No description)");
        set_string(L, env, "author", "");
        set_string(L, env, "copyright", "");
        set_bool(L, env, "exclusive", false);
        set_string(L, env, "icon", "/textures/ui/common/dialogs/mod-manager/generic-icon_bmp.dds");
        set_bool(L, env, "selectable", true);
        set_string(L, env, "hookdir", kDefaultHookDir);
        set_string(L, env, "shadowdir", "/shadow");
        set_string(L, env, "uid", file);
        if (auto ran = lua::run_vfs_script(L, file, env); !ran) {
            const ErrorAt at = split_error(ran.error().message, file);
            add(r, Severity::Error, "mod_info", file, at.line,
                "doesn't run: " + at.text + " (the mod manager leaves the mod out)");
            lua_settop(L, top);
            return r;
        }
        m.readable = true;
        read_fields(r, env);
        lua_settop(L, top);
        return r;
    }

    void read_fields(ModReport& r, int env) {
        ModInfo& m = r.mod;
        lua_State* L = state_.raw();
        if (auto name = string_at(L, env, "name")) m.name = *name;

        push_field(L, env, "uid");
        const int uid_type = lua_type(L, -1);
        if (uid_type == LUA_TSTRING) m.uid = lua_tostring(L, -1);
        lua_pop(L, 1);
        if (uid_type != LUA_TSTRING) {
            add(r, Severity::Error, "uid", m.info_file, 0,
                std::string("uid is ") +
                    (uid_type == LUA_TNIL ? std::string("nil")
                                          : std::string("a ") + lua_typename(L, uid_type)) +
                    ", not a string: the mod manager keys and sorts mods by their uids");
        } else if (m.uid == m.info_file || m.uid.empty()) {
            add(r, Severity::Warning, "uid", m.info_file, 0,
                std::string("has no uid, so its uid is ") +
                    (m.uid.empty() ? "empty" : "its file's path") +
                    ": give it a unique one (a GUID), as FAF's vault requires");
        }

        push_field(L, env, "version");
        const int version_type = lua_type(L, -1);
        if (version_type == LUA_TNUMBER) {
            const double v = lua_tonumber(L, -1);
            std::ostringstream text;
            text << v;
            m.version = text.str();
            if (v != std::floor(v))
                add(r, Severity::Warning, "version", m.info_file, 0,
                    "version " + m.version + " isn't a whole number, as FAF's vault requires");
        } else if (version_type != LUA_TNIL) {
            if (version_type == LUA_TSTRING) m.version = lua_tostring(L, -1);
            add(r, Severity::Warning, "version", m.info_file, 0,
                std::string("version is a ") + lua_typename(L, version_type) +
                    (version_type == LUA_TSTRING ? " ('" + m.version + "')" : std::string()) +
                    ", not a number: FAF's vault requires a whole number");
        }
        lua_pop(L, 1);

        // As the engine's hook lister reads it (LuaPlus's ToString: a number
        // converts); "" is /hook
        push_field(L, env, "hookdir");
        m.hookdir = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        if (!m.hookdir.empty() && m.hookdir.front() != '/') {
            add(r, Severity::Error, "hookdir", m.info_file, 0,
                "hookdir '" + m.hookdir + "' doesn't start with '/': hooks are looked for in " +
                    m.location + m.hookdir + ", so none of the mod's run");
        }

        push_field(L, env, "ui_only");
        m.ui_only = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        push_field(L, env, "exclusive");
        m.exclusive = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        push_field(L, env, "enabled"); // AllMods: `mod.enabled != false`
        m.enabled = !(lua_type(L, -1) == LUA_TBOOLEAN && lua_toboolean(L, -1) == 0);
        lua_pop(L, 1);

        m.required = uid_list(r, env, "requires");
        m.conflicting = uid_list(r, env, "conflicts");
        push_field(L, env, "requiresNames");
        if (lua_istable(L, -1)) {
            const int names = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, names) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING)
                    m.required_names.emplace(lua_tostring(L, -2), lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }

    /// The uids of the list t[key] (requires, conflicts).
    std::vector<std::string> uid_list(ModReport& r, int env, const char* key) {
        lua_State* L = state_.raw();
        push_field(L, env, key);
        std::vector<std::string> out;
        int others = 0;
        if (lua_istable(L, -1)) {
            out = string_values(L, others);
        } else if (!lua_isnil(L, -1)) {
            ++others;
        }
        lua_pop(L, 1);
        if (others > 0)
            add(r, Severity::Warning, "dependency_list", r.mod.info_file, 0,
                std::string(key) +
                    " isn't a list of uids (strings): the mod manager can't read it");
        return out;
    }

    static bool declared_conflict(const ModInfo& m, const std::string& uid, const ModInfo* other) {
        const auto lists = [](const ModInfo& a, const std::string& b) {
            return std::find(a.conflicting.begin(), a.conflicting.end(), b) != a.conflicting.end();
        };
        return lists(m, uid) || (other && lists(*other, m.uid));
    }

    /// The data's GetDependencies(uid), as the mod manager asks it: false
    /// when the data's mods.lua has none (or it fails).
    bool data_dependencies(const ModInfo& m, std::vector<std::string>& missing,
                           std::vector<std::string>& conflicts) {
        lua_State* L = state_.raw();
        if (!dependencies_loaded_) load_dependencies();
        if (dependencies_ == LUA_NOREF) return false;
        const int top = lua_gettop(L);
        set_active_mods(nullptr);
        lua_rawgeti(L, LUA_REGISTRYINDEX, dependencies_);
        lua_pushlstring(L, m.uid.data(), m.uid.size());
        if (lua_pcall(L, 1, 1, 0) != 0 || !lua_istable(L, -1)) {
            spdlog::warn("check-mods: GetDependencies('{}') failed: {}", m.uid,
                         lua_isstring(L, -1) ? lua_tostring(L, -1) : "no table");
            lua_settop(L, top);
            return false;
        }
        const int result = lua_gettop(L);
        missing = set_keys(L, result, "missing");
        conflicts = set_keys(L, result, "conflicts");
        lua_settop(L, top);
        return true;
    }

    /// The data's /lua/mods.lua, imported as the game imports it (run as a
    /// module where the data has no import), and its GetDependencies.
    void load_dependencies() {
        dependencies_loaded_ = true;
        if (!vfs_.file_exists("/lua/mods.lua")) return;
        lua_State* L = state_.raw();
        const int top = lua_gettop(L);
        set_active_mods(nullptr);
        lua_pushstring(L, "import");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_isfunction(L, -1)) {
            lua_pushstring(L, "/lua/mods.lua");
            if (lua_pcall(L, 1, 1, 0) != 0) {
                spdlog::warn("check-mods: importing /lua/mods.lua failed: {}",
                             lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
                lua_settop(L, top);
                return;
            }
        } else {
            lua_pop(L, 1);
            lua_newtable(L);
            const int module = lua_gettop(L);
            lua_newtable(L);
            lua_pushstring(L, "__index");
            lua_pushvalue(L, LUA_GLOBALSINDEX);
            lua_rawset(L, -3);
            lua_setmetatable(L, module);
            if (auto ran = lua::run_vfs_script(L, "/lua/mods.lua", module); !ran) {
                spdlog::warn("check-mods: running /lua/mods.lua failed: {}", ran.error().message);
                lua_settop(L, top);
                return;
            }
        }
        if (lua_istable(L, -1)) {
            push_field(L, lua_gettop(L), "GetDependencies");
            if (lua_isfunction(L, -1)) {
                dependencies_ = luaL_ref(L, LUA_REGISTRYINDEX);
            }
        }
        lua_settop(L, top);
        spdlog::info("check-mods: {}", dependencies_ != LUA_NOREF
                                           ? "dependencies by /lua/mods.lua's GetDependencies"
                                           : "/lua/mods.lua has no GetDependencies");
    }

    /// The file in `target`'s folder whose name is nearest its (a misspelt
    /// hook's), or "".
    std::string nearest_file(const std::string& target) const {
        const std::string dir = dirname(target);
        const std::string name = target.substr(dir.size() + 1);
        std::string best;
        size_t best_distance = std::max<size_t>(2, name.size() / 4) + 1;
        for (const std::string& file : vfs_.find_files(dir.empty() ? "/" : dir, "*")) {
            if (dirname(file) != dir) continue; // below it
            const size_t d = edit_distance(name, file.substr(dirname(file).size() + 1));
            if (d < best_distance || (d == best_distance && !best.empty() && file < best)) {
                if (d < best_distance) best_distance = d;
                best = file;
            }
        }
        return best;
    }

    /// The script `target` with its hooks (the init file's, then `hook`, the
    /// mod's), compiled as doscript would load them.
    void check_hook_chunk(ModReport& r, lua_State* L, const std::string& target,
                          const std::string& hook) {
        const auto base = vfs_.read_file(target);
        if (!base) return;
        Chunk before; // the script and the init file's hooks
        Chunk chunk;  // ...and the mod's
        before.add(target, *base);
        chunk.add(target, *base);
        for (const std::string& h : lua::script_hooks(L, vfs_, target)) {
            const auto data = vfs_.read_file(h);
            if (!data) continue;
            chunk.add(h, *data);
            if (h != hook) before.add(h, *data);
        }
        const std::string error = compile_error(L, chunk.text(), target);
        if (error.empty()) {
            if (ends_in_return(L, chunk.text(), target) && chunk.last_file() == hook &&
                !ends_in_return(L, before.text(), target))
                add(r, Severity::Warning, "hook_ends_in_return", hook, 0,
                    "ends in a return statement: another mod's hook of " + target +
                        " can't follow it");
            return;
        }
        if (ends_in_return(L, before.text(), target)) {
            const std::string last = before.last_file();
            add(r, Severity::Error, "hook_after_return", hook, 0,
                "hooks " + target + ", but " + last +
                    " ends in a return statement, which nothing may follow: with the hook, " +
                    target + " doesn't load");
            return;
        }
        const ErrorAt at = split_error(error, target);
        auto [file, line] = at.line > 0 ? chunk.at(at.line) : std::pair{hook, 0};
        add(r, Severity::Error, "hook_compile", file, line,
            "doesn't compile: " + at.text +
                (file == hook ? std::string() : " (with the hook " + hook + ")") + "; " + target +
                " doesn't load with it");
    }

    /// kBlueprintEnv for a file, recording in the table at `found`: pushed,
    /// or false.
    bool push_blueprint_env(int found) {
        lua_State* L = state_.raw();
        if (blueprint_env_ == LUA_NOREF) {
            if (luaL_loadbuffer(L, kBlueprintEnv, std::char_traits<char>::length(kBlueprintEnv),
                                "=check-mods") != 0 ||
                lua_pcall(L, 0, 1, 0) != 0) {
                spdlog::error("check-mods: {}", lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
                lua_pop(L, 1);
                return false;
            }
            blueprint_env_ = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, blueprint_env_);
        lua_pushvalue(L, found);
        lua_pushvalue(L, LUA_GLOBALSINDEX);
        if (lua_pcall(L, 2, 1, 0) != 0 || !lua_istable(L, -1)) {
            lua_pop(L, 1);
            return false;
        }
        return true;
    }

    /// A blueprint `file` made, at stack index `bp`, of `kind` (Unit...).
    void check_blueprint(ModReport& r, const std::string& file, const std::string& kind, int bp) {
        lua_State* L = state_.raw();
        const std::string source = string_at(L, bp, "Source").value_or(file);
        push_field(L, bp, "Merge");
        const bool merge = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        if (kind == "Mesh") {
            check_lods(r, file, bp, source, merge);
            return;
        }
        if (kind == "Unit" || kind == "Prop" || kind == "Projectile") {
            push_field(L, bp, "Display");
            if (lua_istable(L, -1)) {
                const int display = lua_gettop(L);
                const auto reference = string_at(L, display, "MeshBlueprint");
                push_field(L, display, "Mesh");
                if (lua_istable(L, -1)) {
                    if (reference && !reference->empty()) {
                        add(r, Severity::Warning, "mesh_twice", file, 0,
                            "a " + kind +
                                " blueprint has a mesh inline (Display.Mesh) and by reference "
                                "(Display.MeshBlueprint): the inline one is never used");
                    } else {
                        const int mesh = lua_gettop(L);
                        check_lods(r, file, mesh, string_at(L, mesh, "Source").value_or(source),
                                   merge);
                    }
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
            return;
        }
        for (const EffectTexture& t : kEffectTextures) {
            if (kind != t.kind) continue;
            const auto path = string_at(L, bp, t.field);
            if (path && !path->empty() && !vfs_.file_exists(*path))
                add(r, Severity::Error, "texture_file", file, 0,
                    "an " + kind + " blueprint's " + t.field + " is " + *path +
                        ", which isn't a file");
        }
    }

    /// The LODs of the mesh blueprint at stack index `mesh`, its files found
    /// as the engine finds them: against `source` (RES_CompletePath), or
    /// the file named after it (a merge's are the merged blueprint's).
    void check_lods(ModReport& r, const std::string& file, int mesh, const std::string& source,
                    bool merge) {
        lua_State* L = state_.raw();
        push_field(L, mesh, "LODs");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return;
        }
        const int lods = lua_gettop(L);
        const std::string prefix = source.substr(0, source.rfind('_'));
        for (int i = 1;; ++i) {
            lua_rawgeti(L, lods, i);
            if (!lua_istable(L, -1)) {
                lua_pop(L, 1);
                break;
            }
            const int lod = lua_gettop(L);
            const std::string which = "LOD " + std::to_string(i);
            if (const auto shader = string_at(L, lod, "ShaderName");
                shader && !renderer::has_mesh_technique(*shader)) {
                add(r, Severity::Warning, "shader", file, 0,
                    which + "'s ShaderName '" + *shader +
                        "' has no technique in OpenSupCom's renderer: it draws as Unit");
            }
            for (const char* field : kLodFiles) {
                const bool is_mesh = field == std::string_view("MeshName");
                const auto given = string_at(L, lod, field);
                if (given && !given->empty()) {
                    // A merge's relative names are its target blueprint's
                    if (merge && given->front() != '/' && given->front() != '\\') continue;
                    const std::string path = blueprints::complete_path(*given, source);
                    if (!vfs_.file_exists(path)) {
                        add(r, Severity::Error, is_mesh ? "mesh_file" : "texture_file", file, 0,
                            fmt::format("{}'s {} '{}' is {}, which isn't a file", which, field,
                                        *given, path));
                    }
                } else if (is_mesh && !merge) {
                    const std::string fallback = prefix + "_lod" + std::to_string(i - 1) + ".scm";
                    if (!vfs_.file_exists(fallback))
                        add(r, Severity::Error, "mesh_file", file, 0,
                            fmt::format("{} names no MeshName, and {} (the one named after its "
                                        "blueprint) isn't a file: it has no mesh",
                                        which, fallback));
                }
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
};

/// Whether `name` (a check-mods argument) names `m`: its uid, its folder or
/// mod_info.lua, a folder above it, or its name.
bool names_mod(const std::string& name, const ModInfo& m) {
    if (name == m.uid) return true;
    if (name.starts_with('/') || name.starts_with('\\')) {
        const std::string path = VirtualFileSystem::normalize(name);
        return path == m.location || path == m.info_file ||
               (m.location.starts_with(path) && (path == "/" || (m.location.size() > path.size() &&
                                                                 m.location[path.size()] == '/')));
    }
    return lower(name) == lower(m.uid) || (m.readable && lower(name) == lower(m.name));
}

std::string severity_name(Severity s) {
    return s == Severity::Error ? "error" : "warning";
}

std::string label(const ModInfo& m) {
    if (!m.readable) return m.location;
    return m.name + " (" + (m.uid.empty() ? std::string("no uid") : m.uid) + ") at " + m.location;
}

std::string counts(int errors, int warnings) {
    const auto plural = [](int n, const char* what) {
        return std::to_string(n) + " " + what + (n == 1 ? "" : "s");
    };
    return plural(errors, "error") + ", " + plural(warnings, "warning");
}

nlohmann::json mod_json(const ModInfo& m) {
    return {{"location", m.location}, {"info_file", m.info_file}, {"readable", m.readable},
            {"uid", m.uid},           {"name", m.name},           {"version", m.version},
            {"ui_only", m.ui_only},   {"enabled", m.enabled}};
}

std::string dump(const nlohmann::json& j) {
    // Mods' files are often Latin-1, not UTF-8
    return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

/// Mount a mod named by its folder, archive or mod_info.lua on disk where
/// retail's init file would mount it, /mods/<its name, lowercase, less
/// .zip/.scd>: that location, or "" when `path` is none of those.
std::string mount_from_disk(VirtualFileSystem& vfs, const fs::path& given) {
    std::error_code ec;
    fs::path path = fs::absolute(given, ec).lexically_normal();
    if (path.filename().empty()) path = path.parent_path();
    if (fs::is_regular_file(path, ec) && lower(path.filename().string()) == "mod_info.lua")
        path = path.parent_path();
    std::string name = lower(path.filename().string());
    if (name.empty()) return {}; // the root
    if (fs::is_directory(path, ec)) {
        vfs.mount("/mods/" + name, std::make_unique<vfs::DirectoryMount>(path));
    } else if (const std::string ext = extension(name);
               fs::is_regular_file(path, ec) && (ext == ".zip" || ext == ".scd")) {
        name.resize(name.size() - ext.size());
        vfs.mount("/mods/" + name, std::make_unique<vfs::ZipMount>(path));
    } else {
        return {};
    }
    return "/mods/" + name;
}

} // namespace

int ModReport::errors() const {
    return static_cast<int>(std::count_if(findings.begin(), findings.end(), [](const Finding& f) {
        return f.severity == Severity::Error;
    }));
}

int ModReport::warnings() const {
    return static_cast<int>(findings.size()) - errors();
}

int Report::errors() const {
    int n = static_cast<int>(unmatched.size());
    for (const ModReport& m : mods) n += m.errors();
    return n;
}

int Report::warnings() const {
    int n = 0;
    for (const ModReport& m : mods) n += m.warnings();
    return n;
}

std::vector<ModReport> read_mods(const vfs::VirtualFileSystem& vfs) {
    Checker checker(vfs);
    return checker.read_all();
}

Report check_mods(const vfs::VirtualFileSystem& vfs, const std::vector<std::string>& names) {
    Checker checker(vfs);
    std::vector<ModReport> all = checker.read_all();
    Checker::check_duplicates(all);

    Report report;
    std::vector<bool> chosen(all.size(), names.empty());
    for (const std::string& name : names) {
        bool any = false;
        for (size_t i = 0; i < all.size(); ++i) {
            if (names_mod(name, all[i].mod)) chosen[i] = any = true;
        }
        if (!any) report.unmatched.push_back(name);
    }
    for (size_t i = 0; i < all.size(); ++i) {
        if (!chosen[i]) continue;
        ModReport r = all[i];
        checker.check_dependencies(r, all);
        checker.check_hooks(r);
        checker.check_blueprints(r);
        checker.check_sounds(r);
        report.mods.push_back(std::move(r));
    }
    return report;
}

std::string report_text(const Report& report) {
    std::ostringstream out;
    for (const ModReport& r : report.mods) {
        out << label(r.mod) << ": "
            << (r.findings.empty() ? std::string("clean") : counts(r.errors(), r.warnings()))
            << "\n";
        for (const Finding& f : r.findings) {
            out << "  " << f.file;
            if (f.line > 0) out << ":" << f.line;
            out << ": " << severity_name(f.severity) << ": " << f.message << " [" << f.check
                << "]\n";
        }
    }
    for (const std::string& name : report.unmatched)
        out << "error: no installed mod is " << name << " (a uid, a /mods path or a mod's name)\n";
    out << "Checked " << report.mods.size() << (report.mods.size() == 1 ? " mod: " : " mods: ")
        << counts(report.errors(), report.warnings()) << ".\n";
    return out.str();
}

std::string report_json(const Report& report) {
    nlohmann::json mods = nlohmann::json::array();
    for (const ModReport& r : report.mods) {
        nlohmann::json findings = nlohmann::json::array();
        for (const Finding& f : r.findings) {
            findings.push_back({{"severity", severity_name(f.severity)},
                                {"check", f.check},
                                {"file", f.file},
                                {"line", f.line},
                                {"message", f.message}});
        }
        nlohmann::json m = mod_json(r.mod);
        m["errors"] = r.errors();
        m["warnings"] = r.warnings();
        m["findings"] = std::move(findings);
        mods.push_back(std::move(m));
    }
    return dump({{"mods", std::move(mods)},
                 {"unmatched", report.unmatched},
                 {"errors", report.errors()},
                 {"warnings", report.warnings()}});
}

std::string list_text(const std::vector<ModReport>& mods) {
    std::ostringstream out;
    size_t uid_width = 0;
    for (const ModReport& r : mods) uid_width = std::max(uid_width, r.mod.uid.size());
    for (const ModReport& r : mods) {
        const ModInfo& m = r.mod;
        if (!m.readable) {
            out << m.location << ": its mod_info.lua doesn't run (--check-mods says why)\n";
            continue;
        }
        out << m.uid << std::string(uid_width - m.uid.size() + 2, ' ') << m.name;
        if (!m.version.empty()) out << " v" << m.version;
        out << ", " << m.location;
        if (m.ui_only) out << ", UI only";
        if (!m.enabled) out << ", disabled";
        out << "\n";
    }
    if (mods.empty()) out << "No mods under /mods.\n";
    return out.str();
}

std::string list_json(const std::vector<ModReport>& mods) {
    nlohmann::json out = nlohmann::json::array();
    for (const ModReport& r : mods) out.push_back(mod_json(r.mod));
    return dump(out);
}

int run_mod_command(const lua::InitConfig& config, std::vector<std::string> names, bool list,
                    bool json) {
    if (config.fa_path.empty() || config.init_file.empty()) {
        std::cerr << "Supreme Commander: Forged Alliance not found. Pass --fa-path <dir>, set "
                     "OSC_FA_PATH, or run --print-install to see where we looked.\n";
        return 2;
    }
    VirtualFileSystem vfs;
    // Mounted before the init file's mounts, so a folder being worked on
    // wins over an installed copy of the mod
    for (std::string& name : names) {
        std::error_code ec;
        if (!fs::exists(fs::path(name), ec)) continue; // a uid, a VFS path or a name
        if (std::string location = mount_from_disk(vfs, name); !location.empty())
            name = std::move(location);
    }
    lua::LuaState init_state;
    lua::InitLoader loader;
    if (auto mounted = loader.execute_init(init_state, config, vfs); !mounted) {
        std::cerr << "The game's init file failed: " << mounted.error().message << "\n";
        return 2;
    }
    if (list) {
        const std::vector<ModReport> mods = read_mods(vfs);
        std::cout << (json ? list_json(mods) : list_text(mods));
        return 0;
    }
    const Report report = check_mods(vfs, names);
    std::cout << (json ? report_json(report) : report_text(report));
    return report.errors() > 0 ? 1 : 0;
}

} // namespace osc::app::mods
