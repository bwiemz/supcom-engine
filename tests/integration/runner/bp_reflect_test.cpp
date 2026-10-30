// --bp-reflect-test: blueprints as Moho writes them back
// (docs/plans/2026-10-23-moho-blueprint-reflection-design.md).

#include "bp_reflect_test.hpp"

#include "integration_tests.hpp"
#include "lua/lua_state.hpp"
#include "render_probe.hpp"
#include "vfs/virtual_file_system.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

extern "C" {
#include <lua.h>
}

#include <charconv>
#include <string>
#include <system_error>
#include <utility>

namespace osc::test {

namespace {

/// Runs `body` as a function in `ctx`'s state; what it returns, as a string
/// (a script error's message, prefixed, when it fails).
std::string answer(TestContext& ctx, const std::string& body) {
    auto r =
        ctx.lua_state.do_string("rawset(_G, '__osc_bpr_answer', (function() " + body + " end)())");
    if (!r) {
        return "script error: " + r.error().message;
    }
    lua_State* L = ctx.L;
    lua_pushstring(L, "__osc_bpr_answer");
    lua_rawget(L, LUA_GLOBALSINDEX);
    std::string out = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    lua_pushstring(L, "__osc_bpr_answer");
    lua_pushnil(L);
    lua_rawset(L, LUA_GLOBALSINDEX);
    return out;
}

/// The original game's values (retail-probe fresh-lua, 2026-10-29): each
/// that differs, or 'ok'.
constexpr const char* kRetailValues = R"(
    local want = {
        {'uel0201', 'General', 'CommandCaps', '575'}, {'uel0201', 'General', 'ToggleCaps', '0'},
        {'uel0201', 'Physics', 'BuildOnLayerCaps', 'Land'},
        {'uel0201', 'Footprint', 'SizeX', 1}, {'uel0201', 'Footprint', 'SizeZ', 1},
        {'uel0201', 'Footprint', 'OccupancyCaps', 1},
        {'uel0203', 'Physics', 'BuildOnLayerCaps', '3'}, {'uel0203', 'Footprint', 'SizeZ', 1},
        {'uel0203', 'Footprint', 'OccupancyCaps', 9},
        {'ues0201', 'General', 'CommandCaps', '63'},
        {'ues0201', 'Physics', 'BuildOnLayerCaps', 'Water'},
        {'ues0201', 'Footprint', 'SizeX', 4}, {'ues0201', 'Footprint', 'SizeZ', 4},
        {'ues0201', 'Footprint', 'MinWaterDepth', 1.5},
        {'ues0201', 'Footprint', 'OccupancyCaps', 8},
        {'ues0203', 'General', 'CommandCaps', '524351'},
        {'ues0203', 'Physics', 'BuildOnLayerCaps', '12'}, {'ues0203', 'Footprint', 'SizeZ', 2},
        {'ues0401', 'Footprint', 'SizeX', 12}, {'ues0401', 'Footprint', 'SizeZ', 12},
        {'ueb1101', 'General', 'CommandCaps', '0'}, {'ueb1101', 'Footprint', 'OccupancyCaps', 1},
        {'uea0102', 'General', 'CommandCaps', '4194879'},
        {'uea0102', 'Physics', 'BuildOnLayerCaps', 'Air'},
    }
    local wrong = ''
    for _, w in want do
        local bp = __blueprints[w[1]]
        local got = bp and bp[w[2]] and bp[w[2]][w[3]]
        if got ~= w[4] then
            wrong = wrong .. w[1] .. '.' .. w[2] .. '.' .. w[3] .. ' ' .. type(got) .. ' '
                .. tostring(got) .. ' (' .. tostring(w[4]) .. ') '
        end
    end
    return wrong == '' and 'ok' or wrong
)";

/// The original game's projectile, prop and mesh blueprints and a value given
/// as another type (retail probe bp-typed, 2026-10-30): each that differs, or
/// 'ok'.
constexpr const char* kRetailOthers = R"(
    local shell = '/projectiles/tdfgauss01/tdfgauss01_proj.bp'
    local pine = '/env/evergreen/props/trees/pine06_v2_prop.bp'
    local tank = '/units/uel0201/uel0201_mesh'
    local trees = '/env/evergreen/props/trees/'
    local want = {
        {shell, 'Physics.Lifetime', 15}, {shell, 'Physics.UseGravity', true},
        {shell, 'Physics.BounceVelDamp', 0.5}, {shell, 'CollisionShape', 'None'},
        {shell, 'Economy.BuildTime', 10}, {shell, 'Footprint.SizeX', 1},
        {pine, 'CollisionShape', 'Box'}, {pine, 'LifeBarRender', false},
        {pine, 'SelectionYOffset', 0.5},
        {tank, 'LODs.1.MeshName', '/units/uel0201/uel0201_lod0.scm'},
        {tank, 'LODs.1.AlbedoName', '/units/uel0201/uel0201_albedo.dds'},
        {tank, 'LODs.1.SpecularName', '/units/uel0201/uel0201_SpecTeam.dds'},
        {tank, 'LODs.2.MeshName', '/units/uel0201/uel0201_lod1.scm'},
        {tank, 'LODs.2.AlbedoName', '/units/uel0201/uel0201_lod1_albedo.dds'},
        {tank, 'StraddleWater', false},
        {trees .. 'pine06_v2_mesh', 'LODs.1.AlbedoName', trees .. 'Pine06_V2_albedo.dds'},
        {trees .. 'pine06_v2_mesh', 'LODs.3.NormalsName', trees .. 'pine06_v2_normalsTS.dds'},
        {'uab2305', 'Weapon.1.ManualFire', true},
    }
    local wrong = ''
    for _, w in want do
        local got = __blueprints[w[1]]
        for key in string.gfind(w[2], '[^.]+') do
            if type(got) == 'table' then
                got = got[tonumber(key) or key]
            else
                got = nil
            end
        end
        if got ~= w[3] then
            wrong = wrong .. w[1] .. ' ' .. w[2] .. ' ' .. type(got) .. ' ' .. tostring(got) ..
                ' (' .. tostring(w[3]) .. ') '
        end
    end
    return wrong == '' and 'ok' or wrong
)";

