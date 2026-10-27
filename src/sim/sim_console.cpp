// Moho's sim console: the registry (CSimConCommand, a name compared in any
// case), the parse and dispatch (Sim::TryParseSimCommand, 0x00734870), and
// the commands' handlers (Sim.cpp, SimDebugCommandRegistrations.cpp).
//
// Not here: SimLua (0x007595C0), which runs its text as sim Lua. A game's
// commands are its replay's, so a replay with cheats on could run any code on
// the machine that plays it. SimError, SimAssert, SimCrash and sim_DebugCrash
// kill the game on purpose. The debug overlays (dbg, ren_Steering, AI_Render*),
// TrackStats, DumpUnits, the effect tools (efx_*), AddImpulse, sim_Gravity, the
// pathfinding and checksum variables, ai_InstaBuild and ai_FreeBuild wait for
// what they drive.

#include "sim/sim_console.hpp"

#include "sim/army_brain.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/entity_registry.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <string>
#include <system_error>

#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

namespace osc::sim {

namespace {

bool equals_no_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool starts_with_no_case(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && equals_no_case(text.substr(0, prefix.size()), prefix);
}

/// atoi's answer: leading whitespace, a sign, then digits; 0 for anything else.
int parse_int(std::string_view text) {
    size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
    if (i < text.size() && text[i] == '+') {
        ++i;
    }
    int value = 0;
    const char* const begin = text.data() + i;
    const auto [end, error] = std::from_chars(begin, text.data() + text.size(), value);
    return error == std::errc() && end != begin ? value : 0;
}

/// atof's answer (0 for no number).
f32 parse_float(const std::string& text) {
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    return end == text.c_str() ? 0.0f : static_cast<f32>(value);
}

/// GetCurrentCommandSourceName: who sent the command running now.
std::string source_name(const SimState& sim) {
    const u32 source = sim.current_command_source();
    return source == kEngineSource ? std::string("???") : "source " + std::to_string(source);
}

/// The arguments from the second on, joined by spaces (SimLog, SimWarn).
std::string joined_args(const std::vector<std::string>& args) {
    std::string out;
    for (size_t i = 1; i < args.size(); ++i) {
        if (i > 1) {
            out.push_back(' ');
        }
        out += args[i];
    }
    return out;
}

/// TSimConVarInstance<bool>::HandleConsoleCommand: none toggles, then = n,
/// on/true, off/false, show, tog, or a bare number (not 0 is on).
void bool_var(const char* name, bool& value, const std::vector<std::string>& args) {
    if (args.size() < 2) {
        value = !value;
        spdlog::info("toggled {} is now {}", name, value ? "on" : "off");
        return;
    }
    const std::string& op = args[1];
    if (op == "=" && args.size() > 2) {
        value = parse_int(args[2]) != 0;
    } else if (equals_no_case(op, "on") || equals_no_case(op, "true")) {
        value = true;
    } else if (equals_no_case(op, "off") || equals_no_case(op, "false")) {
        value = false;
    } else if (equals_no_case(op, "show")) {
        spdlog::info("bool {} is {}", name, value ? "on" : "off");
    } else if (equals_no_case(op, "tog")) {
        value = !value;
    } else {
        value = parse_int(op) != 0;
    }
}

/// The live unit `id` names, or null.
Unit* live_unit(SimState& sim, u32 id) {
    Entity* e = sim.entity_registry().find(id);
    return e && !e->destroyed() && e->is_unit() ? static_cast<Unit*>(e) : nullptr;
}

/// Entity::Destroy, through its script as a script's would: its Destroy
/// method, or the registry's for an entity with none.
void destroy_entity(SimState& sim, lua_State* L, Entity& e) {
    if (e.destroyed()) {
        return;
    }
    const u32 id = e.entity_id();
    if (L && e.lua_table_ref() >= 0) {
        const int top = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, e.lua_table_ref());
        lua_pushstring(L, "Destroy");
        lua_gettable(L, -2);
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, top + 1);
            if (lua_pcall(L, 1, 0, 0) != 0) {
                spdlog::warn("Destroy error: {}", lua_tostring(L, -1));
            }
        }
        lua_settop(L, top);
    }
    Entity* still = sim.entity_registry().find(id);
    if (still && !still->destroyed()) {
        sim.entity_registry().destroy(*still);
    }
}

