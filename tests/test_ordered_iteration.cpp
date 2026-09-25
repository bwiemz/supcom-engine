// Deterministic iteration (M195): everything the sim walks, and every list
// it hands to scripts, comes in an order that doesn't depend on the standard
// library's hashing, so every platform runs the same lockstep game.

#include <catch2/catch_test_macros.hpp>

#include "sim/entity_registry.hpp"
#include "sim/manipulator.hpp"
#include "sim/prop.hpp"
#include "sim/unit.hpp"

#include <array>
#include <memory>
#include <vector>

using osc::sim::Entity;
using osc::sim::EntityRegistry;
using osc::sim::Prop;
using osc::sim::Unit;

namespace {

osc::u32 add(EntityRegistry& reg, float x = 0, float z = 0) {
    auto e = std::make_unique<Unit>();
    e->set_position({x, 0, z});
    return reg.register_entity(std::move(e));
}

std::vector<osc::u32> walk(const EntityRegistry& reg) {
    std::vector<osc::u32> ids;
    reg.for_each([&](const Entity& e) { ids.push_back(e.entity_id()); });
    return ids;
}

} // namespace

TEST_CASE("for_each visits entities in id order", "[determinism]") {
    EntityRegistry reg;
    std::vector<osc::u32> ids;
    ids.reserve(200);
    for (int i = 0; i < 200; ++i) ids.push_back(add(reg));
    // Remove a scattering, as deaths and expiring projectiles do.
    for (size_t i = 0; i < ids.size(); i += 3) reg.unregister_entity(ids[i]);
    for (int i = 0; i < 50; ++i) add(reg);
    reg.collect_garbage();
    for (int i = 0; i < 10; ++i) add(reg);

    const auto seen = walk(reg);
    REQUIRE(seen.size() == reg.count());
    for (size_t i = 1; i < seen.size(); ++i) CHECK(seen[i - 1] < seen[i]);
}

TEST_CASE("for_each tolerates the walk creating and removing entities", "[determinism]") {
    EntityRegistry reg;
    const osc::u32 a = add(reg);
    const osc::u32 b = add(reg);
    const osc::u32 c = add(reg);

    std::vector<osc::u32> seen;
    osc::u32 spawned = 0;
    reg.for_each([&](const Entity& e) {
        seen.push_back(e.entity_id());
        if (e.entity_id() == a) {
            reg.unregister_entity(b); // dies before its turn: not visited
            spawned = add(reg);       // born during the walk: visited next walk
        }
    });
    CHECK(seen == std::vector<osc::u32>{a, c});
    CHECK(walk(reg) == std::vector<osc::u32>{a, c, spawned});
}

TEST_CASE("for_each_unit visits the units alone, in id order", "[determinism]") {
    EntityRegistry reg;
    std::vector<osc::u32> units;
    std::vector<osc::u32> props;
    for (int i = 0; i < 60; ++i) {
        if (i % 4 == 0) units.push_back(add(reg));
        else props.push_back(reg.register_entity(std::make_unique<Prop>()));
    }
    const auto unit_walk = [&reg] {
        std::vector<osc::u32> ids;
        reg.for_each_unit([&](const Entity& e) {
            CHECK(e.is_unit());
            ids.push_back(e.entity_id());
        });
        return ids;
    };
    CHECK(unit_walk() == units);

    // Removals leave both walks consistent, before and after compaction.
    reg.unregister_entity(units[3]);
    reg.unregister_entity(props[5]);
    units.erase(units.begin() + 3);
    CHECK(unit_walk() == units);
    reg.collect_garbage();
    CHECK(unit_walk() == units);
    units.push_back(add(reg));
    CHECK(unit_walk() == units);
    CHECK(walk(reg).size() == units.size() + props.size() - 1);

    // A unit born during a unit walk is visited by the next one; one that
    // dies before its turn is skipped.
    std::vector<osc::u32> seen;
    osc::u32 spawned = 0;
    reg.for_each_unit([&](const Entity& e) {
        seen.push_back(e.entity_id());
        if (e.entity_id() == units[0]) {
            reg.unregister_entity(units[1]);
            spawned = add(reg);
        }
    });
    units.erase(units.begin() + 1);
    CHECK(seen == units);
    units.push_back(spawned);
    CHECK(unit_walk() == units);
}

