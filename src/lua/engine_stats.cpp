#include "lua/engine_stats.hpp"

#include "platform/system_info.hpp"
#include "sim/entity_registry.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <utility>

#include <fmt/format.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace osc::lua {

namespace {

/// The child of `parent` named `name`, made after the others if it has none
EngineStats::Item& child(EngineStats::Item& parent, std::string_view name) {
    for (auto& c : parent.children)
        if (c->name == name) return *c;
    parent.children.push_back(std::make_unique<EngineStats::Item>());
    parent.children.back()->name = std::string(name);
    return *parent.children.back();
}

const EngineStats::Item* find_child(const EngineStats::Item& parent, std::string_view name) {
    for (const auto& c : parent.children)
        if (c->name == name) return c.get();
    return nullptr;
}

/// Each part of `path` split at '_', in turn
template <class F> bool for_each_part(std::string_view path, F&& f) {
    while (true) {
        const size_t cut = path.find('_');
        if (!f(path.substr(0, cut))) return false;
        if (cut == std::string_view::npos) return true;
        path.remove_prefix(cut + 1);
    }
}

void push_item(lua_State* L, const EngineStats::Item& item) {
    lua_newtable(L);
    lua_pushstring(L, "Name");
    lua_pushlstring(L, item.name.data(), item.name.size());
    lua_rawset(L, -3);
    // StatItem::ToLua: the value, then its type
    const char* type = nullptr;
    switch (item.type) {
    case EngineStats::Item::Type::Integer:
        lua_pushstring(L, "Value");
        lua_pushnumber(L, item.integer);
        lua_rawset(L, -3);
        type = "Integer";
        break;
    case EngineStats::Item::Type::Float:
        lua_pushstring(L, "Value");
        lua_pushnumber(L, item.number);
        lua_rawset(L, -3);
        type = "Float";
        break;
    case EngineStats::Item::Type::String:
        if (!item.text.empty()) {
            lua_pushstring(L, "Value");
            lua_pushlstring(L, item.text.data(), item.text.size());
            lua_rawset(L, -3);
        }
        type = "String";
        break;
    case EngineStats::Item::Type::None: break;
    }
    if (type) {
        lua_pushstring(L, "Type");
        lua_pushstring(L, type);
        lua_rawset(L, -3);
    }
    // CArmyStatItem::ToLua (0x0070B430): the values per blueprint
    if (!item.blueprints.empty()) {
        lua_pushstring(L, "Blueprints");
        lua_newtable(L);
        for (const auto& [id, value] : item.blueprints) {
            const std::string text = fmt::format("{:.2f}", static_cast<f32>(value));
            lua_pushlstring(L, id.data(), id.size());
            lua_pushlstring(L, text.data(), text.size());
            lua_rawset(L, -3);
        }
        lua_rawset(L, -3);
    }
    if (item.children.empty()) return;
    // The children as table.insert appends them
    lua_pushstring(L, "Children");
    lua_newtable(L);
    int n = 0;
    for (const auto& c : item.children) {
        push_item(L, *c);
        lua_rawseti(L, -2, ++n);
    }
    luaL_setn(L, -1, n);
    lua_rawset(L, -3);
}

} // namespace

EngineStats::Item& EngineStats::item(std::string_view path) {
    Item* at = &root_;
    for_each_part(path, [&](std::string_view part) {
        at = &child(*at, part);
        return true;
    });
    return *at;
}

const EngineStats::Item* EngineStats::find(std::string_view path) const {
    const Item* at = &root_;
    for_each_part(path, [&](std::string_view part) {
        at = find_child(*at, part);
        return at != nullptr;
    });
    return at;
}

void EngineStats::set_int(std::string_view path, i32 value) {
    Item& i = item(path);
    i.type = Item::Type::Integer;
    i.integer = value;
}

void EngineStats::set_float(std::string_view path, f32 value) {
    Item& i = item(path);
    i.type = Item::Type::Float;
    i.number = value;
}

void EngineStats::set_string(std::string_view path, std::string value) {
    Item& i = item(path);
    i.type = Item::Type::String;
    i.text = std::move(value);
}

void EngineStats::push(lua_State* L) const {
    push_item(L, root_);
    // Moho's root always has children (its instance counts are made at
    // startup); before this tree's first stat, an empty list
    if (root_.children.empty()) {
        lua_pushstring(L, "Children");
        lua_newtable(L);
        lua_rawset(L, -3);
    }
}

void EngineStats::publish(lua_State* L) const {
    lua_pushstring(L, "__EngineStats");
    push(L);
    lua_rawset(L, LUA_GLOBALSINDEX);
}

void EngineStats::note_frame(f32 frame_seconds) {
    weighted_frame_seconds_ = weighted_frame_seconds_ * 0.9f + frame_seconds * 0.1f;
    set_float("Frame_Time", weighted_frame_seconds_ * 1000.0f);
    set_float("Frame_FPS", 1.0f / weighted_frame_seconds_);
}

void EngineStats::note_memory() {
    const platform::MemoryUsage usage = platform::memory_usage();
    const auto reserved = static_cast<i64>(usage.address_space);
    const auto resident = static_cast<i64>(usage.resident);
    constexpr i64 kInSmallBlocks = 0;
    set_string("Heap_Reserved", format_byte_size(reserved));
    set_string("Heap_Committed", format_byte_size(resident));
    set_string("Heap_Total", format_byte_size(resident));
    set_string("Heap_InSmallBlocks", format_byte_size(kInSmallBlocks));
    set_string("Heap_InUse", format_byte_size(resident));
    set_string("Heap_TotalCheck", format_byte_size(kInSmallBlocks + resident));
}

void EngineStats::note_entities(const sim::EntityRegistry* registry) {
    constexpr std::array<const char*, 6> kFamilies{"EntityCount_Unit",   "EntityCount_Projectile",
                                                   "EntityCount_Prop",   "EntityCount_Blip",
                                                   "EntityCount_Shield", "EntityCount_Other"};
    std::array<u32, 16> counts{};
    if (registry) counts = registry->family_counts();
    u32 total = 0;
    for (const u32 c : counts) total += c;
    if (total > 0 || find("EntityCount")) set_int("EntityCount", static_cast<i32>(total));
    u32 unknown = 0;
    for (size_t f = 0; f < counts.size(); ++f) {
        if (f >= kFamilies.size()) {
            unknown += counts[f];
            continue;
        }
        if (counts[f] > 0 || find(kFamilies[f])) set_int(kFamilies[f], static_cast<i32>(counts[f]));
    }
    if (unknown > 0 || find("EntityCount_Unknown"))
        set_int("EntityCount_Unknown", static_cast<i32>(unknown));
}

EngineStats& engine_stats() {
    static EngineStats stats;
    return stats;
}

std::string format_byte_size(i64 bytes) {
    if (bytes >= -1024 && bytes <= 1024) return fmt::format("{}B", bytes);
    constexpr std::string_view kSuffixes = "KMGTPE";
    size_t suffix = 0;
    i64 shifted = bytes;
    while ((shifted >= 0x100000 || shifted <= -0x100000) && suffix + 1 < kSuffixes.size()) {
        ++suffix;
        shifted >>= 10;
    }
    const double value = static_cast<double>(shifted) / 1024.0;
    const double size = std::fabs(value);
    const char unit = kSuffixes[suffix];
    if (size < 10.0) return fmt::format("{:.3f}{}", value, unit);
    if (size < 100.0) return fmt::format("{:.2f}{}", value, unit);
    return fmt::format("{:.1f}{}", value, unit);
}

} // namespace osc::lua
