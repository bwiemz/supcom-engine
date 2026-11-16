#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "audio/xact/bank_registry.hpp"
#include "audio/xact/byte_reader.hpp"
#include "audio/xact/global_settings.hpp"
#include "audio/xact/sound_bank.hpp"
#include "audio/xwb_parser.hpp"
#include "vfs/directory_mount.hpp"
#include "vfs/virtual_file_system.hpp"
#include "xact_fixtures.hpp"
#include "support/memory_mount.hpp"
#include "support/temp_path.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

using namespace osc;
using namespace osc::audio::xact;
using Catch::Matchers::WithinAbs;

using namespace osc::test::xact_fixtures;

TEST_CASE("XACT: reads are bounds-checked", "[audio][xact]") {
    const std::vector<u8> data = {1, 2, 3};
    ByteReader r(data);
    CHECK(r.read_u16() == 0x0201);
    CHECK(r.ok());
    CHECK(r.read_u16() == 0); // one byte left
    CHECK_FALSE(r.ok());
    ByteReader past(data, 9);
    CHECK_FALSE(past.ok());
}

TEST_CASE("XACT: volumes are millibels", "[audio][xact]") {
    CHECK_THAT(volume_byte_to_millibels(180), WithinAbs(0.0, 2.0)); // unity
    CHECK(volume_byte_to_millibels(0) == kSilenceMb);
    CHECK(volume_byte_to_millibels(255) > 500.0f); // about +6 dB
    CHECK_THAT(millibels_to_gain(0), WithinAbs(1.0, 1e-6));
    CHECK_THAT(millibels_to_gain(-600), WithinAbs(0.501, 0.001)); // -6 dB
    CHECK(millibels_to_gain(kSilenceMb) == 0.0f);
}

TEST_CASE("XACT: global settings", "[audio][xact]") {
    auto r = GlobalSettings::parse(make_xgs());
    REQUIRE(r.ok());
    const GlobalSettings& gs = r.value();
    REQUIRE(gs.categories.size() == 2);
    CHECK(gs.find_category("Music") == 1);
    CHECK(gs.categories[1].parent == 0);
    CHECK(gs.categories[1].instance_limit == 1);
    CHECK(gs.categories[1].fade_out_ms == 200);
    CHECK(gs.categories[1].limit_behavior == LimitBehavior::ReplaceOldest);
    CHECK(gs.categories[1].volume_mb < 0);
    CHECK(gs.find_variable("Distance") == 0);
    CHECK_FALSE(gs.variables[0].global());
    CHECK(gs.find_variable("TestCutoff") == 1);
    CHECK(gs.variables[1].global());
    CHECK(gs.variables[1].initial == 100.0f);
    REQUIRE(gs.rpcs.size() == 1);
    const RpcCurve* rpc = gs.rpc(gs.rpcs[0].code);
    REQUIRE(rpc);
    CHECK_THAT(rpc->evaluate(-5), WithinAbs(0.0, 1e-3));    // before the first point
    CHECK_THAT(rpc->evaluate(500), WithinAbs(-1000.0, 1e-3)); // linear between
    CHECK_THAT(rpc->evaluate(5000), WithinAbs(-2000.0, 1e-3));
    CHECK(gs.rpc(999999) == nullptr);
}

TEST_CASE("XACT: RPC curve point shapes", "[audio][xact]") {
    RpcCurve c;
    c.points = {{0, 0, RpcCurve::PointType::Fast}, {1, 100, RpcCurve::PointType::Linear}};
    const f32 fast = c.evaluate(0.5f);
    c.points[0].type = RpcCurve::PointType::Slow;
    const f32 slow = c.evaluate(0.5f);
    CHECK(fast > 50.0f); // rises early
    CHECK(slow < 50.0f); // rises late
    CHECK_THAT(c.evaluate(1.0f), WithinAbs(100.0, 1e-4));
}