TEST_CASE("Spatial queries return ids in ascending order", "[determinism]") {
    EntityRegistry reg;
    reg.init_spatial_grid(512, 512);
    // Spread across cells, registered and moved in a scrambled order.
    std::vector<osc::u32> ids;
    ids.reserve(40);
    for (int i = 0; i < 40; ++i)
        ids.push_back(
            add(reg, static_cast<float>((i * 37) % 200), static_cast<float>((i * 53) % 200)));
    for (size_t i = 0; i < ids.size(); i += 4) {
        auto* e = reg.find(ids[i]);
        e->set_position({150.0f - static_cast<float>(i), 0, 20.0f + static_cast<float>(i)});
    }

    for (const auto& found :
         {reg.collect_in_radius(100, 100, 120), reg.collect_in_rect(0, 0, 200, 200)}) {
        REQUIRE(found.size() > 10);
        for (size_t i = 1; i < found.size(); ++i) CHECK(found[i - 1] < found[i]);
    }

    // Without a grid (the O(N) scan) too.
    EntityRegistry flat;
    for (int i = 0; i < 20; ++i) add(flat, static_cast<float>(i), 0);
    const auto all = flat.collect_in_radius(10, 0, 100);
    REQUIRE(all.size() == 20);
    for (size_t i = 1; i < all.size(); ++i) CHECK(all[i - 1] < all[i]);
}

TEST_CASE("A unit's neighbours and enhancements come in a fixed order", "[determinism]") {
    Unit u;
    for (osc::u32 id : {40u, 7u, 1000u, 3u, 99u}) u.add_adjacent(id);
    std::vector<osc::u32> adj(u.adjacent_unit_ids().begin(), u.adjacent_unit_ids().end());
    CHECK(adj == std::vector<osc::u32>{3, 7, 40, 99, 1000});

    u.add_enhancement("RCH", "ResourceAllocation");
    u.add_enhancement("Back", "Shield");
    u.add_enhancement("LCH", "HeavyAntiMatterCannon");
    std::vector<std::string> slots;
    for (const auto& [slot, name] : u.enhancements()) slots.push_back(slot);
    CHECK(slots == std::vector<std::string>{"Back", "LCH", "RCH"});
}

TEST_CASE("units_in_radius is collect_in_radius's live units, in the same order", "[determinism]") {
    // Units and props mixed across cells; units move between cells, one is
    // destroyed but still registered, and a unit and a prop are removed.
    const auto check_matches = [](EntityRegistry& reg) {
        for (const auto& [x, z, r] : std::vector<std::array<float, 3>>{
                 {100, 100, 60}, {0, 0, 300}, {37, 181, 25}, {199, 5, 0.5f}, {-50, -50, 10}}) {
            std::vector<osc::u32> expected;
            for (osc::u32 id : reg.collect_in_radius(x, z, r)) {
                const Entity* e = reg.find(id);
                if (e && e->is_unit() && !e->destroyed()) expected.push_back(id);
            }
            std::vector<osc::u32> got;
            for (const Entity* e : reg.units_in_radius(x, z, r)) got.push_back(e->entity_id());
            INFO("query at " << x << "," << z << " r " << r);
            CHECK(got == expected);
        }
    };
    const auto fill = [](EntityRegistry& reg) {
        std::vector<osc::u32> units;
        std::vector<osc::u32> props;
        for (int i = 0; i < 60; ++i) {
            const float x = static_cast<float>((i * 37) % 200);
            const float z = static_cast<float>((i * 53) % 200);
            if (i % 3 == 0) {
                auto p = std::make_unique<Prop>();
                p->set_position({x, 0, z});
                props.push_back(reg.register_entity(std::move(p)));
            } else {
                units.push_back(add(reg, x, z));
            }
        }
        for (size_t i = 0; i < units.size(); i += 3)
            reg.find(units[i])->set_position(
                {150.0f - static_cast<float>(i), 0, 20.0f + static_cast<float>(2 * i)});
        reg.find(units[5])->mark_destroyed();
        reg.unregister_entity(units[7]);
        reg.unregister_entity(props[2]);
        return units.size();
    };

    EntityRegistry grid;
    grid.init_spatial_grid(512, 512);
    fill(grid);
    check_matches(grid);
    CHECK(grid.units_in_radius(100, 100, 300).size() == 38); // 40 units, 1 gone, 1 destroyed

    EntityRegistry late; // entities first, the grid after (props before map load)
    fill(late);
    late.init_spatial_grid(512, 512);
    check_matches(late);

    EntityRegistry flat; // no grid: the scan
    fill(flat);
    check_matches(flat);
}
