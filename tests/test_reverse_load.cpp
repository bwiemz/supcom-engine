// The Transport order on a unit that calls transports: Moho's
// UNITCOMMAND_TransportReverseLoadUnits.

#include <catch2/catch_test_macros.hpp>

#include "map/heightmap.hpp"
#include "map/terrain.hpp"
#include "renderer/input_handler.hpp"
#include "sim/command_codec.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/manipulator.hpp"
#include "sim/saved_game.hpp"
#include "sim/sim_snapshot.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using osc::sim::CommandType;
using osc::sim::EntityId;
using osc::sim::SimState;
using osc::sim::Unit;

namespace {

constexpr int kReverseLoad = 23;

struct World {
    lua_State* L = lua_open();
    SimState sim{L, nullptr};
    osc::renderer::InputHandler input;

    World() {
        std::vector<osc::u16> heights(129 * 129, 0);
        osc::map::Heightmap hm(128, 128, 1.0f / 128.0f, std::move(heights));
        sim.set_terrain(std::make_unique<osc::map::Terrain>(std::move(hm), 0.0f, false));
        sim.build_pathfinding_grid();
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
        sim.set_fog_of_war("none");
        sim.set_victory_condition("sandbox");
        input.set_player_army(0);
    }
    ~World() { lua_close(L); }
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    Unit& unit(const char* motion, osc::f32 x, osc::f32 z) {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        u->set_motion_type(motion);
        u->set_position({x, 0.0f, z});
        u->set_max_speed(5.0f);
        Unit::Drive drive;
        drive.max_accel = 50.0f;
        u->set_drive(drive);
        u->set_max_health(100.0f);
        u->set_health(100.0f);
        for (const char* cap : {"RULEUCC_Move", "RULEUCC_Guard"}) {
            u->add_command_cap(cap);
        }
        Unit& made = *u;
        sim.entity_registry().register_entity(std::move(u));
        return made;
    }
    Unit& transport(osc::f32 x, osc::f32 z) {
        Unit& t = unit("RULEUMT_Air", x, z);
        for (const char* c : {"TRANSPORTATION", "AIR", "TRANSPORTFOCUS"}) {
            t.add_category(c);
        }
        t.add_command_cap("RULEUCC_Transport");
        return t;
    }
    Unit& tank(osc::f32 x, osc::f32 z) {
        Unit& u = unit("RULEUMT_Land", x, z);
        u.add_category("LAND");
        u.add_command_cap("RULEUCC_CallTransport");
        return u;
    }

