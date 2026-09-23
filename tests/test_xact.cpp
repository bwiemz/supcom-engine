#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "audio/xact/bank_registry.hpp"
#include "audio/xact/byte_reader.hpp"
#include "audio/xact/global_settings.hpp"
#include "audio/xact/sound_bank.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace osc;
using namespace osc::audio::xact;
using Catch::Matchers::WithinAbs;

namespace {

/// Little-endian byte builder for synthetic XACT files.
struct Bytes {
    std::vector<u8> d;
    size_t pos() const { return d.size(); }
    Bytes& u8_(u8 v) { d.push_back(v); return *this; }
    Bytes& u16_(u16 v) { return u8_(static_cast<u8>(v)).u8_(static_cast<u8>(v >> 8)); }
    Bytes& u32_(u32 v) { return u16_(static_cast<u16>(v)).u16_(static_cast<u16>(v >> 16)); }
    Bytes& f32_(f32 f) {
        u32 v;
        std::memcpy(&v, &f, 4);
        return u32_(v);
    }
    Bytes& tag(const char* t) {
        for (int i = 0; i < 4; ++i) u8_(static_cast<u8>(t[i]));
        return *this;
    }
    Bytes& zeros(size_t n) {
        d.insert(d.end(), n, 0);
        return *this;
    }
    Bytes& name64(const std::string& s) {
        std::string padded = s;
        padded.resize(64, '\0');
        d.insert(d.end(), padded.begin(), padded.end());
        return *this;
    }
    Bytes& cstr(const std::string& s) {
        d.insert(d.end(), s.begin(), s.end());
        return u8_(0);
    }
    void patch_u32(size_t at, u32 v) {
        for (int i = 0; i < 4; ++i) d[at + i] = static_cast<u8>(v >> (8 * i));
    }
    void patch_u16(size_t at, u16 v) {
        d[at] = static_cast<u8>(v);
        d[at + 1] = static_cast<u8>(v >> 8);
    }
};

/// A global settings file: categories Global > Music, variable Distance,
/// one linear volume curve over Distance.
std::vector<u8> make_xgs() {
    Bytes b;
    b.tag("XGSF").u16_(43).u16_(42).u16_(0).zeros(8).u8_(3);
    b.u16_(2).u16_(1).u16_(0).u16_(0).u16_(1).u16_(0).u16_(0); // counts
    const size_t offs = b.pos();
    b.zeros(11 * 4); // offsets, patched below
    const size_t categories = b.pos();
    b.u8_(255).u16_(0).u16_(0).u8_(0).u16_(0xFFFF).u8_(180).u8_(2);      // Global
    b.u8_(1).u16_(0).u16_(200).u8_(2 << 3).u16_(0).u8_(160).u8_(3);      // Music
    const size_t variables = b.pos();
    b.u8_(0x0D).f32_(0).f32_(0).f32_(10000);                             // Distance (per cue)
    const size_t rpcs = b.pos();
    b.u16_(0).u8_(2).u16_(0);                                            // Distance -> volume
    b.f32_(0).f32_(0).u8_(0).f32_(1000).f32_(-2000).u8_(0);
    const size_t cat_names = b.pos();
    b.cstr("Global");
    const size_t music_name = b.pos();
    b.cstr("Music");
    const size_t var_name = b.pos();
    b.cstr("Distance");
    const size_t cat_index = b.pos();
    b.u32_(static_cast<u32>(cat_names)).u16_(0xFF).u32_(static_cast<u32>(music_name)).u16_(0xFF);
    const size_t var_index = b.pos();
    b.u32_(static_cast<u32>(var_name)).u16_(0xFF);
    const u32 table[11] = {static_cast<u32>(categories), static_cast<u32>(variables), 0,
                           static_cast<u32>(cat_index),  0, static_cast<u32>(var_index),
                           0, 0, static_cast<u32>(rpcs), 0, 0};
    for (int i = 0; i < 11; ++i) b.patch_u32(offs + 4 * i, table[i]);
    return b.d;
}

/// An XACT 3.0 sound bank "Test" drawing on wave bank `wave_bank`:
/// cue "Click" plays wave 3 (a simple sound); cue "Shot" plays a complex
/// sound whose track picks one of waves 0-2 (no immediate repeat), with a
/// pitch variation of +-200 cents, looping forever, 150 ms in.
std::vector<u8> make_xsb(const std::string& wave_bank) {
    Bytes b;
    b.tag("SDBK").u16_(43).u16_(43).u16_(0).zeros(8).u8_(1);
    b.u16_(1).u16_(1).u16_(0).u16_(2); // simple, complex, unknown, total
    b.u8_(1).u16_(2).u16_(0).u16_(0);  // wave banks, sounds, name length, unknown
    const size_t offs = b.pos();       // simple, complex, names, ?, variation, transition,
    b.zeros(10 * 4);                   // wave bank names, hash, name index, sounds
    b.name64("Test");

    const size_t wb_names = b.pos();
    b.name64(wave_bank);

    const size_t sounds = b.pos();
    // Simple sound: category 1, volume 180 (0 mB), pitch 0.
    b.u8_(0x00).u16_(1).u8_(180).u16_(0).u8_(0).u16_(12).u16_(3).u8_(0);
    // Complex sound with a sound RPC.
    const size_t complex_sound = b.pos();
    b.u8_(0x03).u16_(1).u8_(180).u16_(50).u8_(7);
    const size_t length_at = b.pos();
    b.u16_(0).u8_(1);                       // entry length (patched), 1 track
    b.u16_(7).u8_(1).u32_(1234);            // RPC block: length, 1 code
    const size_t track_header = b.pos();
    b.u8_(180).u32_(0);                     // track volume, events offset (patched)
    const size_t events = b.pos();
    b.u8_(1);                               // 1 event
    b.u32_(6u | (150u << 5)).u16_(0).u8_(0xFF);
    b.u8_(0).u8_(255).u16_(0).u16_(0);      // flags, loop forever, position, angle
    b.u16_(static_cast<u16>(-200)).u16_(200).u8_(180).u8_(180).u8_(0x80); // 3.0 effect variation
    b.u32_(3u | (3u << 16)).u32_(0);        // 3 waves, random without immediate repeat
    for (u16 w = 0; w < 3; ++w) b.u16_(w).u8_(0).u8_(0).u8_(255);
    b.patch_u16(length_at, static_cast<u16>(b.pos() - complex_sound));
    b.patch_u32(track_header + 1, static_cast<u32>(events));

    const size_t simple_cues = b.pos();
    b.u8_(0x04).u32_(static_cast<u32>(sounds));
    const size_t complex_cues = b.pos();
    b.u8_(0x04).u32_(static_cast<u32>(complex_sound)).u32_(0xFFFFFFFF).u8_(2).u16_(10).u16_(300).u8_(2 << 3);

    const size_t click = b.pos();
    b.cstr("Click");
    const size_t shot = b.pos();
    b.cstr("Shot");
    const size_t name_index = b.pos();
    b.u32_(static_cast<u32>(click)).u16_(0xFF).u32_(static_cast<u32>(shot)).u16_(0xFF);

    const u32 table[10] = {static_cast<u32>(simple_cues), static_cast<u32>(complex_cues),
                           static_cast<u32>(click), 0, 0xFFFFFFFF, 0xFFFFFFFF,
                           static_cast<u32>(wb_names), 0xFFFFFFFF,
                           static_cast<u32>(name_index), static_cast<u32>(sounds)};
    for (int i = 0; i < 10; ++i) b.patch_u32(offs + 4 * i, table[i]);
    return b.d;
}

/// A wave bank named `name` with `count` 16-bit mono PCM waves of 8 bytes.
std::vector<u8> make_xwb(const std::string& name, u32 count) {
    Bytes b;
    b.tag("WBND").u32_(43).u32_(44);
    const size_t segs = b.pos();
    b.zeros(5 * 8);
    const size_t bankdata = b.pos();
    b.u32_(0).u32_(count).name64(name).u32_(24).u32_(0).u32_(4).u32_(0).zeros(8);
    const size_t meta = b.pos();
    const u32 fmt = 0u | (1u << 2) | (22050u << 5) | (2u << 23) | (1u << 31);
    for (u32 i = 0; i < count; ++i) b.u32_(4u << 4).u32_(fmt).u32_(i * 8).u32_(8).u32_(0).u32_(4);
    const size_t data = b.pos();
    for (u32 i = 0; i < count * 8; ++i) b.u8_(static_cast<u8>(i));
    const size_t seg[5][2] = {{bankdata, meta - bankdata}, {meta, data - meta}, {0, 0}, {0, 0},
                              {data, b.pos() - data}};
    for (int i = 0; i < 5; ++i) {
        b.patch_u32(segs + 8 * i, static_cast<u32>(seg[i][0]));
        b.patch_u32(segs + 8 * i + 4, static_cast<u32>(seg[i][1]));
    }
    return b.d;
}

void write(const std::filesystem::path& p, const std::vector<u8>& bytes) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                             static_cast<std::streamsize>(bytes.size()));
}

} // namespace

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
    CHECK(simple.category == 1);
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
    const fs::path dir = fs::temp_directory_path() / "osc_xact_registry_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    // The sound bank lists "TestWaves", a wave bank whose file is named
    // otherwise (as retail's XAS_Weapons.xwb is internally XAS_Weapon).
    write(dir / "Test.xsb", make_xsb("TestWaves"));
    write(dir / "Other_File_Name.xwb", make_xwb("TestWaves", 4));
    write(dir / "Game.xgs", make_xgs());

    BankRegistry reg(dir);
    REQUIRE(reg.global_settings());
    CHECK(reg.global_settings()->categories.size() == 2);
    CHECK(reg.sound_bank_names() == std::vector<std::string>{"Test"});
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
