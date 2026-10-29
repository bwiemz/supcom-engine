#include <catch2/catch_test_macros.hpp>

#include "sim/entity.hpp"
#include "sim/entity_registry.hpp"

#include <memory>

using osc::u32;
using osc::sim::Entity;
using osc::sim::EntityRegistry;

namespace {

std::unique_ptr<Entity> script_entity(int army) {
    auto e = std::make_unique<Entity>();
    e->set_army(army);
    return e;
}

} // namespace

TEST_CASE("A pool past 2^20 serials never hands out a live entity's id", "[sim][ids]") {
    EntityRegistry reg;
    const u32 keeper_id = reg.register_entity(script_entity(1));
    const Entity* keeper = reg.find(keeper_id);
    REQUIRE(EntityRegistry::moho_id(keeper_id) == 0x501u << 20);

    for (u32 i = 0; i < (1u << 20); ++i) {
        reg.unregister_entity(reg.register_entity(script_entity(0)));
        if (i % 4096 == 0) {
            reg.collect_garbage();
        }
    }
    const u32 id = reg.register_entity(script_entity(0));

    CHECK(reg.find(keeper_id) == keeper);
    CHECK(EntityRegistry::moho_id(id) == (0x501u << 20 | 1u));
    CHECK(reg.count() == 2);
}
