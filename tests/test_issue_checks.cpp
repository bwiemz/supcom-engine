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
               const char* motion = "RULEUMT_Air") {
        auto u = std::make_unique<Unit>();
        u->set_army(army);
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
