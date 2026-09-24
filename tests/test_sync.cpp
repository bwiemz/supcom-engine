// Deterministic sync-checksum tests — the desync primitive for lockstep
// multiplayer and a single-player determinism guard.

#include <catch2/catch_test_macros.hpp>

#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/shield.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <memory>
#include <string>
#include <vector>

using osc::sim::SimState;
using osc::sim::Unit;

namespace {

struct LuaGuard {
    lua_State* L = lua_open();
    ~LuaGuard() { lua_close(L); }
};

// Populate a sim with an identical starting layout.
void populate(SimState& sim) {
    sim.add_army("ARMY_1", "ARMY_1");
    sim.add_army("ARMY_2", "ARMY_2");
    for (int army = 0; army < 2; ++army) {
        auto u = std::make_unique<Unit>();
        u->set_army(army);
        u->add_category("COMMAND");
        u->set_health(1000.0f);
        u->set_position({static_cast<osc::f32>(army * 50), 0.0f, 0.0f});
        sim.entity_registry().register_entity(std::move(u));
    }
}

} // namespace

TEST_CASE("Identical sims produce identical checksums", "[sync]") {
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr);
    SimState b(gb.L, nullptr);
    populate(a);
    populate(b);

    CHECK(a.compute_sync_checksum() == b.compute_sync_checksum());

    for (int i = 0; i < 20; ++i) {
        a.tick();
        b.tick();
    }
    CHECK(a.compute_sync_checksum() == b.compute_sync_checksum());
}

TEST_CASE("A state divergence changes the checksum", "[sync]") {
    LuaGuard ga, gb;
    SimState a(ga.L, nullptr);
    SimState b(gb.L, nullptr);
    populate(a);
    populate(b);
    REQUIRE(a.compute_sync_checksum() == b.compute_sync_checksum());

    SECTION("position drift") {
        auto* u = static_cast<Unit*>(a.entity_registry().find(1));
        REQUIRE(u != nullptr);
        u->set_position({1.0f, 0.0f, 0.0f});
        CHECK(a.compute_sync_checksum() != b.compute_sync_checksum());
    }
    SECTION("health drift") {
        auto* u = static_cast<Unit*>(a.entity_registry().find(1));
        REQUIRE(u != nullptr);
        u->set_health(999.0f);
        CHECK(a.compute_sync_checksum() != b.compute_sync_checksum());
    }
    SECTION("a destroyed unit") {
        a.entity_registry().find(2)->mark_destroyed();
        CHECK(a.compute_sync_checksum() != b.compute_sync_checksum());
    }
    SECTION("tick count") {
        a.tick();
        CHECK(a.compute_sync_checksum() != b.compute_sync_checksum());
    }
}

TEST_CASE("Each checksum domain covers its own state, and only it", "[sync]") {
    // A change of one kind moves its own domain and no other, so a desync
    // report points at the right state.
    const auto differing = [](const SimState& a, const SimState& b) {
        const auto va = a.checksum_parts().values();
        const auto vb = b.checksum_parts().values();
        std::vector<std::string> out;
        for (size_t i = 0; i < va.size(); ++i)
            if (va[i] != vb[i]) out.emplace_back(SimState::ChecksumParts::kNames[i]);
        return out;
    };
    const auto first_unit = [](SimState& sim) {
        Unit* found = nullptr;
        sim.entity_registry().for_each_unit([&](osc::sim::Entity& e) {
            if (!found) found = static_cast<Unit*>(&e);
        });
        return found;
    };
    using Names = std::vector<std::string>;

    SECTION("a player setting: units") {
        LuaGuard ga, gb;
        SimState a(ga.L, nullptr), b(gb.L, nullptr);
        populate(a);
        populate(b);
        first_unit(b)->set_fire_state(1);
        CHECK(differing(a, b) == Names{"units"});
    }
    SECTION("an order: orders") {
        LuaGuard ga, gb;
        SimState a(ga.L, nullptr), b(gb.L, nullptr);
        populate(a);
        populate(b);
        osc::sim::UnitCommand move;
        move.type = osc::sim::CommandType::Move;
        move.target_pos = {30.0f, 0.0f, 0.0f};
        first_unit(b)->push_command(move, true);
        CHECK(differing(a, b) == Names{"orders"});
    }
    SECTION("a position: entities") {
        LuaGuard ga, gb;
        SimState a(ga.L, nullptr), b(gb.L, nullptr);
        populate(a);
        populate(b);
        first_unit(b)->set_position({1.0f, 0.0f, 0.0f});
        CHECK(differing(a, b) == Names{"entities"});
    }
    SECTION("an economy event: economy_events") {
        LuaGuard ga, gb;
        SimState a(ga.L, nullptr), b(gb.L, nullptr);
        populate(a);
        populate(b);
        b.economy_events().create(first_unit(b)->entity_id(), 0.0, 10.0, 1.0);
        CHECK(differing(a, b) == Names{"economy_events"});
    }
    SECTION("stored resources: armies") {
        LuaGuard ga, gb;
        SimState a(ga.L, nullptr), b(gb.L, nullptr);
        populate(a);
        populate(b);
        b.army_at(0)->set_stored_resources(100.0, 0.0);
        CHECK(differing(a, b) == Names{"armies"});
    }
}