TEST_CASE("XACT: an XACT 3.0 sound bank", "[audio][xact]") {
    auto r = SoundBank::parse(make_xsb("TestWaves"));
    REQUIRE(r.ok());
    const SoundBank& sb = r.value();
    CHECK(sb.name == "Test");
    REQUIRE(sb.wave_banks.size() == 1);
    CHECK(sb.wave_banks[0] == "TestWaves");

    const Cue* click = sb.find_cue("Click");
    REQUIRE(click);
    const Sound& simple = sb.sounds[click->sound];
    CHECK(simple.category == 0);
    REQUIRE(simple.tracks.size() == 1);
    REQUIRE(simple.tracks[0].plays.size() == 1);
    CHECK(simple.tracks[0].plays[0].waves[0].wave == 3);

    const Cue* shot = sb.find_cue("Shot");
    REQUIRE(shot);
    CHECK(shot->instance_limit == 2);
    CHECK(shot->fade_out_ms == 300);
    CHECK(shot->limit_behavior == LimitBehavior::ReplaceOldest);
    const Sound& sound = sb.sounds[shot->sound];
    CHECK(sound.pitch == 50);
    CHECK(sound.priority == 7);
    REQUIRE(sound.rpc_codes.size() == 1);
    CHECK(sound.rpc_codes[0] == 1234);
    REQUIRE(sound.tracks.size() == 1);
    REQUIRE(sound.tracks[0].plays.size() == 1);
    const PlayEvent& e = sound.tracks[0].plays[0];
    CHECK(e.time_ms == 150);
    CHECK(e.loop_count == PlayEvent::kLoopForever);
    CHECK(e.variation == VariationMode::RandomNoImmediateRepeat);
    REQUIRE(e.waves.size() == 3);
    CHECK(e.waves[2].wave == 2);
    CHECK(e.vary_pitch);
    CHECK_FALSE(e.vary_volume);
    CHECK(e.pitch_min == -200);
    CHECK(e.pitch_max == 200);
    CHECK(sb.find_cue("Nope") == nullptr);
}

TEST_CASE("XACT: a 3.0 effect variation's 0x40 varies pitch, 0x80 volume", "[audio][xact]") {
    // FAudio's variation_flags_from_3_0; retail's 0x40-only events keep the
    // tool's default volume range and its 0x80-only ones the default pitch.
    for (const auto& [flags, pitch, volume] :
         {std::tuple{u8{0x40}, true, false}, std::tuple{u8{0x80}, false, true},
          std::tuple{u8{0xC0}, true, true}}) {
        const auto parsed = SoundBank::parse(make_xsb("Waves", 1234, 0, 0, flags));
        REQUIRE(parsed);
        const SoundBank& sb = parsed.value();
        const Cue* shot = sb.find_cue("Shot");
        REQUIRE(shot);
        const PlayEvent& e = sb.sounds[shot->sound].tracks[0].plays[0];
        CHECK(e.vary_pitch == pitch);
        CHECK(e.vary_volume == volume);
    }
}

TEST_CASE("XACT: a track variation's 0x40 picks a new wave for each loop", "[audio][xact]") {
    for (const bool repick : {false, true}) {
        const auto parsed = SoundBank::parse(make_xsb("Waves", 1234, 0, 0, 0x40, repick));
        REQUIRE(parsed);
        const SoundBank& sb = parsed.value();
        const PlayEvent& e = sb.sounds[sb.find_cue("Shot")->sound].tracks[0].plays[0];
        CHECK(e.new_variation_on_loop == repick);
        CHECK(e.variation == VariationMode::RandomNoImmediateRepeat);
    }
}

TEST_CASE("XACT: a damaged sound bank fails to parse", "[audio][xact]") {
    auto good = make_xsb("TestWaves");
    for (size_t cut : {size_t{10}, size_t{100}, good.size() / 2, good.size() - 3}) {
        std::vector<u8> truncated(good.begin(), good.begin() + static_cast<long>(cut));
        INFO("truncated at " << cut);
        CHECK_FALSE(SoundBank::parse(truncated).ok());
    }
    // A wave from a wave bank the bank does not list
    auto bad_bank = good;
    const size_t simple_sound = 138 + 64; // header + one wave bank name
    bad_bank[simple_sound + 11] = 5;
    CHECK_FALSE(SoundBank::parse(bad_bank).ok());
    CHECK_FALSE(SoundBank::parse(std::vector<u8>{'S', 'D', 'B', 'K'}).ok());
}

TEST_CASE("XACT: wave banks resolve by their internal name", "[audio][xact]") {
    namespace fs = std::filesystem;
    const fs::path dir = osc::test::unique_temp_path("osc_xact_registry_test");
    fs::create_directories(dir);
    // The sound bank lists "TestWaves", a wave bank whose file is named
    // otherwise (as retail's XAS_Weapons.xwb is internally XAS_Weapon).
    write(dir / "Test.xsb", make_xsb("TestWaves"));
    write(dir / "Other_File_Name.xwb", make_xwb("TestWaves", 4));
    write(dir / "SupCom.xgs", make_xgs());

    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/sounds", std::make_unique<osc::vfs::DirectoryMount>(dir));
    BankRegistry reg(vfs, "/sounds");
    REQUIRE(reg.global_settings());
    CHECK(reg.global_settings()->categories.size() == 2);
    CHECK(reg.sound_bank_names() == std::vector<std::string>{"test"});
    const SoundBank* sb = reg.sound_bank("test"); // case-blind
    REQUIRE(sb);
    CHECK(reg.sound_bank("Missing") == nullptr);

    const Cue* click = sb->find_cue("Click");
    REQUIRE(click);
    const auto wave = reg.resolve(*sb, sb->sounds[click->sound].tracks[0].plays[0].waves[0]);
    REQUIRE(wave);
    CHECK(wave->bank->bank_name() == "TestWaves");
    CHECK(wave->index == 3);
    const auto data = wave->bank->read_wave_data(wave->index);
    REQUIRE(data.size() == 8);
    CHECK(data[0] == 24); // wave 3 starts at byte 24 of the data segment
    CHECK(wave->bank->entry(wave->index).loop_length == 4);

    WaveChoice missing;
    missing.wave = 9; // past the wave bank's 4 entries
    CHECK_FALSE(reg.resolve(*sb, missing));
    fs::remove_all(dir);
}

