// Moho's engine statistics (faf-re misc/StatItem.cpp): a tree of items by
// '_'-split paths, which the UI state reads as __EngineStats, made afresh
// each frame (STAT_GetLuaTable). Retail's debug window and FPS readout read
// it, and FAF's logger and performance tracker.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "lua/engine_stats.hpp"
#include "lua/lua_state.hpp"
#include "lua/moho_bindings.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/projectile.hpp"
#include "sim/prop.hpp"
#include "sim/unit.hpp"
#include "ui/console.hpp"

#include <memory>
#include <string>

extern "C" {
#include <lua.h>
}

using Catch::Approx;
using osc::lua::EngineStats;
using osc::lua::format_byte_size;

namespace {

/// Run `code` in L, answering its error (empty if none)
std::string run(osc::lua::LuaState& lua, const char* code) {
    auto r = lua.do_string(code);
    return r.ok() ? std::string() : r.error().message;
}

} // namespace

TEST_CASE("The stats tree splits its paths at '_', its items in the order they were made",
          "[stats]") {
    EngineStats stats;
    stats.set_string("Camera_Cursor_Position", "x=1.00,y=2.00,z=3.00");
    stats.set_float("Camera_FocusDistance", 516.5f);
    stats.set_int("EntityCount", 4);
    stats.set_int("EntityCount_Unit", 3);
    stats.set_string("Heap_Address Space_Free", "");
    REQUIRE(stats.find("Camera_Cursor_Position"));
    CHECK(stats.find("Camera_Cursor_Position")->text == "x=1.00,y=2.00,z=3.00");
    CHECK(stats.find("Camera_Cursor")->type == EngineStats::Item::Type::None);
    CHECK_FALSE(stats.find("Camera_Cursor_OCell"));
    CHECK_FALSE(stats.find("Frame"));

    osc::lua::LuaState lua;
    stats.publish(lua.raw());
    const std::string error = run(lua, R"(
        local root = __EngineStats
        assert(root.Name == 'Root' and root.Type == nil and root.Value == nil, 'root')
        local camera, count, heap = root.Children[1], root.Children[2], root.Children[3]
        assert(table.getn(root.Children) == 3, 'three')
        -- A part with no value of its own: its Name and Children alone
        assert(camera.Name == 'Camera' and camera.Type == nil, 'camera')
        local cursor, focus = camera.Children[1], camera.Children[2]
        assert(cursor.Name == 'Cursor' and focus.Name == 'FocusDistance', 'order')
        local position = cursor.Children[1]
        assert(position.Type == 'String' and position.Value == 'x=1.00,y=2.00,z=3.00', 'string')
        assert(position.Children == nil, 'a leaf has none')
        assert(focus.Type == 'Float' and focus.Value == 516.5, 'float')
        -- An item with a value and children has both
        assert(count.Type == 'Integer' and count.Value == 4, 'count')
        assert(count.Children[1].Name == 'Unit' and count.Children[1].Value == 3, 'unit')
        -- An empty string has its Type and no Value
        local free = heap.Children[1].Children[1]
        assert(heap.Children[1].Name == 'Address Space' and free.Name == 'Free', 'path')
        assert(free.Type == 'String' and free.Value == nil, 'empty')
    )");
    INFO(error);
    CHECK(error.empty());

    // Made afresh: a script's changes to the last frame's are gone
    REQUIRE(run(lua, "__EngineStats.Children = nil").empty());
    stats.publish(lua.raw());
    CHECK(run(lua, "assert(table.getn(__EngineStats.Children) == 3)").empty());

    // Before any stat, the root's list is there, empty
    EngineStats none;
    none.publish(lua.raw());
    CHECK(run(lua, "assert(table.getn(__EngineStats.Children) == 0)").empty());
}

TEST_CASE("Byte sizes read as Moho's func_FmtByteSize writes them", "[stats]") {
    CHECK(format_byte_size(0) == "0B");
    CHECK(format_byte_size(1024) == "1024B");
    CHECK(format_byte_size(-1024) == "-1024B");
    CHECK(format_byte_size(1025) == "1.001K");
    CHECK(format_byte_size(2048) == "2.000K");
    CHECK(format_byte_size(0x100000) == "1.000M");
    // The dump of Moho's tree in FAF's performance.lua
    CHECK(format_byte_size(512LL << 20) == "512.0M");
    CHECK(format_byte_size(19094569) == "18.21M");
    CHECK(format_byte_size(391851000) == "373.7M");
    CHECK(format_byte_size(6LL << 30) == "6.000G");
    // Past the 32-bit counts' G
    CHECK(format_byte_size(20LL << 40) == "20.00T");
}

