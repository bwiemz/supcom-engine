#pragma once

// Synthetic XACT files for tests: little-endian builders of a global
// settings file, an XACT 3.0 sound bank and a PCM wave bank.

#include "core/types.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace osc::test::xact_fixtures {

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

/// A global settings file: categories Global > Music (limit 1, replace
/// oldest, 200 ms fade-out), variables Distance (per cue) and TestCutoff
/// (global, 100), and one linear volume curve over Distance (0 mB at 0,
/// -2000 mB from 1000). `rpc_code` receives the curve's code.
inline std::vector<u8> make_xgs(u32* rpc_code = nullptr, u8 music_limit = 1,
                                u8 music_behavior = 2) {
    Bytes b;
    b.tag("XGSF").u16_(43).u16_(42).u16_(0).zeros(8).u8_(3);
    b.u16_(2).u16_(2).u16_(0).u16_(0).u16_(1).u16_(0).u16_(0); // counts
    const size_t offs = b.pos();
    b.zeros(11 * 4); // offsets, patched below
    const size_t categories = b.pos();
    b.u8_(255).u16_(0).u16_(0).u8_(0).u16_(0xFFFF).u8_(180).u8_(2);      // Global
    b.u8_(music_limit).u16_(0).u16_(200).u8_(static_cast<u8>(music_behavior << 3));
    b.u16_(0).u8_(160).u8_(3); // Music
    const size_t variables = b.pos();
    b.u8_(0x0D).f32_(0).f32_(0).f32_(10000);                             // Distance (per cue)
    b.u8_(0x01).f32_(100).f32_(-1).f32_(10000);                          // TestCutoff (global)
    const size_t rpcs = b.pos();
    if (rpc_code) *rpc_code = static_cast<u32>(rpcs);
    b.u16_(0).u8_(2).u16_(0);                                            // Distance -> volume
    b.f32_(0).f32_(0).u8_(0).f32_(1000).f32_(-2000).u8_(0);
    const size_t cat_names = b.pos();
    b.cstr("Global");
    const size_t music_name = b.pos();
    b.cstr("Music");
    const size_t var_name = b.pos();
    b.cstr("Distance");
    const size_t cutoff_name = b.pos();
    b.cstr("TestCutoff");
    const size_t cat_index = b.pos();
    b.u32_(static_cast<u32>(cat_names)).u16_(0xFF).u32_(static_cast<u32>(music_name)).u16_(0xFF);
    const size_t var_index = b.pos();
    b.u32_(static_cast<u32>(var_name)).u16_(0xFF).u32_(static_cast<u32>(cutoff_name)).u16_(0xFF);
    const u32 table[11] = {static_cast<u32>(categories), static_cast<u32>(variables), 0,
                           static_cast<u32>(cat_index),  0, static_cast<u32>(var_index),
                           0, 0, static_cast<u32>(rpcs), 0, 0};
    for (int i = 0; i < 11; ++i) b.patch_u32(offs + 4 * i, table[i]);
    return b.d;
}

/// An XACT 3.0 sound bank "Test" drawing on wave bank `wave_bank`:
/// cue "Click" plays wave 3 once (a simple sound, category Global); cue
/// "Shot" (category Music, cue limit 2, 300 ms fade-out) plays a complex
/// sound with RPC `rpc_code`, whose track picks one of waves 0-2 (no
/// immediate repeat) with a pitch variation of +-200 cents, looping
/// forever, 150 ms in.
inline std::vector<u8> make_xsb(const std::string& wave_bank, u32 rpc_code = 1234,
                                u16 click_category = 0, u8 click_priority = 0) {
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
    // Simple sound: category 0, volume 180 (0 mB), pitch 0.
    b.u8_(0x00).u16_(click_category).u8_(180).u16_(0).u8_(click_priority).u16_(12).u16_(3).u8_(0);
    // Complex sound with a sound RPC.
    const size_t complex_sound = b.pos();
    b.u8_(0x03).u16_(1).u8_(180).u16_(50).u8_(7);
    const size_t length_at = b.pos();
    b.u16_(0).u8_(1);                       // entry length (patched), 1 track
    b.u16_(7).u8_(1).u32_(rpc_code);        // RPC block: length, 1 code
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

/// A wave bank named `name` with `count` 16-bit mono 22050 Hz PCM waves of
/// `samples` samples each (2205: 0.1 s).
inline std::vector<u8> make_xwb(const std::string& name, u32 count, u32 samples = 4) {
    Bytes b;
    b.tag("WBND").u32_(43).u32_(44);
    const size_t segs = b.pos();
    b.zeros(5 * 8);
    const size_t bankdata = b.pos();
    b.u32_(0).u32_(count).name64(name).u32_(24).u32_(0).u32_(4).u32_(0).zeros(8);
    const size_t meta = b.pos();
    const u32 fmt = 0u | (1u << 2) | (22050u << 5) | (2u << 23) | (1u << 31);
    const u32 bytes = samples * 2;
    for (u32 i = 0; i < count; ++i)
        b.u32_(samples << 4).u32_(fmt).u32_(i * bytes).u32_(bytes).u32_(0).u32_(samples);
    const size_t data = b.pos();
    for (u32 i = 0; i < count * bytes; ++i) b.u8_(static_cast<u8>(i));
    const size_t seg[5][2] = {{bankdata, meta - bankdata}, {meta, data - meta}, {0, 0}, {0, 0},
                              {data, b.pos() - data}};
    for (int i = 0; i < 5; ++i) {
        b.patch_u32(segs + 8 * i, static_cast<u32>(seg[i][0]));
        b.patch_u32(segs + 8 * i + 4, static_cast<u32>(seg[i][1]));
    }
    return b.d;
}

inline void write(const std::filesystem::path& p, const std::vector<u8>& bytes) {
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                             static_cast<std::streamsize>(bytes.size()));
}

} // namespace osc::test::xact_fixtures
