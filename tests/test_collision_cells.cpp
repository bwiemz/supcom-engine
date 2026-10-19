// Moho's collision cells (EntityOccupationManager), which GetUnitsInRect and
// GetReclaimablesInRect gather from: what touches a rectangle's 4-unit cells
// by its collision box, in the cells' order.

#include <catch2/catch_test_macros.hpp>

#include "sim/collision_cells.hpp"
#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/prop.hpp"
#include "sim/unit.hpp"

#include <cmath>
#include <memory>
#include <vector>

using osc::f32;
using osc::u32;
using osc::sim::CollisionCells;
using osc::sim::CollisionShape;
using osc::sim::CollisionShapeType;
using osc::sim::Entity;
using osc::sim::EntityRegistry;
using osc::sim::Prop;
using osc::sim::Quaternion;
using osc::sim::Unit;
using osc::sim::Vector3;

namespace {

CollisionShape box(f32 hx, f32 hz) {
    CollisionShape s;
    s.type = CollisionShapeType::BOX;
    s.sx = hx;
    s.sy = 0.5f;
    s.sz = hz;
    return s;
}

CollisionShape sphere(f32 r) {
    CollisionShape s;
    s.type = CollisionShapeType::SPHERE;
    s.sx = r;
    return s;
}

u32 add_unit(EntityRegistry& reg, const Vector3& at, const CollisionShape& shape) {
    auto u = std::make_unique<Unit>();
    u->set_position(at);
    u->set_default_collision_shape(shape);
    return reg.register_entity(std::move(u));
}

u32 add_prop(EntityRegistry& reg, const Vector3& at, const CollisionShape& shape) {
    auto p = std::make_unique<Prop>();
    p->set_position(at);
    p->set_default_collision_shape(shape);
    return reg.register_entity(std::move(p));
}

/// The ids a gather over the world rectangle finds, in its order.
std::vector<u32> gather(EntityRegistry& reg, f32 x0, f32 z0, f32 x1, f32 z1, bool props = true) {
    std::vector<u32> ids;
    for (const Entity* e :
         reg.collision_cells().gather(CollisionCells::query_rect(x0, z0, x1, z1), true, props))
        ids.push_back(e->entity_id());
    return ids;
}

bool same(const CollisionCells::Rect& r, int x, int z, int w, int h) {
    return r.x == x && r.z == z && r.w == w && r.h == h;
}

} // namespace

TEST_CASE("a query covers whole cells, as Moho's func_Rect2fToInt16 rounds it",
          "[collision_cells]") {
    // Aligned: exactly cells 32..47 by 160..175.
    CHECK(same(CollisionCells::query_rect(128, 640, 192, 704), 32, 160, 16, 16));
    // Truncated to whole units, then the far side rounded up to a cell.
    CHECK(same(CollisionCells::query_rect(130.7f, 1, 131.2f, 1), 32, 0, 1, 1));
    CHECK(same(CollisionCells::query_rect(127.9f, 0, 128.1f, 0), 31, 0, 1, 1));
    // Off the map, or inside out: clamped to one cell at the corner.
    CHECK(same(CollisionCells::query_rect(-40, -40, -30, -30), 0, 0, 1, 1));
    CHECK(same(CollisionCells::query_rect(10, 10, 2, 2), 2, 2, 1, 1));
}

TEST_CASE("a box covers the cells its floored and ceiled corners reach (func_AABoxToRect)",
          "[collision_cells]") {
    CHECK(same(CollisionCells::box_rect(127.5f, 0, 128.5f, 1), 31, 0, 2, 1));
    CHECK(same(CollisionCells::box_rect(5, 5, 5, 5), 1, 1, 1, 1));
    // A far side on a cell's edge stops short of the next cell.
    CHECK(same(CollisionCells::box_rect(124, 0, 128, 1), 31, 0, 1, 1));
    // Off the map: clamped to the edge's cell.
    CHECK(same(CollisionCells::box_rect(-3, -3, 2, 2), 0, 0, 1, 1));
}