namespace {

std::vector<char> as_chars(const std::vector<u8>& bytes) {
    return {bytes.begin(), bytes.end()};
}

/// A wave bank built as a streaming one (XACT's WAVEBANK_TYPE_STREAMING:
/// bit 0 of the bank data's flags, which follow the 52-byte header).
std::vector<u8> make_streaming_xwb(const std::string& name) {
    auto bytes = make_xwb(name, 4);
    bytes[52] |= 1;
    return bytes;
}

} // namespace

TEST_CASE("XACT: /sounds holds every mount's banks, the first mount's on a clash",
          "[audio][xact]") {
    // FAF's init mounts a mod's sounds folder at /sounds ahead of FA's
    // (init_faf.lua's MountModContent); retail's mounts FA's folder at /.
    // Moho's engine lists /sounds through the VFS (func_LoadSoundPath).
    namespace fs = std::filesystem;
    const fs::path root = osc::test::unique_temp_path("osc_xact_mounts_test");
    const fs::path mod = root / "mod_sounds";
    const fs::path fa = root / "fa";
    fs::create_directories(mod / "Extra");
    fs::create_directories(fa / "Sounds");
    write(fa / "Sounds" / "SupCom.xgs", make_xgs());
    write(fa / "Sounds" / "Test.xsb", make_xsb("TestWaves"));
    write(fa / "Sounds" / "TestWaves.xwb", make_xwb("TestWaves", 4));
    // The mod's bank and its waves, a Test.xsb of its own, a streaming
    // bank, and a bank in a folder below /sounds (Moho lists the folder
    // itself only)
    write(mod / "Mod.xsb", make_xsb("ModWaves"));
    write(mod / "ModWaves.xwb", make_xwb("ModWaves", 4));
    write(mod / "Test.xsb", make_xsb("ModWaves"));
    write(mod / "ModStream.xwb", make_streaming_xwb("ModStream"));
    write(mod / "Extra" / "Deep.xsb", make_xsb("ModWaves"));
    write(mod / "Extra" / "DeepWaves.xwb", make_xwb("DeepWaves", 4));

    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/sounds", std::make_unique<osc::vfs::DirectoryMount>(mod));
    vfs.mount("/", std::make_unique<osc::vfs::DirectoryMount>(fa));
    BankRegistry reg(vfs, "/sounds");
    REQUIRE(reg.global_settings()); // FA's, from the second mount
    CHECK(reg.sound_bank_names() == std::vector<std::string>{"mod", "test"});
    CHECK(reg.sound_bank("Deep") == nullptr);
    CHECK(reg.wave_bank("DeepWaves") == nullptr);
    REQUIRE(reg.wave_bank("TestWaves"));
    const osc::audio::XwbParser* mod_waves = reg.wave_bank("ModWaves");
    REQUIRE(mod_waves);
    CHECK(mod_waves->path() == mod / "ModWaves.xwb"); // read from its file
    CHECK_FALSE(mod_waves->in_memory());
    // A streaming bank in a folder streams from its file, as Moho's does
    const osc::audio::XwbParser* stream = reg.wave_bank("ModStream");
    REQUIRE(stream);
    CHECK(stream->streaming());

    // The mod's cue resolves to the mod's waves
    const SoundBank* mod_bank = reg.sound_bank("Mod");
    REQUIRE(mod_bank);
    const Cue* click = mod_bank->find_cue("Click");
    REQUIRE(click);
    const auto wave =
        reg.resolve(*mod_bank, mod_bank->sounds[click->sound].tracks[0].plays[0].waves[0]);
    REQUIRE(wave);
    CHECK(wave->bank == mod_waves);
    CHECK(wave->bank->read_wave_data(wave->index).size() == 8);

    // Both mounts hold Test.xsb: the first's is the one
    const SoundBank* test = reg.sound_bank("Test");
    REQUIRE(test);
    REQUIRE(test->wave_banks.size() == 1);
    CHECK(test->wave_banks[0] == "ModWaves");

    // Mounted the other way round, FA's is
    osc::vfs::VirtualFileSystem fa_first;
    fa_first.mount("/", std::make_unique<osc::vfs::DirectoryMount>(fa));
    fa_first.mount("/sounds", std::make_unique<osc::vfs::DirectoryMount>(mod));
    BankRegistry reg2(fa_first, "/sounds");
    CHECK(reg2.sound_bank_names() == std::vector<std::string>{"mod", "test"});
    const SoundBank* test2 = reg2.sound_bank("Test");
    REQUIRE(test2);
    REQUIRE(test2->wave_banks.size() == 1);
    CHECK(test2->wave_banks[0] == "TestWaves");
    fs::remove_all(root);
}

