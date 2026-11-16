// opensupcom --check-mods (src/app/mod_check.cpp): each check against a
// synthetic game, a temporary folder mounted as the VFS, with a mod per
// defect.

#include <catch2/catch_test_macros.hpp>

#include "app/mod_check.hpp"
#include "support/temp_path.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace osc::app::mods;

namespace {

namespace fs = std::filesystem;

void write(const fs::path& root, const std::string& rel, const std::string& text) {
    const fs::path file = root / rel;
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

/// The game: a few scripts and textures, and mods under /mods, each with
/// one kind of defect (or none). Written before the VFS first looks, as a
/// directory mount indexes its files once.
void write_game(const fs::path& root) {
    write(root, "lua/siminit.lua", "local x = 1\n");
    write(root, "lua/userinit.lua", "local u = 1\n");
    write(root, "lua/ends.lua", "local M = {}\nreturn M\n");
    write(root, "textures/a.dds", "dds");

    // Clean: a hook, a unit whose mesh and textures are there (one by a
    // relative path, one found by its name, as the engine finds an LOD's
    // left out), and a merge whose relative names are its target's
    write(root, "mods/good/mod_info.lua",
          "name = 'Good'\nuid = 'good-uid'\nversion = 3\nauthor = 'me'\n");
    write(root, "mods/good/hook/lua/siminit.lua", "x = x + 1\n");
    write(root, "mods/good/units/gub0001/gub0001_unit.bp",
          "UnitBlueprint {\n"
          "    Audio = { Fire = Sound { Bank = 'UEL', Cue = 'Fire' } },\n"
          "    Display = { Mesh = { LODs = {\n"
          "        { ShaderName = 'Aeon', MeshName = 'gub0001_lod0.scm',\n"
          "          AlbedoName = 'gub0001_albedo.dds', NormalsName = '../shared/n.dds' },\n"
          "        { ShaderName = 'TMeshAlpha' },\n"
          "    } } },\n"
          "}\n");
    write(root, "mods/good/units/gub0001/gub0001_lod0.scm", "scm");
    write(root, "mods/good/units/gub0001/gub0001_lod1.scm", "scm");
    write(root, "mods/good/units/gub0001/gub0001_albedo.dds", "dds");
    write(root, "mods/good/units/shared/n.dds", "dds");
    write(root, "mods/good/units/uel0001/uel0001_unit.bp",
          "UnitBlueprint { Merge = true, BlueprintId = 'uel0001',\n"
          "    Display = { Mesh = { LODs = { { MeshName = 'theirs.scm' } } } } }\n");

    // mod_info.lua that doesn't compile, and one that fails as it runs
    write(root, "mods/badinfo/mod_info.lua", "name = 'Bad'\nuid = 'bad'\nversion is 2\n");
    write(root, "mods/runtime/mod_info.lua", "uid = 'rt'\nprint('hi')\n");

    // Fields: no uid, a fractional version, a hookdir without '/', a mod
    // required that isn't there and one conflicting that is
    write(root, "mods/fields/mod_info.lua",
          "name = 'Fields'\nversion = 1.5\nhookdir = 'hook'\n"
          "requires = { 'not-installed' }\nrequiresNames = { ['not-installed'] = 'Lib' }\n"
          "conflicts = { 'snd' }\n");

    // Two mods, one uid
    write(root, "mods/dup1/mod_info.lua", "name = 'One'\nuid = 'same'\n");
    write(root, "mods/dup2/mod_info.lua", "name = 'Two'\nuid = 'same'\n");

    // Hooks: a misspelt path; a script that ends in return; one that
    // doesn't compile with the hook (its error on the hook's line 2); a
    // hook that ends in return; a texture in the hook folder
    write(root, "mods/hooks/mod_info.lua", "name = 'Hooks'\nuid = 'hooks'\n");
    write(root, "mods/hooks/hook/lua/simint.lua", "x = 2\n");
    write(root, "mods/hooks/hook/lua/ends.lua", "M.x = 1\n");
    write(root, "mods/hooks/hook/lua/userinit.lua", "local a = 1\nx = = 1\n");
    write(root, "mods/hooks/hook/lua/siminit.lua", "local y = 2\nreturn\n");
    write(root, "mods/hooks/hook/textures/a.dds", "dds");
    // ...one of another mod's file, not installed; a .bp hook that is no
    // blueprint alone, which Blueprints.lua loads as one all the same
    write(root, "mods/hooks/hook/mods/other/lua/x.lua", "x = 1\n");
    write(root, "units/uel0001/uel0001_unit.bp", "local Base = 1\nUnitBlueprint { }\n");
    write(root, "mods/hooks/hook/units/uel0001/uel0001_unit.bp", "Base = Base + 1\n");

    // Blueprints
    write(root, "mods/bps/mod_info.lua", "name = 'Bps'\nuid = 'bps'\n");
    write(root, "mods/bps/units/syn/syn_unit.bp", "UnitBlueprint {\n    Display = = 1,\n}\n");
    write(root, "mods/bps/units/run/run_unit.bp", "Nope()\n");
    write(root, "mods/bps/units/sh/sh_unit.bp",
          "UnitBlueprint { Display = { Mesh = { LODs = { {\n"
          "    ShaderName = 'NoSuchTechnique', MeshName = 'missing.scm',\n"
          "    AlbedoName = '/textures/nope.dds' } } } } }\n");
    write(root, "mods/bps/units/nomesh/nomesh_unit.bp",
          "UnitBlueprint { Display = { Mesh = { LODs = { { ShaderName = 'Unit' } } } } }\n");
    write(root, "mods/bps/units/twice/twice_unit.bp",
          "UnitBlueprint { Display = { MeshBlueprint = '/units/x/x_mesh',\n"
          "    Mesh = { LODs = { { MeshName = 'gone.scm' } } } } }\n");
    write(root, "mods/bps/effects/e.bp", "EmitterBlueprint { Texture = '/textures/none.dds' }\n");

    // Sound banks that don't parse
    write(root, "mods/snd/mod_info.lua", "name = 'Snd'\nuid = 'snd'\n");
    write(root, "mods/snd/sounds/bad.xsb", "not a sound bank");
    write(root, "mods/snd/sounds/bad.xwb", "not a wave bank");
}

struct Game {
    osc::test::TempDir dir{"osc_mod_check"};
    osc::vfs::VirtualFileSystem vfs;
    Game() {
        write_game(dir.path());
        vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(dir.path()));
    }
};

const ModReport& mod_at(const Report& report, const std::string& location) {
    const auto it = std::find_if(report.mods.begin(), report.mods.end(),
                                 [&](const ModReport& r) { return r.mod.location == location; });
    REQUIRE(it != report.mods.end());
    return *it;
}

/// The findings of a check, in order.
std::vector<Finding> of(const ModReport& r, const std::string& check) {
    std::vector<Finding> out;
    for (const Finding& f : r.findings)
        if (f.check == check) out.push_back(f);
    return out;
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

TEST_CASE("check-mods: a clean mod reports nothing", "[mods][check-mods]") {
    Game game;
    const Report report = check_mods(game.vfs, {"good-uid"});
    REQUIRE(report.mods.size() == 1);
    const ModReport& good = report.mods[0];
    CHECK(good.mod.readable);
    CHECK(good.mod.name == "Good");
    CHECK(good.mod.version == "3");
    for (const Finding& f : good.findings) UNSCOPED_INFO(f.check << ": " << f.message);
    CHECK(good.findings.empty());
    CHECK(report.errors() == 0);
    CHECK(report.warnings() == 0);
}

TEST_CASE("check-mods: a mod_info.lua that doesn't run, by its line", "[mods][check-mods]") {
    Game game;
    const Report report = check_mods(game.vfs, {"/mods/badinfo", "/mods/runtime"});
    REQUIRE(report.mods.size() == 2);

    const auto syntax = of(mod_at(report, "/mods/badinfo"), "mod_info");
    REQUIRE(syntax.size() == 1);
    CHECK(syntax[0].severity == Severity::Error);
    CHECK(syntax[0].file == "/mods/badinfo/mod_info.lua");
    CHECK(syntax[0].line == 3);
    CHECK(has(syntax[0].message, "expected"));

    // A mod_info.lua runs in a table of LoadModInfo's defaults, not the globals
    const auto runtime = of(mod_at(report, "/mods/runtime"), "mod_info");
    REQUIRE(runtime.size() == 1);
    CHECK(runtime[0].line == 2);
    CHECK(has(runtime[0].message, "print"));
    CHECK_FALSE(mod_at(report, "/mods/runtime").mod.readable);
    CHECK(report.errors() == 2);
}

TEST_CASE("check-mods: mod_info.lua's fields", "[mods][check-mods]") {
    Game game;
    const Report report = check_mods(game.vfs, {"/mods/fields"});
    REQUIRE(report.mods.size() == 1);
    const ModReport& r = report.mods[0];

    const auto uid = of(r, "uid"); // defaulted to the file's path
    REQUIRE(uid.size() == 1);
    CHECK(uid[0].severity == Severity::Warning);
    CHECK(r.mod.uid == "/mods/fields/mod_info.lua");

    const auto version = of(r, "version");
    REQUIRE(version.size() == 1);
    CHECK(version[0].severity == Severity::Warning);
    CHECK(has(version[0].message, "1.5"));

    // "hook" for "/hook": its hooks are looked for in /mods/fieldshook
    const auto hookdir = of(r, "hookdir");
    REQUIRE(hookdir.size() == 1);
    CHECK(hookdir[0].severity == Severity::Error);
    CHECK(has(hookdir[0].message, "/mods/fieldshook"));

    // No mods.lua in this game: retail's rule, by hand
    const auto requires_ = of(r, "requires");
    REQUIRE(requires_.size() == 1);
    CHECK(requires_[0].severity == Severity::Warning);
    CHECK(has(requires_[0].message, "not-installed (Lib)"));
    const auto conflicts = of(r, "conflicts");
    REQUIRE(conflicts.size() == 1);
    CHECK(has(conflicts[0].message, "snd (Snd)"));

    CHECK(report.errors() == 1);
    CHECK(report.warnings() == 4);
}

TEST_CASE("check-mods: two mods with one uid", "[mods][check-mods]") {
    Game game;
    const Report report = check_mods(game.vfs, {"same"});
    REQUIRE(report.mods.size() == 2);
    for (const char* location : {"/mods/dup1", "/mods/dup2"}) {
        const auto dup = of(mod_at(report, location), "duplicate_uid");
        REQUIRE(dup.size() == 1);
        CHECK(dup[0].severity == Severity::Error);
    }
    CHECK(has(of(mod_at(report, "/mods/dup1"), "duplicate_uid")[0].message,
              "/mods/dup2/mod_info.lua"));
}

TEST_CASE("check-mods: hooks", "[mods][check-mods]") {
    Game game;
    const Report report = check_mods(game.vfs, {"hooks"});
    REQUIRE(report.mods.size() == 1);
    const ModReport& r = report.mods[0];

    const auto target = of(r, "hook_target");
    REQUIRE(target.size() == 2);
    CHECK(target[0].severity == Severity::Error);
    CHECK(target[0].file == "/mods/hooks/hook/lua/simint.lua");
    CHECK(has(target[0].message, "/lua/simint.lua"));
    CHECK(has(target[0].message, "did you mean /lua/siminit.lua"));
    // Of another mod's file: there only when that mod is installed
    CHECK(target[1].severity == Severity::Warning);
    CHECK(target[1].file == "/mods/hooks/hook/mods/other/lua/x.lua");

    const auto after_return = of(r, "hook_after_return");
    REQUIRE(after_return.size() == 1);
    CHECK(after_return[0].file == "/mods/hooks/hook/lua/ends.lua");
    CHECK(has(after_return[0].message, "/lua/ends.lua ends in a return"));

    // The chunk's error, on the hook's own line
    const auto compile = of(r, "hook_compile");
    REQUIRE(compile.size() == 1);
    CHECK(compile[0].file == "/mods/hooks/hook/lua/userinit.lua");
    CHECK(compile[0].line == 2);

    const auto ends = of(r, "hook_ends_in_return");
    REQUIRE(ends.size() == 1);
    CHECK(ends[0].severity == Severity::Warning);
    CHECK(ends[0].file == "/mods/hooks/hook/lua/siminit.lua");

    const auto not_script = of(r, "hook_not_script");
    REQUIRE(not_script.size() == 1);
    CHECK(not_script[0].severity == Severity::Warning);
    CHECK(not_script[0].file == "/mods/hooks/hook/textures/a.dds");

    // A .bp hook compiles with its blueprint, but runs alone as one too
    const auto alone = of(r, "blueprint_run");
    REQUIRE(alone.size() == 1);
    CHECK(alone[0].file == "/mods/hooks/hook/units/uel0001/uel0001_unit.bp");
    CHECK(has(alone[0].message, "hook folder"));

    CHECK(report.errors() == 4);
    CHECK(report.warnings() == 3);
}

TEST_CASE("check-mods: a mod's code runs with no file access", "[mods][check-mods]") {
    Game game;
    const std::filesystem::path out = game.dir.path() / "written.txt";
    write(game.dir.path(), "mods/sneaky/mod_info.lua", "name = 'Sneaky'\nuid = 'sneaky'\n");
    write(game.dir.path(), "mods/sneaky/units/w/w_unit.bp",
          "local f = io.open('" + out.generic_string() +
              "', 'w')\nf:write('x')\nf:close()\nUnitBlueprint {}\n");
    write(game.dir.path(), "mods/sneaky/units/d/d_unit.bp",
          "dofile('" + out.generic_string() + "')\nUnitBlueprint {}\n");
    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(game.dir.path()));
    const Report report = check_mods(vfs, {"sneaky"});
    CHECK(of(mod_at(report, "/mods/sneaky"), "blueprint_run").size() == 2);
    CHECK_FALSE(std::filesystem::exists(out));
}

TEST_CASE("check-mods: blueprints", "[mods][check-mods]") {
    Game game;
    const Report report = check_mods(game.vfs, {"bps"});
    REQUIRE(report.mods.size() == 1);
    const ModReport& r = report.mods[0];

    const auto syntax = of(r, "blueprint_compile");
    REQUIRE(syntax.size() == 1);
    CHECK(syntax[0].file == "/mods/bps/units/syn/syn_unit.bp");
    CHECK(syntax[0].line == 2);

    const auto runtime = of(r, "blueprint_run");
    REQUIRE(runtime.size() == 1);
    CHECK(runtime[0].file == "/mods/bps/units/run/run_unit.bp");
    CHECK(runtime[0].line == 1);
    CHECK(has(runtime[0].message, "Nope"));

    const auto shader = of(r, "shader");
    REQUIRE(shader.size() == 1);
    CHECK(shader[0].severity == Severity::Warning);
    CHECK(has(shader[0].message, "NoSuchTechnique"));

    // Resolved against the blueprint's folder, as RES_CompletePath does; one
    // left out, named after the blueprint
    const auto meshes = of(r, "mesh_file");
    REQUIRE(meshes.size() == 2);
    CHECK(has(meshes[0].message, "/mods/bps/units/nomesh/nomesh_lod0.scm"));
    CHECK(has(meshes[1].message, "/mods/bps/units/sh/missing.scm"));

    const auto textures = of(r, "texture_file");
    REQUIRE(textures.size() == 2);
    CHECK(textures[0].file == "/mods/bps/effects/e.bp");
    CHECK(has(textures[0].message, "/textures/none.dds"));
    CHECK(has(textures[1].message, "/textures/nope.dds"));

    // An inline mesh beside a reference is never used, so not checked
    const auto twice = of(r, "mesh_twice");
    REQUIRE(twice.size() == 1);
    CHECK(twice[0].file == "/mods/bps/units/twice/twice_unit.bp");

    CHECK(report.errors() == 6);
    CHECK(report.warnings() == 2);
}

TEST_CASE("check-mods: sound banks that don't parse", "[mods][check-mods]") {
    Game game;
    const Report report = check_mods(game.vfs, {"snd"});
    REQUIRE(report.mods.size() == 1);
    CHECK(of(report.mods[0], "sound_bank").size() == 1);
    CHECK(of(report.mods[0], "wave_bank").size() == 1);
    CHECK(report.errors() == 2);
    // Another mod's conflicts name it: a conflict of its own, as
    // GetDependencies counts them
    const auto conflicts = of(report.mods[0], "conflicts");
    REQUIRE(conflicts.size() == 1);
    CHECK(has(conflicts[0].message, "/mods/fields/mod_info.lua (Fields)"));
}

TEST_CASE("check-mods: the data's GetDependencies decides", "[mods][check-mods]") {
    osc::test::TempDir dir("osc_mod_check_deps");
    write(dir.path(), "lua/mods.lua",
          "function GetDependencies(uid)\n"
          "    if uid == 'needs' then return { missing = { ['from-data'] = true } } end\n"
          "    return {}\n"
          "end\n");
    write(dir.path(), "mods/needs/mod_info.lua",
          "name = 'Needs'\nuid = 'needs'\nrequires = { 'listed' }\n");
    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(dir.path()));

