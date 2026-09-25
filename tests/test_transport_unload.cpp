// A transport's unload order (M206g): all its cargo, or, from
// IssueTransportUnloadSpecific, the cargo chosen when the order was given
// (Moho's UNITCOMMAND_TransportUnloadSpecificUnits); the rest stays aboard.

#include <catch2/catch_test_macros.hpp>

#include "sim/command_codec.hpp"
#include "sim/command_scheduler.hpp"
#include "sim/manipulator.hpp"
#include "sim/sim_state.hpp"
#include "sim/unit.hpp"
#include "sim/unit_command.hpp"

extern "C" {
#include <lua.h>
#include <lualib.h>
}

#include <memory>
#include <vector>

using osc::u32;
using osc::sim::CommandType;
using osc::sim::SimState;
using osc::sim::Unit;
using osc::sim::UnitCommand;

namespace {

/// A transport carrying two units, over open ground (no terrain), in a sim
/// with a bare Lua state: no scripts, so no hooks.
struct Ferry {
    lua_State* L = lua_open();
    SimState sim{L, nullptr};
    u32 transport = 0;
    u32 first = 0;
    u32 second = 0;

    Ferry() {
        luaopen_base(L);
        transport = spawn();
        first = spawn();
        second = spawn();
        unit(transport).set_transport_capacity(4);
        unit(first).attach_to_transport(&unit(transport), sim.entity_registry(), L);
        unit(second).attach_to_transport(&unit(transport), sim.entity_registry(), L);
    }
    ~Ferry() { lua_close(L); }
    Ferry(const Ferry&) = delete;
    Ferry& operator=(const Ferry&) = delete;

    u32 spawn() {
        auto u = std::make_unique<Unit>();
        u->set_army(0);
        return sim.entity_registry().register_entity(std::move(u));
    }
    Unit& unit(u32 id) { return *static_cast<Unit*>(sim.entity_registry().find(id)); }

    /// Unload where the transport stands (within reach at once).
    void unload(std::vector<u32> ids) {
        UnitCommand cmd;
        cmd.type = CommandType::TransportUnload;
        cmd.target_pos = unit(transport).position();
        cmd.unload_ids = std::move(ids);
        unit(transport).push_command(cmd, false);
    }
};

} // namespace

TEST_CASE("A specific unload drops only its cargo", "[transport]") {
    Ferry f;
    REQUIRE(f.unit(f.transport).cargo_ids() == std::vector<u32>{f.first, f.second});
    f.unload({f.second});
    f.sim.tick();
    CHECK(f.unit(f.second).transport_id() == 0);
    CHECK(f.unit(f.first).transport_id() == f.transport);
    CHECK(f.unit(f.transport).cargo_ids() == std::vector<u32>{f.first});
    CHECK(f.unit(f.transport).command_queue().empty());
}

TEST_CASE("An unload without a cargo list drops all of it", "[transport]") {
    Ferry f;
    f.unload({});
    f.sim.tick();
    CHECK(f.unit(f.first).transport_id() == 0);
    CHECK(f.unit(f.second).transport_id() == 0);
    CHECK(f.unit(f.transport).cargo_ids().empty());
}

TEST_CASE("A specific unload whose cargo has gone ends at once", "[transport]") {
    Ferry f;
    const u32 elsewhere = f.spawn(); // never aboard
    f.unload({elsewhere});
    f.sim.tick();
    CHECK(f.unit(f.transport).command_queue().empty());
    CHECK(f.unit(f.transport).cargo_ids() == std::vector<u32>{f.first, f.second});
}

TEST_CASE("A specific unload's cargo survives the codec", "[transport][replay]") {
    osc::sim::ScheduledCommand c;
    c.command.type = CommandType::TransportUnload;
    c.command.unload_ids = {7, 9};
    c.unit_ids = {3};
    std::vector<osc::u8> bytes;
    osc::sim::ByteWriter w(bytes);
    osc::sim::write_command(w, c);

    osc::sim::ByteReader r(bytes);
    osc::sim::ScheduledCommand back;
    REQUIRE(osc::sim::read_command(r, back));
    CHECK(back.command.unload_ids == std::vector<u32>{7, 9});
    CHECK(back.unit_ids == std::vector<u32>{3});
}

TEST_CASE("The orders checksum sees which cargo an unload drops", "[transport][sync]") {
    Ferry a, b;
    a.unload({a.first});
    b.unload({b.second});
    CHECK(a.sim.checksum_parts().orders != b.sim.checksum_parts().orders);
    // Without a list, an order hashes as it did before the list existed.
    Ferry c, d;
    c.unload({});
    d.unload({});
    CHECK(c.sim.checksum_parts().orders == d.sim.checksum_parts().orders);
}
