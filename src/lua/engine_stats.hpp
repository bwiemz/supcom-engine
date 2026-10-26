#pragma once

// Moho's engine statistics (EngineStats, faf-re misc/StatItem.cpp): a tree of
// named items the engine sets as it runs, which the UI state reads as
// __EngineStats, made afresh from the tree every frame (CUIManager's
// PublishEngineStatsToLua, STAT_GetLuaTable 0x0041B390). Retail's debug
// window (/lua/debug/EngineStats.lua, the ShowStats console command) and
// FPS readout (uidebug.lua) read it, and FAF's logger, performance tracker
// and cursor depth.

#include "core/types.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct lua_State;

namespace osc::sim {
class EntityRegistry;
}

namespace osc::lua {

class EngineStats {
public:
    EngineStats() { root_.name = "Root"; }

    /// One part of a path ("Cursor" of "Camera_Cursor_Position"), with a
    /// value once one is set
    struct Item {
        enum class Type { None, Integer, Float, String };
        std::string name;
        Type type = Type::None;
        i32 integer = 0;
        f32 number = 0.0f;
        std::string text;
        std::vector<std::unique_ptr<Item>> children; ///< in the order they were made
    };

    /// The item at `path`, its parts split at '_' (Moho's GetItem): made, and
    /// any parent it lacks, each after its parent's other children.
    Item& item(std::string_view path);
    /// The item at `path`, or null
    const Item* find(std::string_view path) const;

    void set_int(std::string_view path, i32 value);
    void set_float(std::string_view path, f32 value);
    void set_string(std::string_view path, std::string value);

    /// STAT_GetLuaTable: the tree as a new table on L's stack. Each item has
    /// its Name, its Value and Type ("Integer", "Float" or "String"; an empty
    /// string's Value left out) once it has one, and the list of its
    /// Children if it has any (the root an empty one before any stat).
    void push(lua_State* L) const;
    /// PublishEngineStatsToLua: that table as L's __EngineStats
    void publish(lua_State* L) const;

    /// REN_Frame (0x007FA2C0): the frame's time, smoothed a tenth at a time,
    /// as Frame_Time (ms) and Frame_FPS
    void note_frame(f32 frame_seconds);
    /// CScApp::Main's heap figures (Heap_*), from what the process holds:
    /// Reserved its address space, Committed, Total and InUse what of it is
    /// resident, InSmallBlocks 0 (there is no small-block heap here) and
    /// TotalCheck InSmallBlocks + InUse, as faf-re's x64 port gives one
    /// figure for all but the small blocks
    void note_memory();
    /// EntityDB's counts (func_EngineStats_ChngEntityCount 0x00684030):
    /// EntityCount, and EntityCount_Unit, _Projectile, _Prop, _Blip, _Shield,
    /// _Other by an id's family, each made as its first entity comes. With no
    /// registry (no game) the counts made are 0.
    void note_entities(const sim::EntityRegistry* registry);

    const Item& root() const { return root_; }

private:
    Item root_;
    f32 weighted_frame_seconds_ = 0.0f;
};

/// The process's statistics, one tree as Moho keeps (GetEngineStats). The
/// UI's thread alone uses it.
EngineStats& engine_stats();

/// func_FmtByteSize (0x008D1040): "%dB" up to 1024 bytes, then K, M, G (and,
/// past Moho's 32-bit counts, T, P, E) with three, two or one decimals as the
/// figure is under 10, under 100 or more: "512.0M", "18.21M".
std::string format_byte_size(i64 bytes);

} // namespace osc::lua