    std::optional<osc::renderer::IssuedCommand> transport_click(osc::f32 x, osc::f32 z) {
        osc::renderer::CommandMode mode;
        mode.mode = "order";
        mode.name = "RULEUCC_Transport";
        return input.click_in_command_mode(sim, mode, x, z, false);
    }
    void apply() {
        for (int t = 0; t < 5 && !sim.command_scheduler().pending().empty(); ++t) {
            sim.tick();
        }
    }
    void move_far(Unit& u) {
        osc::sim::UnitCommand move;
        move.type = CommandType::Move;
        move.target_pos = {120.0f, 0.0f, 120.0f};
        u.push_command(move, true);
    }
};

int head(const Unit& u) {
    return u.command_queue().empty() ? 0 : osc::sim::moho_command_number(u.command_queue().front());
}

std::vector<EntityId> sorted(std::vector<EntityId> ids) {
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace

TEST_CASE("A Transport click on a unit that calls transports gives the selection a reverse load "
          "at it",
          "[transport][reverse-load]") {
    World w;
    Unit& t = w.transport(40.0f, 40.0f);
    Unit& y = w.tank(20.0f, 40.0f);
    Unit& x = w.tank(60.0f, 40.0f);
    w.input.set_selected({t.entity_id(), y.entity_id()});
    const auto issued = w.transport_click(60.0f, 40.0f);
    REQUIRE(issued);
    CHECK(issued->type == "TransportReverseLoadUnits");
    CHECK(issued->target_id == x.entity_id());
    CHECK(issued->units == sorted({t.entity_id(), y.entity_id()}));
    const auto pending = w.sim.command_scheduler().pending();
    REQUIRE(pending.size() == 1);
    CHECK(osc::sim::moho_command_number(pending[0].command) == kReverseLoad);
    CHECK(pending[0].command.target_id == x.entity_id());

    w.apply();
    CHECK(head(t) == kReverseLoad);
    CHECK(head(x) == kReverseLoad);
    CHECK(y.command_queue().empty());
    CHECK(t.pickup_running());
    CHECK(t.pickup_ids() == std::vector<EntityId>{x.entity_id()});
    CHECK(x.awaited_transport() == t.entity_id());
}

TEST_CASE("The reverse load goes to the selected transport nearest its target, an idle one at "
          "half its distance, and leaves the rest of the selection be",
          "[transport][reverse-load]") {
    World w;
    Unit& x = w.tank(60.0f, 40.0f);
    Unit& near = w.transport(60.0f, 62.0f);
    Unit& far = w.transport(60.0f, 74.0f);
    Unit& y = w.tank(20.0f, 40.0f);
    w.move_far(y);
    w.input.set_selected({near.entity_id(), far.entity_id(), y.entity_id()});

    SECTION("the nearer busy, the farther idle: the idle one") {
        w.move_far(near);
        REQUIRE(w.transport_click(60.0f, 40.0f));
        w.apply();
        CHECK(head(far) == kReverseLoad);
        CHECK(head(near) == 2);
        CHECK(x.awaited_transport() == far.entity_id());
    }
    SECTION("both idle: the nearer") {
        REQUIRE(w.transport_click(60.0f, 40.0f));
        w.apply();
        CHECK(head(near) == kReverseLoad);
        CHECK(far.command_queue().empty());
        CHECK(x.awaited_transport() == near.entity_id());
    }
    SECTION("both busy: the nearer") {
        w.move_far(near);
        w.move_far(far);
        REQUIRE(w.transport_click(60.0f, 40.0f));
        w.apply();
        CHECK(head(near) == kReverseLoad);
        CHECK(head(far) == 2);
    }
    CHECK(head(y) == 2);
    CHECK(head(x) == kReverseLoad);
}

TEST_CASE("A reverse load gives no unit an order when no selected transport has room for its "
          "target, or its target doesn't move",
          "[transport][reverse-load]") {
    World w;
    Unit& t = w.transport(40.0f, 40.0f);
    Unit& x = w.tank(60.0f, 40.0f);
    w.move_far(t);
    w.input.set_selected({t.entity_id()});

    SECTION("a full transport") {
        Unit& aboard = w.tank(40.0f, 40.0f);
        t.set_transport_capacity(1);
        aboard.attach_to_transport(&t, w.sim.entity_registry(), w.L);
    }
    SECTION("a target that doesn't move") {
        x.set_motion_type("RULEUMT_None");
    }
    REQUIRE(w.transport_click(60.0f, 40.0f));
    w.apply();
    CHECK(head(t) == 2);
    CHECK(x.command_queue().empty());
}

TEST_CASE("A right click with a transport selected on a unit it carries is a reverse load",
          "[transport][reverse-load][right-click]") {
    World w;
    Unit& t = w.transport(40.0f, 40.0f);
    Unit& x = w.tank(60.0f, 40.0f);
    w.input.set_selected({t.entity_id()});
    const auto order_on = [&](const Unit& on) {
        const auto orders = w.input.right_click_orders(w.sim, on.position().x, on.position().z);
        REQUIRE(orders.size() == 1);
        CHECK(orders.front().first.target_id == on.entity_id());
        return osc::sim::moho_command_number(orders.front().first);
    };

    CHECK(order_on(x) == kReverseLoad);
    const auto issued = w.input.right_click_at(w.sim, 60.0f, 40.0f, false);
    REQUIRE(issued.size() == 1);
    CHECK(issued.front().type == "TransportReverseLoadUnits");

    SECTION("not by a transport that isn't TRANSPORTFOCUS") {
        Unit& plain = w.unit("RULEUMT_Air", 40.0f, 20.0f);
        plain.add_category("TRANSPORTATION");
        w.input.set_selected({plain.entity_id()});
        CHECK(order_on(x) != kReverseLoad);
    }
    SECTION("not on an experimental") {
        x.add_category("EXPERIMENTAL");
        CHECK(order_on(x) != kReverseLoad);
    }
    SECTION("a commander only by a transport that carries one") {
        x.add_category("COMMAND");
        CHECK(order_on(x) != kReverseLoad);
        t.add_category("CANTRANSPORTCOMMANDER");
        CHECK(order_on(x) == kReverseLoad);
    }
    SECTION("an aircraft only by an air staging platform") {
        Unit& plane = w.unit("RULEUMT_Air", 60.0f, 70.0f);
        plane.add_category("AIR");
        plane.add_command_cap("RULEUCC_CallTransport");
        CHECK(order_on(plane) != kReverseLoad);
        Unit& pad = w.unit("RULEUMT_None", 20.0f, 70.0f);
        for (const char* c : {"AIRSTAGINGPLATFORM", "TRANSPORTFOCUS"}) {
            pad.add_category(c);
        }
        w.input.set_selected({pad.entity_id()});
        CHECK(order_on(plane) == kReverseLoad);
    }
}

TEST_CASE("A reverse load survives the codec and a save, its transport kept",
          "[transport][reverse-load][replay]") {
    World w;
    Unit& t = w.transport(40.0f, 40.0f);
    Unit& x = w.tank(60.0f, 40.0f);
    w.input.set_selected({t.entity_id()});
    REQUIRE(w.transport_click(60.0f, 40.0f));
    const auto pending = w.sim.command_scheduler().pending();
    REQUIRE(pending.size() == 1);
    std::vector<osc::u8> bytes;
    osc::sim::ByteWriter out(bytes);
    osc::sim::write_command(out, pending[0]);
    osc::sim::ByteReader in(bytes);
    osc::sim::ScheduledCommand back;
    REQUIRE(osc::sim::read_command(in, back));
    CHECK(osc::sim::moho_command_number(back.command) == kReverseLoad);
    CHECK(back.command.target_id == x.entity_id());

    w.apply();
    REQUIRE(head(x) == kReverseLoad);
    const osc::sim::SavedGame save = osc::sim::save_game(w.sim, "loading");
    World loaded;
    loaded.transport(40.0f, 40.0f);
    loaded.tank(60.0f, 40.0f);
    const std::string err = osc::sim::load_snapshot(loaded.sim, save.snapshot);
    INFO(err);
    REQUIRE(err.empty());
    const auto* tank = static_cast<const Unit*>(loaded.sim.entity_registry().find(x.entity_id()));
    REQUIRE(tank);
    CHECK(head(*tank) == kReverseLoad);
    CHECK(tank->awaited_transport() == t.entity_id());
    CHECK(loaded.sim.compute_sync_checksum() == w.sim.compute_sync_checksum());
}
