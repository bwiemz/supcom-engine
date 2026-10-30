// The checks of Moho's UNIT_IssueCommand and ProcessIssuedUnitCommand (faf-re sim/Sim.cpp).

#include <catch2/catch_test_macros.hpp>

#include "sim/army_brain.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
}

#include <array>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

struct IssueWorld {
    lua_State* L = lua_open();
    SimState sim{L, nullptr};

    IssueWorld() {
        sim.add_army("ARMY_1", "ARMY_1");
        sim.add_army("ARMY_2", "ARMY_2");
    }
    ~IssueWorld() { lua_close(L); }
    IssueWorld(const IssueWorld&) = delete;
    IssueWorld& operator=(const IssueWorld&) = delete;

    Unit& make(int army, std::initializer_list<const char*> categories,
               const char* motion = "RULEUMT_Air", osc::sim::Vector3 at = {}) {
        auto u = std::make_unique<Unit>();
        u->set_army(army);
        u->set_position(at);
        u->set_motion_type(motion);
        for (const char* c : categories) {
            u->add_category(c);
        }
        for (const char* cap : {"RULEUCC_Move", "RULEUCC_Guard", "RULEUCC_Patrol"}) {
            u->add_command_cap(cap);
        }
        Unit& made = *u;
        sim.entity_registry().register_entity(std::move(u));
        return made;
    }

    bool takes(Unit& u, CommandType type, osc::sim::EntityId target = {}, bool clear = false) {
        UnitCommand c;
        c.type = type;
        c.target_id = target;
        c.target_pos = {100.0f, 0.0f, 100.0f};
        const bool taken = sim.route_command({u.entity_id()}, c, clear) != 0;
        UnitCommand stop;
        stop.type = CommandType::Stop;
        sim.route_command({u.entity_id()}, stop, true);
        return taken;
    }
};

} // namespace

TEST_CASE("A carried unit takes a move or a guard only as a pod or from a carrier, and a reclaim, "
          "capture or repair only as a pod",
          "[routing][transport]") {
    struct Case {
        const char* holder;
        bool pod;
        std::array<bool, 7> taken;
    };
    const Case cases[] = {
        {nullptr, false, {true, true, true, true, true, true, true}},
        {nullptr, true, {true, true, true, true, true, true, true}},
        {"TRANSPORTATION", false, {false, false, false, false, false, false, false}},
        {"TRANSPORTATION", true, {true, true, true, true, true, true, true}},
        {"CARRIER", false, {true, true, true, true, false, false, false}},
        {"CARRIER", true, {true, true, true, true, true, true, true}},
    };
    const CommandType orders[] = {
        CommandType::Move,    CommandType::Patrol,  CommandType::AggressiveMove, CommandType::Guard,
        CommandType::Reclaim, CommandType::Capture, CommandType::Repair};
    for (const Case& c : cases) {
        IssueWorld w;
        Unit& u = c.pod ? w.make(0, {"POD", "RECLAIM", "CAPTURE", "REPAIR"})
                        : w.make(0, {"RECLAIM", "CAPTURE", "REPAIR"});
        if (c.holder) {
            u.set_transport_id(w.make(0, {c.holder}).entity_id());
        }
        Unit& friend_unit = w.make(0, {"RECLAIMABLE"}, "RULEUMT_Land");
        Unit& enemy = w.make(1, {"RECLAIMABLE"}, "RULEUMT_Land");
        for (size_t i = 0; i < std::size(orders); ++i) {
            const osc::sim::EntityId target = orders[i] == CommandType::Capture ? enemy.entity_id()
                                              : orders[i] == CommandType::Move ||
                                                      orders[i] == CommandType::Patrol ||
                                                      orders[i] == CommandType::AggressiveMove
                                                  ? osc::sim::EntityId{}
                                                  : friend_unit.entity_id();
            INFO((c.holder ? c.holder : "free")
                 << (c.pod ? " pod" : "") << ", order " << static_cast<int>(orders[i]));
            CHECK(w.takes(u, orders[i], target) == c.taken[i]);
        }
    }
}

