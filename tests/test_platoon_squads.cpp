#include <catch2/catch_test_macros.hpp>

#include "sim/platoon.hpp"

using osc::sim::Platoon;

TEST_CASE("Platoon squads: Moho's classes read in any case", "[sim][platoon]") {
    CHECK(Platoon::squad_class("Unassigned") == 0);
    CHECK(Platoon::squad_class("") == 0);
    CHECK(Platoon::squad_class("Attack") == 1);
    CHECK(Platoon::squad_class("artillery") == 2);
    CHECK(Platoon::squad_class("GUARD") == 3);
    CHECK(Platoon::squad_class("Support") == 4);
    CHECK(Platoon::squad_class("scout") == 5);
    CHECK(Platoon::squad_class("Scouts") == -1);
    CHECK(Platoon::squad_class("TransportPool") == -1);
}

TEST_CASE("Platoon squads: a unit is in its squad by class, else by name", "[sim][platoon]") {
    Platoon p;
    p.add_unit(1);
    p.set_unit_squad(1, "Scout");
    p.add_unit(2);
    p.set_unit_squad(2, "MySquad");
    p.add_unit(3); // no squad: Unassigned

    CHECK(p.in_squad(1, "scout"));
    CHECK(p.in_squad(1, "SCOUT"));
    CHECK_FALSE(p.in_squad(1, "Attack"));
    CHECK(p.in_squad(2, "MySquad"));
    CHECK_FALSE(p.in_squad(2, "mysquad"));
    CHECK_FALSE(p.in_squad(2, "Unassigned"));
    CHECK(p.in_squad(3, "unassigned"));
    CHECK_FALSE(p.in_squad(3, "Scout"));
}