TEST_CASE("a gather takes what its box touches, not what stands in the rectangle",
          "[collision_cells]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    // Centred outside the rectangle (x 128 up), its box reaches into it.
    const u32 reaching = add_unit(reg, {126, 0, 140}, box(3, 1));
    // Centred outside, its box short of it.
    add_unit(reg, {124, 0, 140}, box(0.5f, 0.5f));
    // Centred inside with no shape: in no cell.
    add_unit(reg, {150, 0, 140}, CollisionShape{});
    const u32 inside = add_prop(reg, {150, 0, 150}, box(1, 1));
    CHECK(gather(reg, 128, 128, 192, 192) == std::vector<u32>{reaching, inside});
    // GetUnitsInRect's: units alone.
    CHECK(gather(reg, 128, 128, 192, 192, false) == std::vector<u32>{reaching});
}

TEST_CASE("a gather goes cell by cell along each row, units then props, the last linked first",
          "[collision_cells]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    const u32 prop = add_prop(reg, {10, 0, 10}, box(0.5f, 0.5f));
    const u32 first = add_unit(reg, {10, 0, 10}, box(0.5f, 0.5f));
    const u32 second = add_unit(reg, {10.5f, 0, 10.5f}, box(0.5f, 0.5f));
    // Cell (3, 2) comes before cell (2, 3): its row is earlier.
    const u32 next_row = add_unit(reg, {10, 0, 14}, box(0.5f, 0.5f));
    const u32 along = add_unit(reg, {14, 0, 10}, box(0.5f, 0.5f));
    CHECK(gather(reg, 8, 8, 16, 16) == std::vector<u32>{second, first, prop, along, next_row});
    // A box over several cells comes once, where it is first met.
    const u32 wide = add_unit(reg, {12, 0, 10}, box(3, 0.5f));
    CHECK(gather(reg, 8, 8, 16, 16) ==
          std::vector<u32>{wide, second, first, prop, along, next_row});
}

TEST_CASE("a move relinks as it is committed, and only if its cells changed", "[collision_cells]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    const u32 a = add_unit(reg, {10, 0, 10}, box(0.5f, 0.5f));
    const u32 b = add_unit(reg, {10, 0, 10}, box(0.5f, 0.5f));
    Entity& ea = *reg.find(a);
    Entity& eb = *reg.find(b);

    // Within its cells: no relink, so b stays first.
    ea.set_position({10.5f, 0, 10.2f});
    reg.advance_coords();
    CHECK(gather(reg, 8, 8, 12, 12) == std::vector<u32>{b, a});

    // Away: the cells see it only once they advance.
    ea.set_position({100, 0, 100});
    CHECK(gather(reg, 8, 8, 12, 12) == std::vector<u32>{b, a});
    CHECK(gather(reg, 96, 96, 104, 104).empty());
    reg.advance_coords();
    CHECK(gather(reg, 8, 8, 12, 12) == std::vector<u32>{b});
    CHECK(gather(reg, 96, 96, 104, 104) == std::vector<u32>{a});

    // Two moves in one tick relink as they are committed, in id order (Moho's coordinate
    // list), both into cell (25, 25): b is committed last, so it comes first.
    ea.set_position({40, 0, 40});
    eb.set_position({102, 0, 101.8f});
    ea.set_position({102, 0, 102});
    reg.advance_coords();
    CHECK(gather(reg, 96, 96, 104, 104) == std::vector<u32>{b, a});

    // Away and back within a tick: its cells never changed, so no relink.
    ea.set_position({300, 0, 300});
    ea.set_position({102, 0, 102});
    reg.advance_coords();
    CHECK(gather(reg, 96, 96, 104, 104) == std::vector<u32>{b, a});
}

TEST_CASE("turning a long box moves its cells", "[collision_cells]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    // 12 long across x: cells 24 to 26 of row 25.
    const u32 id = add_unit(reg, {102, 0, 102}, box(6, 0.5f));
    CHECK(gather(reg, 104, 100, 107, 103) == std::vector<u32>{id}); // cell (26, 25)
    CHECK(gather(reg, 100, 104, 103, 107).empty());                 // cell (25, 26)
    // A quarter turn about y: its length now runs along z, and its cells
    // follow once they advance.
    const f32 h = std::sqrt(0.5f);
    reg.find(id)->set_orientation(Quaternion{0, h, 0, h});
    CHECK(gather(reg, 104, 100, 107, 103) == std::vector<u32>{id});
    reg.advance_coords();
    CHECK(gather(reg, 104, 100, 107, 103).empty());
    CHECK(gather(reg, 100, 104, 103, 107) == std::vector<u32>{id});
}