/// ForEachTargetArmyUnit's units: those of the armies the arguments name
/// (0-based; one that isn't a number or an army is passed over, and if none
/// is left, no unit), or every army's. Army by army, as CUnitIterAllArmies
/// walks them.
std::vector<u32> target_army_units(SimState& sim, const std::vector<std::string>& args) {
    std::vector<i32> armies;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& text = args[i];
        long index = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), index);
        if (text.empty() || error != std::errc() || end != text.data() + text.size() || index < 0 ||
            static_cast<size_t>(index) >= sim.army_count()) {
            continue;
        }
        const auto army = static_cast<i32>(index);
        if (std::find(armies.begin(), armies.end(), army) == armies.end()) {
            armies.push_back(army);
        }
    }
    if (args.size() > 1 && armies.empty()) {
        return {};
    }
    std::vector<u32> ids;
    for (size_t a = 0; a < sim.army_count(); ++a) {
        const auto army = static_cast<i32>(a);
        if (!armies.empty() && std::find(armies.begin(), armies.end(), army) == armies.end()) {
            continue;
        }
        sim.entity_registry().for_each_unit([&](Entity& e) {
            if (e.army() == army && !e.destroyed()) {
                ids.push_back(e.entity_id());
            }
        });
    }
    return ids;
}

// --- The commands ---

void no_damage(SimConContext& ctx) {
    bool_var("NoDamage", ctx.sim.con_vars().no_damage, ctx.args);
}

void run_opponent_ai(SimConContext& ctx) {
    bool_var("AI_RunOpponentAI", ctx.sim.con_vars().run_opponent_ai, ctx.args);
}

/// BlingBling (0x0064BBE0): the focus army's extra storage 10000 more of
/// each, and as much again as its whole storage now holds put in it.
void bling_bling(SimConContext& ctx) {
    ArmyBrain* brain = ctx.focus_army >= 0 ? ctx.sim.get_army(ctx.focus_army) : nullptr;
    if (!brain) {
        spdlog::info("No focus army.");
        return;
    }
    brain->set_extra_storage(brain->extra_storage_mass() + 10000.0,
                             brain->extra_storage_energy() + 10000.0);
    EconomyState& economy = brain->economy();
    economy.energy.stored += economy.energy.max_storage;
    economy.mass.stored += economy.mass.max_storage;
}

/// ZeroExtraStorage (0x0064BCA0): the focus army's extra storage none.
void zero_extra_storage(SimConContext& ctx) {
    ArmyBrain* brain = ctx.focus_army >= 0 ? ctx.sim.get_army(ctx.focus_army) : nullptr;
    if (!brain) {
        spdlog::info("No focus army.");
        return;
    }
    brain->set_extra_storage(0.0, 0.0);
}

/// SallyShears: every army's fog of war turned over (ReconSetFogOfWar(!fog)).
/// The engine keeps one for the game, as every army's starts the same.
void sally_shears(SimConContext& ctx) {
    ctx.sim.set_fog_of_war(ctx.sim.fog_mode() == FogMode::None ? "explored" : "none");
}

/// CDamage's zero vector, as the sim's Vector() makes one (nil without it).
void push_zero_vector(lua_State* L) {
    lua_pushstring(L, "Vector");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        return;
    }
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    if (lua_pcall(L, 3, 1, 0) != 0) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
}