TEST_CASE("An order whose target lies off the playable area or outside the No Rush zone is refused",
          "[routing][norush]") {
    struct Case {
        const char* what;
        CommandType type;
        osc::f32 x;
        bool at_unit;
        bool whole_map;
        bool no_rush;
        bool taken;
    };
    const Case cases[] = {
        {"a move inside", CommandType::Move, 90.0f, false, false, false, true},
        {"a move outside", CommandType::Move, 110.0f, false, false, false, false},
        {"a patrol outside", CommandType::Patrol, 110.0f, false, false, false, false},
        {"an attack-move outside", CommandType::AggressiveMove, 110.0f, false, false, false, false},
        {"a ground attack outside", CommandType::Attack, 110.0f, false, false, false, false},
        {"a guard of a unit outside", CommandType::Guard, 110.0f, true, false, false, false},
        {"a guard of a unit inside", CommandType::Guard, 90.0f, true, false, false, true},
        {"a move outside by a whole-map army", CommandType::Move, 110.0f, false, true, false, true},
        {"a stop, no target", CommandType::Stop, 110.0f, false, false, false, true},
        {"a move in the zone", CommandType::Move, 40.0f, false, false, true, true},
        {"a move out of the zone", CommandType::Move, 60.0f, false, false, true, false},
        {"a guard of a unit out of the zone", CommandType::Guard, 60.0f, true, false, true, false},
        {"a stop in No Rush", CommandType::Stop, 60.0f, false, false, true, true},
    };
    for (const Case& c : cases) {
        IssueWorld w;
        w.sim.set_playable_rect(0.0f, 0.0f, 100.0f, 100.0f);
        w.sim.get_army(0)->set_use_whole_map(c.whole_map);
        w.sim.get_army(0)->set_start_position({0.0f, 0.0f, 0.0f});
        if (c.no_rush) {
            w.sim.set_no_rush(300.0f, 50.0f);
        }
        Unit& u = w.make(0, {}, "RULEUMT_Land");
        UnitCommand order;
        order.type = c.type;
        order.target_pos = {c.x, 0.0f, 10.0f};
        if (c.at_unit) {
            Unit& target = w.make(0, {}, "RULEUMT_Land", {c.x, 0.0f, 10.0f});
            order.target_id = target.entity_id();
            order.target_pos = {};
        }
        INFO(c.what);
        CHECK(w.sim.takes_command(u, order) == c.taken);
    }
}

TEST_CASE("A cleared order that repeats its units' only order is dropped", "[routing]") {
    struct Case {
        const char* what;
        CommandType first;
        CommandType again;
        osc::f32 x;
        bool clear;
        bool both;
        bool queued_more;
        bool dropped;
    };
    const Case cases[] = {
        {"a move to the same point", CommandType::Move, CommandType::Move, 50.0f, true, false,
         false, true},
        {"queued, not cleared", CommandType::Move, CommandType::Move, 50.0f, false, false, false,
         false},
        {"to another point", CommandType::Move, CommandType::Move, 60.0f, true, false, false,
         false},
        {"to more units than hold it", CommandType::Move, CommandType::Move, 50.0f, true, true,
         false, false},
        {"behind another order", CommandType::Move, CommandType::Move, 50.0f, true, false, true,
         false},
        {"of another type", CommandType::Move, CommandType::Patrol, 50.0f, true, false, false,
         false},
        {"a patrol to the same point", CommandType::Patrol, CommandType::Patrol, 50.0f, true, false,
         false, true},
        {"an attack-move to the same point", CommandType::AggressiveMove,
         CommandType::AggressiveMove, 50.0f, true, false, false, true},
        {"a guard of the same unit", CommandType::Guard, CommandType::Guard, 50.0f, true, false,
         false, false},
        {"an attack on the same unit", CommandType::Attack, CommandType::Attack, 50.0f, true, false,
         false, true},
    };
    for (const Case& c : cases) {
        IssueWorld w;
        Unit& a = w.make(0, {}, "RULEUMT_Land");
        Unit& b = w.make(0, {}, "RULEUMT_Land");
        Unit& enemy = w.make(1, {}, "RULEUMT_Land", {50.0f, 0.0f, 50.0f});
        Unit& ward = w.make(0, {}, "RULEUMT_Land", {50.0f, 0.0f, 50.0f});
        const auto order = [&](CommandType type, osc::f32 x) {
            UnitCommand o;
            o.type = type;
            o.target_pos = {x, 0.0f, 50.0f};
            o.target_id = type == CommandType::Attack  ? enemy.entity_id()
                          : type == CommandType::Guard ? ward.entity_id()
                                                       : osc::sim::EntityId{};
            return o;
        };
        REQUIRE(w.sim.route_command({a.entity_id()}, order(c.first, 50.0f), true) != 0);
        if (c.queued_more) {
            w.sim.route_command({a.entity_id()}, order(CommandType::Move, 80.0f), false);
        }
        const osc::u32 before = a.command_queue().front().command_id;
        const size_t size = a.command_queue().size();
        std::vector<osc::sim::EntityId> to = {a.entity_id()};
        if (c.both) {
            to.push_back(b.entity_id());
        }
        w.sim.route_command(to, order(c.again, c.x), c.clear);
        INFO(c.what);
        CHECK((a.command_queue().front().command_id == before &&
               a.command_queue().size() == size) == c.dropped);
    }
}

