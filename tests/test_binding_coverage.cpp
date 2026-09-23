#include <catch2/catch_test_macros.hpp>

#include "lua/binding_coverage.hpp"

#include <set>
#include <string>

using namespace osc::lua::coverage;

namespace {

LuaReferences scan(const char* src) {
    LuaReferences refs;
    scan_lua_source(src, "/lua/test.lua", refs);
    return refs;
}

bool has_method(const LuaReferences& r, const std::string& n) {
    return r.method_calls.count(n) != 0;
}
bool has_global(const LuaReferences& r, const std::string& n) {
    return r.global_calls.count(n) != 0;
}

} // namespace

TEST_CASE("scanner finds method and global calls", "[coverage]") {
    auto r = scan(R"lua(
        local u = CreateUnitHPR('uel0001', 1, 0, 0, 0, 0, 0, 0)
        u:SetHealth(nil, 10)
        self.Brain:GetArmyIndex()
        WaitSeconds(1)
    )lua");
    CHECK(has_global(r, "CreateUnitHPR"));
    CHECK(has_global(r, "WaitSeconds"));
    CHECK(has_method(r, "SetHealth"));
    CHECK(has_method(r, "GetArmyIndex"));
    CHECK(r.method_calls.at("SetHealth") == "/lua/test.lua:3");
}

TEST_CASE("scanner ignores strings and all comment forms", "[coverage]") {
    auto r = scan(R"lua(
        local s = "obj:NotAMethod() Global()"
        local t = 'x:AlsoNot()'
        local l = [[ y:LongString() Z() ]]
        -- c:LineComment() G1()
        # c:HashComment() G2()   (LuaPlus)
        --[[ c:BlockComment()
             G3() ]]
        real:Call()
    )lua");
    CHECK(has_method(r, "Call"));
    for (const char* n : {"NotAMethod", "AlsoNot", "LongString", "LineComment",
                          "HashComment", "BlockComment"}) {
        INFO(n);
        CHECK_FALSE(has_method(r, n));
    }
    for (const char* n : {"Global", "Z", "G1", "G2", "G3"}) {
        INFO(n);
        CHECK_FALSE(has_global(r, n));
    }
}

TEST_CASE("scanner records Lua definitions so they are not engine gaps", "[coverage]") {
    auto r = scan(R"lua(
        function TopLevel() end
        function Mod.Nested(a) end
        function Class:Method() end
        Unit = Class(moho.unit_methods) {
            OnCreate = function(self) end,
        }
        local function Helper() end
        local Alias = import('/lua/x.lua').Thing
        Assigned = function() end
    )lua");
    for (const char* n : {"TopLevel", "Nested", "Method", "OnCreate", "Helper",
                          "Alias", "Assigned"}) {
        INFO(n);
        CHECK(r.defined.count(n) == 1);
    }
    // A definition is not a call.
    CHECK_FALSE(has_global(r, "TopLevel"));
    CHECK_FALSE(has_global(r, "Helper"));
    // Class(...) and import(...) are calls.
    CHECK(has_global(r, "Class"));
    CHECK(has_global(r, "import"));
}

TEST_CASE("keywords and field calls are not global calls", "[coverage]") {
    auto r = scan(R"lua(
        if (x) then end
        while (y) do end
        return (z)
        table.insert(t, 1)
        obj.Field(1)
    )lua");
    for (const char* n : {"if", "while", "return", "insert", "Field"}) {
        INFO(n);
        CHECK_FALSE(has_global(r, n));
    }
}

TEST_CASE("coverage reports referenced names that are neither defined nor registered",
          "[coverage]") {
    auto r = scan(R"lua(
        Known()
        Missing()
        DefinedHere()
        function DefinedHere() end
        u:BoundMethod()
        u:UnboundMethod()
    )lua");
    const std::set<std::string> globals{"Known"};
    const std::set<std::string> methods{"BoundMethod"};
    auto report = compute_coverage(r, globals, methods);
    REQUIRE(report.missing_globals.size() == 1);
    CHECK(report.missing_globals[0].first == "Missing");
    REQUIRE(report.missing_methods.size() == 1);
    CHECK(report.missing_methods[0].first == "UnboundMethod");
}

TEST_CASE("baseline ratchet flags only new gaps", "[coverage]") {
    CoverageReport report;
    report.missing_globals = {{"OldGap", "a:1"}, {"NewGap", "b:2"}};
    report.missing_methods = {{"OldMethod", "c:3"}};
    const auto baseline = parse_baseline("# comment\nG OldGap\nM OldMethod\nG Closed\n");
    auto delta = diff_against_baseline(report, baseline);
    REQUIRE(delta.new_gaps.size() == 1);
    CHECK(delta.new_gaps[0] == "G NewGap");
    REQUIRE(delta.closed_gaps.size() == 1);
    CHECK(delta.closed_gaps[0] == "G Closed");
}

TEST_CASE("parameters and loop variables are definitions", "[coverage]") {
    auto r = scan(R"lua(
        function Run(callbackFunc, onDone)
            callbackFunc()
            onDone()
        end
        local f = function(self, cb) cb(self) end
        for i, fn in handlers do fn() end
        for n = 1, 10 do end
    )lua");
    for (const char* n : {"callbackFunc", "onDone", "self", "cb", "i", "fn", "n"}) {
        INFO(n);
        CHECK(r.defined.count(n) == 1);
    }
    auto report = compute_coverage(r, {}, {});
    CHECK(report.missing_globals.empty());
}