    const Report report = check_mods(vfs, {});
    REQUIRE(report.mods.size() == 1);
    const auto requires_ = of(report.mods[0], "requires");
    REQUIRE(requires_.size() == 1);
    CHECK(has(requires_[0].message, "from-data"));
    CHECK_FALSE(has(requires_[0].message, "listed"));
}

TEST_CASE("check-mods: which mods a name names", "[mods][check-mods]") {
    Game game;
    // A player's mod, mounted at /mods/<name> as retail's init file mounts them
    osc::test::TempDir player("osc_mod_check_player");
    write(player.path(), "mod_info.lua", "name = 'Mine'\nuid = 'mine'\n");
    game.vfs.mount("/mods/mine", std::make_unique<osc::vfs::DirectoryMount>(player.path()));

    CHECK(check_mods(game.vfs, {"mine"}).mods.size() == 1);
    CHECK(check_mods(game.vfs, {"/mods/mine"}).mods.size() == 1);
    CHECK(check_mods(game.vfs, {"/mods/mine/mod_info.lua"}).mods.size() == 1);
    CHECK(check_mods(game.vfs, {"GOOD"}).mods.size() == 1); // a name, in any case
    const Report all = check_mods(game.vfs, {});
    CHECK(all.mods.size() == 10);
    CHECK(check_mods(game.vfs, {"/mods"}).mods.size() == all.mods.size());
    CHECK(read_mods(game.vfs).size() == all.mods.size());

    const Report none = check_mods(game.vfs, {"no-such-mod"});
    CHECK(none.mods.empty());
    REQUIRE(none.unmatched.size() == 1);
    CHECK(none.errors() == 1);
}