TEST_CASE("Frame_Time and Frame_FPS smooth the frame's time a tenth at a time, as REN_Frame",
          "[stats]") {
    EngineStats stats;
    stats.note_frame(0.1f);
    CHECK(stats.find("Frame_Time")->number == Approx(10.0f));
    CHECK(stats.find("Frame_FPS")->number == Approx(100.0f));
    stats.note_frame(0.1f);
    CHECK(stats.find("Frame_Time")->number == Approx(19.0f));
    CHECK(stats.find("Frame_FPS")->number == Approx(1.0f / 0.019f));
    for (int i = 0; i < 200; ++i) stats.note_frame(0.02f);
    CHECK(stats.find("Frame_Time")->number == Approx(20.0f));
    CHECK(stats.find("Frame_FPS")->number == Approx(50.0f));
}

TEST_CASE("The entity counts follow the ids' families, each made as its first entity comes",
          "[stats]") {
    EngineStats stats;
    stats.note_entities(nullptr);
    CHECK_FALSE(stats.find("EntityCount"));

    osc::sim::EntityRegistry reg;
    reg.register_entity(std::make_unique<osc::sim::Prop>());
    reg.register_entity(std::make_unique<osc::sim::Prop>());
    stats.note_entities(&reg);
    CHECK(stats.find("EntityCount")->integer == 2);
    CHECK(stats.find("EntityCount_Prop")->integer == 2);
    CHECK_FALSE(stats.find("EntityCount_Unit"));

    auto unit = std::make_unique<osc::sim::Unit>();
    unit->set_army(0);
    const osc::sim::EntityId unit_id = reg.register_entity(std::move(unit));
    reg.register_entity(std::make_unique<osc::sim::Projectile>());
    stats.note_entities(&reg);
    CHECK(stats.find("EntityCount")->integer == 4);
    CHECK(stats.find("EntityCount_Unit")->integer == 1);
    CHECK(stats.find("EntityCount_Projectile")->integer == 1);
    // In the order they came: the props, then the unit and the projectile
    const auto& families = stats.find("EntityCount")->children;
    REQUIRE(families.size() == 3);
    CHECK(families[0]->name == "Prop");
    CHECK(families[1]->name == "Unit");
    CHECK(families[2]->name == "Projectile");

    // Gone, its count stays at 0; with no game, all do
    reg.unregister_entity(unit_id);
    stats.note_entities(&reg);
    CHECK(stats.find("EntityCount_Unit")->integer == 0);
    CHECK(stats.find("EntityCount")->integer == 3);
    stats.note_entities(nullptr);
    CHECK(stats.find("EntityCount")->integer == 0);
    CHECK(stats.find("EntityCount_Prop")->integer == 0);
}

TEST_CASE("The Heap stats give the process's memory in Moho's six figures", "[stats]") {
    EngineStats stats;
    stats.note_memory();
    for (const char* name : {"Heap_Reserved", "Heap_Committed", "Heap_Total", "Heap_InSmallBlocks",
                             "Heap_InUse", "Heap_TotalCheck"}) {
        INFO(name);
        REQUIRE(stats.find(name));
        CHECK(stats.find(name)->type == EngineStats::Item::Type::String);
    }
    CHECK(stats.find("Heap_InSmallBlocks")->text == "0B");
    // No small blocks: the check is what is in use
    CHECK(stats.find("Heap_TotalCheck")->text == stats.find("Heap_InUse")->text);
#ifdef __linux__
    // A running process has some of its memory resident
    CHECK(stats.find("Heap_InUse")->text != "0B");
#endif
}

TEST_CASE("ShowStats toggles retail's engine statistics window on a section, or all",
          "[stats][console]") {
    osc::lua::LuaState lua;
    osc::ui::Console console;
    osc::lua::register_console_commands(console);
    REQUIRE(run(lua, R"(
        toggled = {}
        modules = { ['/lua/debug/EngineStats.lua'] = {
            Toggle = function(section) table.insert(toggled, section) end } }
        function import(name) return modules[name] end
    )")
                .empty());
    console.execute(lua.raw(), "ShowStats");
    console.execute(lua.raw(), "ShowStats frame");
    CHECK(run(lua, "assert(table.getn(toggled) == 2 and toggled[1] == 'all' and "
                   "toggled[2] == 'frame')")
              .empty());
}