/// DamageUnit <amount> (0x0064BDA0): each selected unit dealt that much,
/// single target, by no one, a zero vector, of type "Debug" (SIM_Damage, as
/// the Damage global deals it; none at all for no more than nothing after its
/// armour).
void damage_unit(SimConContext& ctx) {
    if (ctx.args.size() < 2) {
        spdlog::info("Syntax: DamageUnit <amount>");
        return;
    }
    const f32 amount = parse_float(ctx.args[1]);
    lua_State* L = ctx.L;
    for (const u32 id : ctx.selected) {
        Unit* unit = live_unit(ctx.sim, id);
        if (!L || !unit || unit->lua_table_ref() < 0) {
            continue;
        }
        lua_pushstring(L, "Damage");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return;
        }
        lua_pushnil(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, unit->lua_table_ref());
        lua_pushnumber(L, amount);
        push_zero_vector(L);
        lua_pushstring(L, "Debug");
        if (lua_pcall(L, 5, 0, 0) != 0) {
            spdlog::warn("DamageUnit: {}", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    }
}

/// KillAll [armies] (0x006B6B40): each unit killed, by no one.
void kill_all(SimConContext& ctx) {
    for (const u32 id : target_army_units(ctx.sim, ctx.args)) {
        if (Unit* unit = live_unit(ctx.sim, id)) {
            ctx.sim.kill_unit(*unit, "");
        }
    }
}

/// DestroyAll [armies] (0x006B6DC0): each unit destroyed.
void destroy_all(SimConContext& ctx) {
    for (const u32 id : target_army_units(ctx.sim, ctx.args)) {
        if (Unit* unit = live_unit(ctx.sim, id)) {
            destroy_entity(ctx.sim, ctx.L, *unit);
        }
    }
}

/// Purge <shield|projectile|unit|prop|other|all> [army]: every entity of the
/// kind, of that army if one is named, neither dead nor going, destroyed.
void purge(SimConContext& ctx) {
    if (ctx.args.size() < 2) {
        return;
    }
    enum class Kind { All, Projectile, Unit, Shield, Other, Prop };
    const std::string& token = ctx.args[1];
    Kind kind{};
    if (equals_no_case(token, "all") || starts_with_no_case(token, "entity")) kind = Kind::All;
    else if (starts_with_no_case(token, "projectile")) kind = Kind::Projectile;
    else if (starts_with_no_case(token, "unit")) kind = Kind::Unit;
    else if (starts_with_no_case(token, "shield")) kind = Kind::Shield;
    else if (starts_with_no_case(token, "other")) kind = Kind::Other;
    else if (starts_with_no_case(token, "prop")) kind = Kind::Prop;
    else {
        spdlog::info("Unknown type {}", token);
        return;
    }
    const i32 army = ctx.args.size() > 2 ? parse_int(ctx.args[2]) : -1;
    std::vector<u32> ids;
    ctx.sim.entity_registry().for_each([&](Entity& e) {
        bool match = false;
        switch (kind) {
        case Kind::All: match = true; break;
        case Kind::Projectile: match = e.is_projectile(); break;
        case Kind::Unit: match = e.is_unit(); break;
        case Kind::Shield: match = e.is_shield(); break;
        case Kind::Prop: match = e.is_prop(); break;
        case Kind::Other:
            match = !e.is_unit() && !e.is_projectile() && !e.is_shield() && !e.is_prop();
            break;
        }
        const bool dead = e.is_unit() && static_cast<const Unit&>(e).is_dying();
        if (match && (army < 0 || e.army() == army) && !dead && !e.destroyed()) {
            ids.push_back(e.entity_id());
        }
    });
    for (const u32 id : ids) {
        if (Entity* e = ctx.sim.entity_registry().find(id)) {
            destroy_entity(ctx.sim, ctx.L, *e);
        }
    }
}

void sim_log(SimConContext& ctx) {
    spdlog::info("{}", joined_args(ctx.args));
}

void sim_warn(SimConContext& ctx) {
    spdlog::warn("{}", joined_args(ctx.args));
}

/// ScenarioMethod <name>: the scenario's ScenarioInfo.Env[name](), or a
/// warning that it has none. A base library loader isn't run: Env falls
/// through to the globals, and the command can come from another player.
void scenario_method(SimConContext& ctx) {
    lua_State* L = ctx.L;
    if (ctx.args.size() < 2 || !L) {
        return;
    }
    const std::string& name = ctx.args[1];
    lua_pushstring(L, "ScenarioInfo");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushstring(L, "Env");
        lua_gettable(L, -2);
        if (lua_istable(L, -1)) {
            lua_pushlstring(L, name.data(), name.size());
            lua_gettable(L, -2);
            if (lua_isfunction(L, -1)) {
                if (is_base_loader(L, name, -1)) {
                    spdlog::warn("ScenarioMethod: '{}' is one of Lua's loaders; not run", name);
                    return;
                }
                if (lua_pcall(L, 0, 0, 0) != 0) {
                    spdlog::warn("ScenarioMethod '{}': {}", name, lua_tostring(L, -1));
                }
                return;
            }
        }
    }
    spdlog::warn("ScenarioMethod '{}' not defined", name);
}

void set_consumption(SimConContext& ctx, bool on) {
    for (const u32 id : ctx.selected) {
        if (Unit* unit = live_unit(ctx.sim, id)) {
            unit->set_consumption_active(on, ctx.sim.get_army(unit->army()));
        }
    }
}

/// DebugSetProductionActive / InActive: each selected unit's production
/// switched, its script told (OnProductionActive / OnProductionInActive).
void set_production(SimConContext& ctx, bool on) {
    for (const u32 id : ctx.selected) {
        if (Unit* unit = live_unit(ctx.sim, id)) {
            unit->economy().production_active = on;
            unit->call_lua_method(ctx.L, on ? "OnProductionActive" : "OnProductionInActive");
        }
    }
}

} // namespace

const std::vector<SimConCommand>& sim_commands() {
    // As SimDebugCommandRegistrations registers them; the first flag is
    // mAllowWithoutCheats.
    static const std::vector<SimConCommand> kCommands = {
        {"AI_RunOpponentAI", true, run_opponent_ai},
        {"BlingBling", false, bling_bling},
        {"DamageUnit", false, damage_unit},
        {"DebugSetConsumptionActive", false, [](SimConContext& c) { set_consumption(c, true); }},
        {"DebugSetConsumptionInActive", false, [](SimConContext& c) { set_consumption(c, false); }},
        {"DebugSetProductionActive", false, [](SimConContext& c) { set_production(c, true); }},
        {"DebugSetProductionInActive", false, [](SimConContext& c) { set_production(c, false); }},
        {"DestroyAll", false, destroy_all},
        {"KillAll", false, kill_all},
        {"NoDamage", false, no_damage},
        {"Purge", false, purge},
        {"SallyShears", false, sally_shears},
        {"ScenarioMethod", false, scenario_method},
        {"SimLog", false, sim_log},
        {"SimWarn", false, sim_warn},
        {"ZeroExtraStorage", false, zero_extra_storage},
    };
    return kCommands;
}