/// FAF's unit view against each .bp's CommandCaps (FAF's copy,
/// CommandCapsHash). Moho writes a single cap as its name, which has no bits.
constexpr const char* kFafAbilities = R"(
    local detail = import('/lua/ui/game/unitviewdetail.lua')
    local caps = {
        ability_repairs = 'RULEUCC_Repair', ability_reclaim = 'RULEUCC_Reclaim',
        ability_capture = 'RULEUCC_Capture', ability_transport = 'RULEUCC_Transport',
        ability_submersible = 'RULEUCC_Dive', ability_transportable = 'RULEUCC_CallTransport',
        ability_sacrifice = 'RULEUCC_Sacrifice',
    }
    local category = {ability_transport = 'TRANSPORTATION'}
    local units, wrong, first, found = 0, 0, '', {}
    for id, bp in __blueprints do
        local hash = type(bp) == 'table' and bp.General and bp.General.CommandCapsHash
        if type(hash) == 'table' then
            units = units + 1
            local n = 0
            for _, on in hash do
                if on then n = n + 1 end
            end
            for ability, cap in caps do
                local cat = category[ability]
                local want = n > 1 and hash[cap] and (not cat or bp.CategoriesHash[cat]) and true
                    or false
                local ok, got = pcall(detail.IsAbilityExist[ability], bp)
                got = ok and got and true or false
                if got then found[ability] = (found[ability] or 0) + 1 end
                if got ~= want then
                    wrong = wrong + 1
                    if first == '' then first = id .. ' ' .. ability .. ' ' .. tostring(got) end
                end
            end
        end
    end
    local missing = 0
    local counts = ''
    for ability in caps do
        if not found[ability] then missing = missing + 1 end
        counts = counts .. ' ' .. ability .. '=' .. (found[ability] or 0)
    end
    return units .. ' ' .. wrong .. ' ' .. missing .. ' ' .. first .. counts
)";

/// FAF's formations' footprint sizes (CategorizeUnits).
constexpr const char* kFafFormation = R"(
    local CategorizeUnits = import('/lua/shared/formations/categorizeunits.lua').CategorizeUnits
    local x, z = GetArmyBrain('ARMY_1'):GetArmyStartPos()
    local units = {}
    for i, id in {'uel0201', 'uel0201', 'url0107', 'uel0401', 'ues0201', 'uas0201'} do
        local ux = x + 8 * i
        table.insert(units, CreateUnitHPR(id, 'ARMY_1', ux, GetTerrainHeight(ux, z + 20), z + 20,
                                          0, 0, 0))
    end
    local list = CategorizeUnits(units)
    local out = ''
    for _, kind in {'Land', 'Naval'} do
        local sizes = {}
        for fs in list[kind].FootprintCounts do
            table.insert(sizes, fs)
        end
        table.sort(sizes)
        out = out .. kind .. ':'
        for _, fs in sizes do
            out = out .. ' ' .. fs .. '=' .. list[kind].FootprintCounts[fs]
        end
        out = out .. ' '
    end
    for _, u in units do
        u:Destroy()
    end
    return out
)";

} // namespace

void test_bp_reflect(TestContext& ui, TestContext& sim, const std::function<void(int)>& play) {
    spdlog::info("=== Reflected fields: flag enums and footprints as Moho writes them back ===");
    Tally t;
    play(2);
    if (!ui.vfs.file_exists("/lua/shared/formations/categorizeunits.lua")) {
        for (auto [state, ctx] : {std::pair{"sim", &sim}, std::pair{"UI", &ui}}) {
            const std::string got = answer(*ctx, kRetailValues);
            t.check(got == "ok", fmt::format("Test 1: the {} state reads the original game's flag "
                                             "enums and footprints: {}",
                                             state, got));
            const std::string others = answer(*ctx, kRetailOthers);
            t.check(others == "ok",
                    fmt::format("Test 2: the {} state reads the original game's projectile, prop "
                                "and mesh blueprints and a boolean given as 1: {}",
                                state, others));
        }
        return;
    }
    const std::string abilities = answer(ui, kFafAbilities);
    unsigned counts[3] = {};
    const char* at = abilities.data();
    const char* const end = at + abilities.size();
    bool read = true;
    for (unsigned& n : counts) {
        const auto [next, ec] = std::from_chars(at, end, n);
        read = read && ec == std::errc{};
        at = next < end ? next + 1 : end;
    }
    const auto [units, wrong, missing] = counts;
    t.check(read && units >= 100 && wrong == 0 && missing == 0,
            fmt::format("Test 3: FAF's unit view finds each unit's abilities by its CommandCaps "
                        "(units, wrong, abilities never found, first wrong, counts): {}",
                        abilities));
    const std::string formation = answer(sim, kFafFormation);
    t.check(formation == "Land: 1=3 6=1 Naval: 4=2 ",
            fmt::format("Test 4: FAF's formations size units by their footprint classes: '{}'",
                        formation));
    play(1);
}

} // namespace osc::test