TEST_CASE("check-mods: the report as text and as JSON", "[mods][check-mods]") {
    Game game;
    const Report report = check_mods(game.vfs, {"/mods/badinfo", "good-uid", "/mods/fields"});

    const std::string text = report_text(report);
    CHECK(has(text, "/mods/badinfo/mod_info.lua:3: error: doesn't run"));
    CHECK(has(text, "Good (good-uid) at /mods/good: clean"));
    CHECK(has(text, "/mods/fields/mod_info.lua: warning: version 1.5"));
    CHECK(has(text, "Checked 3 mods: 2 errors, 4 warnings."));

    const auto json = nlohmann::json::parse(report_json(report));
    CHECK(json["errors"] == 2);
    CHECK(json["warnings"] == 4);
    REQUIRE(json["mods"].size() == 3);
    const auto& bad = json["mods"][0];
    CHECK(bad["location"] == "/mods/badinfo");
    CHECK(bad["readable"] == false);
    REQUIRE(bad["findings"].size() == 1);
    CHECK(bad["findings"][0]["check"] == "mod_info");
    CHECK(bad["findings"][0]["line"] == 3);
    CHECK(bad["findings"][0]["severity"] == "error");

    const std::vector<ModReport> mods = read_mods(game.vfs);
    const std::string list = list_text(mods);
    CHECK(has(list, "good-uid"));
    CHECK(has(list, "/mods/badinfo: its mod_info.lua doesn't run"));
    CHECK(nlohmann::json::parse(list_json(mods)).size() == mods.size());
}