TEST_CASE("a new shape relinks at once; none takes it out", "[collision_cells]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    const u32 id = add_unit(reg, {100, 0, 100}, box(0.5f, 0.5f));
    CHECK(gather(reg, 116, 96, 120, 104).empty());
    reg.find(id)->set_collision_shape(sphere(20));
    CHECK(gather(reg, 116, 96, 120, 104) == std::vector<u32>{id});
    reg.find(id)->set_collision_shape(CollisionShape{});
    CHECK(gather(reg, 96, 96, 104, 104).empty());
    reg.find(id)->revert_collision_shape();
    CHECK(gather(reg, 96, 96, 104, 104) == std::vector<u32>{id});
}

TEST_CASE("a destroyed entity is found until it is freed", "[collision_cells]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    const u32 id = add_unit(reg, {100, 0, 100}, box(0.5f, 0.5f));
    // Moved, then destroyed in the same tick: its move is dropped with it.
    reg.find(id)->set_position({200, 0, 200});
    reg.unregister_entity(id);
    CHECK(gather(reg, 96, 96, 104, 104) == std::vector<u32>{id});
    reg.collect_garbage();
    CHECK(gather(reg, 96, 96, 104, 104).empty());
    reg.advance_coords();
    CHECK(gather(reg, 196, 196, 204, 204).empty());
}

TEST_CASE("a box past the map's edge wraps into the next row, as Moho's index does",
          "[collision_cells]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512); // 128 cells across
    // Cell x 130 of row 10 is cell 2 of row 11.
    const u32 id = add_unit(reg, {522, 0, 42}, box(0.5f, 0.5f));
    CHECK(gather(reg, 8, 44, 12, 48) == std::vector<u32>{id});
    // A gather itself stops at the edge.
    CHECK(gather(reg, 508, 40, 600, 44).empty());
}

TEST_CASE("a box wider than the map covers it once; wild numbers stay defined",
          "[collision_cells]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512); // 128 cells square
    // A broken orientation once scaled boxes past 10^8 units: Moho would
    // fill 65535 cells square, where the map has 128.
    const u32 id = add_unit(reg, {100, 0, 100}, sphere(1e7f));
    CHECK(reg.find(id)->cell_link().w == 128);
    CHECK(reg.find(id)->cell_link().h == 128);
    CHECK(gather(reg, 500, 500, 504, 504) == std::vector<u32>{id});
    // NaN and infinities are clamped before converting to ints.
    CHECK(same(CollisionCells::query_rect(std::nanf(""), -INFINITY, INFINITY, 1e30f), 0, 0, 65535,
               65535));
    CHECK(same(CollisionCells::box_rect(std::nanf(""), 8, 9, INFINITY), 0, 2, 3, 65533));
}

TEST_CASE("a snapshot's links come back in their order, and go on from its count",
          "[collision_cells]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    const u32 a = add_unit(reg, {10, 0, 10}, box(0.5f, 0.5f));
    const u32 b = add_prop(reg, {10, 0, 10}, box(0.5f, 0.5f));
    const u32 c = add_unit(reg, {10, 0, 10}, box(0.5f, 0.5f));
    // a goes and comes back: relinked last, so first among the units.
    reg.find(a)->set_position({100, 0, 100});
    reg.advance_coords();
    reg.find(a)->set_position({10, 0, 10});
    reg.advance_coords();
    const auto before = gather(reg, 8, 8, 12, 12);
    CHECK(before == std::vector<u32>{a, c, b});
    const auto count = reg.collision_cells().link_count();

    // As a load does: cells emptied, each entity's seq kept.
    auto& cells = reg.collision_cells();
    cells.clear();
    cells.restore({reg.find(c), reg.find(a), reg.find(b)}, count);
    CHECK(gather(reg, 8, 8, 12, 12) == before);
    CHECK(cells.link_count() == count);
    const u32 d = add_unit(reg, {10, 0, 10}, box(0.5f, 0.5f));
    CHECK(reg.find(d)->cell_link().seq == count + 1);
    CHECK(gather(reg, 8, 8, 12, 12) == std::vector<u32>{d, a, c, b});
}
