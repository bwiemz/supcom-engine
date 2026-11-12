#include "lua/sim_sync.hpp"

#include "blueprints/blueprint_store.hpp"
#include "core/console_output.hpp"
#include "core/lua_copy.hpp"
#include "core/test_status.hpp"
#include "lua/engine_stats.hpp"
#include "sim/army_brain.hpp"
#include "sim/blueprint_categories.hpp"
#include "sim/lua_sim.hpp"
#include "sim/script_class.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

namespace {

/// pcall global `name` with the nargs arguments on top of the stack
/// (consumed). A missing function is not an error.
void call_global(lua_State* L, const char* name, int nargs) {
    const int base = lua_gettop(L) - nargs;
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, base);
        return;
    }
    lua_insert(L, base + 1);
    if (lua_pcall(L, nargs, 0, 0) != 0) {
        const char* raw = lua_tostring(L, -1);
        std::string err = std::string(name) + " error: " + (raw ? raw : "(unknown)");
        spdlog::warn("{}", err);
        if (test_status::count_lua_failures())
            test_status::record_failure("Lua sync error: " + err);
    }
    lua_settop(L, base);
}

int registry_int(lua_State* L, const char* key, int fallback) {
    lua_pushstring(L, key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const int v = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : fallback;
    lua_pop(L, 1);
    return v;
}

void set_registry_int(lua_State* L, const char* key, int v) {
    lua_pushstring(L, key);
    lua_pushnumber(L, v);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

/// 0-based army (-1 observer) -> Lua's 1-based (-1 stays).
lua_Number to_lua_army(int army) { return army >= 0 ? army + 1 : -1; }

/// Take (and clear) a focus request pending in `L`'s registry.
bool take_focus_request(lua_State* L, int& requested, const char* key = kFocusArmyRequestKey) {
    lua_pushstring(L, key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const bool pending = lua_isnumber(L, -1) != 0;
    if (pending) requested = static_cast<int>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    if (pending) {
        lua_pushstring(L, key);
        lua_pushnil(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
    return pending;
}

void apply_focus_request(lua_State* sim_L, lua_State* ui_L, std::optional<int>& ui_request) {
    // The sim's (FAF's SetFocusArmy), then the UI's: both set the driver's
    // pending focus in Moho, the later winning
    int requested = 0;
    const bool from_sim = take_focus_request(sim_L, requested);
    bool from_ui = false;
    if (ui_L) {
        from_ui = take_focus_request(ui_L, requested);
    } else if (ui_request) {
        requested = *ui_request;
        ui_request.reset();
        from_ui = true;
    }
    if (!from_sim && !from_ui) return;

    const int old = registry_int(ui_L ? ui_L : sim_L, "__osc_focus_army", 0);
    if (requested == old) return;
    if (ui_L) set_registry_int(ui_L, "__osc_focus_army", requested);
    set_registry_int(sim_L, "__osc_focus_army", requested);

    lua_pushnumber(sim_L, to_lua_army(requested));
    lua_pushnumber(sim_L, to_lua_army(old));
    call_global(sim_L, "NoteFocusArmyChanged", 2);

    lua_pushstring(sim_L, "Sync");
    lua_rawget(sim_L, LUA_GLOBALSINDEX);
    if (lua_istable(sim_L, -1)) {
        lua_pushstring(sim_L, "FocusArmyChanged");
        lua_newtable(sim_L);
        lua_pushstring(sim_L, "new");
        lua_pushnumber(sim_L, to_lua_army(requested));
        lua_rawset(sim_L, -3);
        lua_pushstring(sim_L, "old");
        lua_pushnumber(sim_L, to_lua_army(old));
        lua_rawset(sim_L, -3);
        lua_rawset(sim_L, -3);
    }
    lua_pop(sim_L, 1);
}

/// Take (and clear) a selection pending in the UI state's registry.
bool take_debug_selection(lua_State* ui_L, std::vector<sim::EntityId>& out) {
    lua_pushstring(ui_L, kDebugSelectionKey);
    lua_rawget(ui_L, LUA_REGISTRYINDEX);
    if (!lua_istable(ui_L, -1)) {
        lua_pop(ui_L, 1);
        return false;
    }
    const int ids = lua_gettop(ui_L);
    out.clear();
    for (int i = 1;; ++i) {
        lua_rawgeti(ui_L, ids, i);
        if (lua_isnil(ui_L, -1)) {
            lua_pop(ui_L, 1);
            break;
        }
        if (const sim::EntityId id = sim::to_entity_key(ui_L, -1)) out.push_back(id);
        lua_pop(ui_L, 1);
    }
    lua_pop(ui_L, 1);
    lua_pushstring(ui_L, kDebugSelectionKey);
    lua_pushnil(ui_L);
    lua_rawset(ui_L, LUA_REGISTRYINDEX);
    return true;
}

/// Moho's Sim::Sync copying in the driver's filter: a selection pending since the last
/// Sync becomes the sim's (Sim::mSyncFilter.maskB). Taken from ui_L, or, on the sim's
/// thread, from what prepare_threaded took.
void apply_debug_selection(lua_State* sim_L, lua_State* ui_L,
                           std::optional<std::vector<sim::EntityId>>& ui_selection) {
    std::vector<sim::EntityId> ids;
    if (ui_L) {
        if (!take_debug_selection(ui_L, ids)) return;
    } else if (ui_selection) {
        ids = std::move(*ui_selection);
        ui_selection.reset();
    } else {
        return;
    }
    if (sim::SimState* sim = sim::get_sim(sim_L)) sim->set_debug_selection(std::move(ids));
}

/// Moho's Sim::Sync (0x00747B8x): with an army set by SetArmyStatsSyncArmy,
/// its stats as the sim's Sync.__ArmyStats (Stats<CArmyStatItem>'s tree, by
/// STAT_GetLuaTable), the tick on it. The engine keeps an army's stats as
/// numbers by name: each is a Float, made in the names' order (Moho's in the
/// order the game made them).
void sync_army_stats(lua_State* sim_L) {
    sim::SimState* sim = sim::get_sim(sim_L);
    const sim::ArmyBrain* brain = sim ? sim->get_army(sim->sync_army()) : nullptr;
    if (!brain) return;
    EngineStats tree;
    const std::map<std::string, f64> stats(brain->stats().begin(), brain->stats().end());
    for (const auto& [name, value] : stats) tree.set_float(name, static_cast<f32>(value));
    const std::map<std::string, std::map<std::string, f64>> per_blueprint(
        brain->all_blueprint_stats().begin(), brain->all_blueprint_stats().end());
    for (const auto& [name, values] : per_blueprint) tree.item(name).blueprints = values;
    lua_pushstring(sim_L, "Sync");
    lua_rawget(sim_L, LUA_GLOBALSINDEX);
    if (lua_istable(sim_L, -1)) {
        lua_pushstring(sim_L, "__ArmyStats");
        tree.push(sim_L);
        lua_pushstring(sim_L, "Tick");
        lua_pushnumber(sim_L, sim->tick_count());
        lua_rawset(sim_L, -3);
        lua_rawset(sim_L, -3);
    }
    lua_pop(sim_L, 1);
}

void push_sim_sync(lua_State* sim_L) {
    lua_pushstring(sim_L, "Sync");
    lua_rawget(sim_L, LUA_GLOBALSINDEX);
    if (!lua_istable(sim_L, -1)) {
        lua_pop(sim_L, 1);
        lua_newtable(sim_L);
    }
}

/// What CArmyStats::ArmyXmlStatsNode reads of a unit blueprint: the
/// categories it is in (its own id among them, as Moho's
/// RegisterBlueprintCategoryMembership makes it one), its Description as the
/// blueprint has it (RBlueprint::mDescription, a LOC tag and all) and its
/// costs.
struct StatsBlueprint {
    std::unordered_set<std::string> categories;
    std::string description;
    sim::BuildEconomy cost;
};

StatsBlueprint read_stats_blueprint(lua_State* L, const std::string& id) {
    StatsBlueprint bp;
    bp.categories.insert(id);
    if (!L) return bp;
    const int top = lua_gettop(L);
    if (blueprints::push_blueprint(L, id)) {
        sim::collect_blueprint_categories(L, lua_gettop(L), bp.categories);
        lua_pushstring(L, "Description");
        lua_rawget(L, -2);
        if (lua_type(L, -1) == LUA_TSTRING) bp.description = lua_tostring(L, -1);
    }
    lua_settop(L, top);
    bp.cost = sim::blueprint_build_economy(L, id);
    return bp;
}

/// `text` as an XML attribute value: & < > " ' as entities. Moho writes its
/// strings unescaped (STR_Printf, faf-re CArmyStats.cpp; the Steam exe hands
/// them straight to its sprintf), so a LOC-tagged Description already broke
/// its documents. The engine escapes every string it puts in one: a
/// nickname comes from another peer through the lobby and a blueprint from
/// any mod, and the document goes to the GPGNet client, where a stray quote
/// or tag would corrupt it or inject into it.
std::string xml_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}

using BlueprintLanes = std::map<std::string, f64>;
using StatsBlueprints = std::map<std::string, StatsBlueprint>;

/// Moho's CArmyStatItem::SumCategory: a stat's per-blueprint values summed,
/// as a float, over the blueprints in `category`; 0 for a stat without them.
f32 sum_category(const BlueprintLanes* lanes, const StatsBlueprints& bps,
                 const std::string& category) {
    f32 total = 0.0f;
    if (!lanes) return total;
    for (const auto& [id, value] : *lanes) {
        const auto it = bps.find(id);
        if (it != bps.end() && it->second.categories.contains(category)) {
            total += static_cast<f32>(value);
        }
    }
    return total;
}

/// The sum as its %d takes it: truncated (cvttss2si; past an int, INT32_MIN).
i32 sum_category_int(const BlueprintLanes* lanes, const StatsBlueprints& bps,
                     const std::string& category) {
    const f32 sum = sum_category(lanes, bps, category);
    return sum >= -2147483648.0f && sum < 2147483648.0f ? static_cast<i32>(sum)
                                                        : std::numeric_limits<i32>::min();
}

/// A float as its %.2f prints it.
double as_float(f64 value) {
    return static_cast<double>(static_cast<f32>(value));
}

/// The categories SummaryStats counts, in Moho's order.
constexpr const char* kSummaryCategories[] = {
    "AIR",
    "LAND",
    "NAVAL",
    "ENGINEER",
    "ARTILLERY",
    "ANTIAIR",
    "TRANSPORTATION",
    "STRUCTURE",
    "FACTORY",
    "ENERGYPRODUCTION",
    "MASSPRODUCTION",
    "DEFENSE",
    "TECH1",
    "TECH2",
    "TECH3",
};

/// Moho's CArmyStats::ArmyXmlStatsNode (faf-re CArmyStats.cpp; Steam exe
/// 0x7136C0): the army's <Army> element, each line after `indent`. Its
/// units are the blueprints it has a Units_Active or Enemies_Killed count
/// for, each with those counts and its Units_Killed (its losses) summed over
/// the category its id names, the damage its units dealt and took, and the
/// blueprint's costs; then the built and killed counts over fixed
/// categories, and the economy's totals. ("built" is Units_Active: the
/// units alive now.) Moho lists the units by their blueprints' addresses;
/// the engine by id. Every string goes through xml_escape, unlike Moho's.
/// The damage lanes are Moho's SIM_DoDamagePoint's, which the engine
/// doesn't keep yet: 0.
void append_army_xml(std::string& xml, lua_State* L, const sim::ArmyBrain& brain,
                     std::string_view indent, StatsBlueprints& bps) {
    const BlueprintLanes* active = brain.blueprint_stats("Units_Active");
    const BlueprintLanes* lost = brain.blueprint_stats("Units_Killed");
    const BlueprintLanes* killed = brain.blueprint_stats("Enemies_Killed");
    const BlueprintLanes* dealt = brain.blueprint_stats("Units_TotalDamageDealt");
    const BlueprintLanes* received = brain.blueprint_stats("Units_TotalDamageReceive");
    for (const BlueprintLanes* lanes : {active, lost, killed, dealt, received}) {
        if (!lanes) continue;
        for (const auto& lane : *lanes) {
            if (!bps.contains(lane.first))
                bps.emplace(lane.first, read_stats_blueprint(L, lane.first));
        }
    }
    std::set<std::string> units;
    for (const BlueprintLanes* lanes : {active, killed}) {
        if (!lanes) continue;
        for (const auto& lane : *lanes) units.insert(lane.first);
    }

    xml += fmt::format("{}<Army index=\"{}\" name=\"{}\">\n", indent, brain.index(),
                       xml_escape(brain.nickname()));
    xml += fmt::format("{}    <UnitStats>\n", indent);
    for (const std::string& id : units) {
        const StatsBlueprint& bp = bps.at(id);
        xml += fmt::format(
            "{}      <Unit id=\"{}\" type=\"{}\" built=\"{}\" lost=\"{}\" killed=\"{}\" "
            "damagedealt=\"{:.2f}\" damagereceived=\"{:.2f}\" masscost=\"{:.2f}\" "
            "energycost=\"{:.2f}\" buildtime=\"{:.2f}\"/>\n",
            indent, xml_escape(id), xml_escape(bp.description), sum_category_int(active, bps, id),
            sum_category_int(lost, bps, id), sum_category_int(killed, bps, id),
            static_cast<double>(sum_category(dealt, bps, id)),
            static_cast<double>(sum_category(received, bps, id)), as_float(bp.cost.mass),
            as_float(bp.cost.energy), as_float(bp.cost.time));
    }
    xml += fmt::format("{}    </UnitStats>\n", indent);
    xml += fmt::format("{}    <SummaryStats>\n", indent);
    for (const char* category : kSummaryCategories) {
        xml += fmt::format("{}      <Category type=\"{}\" built=\"{}\" killed=\"{}\"/>\n", indent,
                           xml_escape(category), sum_category_int(active, bps, category),
                           sum_category_int(killed, bps, category));
    }
    xml += fmt::format("{}    </SummaryStats>\n", indent);
    xml += fmt::format("{}    <EconomyStats>\n", indent);
    for (const char* resource : {"Energy", "Mass"}) {
        const std::string suffix = std::string("_") + resource;
        xml += fmt::format(
            "{}      <{} produced=\"{:.2f}\" consumed=\"{:.2f}\" storage=\"{:.2f}\"/>\n", indent,
            xml_escape(resource), as_float(brain.get_stat("Economy_TotalProduced" + suffix)),
            as_float(brain.get_stat("Economy_TotalConsumed" + suffix)),
            as_float(brain.get_stat("Economy_MaxStorage" + suffix)));
    }
    xml += fmt::format("{}    </EconomyStats>\n", indent);
    xml += fmt::format("{}</Army>\n", indent);
}

/// The beat's army stats document if SubmitXMLArmyStats asked for one (the
/// request taken), else empty.
std::string take_army_stats(lua_State* sim_L, sim::SimState* sim) {
    if (!sim || !sim->take_xml_army_stats_request()) return {};
    return army_stats_xml(sim_L, *sim);
}

} // namespace

std::string army_stats_xml(lua_State* sim_L, const sim::SimState& sim) {
    std::string xml = "<?xml version=\"1.0\" ?>\n"
                      "<GameStats xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
                      "xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">\n";
    StatsBlueprints bps;
    for (size_t i = 0; i < sim.army_count(); ++i) {
        if (const sim::ArmyBrain* brain = sim.army_at(i)) {
            append_army_xml(xml, sim_L, *brain, "  ", bps);
        }
    }
    xml += "</GameStats>\n";
    return xml;
}

int focus_army(lua_State* ui_L) {
    return registry_int(ui_L, "__osc_focus_army", 0);
}

void request_debug_selection(lua_State* ui_L, const std::vector<sim::EntityId>& ids) {
    if (!ui_L) return;
    lua_pushstring(ui_L, kDebugSelectionKey);
    lua_newtable(ui_L);
    int i = 0;
    for (const sim::EntityId id : ids) {
        sim::push_entity_key(ui_L, id);
        lua_rawseti(ui_L, -2, ++i);
    }
    lua_rawset(ui_L, LUA_REGISTRYINDEX);
}

void set_focus_army(lua_State* sim_L, lua_State* ui_L, int army) {
    if (sim_L) set_registry_int(sim_L, "__osc_focus_army", army);
    if (ui_L) set_registry_int(ui_L, "__osc_focus_army", army);
}

SyncQueue::SyncQueue() : packets_state_(lua_open()) {}

SyncQueue::~SyncQueue() {
    lua_close(packets_state_);
}

void SyncQueue::queue(lua_State* sim_L, lua_State* ui_L) {
    if (!sim_L) {
        return;
    }
    apply_ui_filter(sim_L, ui_L);
    sync_army_stats(sim_L);
    push_sim_sync(sim_L);
    copy_lua_value(sim_L, -1, packets_state_);
    lua_pop(sim_L, 1);
    packets_.push_back(luaL_ref(packets_state_, LUA_REGISTRYINDEX));
    sim::SimState* sim = sim::get_sim(sim_L);
    print_lines_.push_back(sim ? sim->take_print_lines() : std::vector<std::string>{});
    army_stats_.push_back(take_army_stats(sim_L, sim));
    call_global(sim_L, "ResetSyncTable", 0);
}

void SyncQueue::prepare_threaded(lua_State* sim_L, lua_State* ui_L) {
    if (!sim_L || !ui_L) {
        return;
    }
    // Replay UI starts as observer. Transfer its current focus as well as the
    // request, while both states belong to the main thread, so the first beat
    // reports the same old focus as serial Sync.
    set_registry_int(sim_L, "__osc_focus_army", registry_int(ui_L, "__osc_focus_army", 0));
    int requested = 0;
    if (take_focus_request(ui_L, requested)) {
        ui_request_ = requested;
    }
    std::vector<sim::EntityId> selection;
    if (take_debug_selection(ui_L, selection)) {
        ui_selection_ = std::move(selection);
    }
}

void SyncQueue::apply_ui_filter(lua_State* sim_L, lua_State* ui_L) {
    apply_debug_selection(sim_L, ui_L, ui_selection_);
    apply_focus_request(sim_L, ui_L, ui_request_);
}

std::string SyncQueue::deliver(lua_State* sim_L, lua_State* ui_L) {
    if (!sim_L || !ui_L) {
        return {};
    }
    const bool queued = !packets_.empty();
    lua_State* from = queued ? packets_state_ : sim_L;
    std::vector<std::string> print_lines;
    std::string army_stats;
    if (queued) {
        lua_rawgeti(packets_state_, LUA_REGISTRYINDEX, packets_.front());
        luaL_unref(packets_state_, LUA_REGISTRYINDEX, packets_.front());
        packets_.pop_front();
        print_lines = std::move(print_lines_.front());
        print_lines_.pop_front();
        army_stats = std::move(army_stats_.front());
        army_stats_.pop_front();
    } else {
        if (sim::SimState* sim = sim::get_sim(sim_L)) {
            print_lines = sim->take_print_lines();
            army_stats = take_army_stats(sim_L, sim);
        }
        apply_ui_filter(sim_L, ui_L);
        sync_army_stats(sim_L);
        push_sim_sync(sim_L);
    }

    // A worker applies focus changes in the sim only. Publish the matching
    // UI focus with this beat's Sync, after the worker has been collected.
    lua_pushstring(from, "FocusArmyChanged");
    lua_rawget(from, -2);
    if (lua_istable(from, -1)) {
        lua_pushstring(from, "new");
        lua_rawget(from, -2);
        if (lua_isnumber(from, -1)) {
            const int army = static_cast<int>(lua_tonumber(from, -1));
            set_registry_int(ui_L, "__osc_focus_army", army > 0 ? army - 1 : -1);
        }
        lua_pop(from, 1);
    }
    lua_pop(from, 1);

    // PreviousSync = Sync; Sync = copy of the beat's
    lua_pushstring(ui_L, "PreviousSync");
    lua_pushstring(ui_L, "Sync");
    lua_rawget(ui_L, LUA_GLOBALSINDEX);
    lua_rawset(ui_L, LUA_GLOBALSINDEX);
    lua_pushstring(ui_L, "Sync");
    copy_lua_value(from, -1, ui_L);
    lua_rawset(ui_L, LUA_GLOBALSINDEX);
    lua_pop(from, 1);

    if (!queued) {
        call_global(sim_L, "ResetSyncTable", 0);
    }
    call_global(ui_L, "OnSync", 0);
    for (const std::string& line : print_lines) {
        core::console_print(ui_L, line);
    }
    return army_stats;
}

void SyncQueue::clear() {
    for (const int ref : packets_) {
        luaL_unref(packets_state_, LUA_REGISTRYINDEX, ref);
    }
    packets_.clear();
    print_lines_.clear();
    army_stats_.clear();
    ui_request_.reset();
    ui_selection_.reset();
}

} // namespace osc::lua