TEST_CASE("XACT: a bank in an archive is read into memory, but not a streaming one",
          "[audio][xact]") {
    // An archive mounted at /sounds (a mod's .scd, say). Moho reads an
    // in-memory wave bank and a sound bank from it, but opens a streaming
    // wave bank as a file (AudioStreamingWaveBankLoader::Load's CreateFileA),
    // which fails inside an archive: "Error loading WaveBank".
    auto archive = std::make_unique<osc::test::MemoryMount>();
    archive->add("/SupCom.xgs", as_chars(make_xgs()));
    archive->add("/Arc.xsb", as_chars(make_xsb("ArcWaves")));
    archive->add("/ArcWaves.xwb", as_chars(make_xwb("ArcWaves", 4)));
    archive->add("/Stream.xwb", as_chars(make_streaming_xwb("StreamWaves")));
    // A folder mounted after it holds the same files: the archive's are the
    // ones, read from it, not from the folder's files on disk
    namespace fs = std::filesystem;
    const fs::path folder = osc::test::unique_temp_path("osc_xact_archive_test");
    fs::create_directories(folder);
    write(folder / "Arc.xsb", make_xsb("FolderWaves"));
    write(folder / "ArcWaves.xwb", make_xwb("ArcWaves", 6));
    osc::vfs::VirtualFileSystem vfs;
    vfs.mount("/sounds", std::move(archive));
    vfs.mount("/sounds", std::make_unique<osc::vfs::DirectoryMount>(folder));

    BankRegistry reg(vfs, "/sounds");
    REQUIRE(reg.global_settings());
    CHECK(reg.sound_bank_names() == std::vector<std::string>{"arc"});
    CHECK(reg.wave_bank("StreamWaves") == nullptr);
    const osc::audio::XwbParser* waves = reg.wave_bank("ArcWaves");
    REQUIRE(waves);
    CHECK(waves->in_memory());
    CHECK(waves->path().empty());
    CHECK(waves->entry_count() == 4);

    const SoundBank* sb = reg.sound_bank("arc");
    REQUIRE(sb);
    REQUIRE(sb->wave_banks.size() == 1);
    CHECK(sb->wave_banks[0] == "ArcWaves");
    const Cue* click = sb->find_cue("Click");
    REQUIRE(click);
    const auto wave = reg.resolve(*sb, sb->sounds[click->sound].tracks[0].plays[0].waves[0]);
    REQUIRE(wave);
    CHECK(wave->bank == waves);
    const auto data = waves->read_wave_data(wave->index);
    REQUIRE(data.size() == 8);
    CHECK(data[0] == 24); // wave 3 starts at byte 24 of the data segment

    // A bank cut short in memory fails as one on disk does
    const auto good = make_xwb("ArcWaves", 4);
    osc::audio::XwbParser cut;
    CHECK_FALSE(
        cut.parse(std::make_shared<const std::vector<u8>>(good.begin(), good.begin() + 60)).ok());
    fs::remove_all(folder);
}

TEST_CASE("XACT: a damaged wave bank fails to load", "[audio][xact]") {
    namespace fs = std::filesystem;
    const fs::path file = osc::test::unique_temp_path("osc_xact_bad", ".xwb");
    const auto good = make_xwb("TestWaves", 4);
    {
        write(file, good);
        osc::audio::XwbParser p;
        REQUIRE(p.parse(file).ok());
        CHECK(p.entry_count() == 4);
    }
    {
        // Bank data too short to hold the 64-byte name
        auto bytes = good;
        bytes[12 + 4] = 30; // segment 0 length
        bytes[12 + 5] = bytes[12 + 6] = bytes[12 + 7] = 0;
        write(file, bytes);
        osc::audio::XwbParser p;
        CHECK_FALSE(p.parse(file).ok());
    }
    {
        // An entry count far past the entry table
        auto bytes = good;
        const size_t count_at = 52 + 4; // bank data follows the 52-byte header
        bytes[count_at] = bytes[count_at + 1] = bytes[count_at + 2] = 0xFF;
        bytes[count_at + 3] = 0x0F;
        write(file, bytes);
        osc::audio::XwbParser p;
        CHECK_FALSE(p.parse(file).ok());
    }
    fs::remove(file);
}