const SimConCommand* find_sim_command(std::string_view name) {
    for (const SimConCommand& command : sim_commands()) {
        if (equals_no_case(command.name, name)) {
            return &command;
        }
    }
    return nullptr;
}

namespace {

/// ParseOneSimCommand over `text`: its first command's tokens, and where the
/// next one starts (npos for none).
size_t parse_one_sim_command(std::string_view text, std::vector<std::string>& tokens) {
    tokens.clear();
    size_t next = std::string_view::npos;
    std::string token;
    bool quoted = false;
    bool escaping = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (escaping) {
            token.push_back(c);
            escaping = false;
            continue;
        }
        if (quoted && c == '\\') {
            escaping = true;
            continue;
        }
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && c == ';') {
            next = i + 1;
            break;
        }
        if (!quoted && c == '#') {
            break;
        }
        if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
            continue;
        }
        token.push_back(c);
    }
    if (escaping) {
        token.push_back('\\');
    }
    if (!token.empty()) {
        tokens.push_back(token);
    }
    return next;
}

} // namespace

void parse_sim_command(std::string_view text, std::vector<std::string>& tokens, std::string& rest) {
    const size_t next = parse_one_sim_command(text, tokens);
    rest = next == std::string_view::npos ? std::string() : std::string(text.substr(next));
}

std::string unparse_sim_command(const std::vector<std::string>& tokens) {
    std::string text;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i != 0) {
            text.push_back(' ');
        }
        const std::string& token = tokens[i];
        bool quote = token.empty();
        for (const char c : token) {
            if (std::isspace(static_cast<unsigned char>(c)) || c == ';' || c == '#') {
                quote = true;
            }
        }
        if (!quote) {
            text += token;
            continue;
        }
        text.push_back('"');
        for (const char c : token) {
            if (c == '\\' || c == '"') {
                text.push_back('\\');
            }
            text.push_back(c);
        }
        text.push_back('"');
    }
    return text;
}

void execute_debug_command(SimState& sim, lua_State* L, std::string_view text, const Vector3& at,
                           i32 focus_army, const std::vector<u32>& selected) {
    // ExecuteDebugCommand's selection: those of the ids still units, a set
    // (BVSet<EntId>) that walks them in id order, each once.
    std::vector<u32> units;
    for (const u32 id : selected) {
        if (live_unit(sim, id)) {
            units.push_back(id);
        }
    }
    std::sort(units.begin(), units.end());
    units.erase(std::unique(units.begin(), units.end()), units.end());
    // Each command by where it starts, so a long chain costs its length once.
    std::vector<std::string> tokens;
    for (size_t at_char = 0; at_char < text.size();) {
        const size_t next = parse_one_sim_command(text.substr(at_char), tokens);
        if (!tokens.empty()) {
            const SimConCommand* command = find_sim_command(tokens.front());
            if (!command) {
                spdlog::info("Unknown sim command '{}' [invoked by {}]", tokens.front(),
                             source_name(sim));
            } else {
                const bool cheat = !command->allow_without_cheats;
                // One that needs cheats, without them, ends the whole line.
                if (cheat && !sim.cheats_enabled()) {
                    return;
                }
                if (cheat) {
                    spdlog::info("{}: {}", source_name(sim), unparse_sim_command(tokens));
                }
                SimConContext ctx{sim, L, tokens, at, focus_army, units};
                const int top = L ? lua_gettop(L) : 0;
                command->run(ctx);
                if (L) {
                    lua_settop(L, top);
                }
            }
        }
        if (next == std::string_view::npos) {
            break;
        }
        at_char += next;
    }
}

bool is_base_loader(lua_State* L, const std::string& name, int idx) {
    constexpr std::array<const char*, 5> kLoaders{"dofile", "require", "loadfile", "loadstring",
                                                  "loadlib"};
    const lua_CFunction f = lua_tocfunction(L, idx);
    for (const char* loader : kLoaders) {
        if (name == loader) return true;
        lua_pushstring(L, loader);
        lua_rawget(L, LUA_GLOBALSINDEX);
        const bool same = f && lua_tocfunction(L, -1) == f;
        lua_pop(L, 1);
        if (same) return true;
    }
    return false;
}

} // namespace osc::sim
