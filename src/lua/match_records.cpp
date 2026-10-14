#include "lua/match_records.hpp"

#include "lua/lua_state.hpp"
#include "lua/special_files.hpp"

#include <spdlog/spdlog.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace osc::lua {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

/// FA's naming: the third letter of a unit's id says what it is, b for a
/// building (ueb1101). Mods mostly follow it.
bool is_structure(const std::string& bp) {
    return bp.size() >= 3 && (bp[2] == 'b' || bp[2] == 'B');
}

double num(const json& record, const char* key) {
    const auto it = record.find(key);
    return it != record.end() && it->is_number() ? it->get<double>() : 0.0;
}

/// One sample, as the screens read it: the tick; mass and energy income a
/// second; units alive; land, air, naval, engineers, structures; mass from
/// extractors, fabricators, commander, reclaim, other; energy from
/// generators, fabricators, commander, reclaim, other.
json point(const json& r) {
    static const char* kinds[] = {"land", "air", "naval", "engineers", "structures"};
    static const char* mass[] = {"extractors", "fabricators", "commander", "reclaim", "other"};
    static const char* energy[] = {"generators", "fabricators", "commander", "reclaim", "other"};
    json p = json::array({num(r, "tick"), num(r, "Economy_Income_Mass") * 10.0,
                          num(r, "Economy_Income_Energy") * 10.0, num(r, "Units_Active")});
    const json empty = json::object();
    const json& units = r.contains("units") ? r["units"] : empty;
    const json& mf = r.contains("mass_from") ? r["mass_from"] : empty;
    const json& ef = r.contains("energy_from") ? r["energy_from"] : empty;
    for (const char* k : kinds) p.push_back(num(units, k));
    for (const char* k : mass) p.push_back(num(mf, k));
    for (const char* k : energy) p.push_back(num(ef, k));
    return p;
}

/// At most `most` of `points`, evenly spaced, the last always among them
json thin(const std::vector<json>& points, size_t most) {
    json out = json::array();
    if (points.size() <= most) {
        for (const auto& p : points) out.push_back(p);
        return out;
    }
    for (size_t i = 0; i < most; ++i) out.push_back(points[i * (points.size() - 1) / (most - 1)]);
    return out;
}

} // namespace

