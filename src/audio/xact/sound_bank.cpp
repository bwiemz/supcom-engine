#include "audio/xact/sound_bank.hpp"

#include "audio/xact/byte_reader.hpp"

#include <string>

namespace osc::audio::xact {

namespace {

constexpr u16 kContent30 = 43; ///< XACT 3.0: events inside sounds, short effect variation

constexpr u8 kSoundComplex = 0x01;
constexpr u8 kSoundRpc = 0x02;
constexpr u8 kSoundTrackRpc = 0x04;
constexpr u8 kSoundRpcMask = 0x0E;
constexpr u8 kSoundDsp = 0x10;

constexpr u8 kCueSingleSound = 0x04;

enum EventType : u8 {
    kStop = 0,
    kPlayWave = 1,
    kPlayWaveTrackVariation = 3,
    kPlayWaveEffectVariation = 4,
    kPlayWaveTrackEffectVariation = 6,
    kPitch = 7,
    kVolume = 8,
    kMarker = 9,
    kPitchRepeating = 16,
    kVolumeRepeating = 17,
    kMarkerRepeating = 18,
};

std::vector<u32> read_rpc_codes(ByteReader& r) {
    std::vector<u32> codes(r.read_u8());
    for (auto& c : codes) c = r.read_u32();
    return codes;
}

/// The effect-variation block of a play event.
void read_effect_variation(ByteReader& r, u16 content, PlayEvent& e) {
    e.pitch_min = r.read_s16();
    e.pitch_max = r.read_s16();
    e.volume_min_mb = volume_byte_to_millibels(r.read_u8());
    e.volume_max_mb = volume_byte_to_millibels(r.read_u8());
    if (content == kContent30) {
        // 3.0: one flag byte, no filter variation (0x80 pitch, 0x40 volume;
        // every retail event with a flag has a real range for it).
        const u8 flags = r.read_u8();
        e.vary_pitch = (flags & 0x80) != 0;
        e.vary_volume = (flags & 0x40) != 0;
    } else {
        r.skip(4 * 4); // filter frequency and Q ranges
        const u16 flags = r.read_u16();
        e.vary_pitch = (flags & 0x1000) != 0;
        e.vary_volume = (flags & 0x2000) != 0;
    }
}

/// The waves of a track-variation play event.
void read_track_variation(ByteReader& r, PlayEvent& e) {
    const u32 info = r.read_u32();
    const u16 count = static_cast<u16>(info & 0xFFFF);
    e.variation = static_cast<VariationMode>((info >> 16) & 0x7);
    r.skip(4); // unknown
    e.waves.resize(count);
    for (auto& w : e.waves) {
        w.wave = r.read_u16();
        w.bank = r.read_u8();
        w.weight_min = r.read_u8();
        w.weight_max = r.read_u8();
    }
}

/// A track's events: its play events are kept, the rest skipped.
Result<void> read_track_events(ByteReader& r, u16 content, Track& track) {
    const u8 count = r.read_u8();
    for (u8 i = 0; i < count; ++i) {
        const u32 info = r.read_u32();
        const u16 random_offset = r.read_u16();
        if (r.read_u8() != 0xFF) return Error("XSB: bad track event separator");
        const auto type = static_cast<u8>(info & 0x1F);
        PlayEvent e;
        e.time_ms = (info >> 5) & 0xFFFF;
        e.random_offset_ms = random_offset;
        switch (type) {
        case kStop:
            r.skip(1);
            break;
        case kPlayWave:
        case kPlayWaveEffectVariation: {
            r.skip(1); // flags
            WaveChoice w;
            w.wave = r.read_u16();
            w.bank = r.read_u8();
            e.waves.push_back(w);
            e.loop_count = r.read_u8();
            r.skip(2 + 2); // position, angle
            if (type == kPlayWaveEffectVariation) read_effect_variation(r, content, e);
            track.plays.push_back(std::move(e));
            break;
        }
        case kPlayWaveTrackVariation:
        case kPlayWaveTrackEffectVariation:
            r.skip(1); // flags
            e.loop_count = r.read_u8();
            r.skip(2 + 2); // position, angle
            if (type == kPlayWaveTrackEffectVariation) read_effect_variation(r, content, e);
            read_track_variation(r, e);
            track.plays.push_back(std::move(e));
            break;
        case kPitch:
        case kVolume:
        case kPitchRepeating:
        case kVolumeRepeating:
            if (r.read_u8() & 0x01) { // ramp
                r.skip(4 + 4 + 4 + 2);
            } else { // equation
                r.skip(1 + 4 + 4 + 5);
                if (type == kPitchRepeating || type == kVolumeRepeating) r.skip(2 + 2);
            }
            break;
        case kMarker:
            r.skip(4);
            break;
        case kMarkerRepeating:
            r.skip(4 + 2 + 2);
            break;
        default:
            return Error("XSB: unknown track event type " + std::to_string(type));
        }
    }
    return {};
}

} // namespace

const Cue* SoundBank::find_cue(std::string_view name) const {
    auto it = cue_index_.find(std::string(name));
    return it == cue_index_.end() ? nullptr : &cues[it->second];
}

Result<SoundBank> SoundBank::parse(std::span<const u8> data) {
    ByteReader r(data);
    if (r.read_u32() != 0x4B424453) return Error("XSB: not an XACT sound bank");
    const u16 content = r.read_u16();
    if (content < kContent30 || content > 46) return Error("XSB: unsupported content version");
    r.skip(2 + 2 + 8 + 1); // tool version, CRC, last modified, platform
    const u16 simple_count = r.read_u16();
    const u16 complex_count = r.read_u16();
    r.skip(2 + 2); // unknown, total cue count (aligned)
    const u8 wave_bank_count = r.read_u8();
    const u16 sound_count = r.read_u16();
    r.skip(2 + 2); // cue name table length, unknown
    const i32 simple_off = r.read_s32();
    const i32 complex_off = r.read_s32();
    r.skip(4 + 4); // cue names (reached through the name index), unknown
    r.skip(4 + 4); // cue variation tables (read through each cue), transitions
    const i32 wave_bank_names_off = r.read_s32();
    r.skip(4); // cue name hash
    const i32 cue_name_index_off = r.read_s32();
    const i32 sounds_off = r.read_s32();
    const size_t name_at = r.pos();
    if (!r.ok()) return Error("XSB: truncated header");

    SoundBank sb;
    sb.name = r.cstr(name_at, 64);
    sb.wave_banks.resize(wave_bank_count);
    for (u8 i = 0; i < wave_bank_count; ++i)
        sb.wave_banks[i] = r.cstr(static_cast<size_t>(wave_bank_names_off) + 64u * i, 64);

    // Sounds, and their offsets: how cues refer to them.
    std::unordered_map<u32, u32> sound_at;
    if (sound_count > 0) r.seek(static_cast<size_t>(sounds_off)); // -1 in an empty bank
    sb.sounds.resize(sound_count);
    for (u16 i = 0; i < sound_count && r.ok(); ++i) {
        Sound& s = sb.sounds[i];
        const auto start = static_cast<u32>(r.pos());
        sound_at.emplace(start, i);
        const u8 flags = r.read_u8();
        s.category = r.read_u16();
        s.volume_mb = volume_byte_to_millibels(r.read_u8());
        s.pitch = r.read_s16();
        s.priority = r.read_u8();
        const u16 entry_length = r.read_u16();

        u8 track_count = 1;
        if (flags & kSoundComplex) {
            track_count = r.read_u8();
        } else {
            PlayEvent e;
            WaveChoice w;
            w.wave = r.read_u16();
            w.bank = r.read_u8();
            e.waves.push_back(w);
            s.tracks.resize(1);
            s.tracks[0].plays.push_back(std::move(e));
        }
        if (flags & kSoundComplex) s.tracks.resize(track_count);
        if (flags & kSoundRpcMask) {
            const size_t rpc_start = r.pos();
            const u16 rpc_length = r.read_u16();
            if (flags & kSoundRpc) s.rpc_codes = read_rpc_codes(r);
            if (flags & kSoundTrackRpc)
                for (auto& t : s.tracks) t.rpc_codes = read_rpc_codes(r);
            r.seek(rpc_start + rpc_length);
        }
        if (flags & kSoundDsp) {
            r.skip(2);
            r.skip(4u * r.read_u8());
        }
        if (flags & kSoundComplex) {
            std::vector<u32> event_offsets(track_count);
            for (u8 t = 0; t < track_count; ++t) {
                s.tracks[t].volume_mb = volume_byte_to_millibels(r.read_u8());
                event_offsets[t] = r.read_u32();
                if (content != kContent30) r.skip(2 + 2); // filter
            }
            const size_t after_headers = r.pos();
            for (u8 t = 0; t < track_count; ++t) {
                ByteReader events(data, event_offsets[t]);
                if (auto res = read_track_events(events, content, s.tracks[t]); !res)
                    return Error(res.error().message + " (sound " + std::to_string(i) + ")");
                if (!events.ok()) return Error("XSB: truncated track events");
            }
            r.seek(after_headers);
        }
        // 3.0 keeps a sound's events inside its entry, so its length is the
        // way to the next sound; later versions store events elsewhere.
        if (content == kContent30) r.seek(start + entry_length);
    }
    if (!r.ok()) return Error("XSB: truncated sound data");

    // Cues: simple ones play a sound; complex ones a sound (single) or a
    // variation table.
    auto sound_of = [&](u32 code, u32& out) {
        auto it = sound_at.find(code);
        if (it == sound_at.end()) return false;
        out = it->second;
        return true;
    };
    sb.cues.resize(static_cast<size_t>(simple_count) + complex_count);
    if (simple_count > 0) r.seek(static_cast<size_t>(simple_off));
    for (u16 i = 0; i < simple_count; ++i) {
        Cue& c = sb.cues[i];
        r.skip(1); // flags
        if (!sound_of(r.read_u32(), c.sound)) return Error("XSB: cue without a sound");
    }
    if (complex_count > 0) r.seek(static_cast<size_t>(complex_off));
    for (u16 i = 0; i < complex_count; ++i) {
        Cue& c = sb.cues[simple_count + i];
        const u8 flags = r.read_u8();
        const u32 code = r.read_u32();
        r.skip(4); // transitions
        c.instance_limit = r.read_u8();
        c.fade_in_ms = r.read_u16();
        c.fade_out_ms = r.read_u16();
        c.limit_behavior = static_cast<LimitBehavior>(r.read_u8() >> 3);
        if (flags & kCueSingleSound) {
            if (!sound_of(code, c.sound)) return Error("XSB: cue without a sound");
        } else {
            // A cue-level variation table. FA has none; take its first
            // sound entry rather than refuse the bank.
            ByteReader v(data, code);
            const u32 info = v.read_u32();
            const auto type = static_cast<u8>((info >> 19) & 0x7);
            v.skip(2 + 2);
            if ((info & 0xFFFF) == 0 || (type != 1 && type != 3) ||
                !sound_of(v.read_u32(), c.sound))
                return Error("XSB: unsupported cue variation table");
        }
    }
    if (!r.ok()) return Error("XSB: truncated cue data");

    for (size_t i = 0; i < sb.cues.size() && cue_name_index_off >= 0; ++i) {
        ByteReader idx(data, static_cast<size_t>(cue_name_index_off) + i * 6);
        sb.cues[i].name = r.cstr(idx.read_u32());
        if (!idx.ok() || !r.ok()) return Error("XSB: bad cue name");
        sb.cue_index_.emplace(sb.cues[i].name, i);
    }

    // Every wave reference must name a listed wave bank.
    for (const auto& s : sb.sounds)
        for (const auto& t : s.tracks)
            for (const auto& p : t.plays)
                for (const auto& w : p.waves)
                    if (w.bank >= wave_bank_count)
                        return Error("XSB: wave from an unlisted wave bank");
    return sb;
}

} // namespace osc::audio::xact
