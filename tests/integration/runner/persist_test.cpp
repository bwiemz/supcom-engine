// --persist-test (M208c-a): the sim's whole Lua heap -- blueprints, the
// class system, an AI's threads suspended mid-function, frozen tables --
// saves, loads into a fresh Lua state, and saves again. The loaded heap
// reads the same and iterates its tables in the same order, and its save
// loads too. Once at load, and again 300 ticks in: the blueprints frozen,
// the AI's threads under way, collections behind it and a sweep maybe under
// way.
//
// Not the same bytes: a table with a key that hashes by address (the
// registry's light userdata, a set of units) is built again on a load and
// iterates in an order of its own, which numbers the objects after it
// differently. Its order differs between two runs of a game anyway (see
// lpersist.cpp). The same size says the same objects came back.
//
// The loaded state's C functions and light userdata are the sim's own (the
// hooks keep each address as it is), so it is only read, never run.

#include "integration_tests.hpp"
#include "render_probe.hpp"

#include "sim/sim_state.hpp"

#include <lpersist.h>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>


namespace osc::test {

namespace {

/// Hooks that name each light userdata by when the save first met it, and
/// find it again as the same address.
struct Addresses {
    std::vector<void*> list;
    std::unordered_map<void*, std::uint64_t> index;

    lua_PersistHooks hooks() {
        lua_PersistHooks h;
        h.ud = this;
        h.name = [](void* ud, void* p, std::uint32_t* kind, std::uint64_t* id) {
            auto* a = static_cast<Addresses*>(ud);
            auto [it, added] = a->index.emplace(p, a->list.size());
            if (added) a->list.push_back(p);
            *kind = 0;
            *id = it->second;
            return true;
        };
        h.find = [](void* ud, std::uint32_t kind, std::uint64_t id, void** p) {
            auto* a = static_cast<Addresses*>(ud);
            if (kind != 0 || id >= a->list.size()) return false;
            *p = a->list[static_cast<std::size_t>(id)];
            return true;
        };
        return h;
    }
};

/// What a state reads, in the order it iterates: the globals' names and
/// kinds, the blueprints three levels down, the first army's brain. (Keys
/// that hash by address iterate in a loaded heap's own order, so they are
/// left out.) It writes nothing: a new global could make `_G` grow, and
/// table.insert records its table in lauxlib's (weak) sizes table, which a
/// save writes until the next collection.
const char* kDigest = R"(
    local out, n = {}, 0
    local function add(s) n = n + 1 out[n] = s end
    local function keys(t, depth)
        for k, v in t do
            local kt = type(k)
            if kt == 'string' or kt == 'number' or kt == 'boolean' then
                add(tostring(k))
                local vt = type(v)
                if vt == 'string' or vt == 'number' or vt == 'boolean' then add(tostring(v))
                elseif vt == 'table' and depth > 0 then keys(v, depth - 1)
                else add(vt) end
            end
        end
    end
    keys(_G, 0)
    keys(__blueprints, 3)
    keys(ArmyBrains[1], 1)
    return table.concat(out, ';')
)";

std::string digest(lua_State* L) {
    if (luaL_loadbuffer(L, kDigest, std::char_traits<char>::length(kDigest), "digest") != 0 ||
        lua_pcall(L, 0, 1, 0) != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
        lua_settop(L, 0);
        return "error: " + err;
    }
    std::string d = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_settop(L, 0);
    return d;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

void test_persist(TestContext& ctx) {
    spdlog::info("=== PERSIST TEST: the sim's Lua heap saves and loads (M208c-a) ===");
    Tally t;

    const auto round_trip = [&](const std::string& when) {
        const std::string want = digest(ctx.L);
        Addresses addresses;
        std::string saved;
        auto t0 = std::chrono::steady_clock::now();
        std::string err = lua_persist(ctx.L, saved, addresses.hooks());
        const double save_ms = ms_since(t0);
        t.check(err.empty(), when + ": the heap saves" + (err.empty() ? "" : " (" + err + ")"));
        if (!err.empty()) return;

        // A load, then a load of its save
        std::string from = saved;
        double load_ms = 0;
        for (int generation = 1; generation <= 2; ++generation) {
            const std::string what = when + ", load " + std::to_string(generation);
            lua_State* loaded = lua_open();
            t0 = std::chrono::steady_clock::now();
            err = lua_unpersist(loaded, from, addresses.hooks());
            if (generation == 1) load_ms = ms_since(t0);
            t.check(err.empty(),
                    what + ": it loads into a fresh state" + (err.empty() ? "" : " (" + err + ")"));
            if (err.empty()) {
                const std::string got = digest(loaded);
                t.check(!want.empty() && got == want,
                        what + ": it reads and iterates as the sim's (" +
                            std::to_string(want.size()) + " bytes of digest" +
                            (got.rfind("error", 0) == 0 ? ", " + got : "") + ")");
                std::string again;
                err = lua_persist(loaded, again, addresses.hooks());
                t.check(err.empty() && again.size() == saved.size(),
                        what + ": it saves, to as many bytes (" + std::to_string(saved.size()) +
                            " and " + std::to_string(again.size()) +
                            (err.empty() ? "" : ", " + err) + ")");
                from = std::move(again);
            }
            lua_close(loaded);
            if (!err.empty()) break;
        }
        spdlog::info("Persist test, {}: {:.1f} MB, saved in {:.0f} ms, loaded in {:.0f} ms, "
                     "{} light userdata",
                     when, static_cast<double>(saved.size()) / (1024.0 * 1024.0), save_ms, load_ms,
                     addresses.list.size());
    };

    round_trip("at load");
    for (int i = 0; i < 300; ++i) ctx.sim.tick();
    round_trip("300 ticks in");

    spdlog::info("Persist test: {}/{} passed", t.pass, t.pass + t.fail);
}

} // namespace osc::test