std::optional<json> read_match_record(const fs::path& path, bool summary) {
    std::ifstream in(path);
    if (!in) return std::nullopt;
    json match;
    json end;
    std::map<int, std::vector<json>> samples;
    std::map<int, json> last_sample;
    std::vector<json> events;
    for (std::string line; std::getline(in, line);) {
        if (line.empty()) continue;
        json r = json::parse(line, nullptr, false);
        if (r.is_discarded() || !r.is_object()) continue;
        const std::string kind = r.value("record", "");
        if (kind == "match") match = std::move(r);
        else if (kind == "end") end = std::move(r);
        else if (kind == "sample") {
            const int army = static_cast<int>(num(r, "army"));
            if (!summary) samples[army].push_back(point(r));
            last_sample[army] = std::move(r);
        } else if (kind == "event") events.push_back(std::move(r));
    }
    if (match.is_null()) return std::nullopt;

    json out = json::object();
    out["schema"] = match.value("schema", 1);
    out["build"] = match.value("build", "");
    if (match.contains("seed")) out["seed"] = match["seed"].dump();
    double ticks = 0;
    for (const auto& [army, r] : last_sample) ticks = std::max(ticks, num(r, "tick"));
    out["ticks"] = ticks;
    out["ended"] = end.value("ended", "");
    const json results = end.contains("army_results") ? end["army_results"] : json::array();

    // Per army: what it built, lost and destroyed, from the events
    std::map<int, int> built, lost, lost_unfinished, destroyed;
    std::map<int, json> structures, units;
    json notable = json::array();
    for (const auto& e : events) {
        const std::string kind = e.value("kind", "");
        const int army = static_cast<int>(num(e, "army"));
        if (kind == "unit_built") {
            ++built[army];
            const std::string bp = e.value("bp", "");
            if (is_structure(bp)) {
                structures[army].push_back(json::array({num(e, "tick"), bp}));
            } else {
                auto& n = units[army][bp];
                n = (n.is_number() ? n.get<int>() : 0) + 1;
            }
        } else if (kind == "unit_lost") {
            const bool complete = e.value("complete", false);
            (complete ? lost : lost_unfinished)[army]++;
            const int by = static_cast<int>(num(e, "by_army"));
            if (complete && by > 0 && by != army) ++destroyed[by];
        }
        const bool keep = (kind == "unit_lost" && e.value("complete", false)) ||
                          kind == "commander_hit" || kind == "enhanced" || kind == "army_result";
        if (!summary && keep && notable.size() < kMaxNotable) {
            json n = json::object();
            for (const char* k :
                 {"tick", "kind", "army", "bp", "by_bp", "by_army", "health", "name", "result"})
                if (e.contains(k)) n[k] = e[k];
            notable.push_back(std::move(n));
        }
    }

    json armies = json::array();
    for (const auto& a : match.value("armies", json::array())) {
        if (a.value("civilian", false)) continue;
        const int idx = a.value("idx", 0);
        json army = json::object();
        army["idx"] = idx;
        army["name"] = a.value("nickname", a.value("name", ""));
        army["faction"] = a.value("faction", 0);
        army["human"] = a.value("human", false);
        army["ai"] = a.value("human", false) ? "" : a.value("ai", "");
        army["result"] = idx >= 1 && static_cast<size_t>(idx) <= results.size()
                             ? results[static_cast<size_t>(idx - 1)]
                             : json("");
        army["built"] = built[idx];
        army["lost"] = lost[idx];
        army["lostUnfinished"] = lost_unfinished[idx];
        army["destroyed"] = destroyed[idx];
        if (const auto it = last_sample.find(idx); it != last_sample.end()) {
            army["last"] = point(it->second);
            army["massMade"] = num(it->second, "Economy_TotalProduced_Mass");
            army["energyMade"] = num(it->second, "Economy_TotalProduced_Energy");
        }
        if (!summary) {
            army["samples"] = thin(samples[idx], kMaxPoints);
            army["structures"] = structures[idx].is_null() ? json::array() : structures[idx];
            army["units"] = units[idx].is_null() ? json::object() : units[idx];
        }
        armies.push_back(std::move(army));
    }
    out["armies"] = std::move(armies);
    if (!summary) out["notable"] = std::move(notable);
    return out;
}

void push_json(lua_State* L, const json& value) {
    switch (value.type()) {
    case json::value_t::object:
        lua_newtable(L);
        for (const auto& [key, item] : value.items()) {
            lua_pushstring(L, key.c_str());
            push_json(L, item);
            lua_rawset(L, -3);
        }
        return;
    case json::value_t::array: {
        lua_newtable(L);
        int i = 1;
        for (const auto& item : value) {
            push_json(L, item);
            lua_rawseti(L, -2, i++);
        }
        return;
    }
    case json::value_t::string:
        lua_pushstring(L, value.get_ref<const std::string&>().c_str());
        return;
    case json::value_t::boolean: lua_pushboolean(L, value.get<bool>() ? 1 : 0); return;
    case json::value_t::number_integer:
    case json::value_t::number_unsigned:
    case json::value_t::number_float: lua_pushnumber(L, value.get<double>()); return;
    default: lua_pushnil(L); return;
    }
}

namespace {

/// GetMatchRecord(profile, base[, summary]) -> the record, or nil
int l_GetMatchRecord(lua_State* L) {
    auto* files = get_special_files(L);
    const char* profile = luaL_checkstring(L, 1);
    const char* base = luaL_checkstring(L, 2);
    const bool summary = lua_toboolean(L, 3) != 0;
    const auto* type = SpecialFiles::find_type("Replay");
    fs::path path = files && type ? files->path(*type, profile, base) : fs::path();
    if (path.empty()) {
        lua_pushnil(L);
        return 1;
    }
    path.replace_extension("jsonl");
    try {
        const auto record = read_match_record(path, summary);
        if (!record) {
            lua_pushnil(L);
            return 1;
        }
        push_json(L, *record);
    } catch (const std::exception& error) {
        spdlog::warn("GetMatchRecord: {} could not be read: {}", path.string(), error.what());
        lua_pushnil(L);
    }
    return 1;
}

} // namespace

void register_match_record_bindings(LuaState& state) {
    state.register_function("GetMatchRecord", l_GetMatchRecord);
}

} // namespace osc::lua