TEST_CASE("Only a surfacing sub takes a Dive", "[routing]") {
    for (const auto& [motion, taken] :
         {std::pair{"RULEUMT_SurfacingSub", true}, std::pair{"RULEUMT_Water", false},
          std::pair{"RULEUMT_Land", false}, std::pair{"RULEUMT_Air", false}}) {
        IssueWorld w;
        Unit& u = w.make(0, {"SUBMERSIBLE"}, motion);
        u.add_command_cap("RULEUCC_Dive");
        INFO(motion);
        CHECK(w.takes(u, CommandType::Dive) == taken);
    }
}

TEST_CASE("A capture, repair or sacrifice goes only to a target Moho's issue lets it have",
          "[routing]") {
    struct Case {
        const char* what;
        CommandType type;
        const char* target;
        bool taken;
    };
    const Case cases[] = {
        {"capture an enemy", CommandType::Capture, "enemy", true},
        {"capture an ally", CommandType::Capture, "ally", false},
        {"capture its own", CommandType::Capture, "own", false},
        {"capture an uncapturable enemy", CommandType::Capture, "uncapturable", false},
        {"capture an unfinished enemy", CommandType::Capture, "unfinished", false},
        {"capture nothing named", CommandType::Capture, nullptr, true},
        {"repair its own", CommandType::Repair, "own", true},
        {"repair an ally", CommandType::Repair, "ally", true},
        {"repair an unfinished one of its own", CommandType::Repair, "own unfinished", true},
        {"repair an enemy", CommandType::Repair, "enemy", false},
        {"repair itself", CommandType::Repair, "self", false},
        {"repair nothing named", CommandType::Repair, nullptr, false},
        {"sacrifice to its own", CommandType::Sacrifice, "own", true},
        {"sacrifice to an ally", CommandType::Sacrifice, "ally", true},
        {"sacrifice to an enemy", CommandType::Sacrifice, "enemy", false},
        {"sacrifice to itself", CommandType::Sacrifice, "self", false},
        {"sacrifice without the order", CommandType::Sacrifice, "no cap", false},
    };
    for (const Case& c : cases) {
        IssueWorld w;
        w.sim.add_army("ARMY_3", "ARMY_3");
        w.sim.get_army(0)->set_alliance(2, osc::sim::Alliance::Ally);
        w.sim.get_army(2)->set_alliance(0, osc::sim::Alliance::Ally);
        Unit& u = w.make(0, {"CAPTURE", "REPAIR"}, "RULEUMT_Land");
        const std::string target = c.target ? c.target : "";
        if (target != "no cap") {
            u.add_command_cap("RULEUCC_Sacrifice");
        }
        osc::sim::EntityId id;
        if (target == "self" || target == "no cap") {
            id = target == "self" ? u.entity_id() : w.make(0, {}, "RULEUMT_Land").entity_id();
        } else if (!target.empty()) {
            const int army = target == "ally" ? 2 : target.rfind("own", 0) == 0 ? 0 : 1;
            Unit& t = w.make(army, {}, "RULEUMT_Land");
            t.set_capturable(target != "uncapturable");
            if (target == "unfinished" || target == "own unfinished") {
                t.set_is_being_built(true);
            }
            id = t.entity_id();
        }
        INFO(c.what);
        CHECK(w.takes(u, c.type, id) == c.taken);
    }
}
