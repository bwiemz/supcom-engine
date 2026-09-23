#define NOMINMAX
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "audio/sound_manager.hpp"
#include "audio/xact/bank_registry.hpp"
#include "audio/xwb_parser.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace osc::audio {

namespace {

constexpr u32 kForever = 0xFFFFFFFF;
constexpr size_t kWaveCacheBytes = 128u << 20; ///< decoded-ready waves kept around
constexpr int kMaxCategoryDepth = 32;

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

} // namespace

// ---- Pimpl structs (keep miniaudio.h out of header) ----

struct SoundManager::AudioEngine {
    ma_engine engine;
    bool initialized = false;
};

struct SoundManager::WaveData {
    std::vector<u8> wav; ///< a WAV file in memory, for miniaudio's decoder
    f64 seconds = 0;     ///< length at unit pitch
};

/// One wave playing on a track. Not movable (miniaudio keeps pointers into
/// it), so voices live on the heap.
struct SoundManager::Voice {
    std::shared_ptr<const WaveData> data;
    ma_decoder decoder{};
    ma_sound sound{};
    bool decoder_init = false;
    bool sound_init = false;
    u32 loops_left = 0; ///< plays after this one (kForever: until stopped)
    f64 ends = 0;       ///< headless: when this play ends
    f32 variation_mb = 0;
    f32 variation_cents = 0;
    bool done = false;

    Voice() = default;
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;
    ~Voice() {
        if (sound_init) ma_sound_uninit(&sound);
        if (decoder_init) ma_decoder_uninit(&decoder);
    }
    f64 seconds_at(f32 cents) const {
        return data ? data->seconds / std::pow(2.0, cents / 1200.0) : 0.0;
    }
};

struct SoundManager::CueInstance {
    enum class State { Playing, FadingOut, Releasing };

    SoundHandle handle = INVALID_SOUND;
    const xact::SoundBank* bank = nullptr;
    const xact::Cue* cue = nullptr;
    const xact::Sound* sound = nullptr;
    std::string bank_name;
    bool positional = false;
    sim::Vector3 pos{};
    bool force_loop = false;
    f64 started = 0;
    f64 fade_in = 0; ///< seconds
    State state = State::Playing;
    f64 stop_started = 0;
    f64 stop_duration = 0;
    f32 gain = 1; ///< last applied (for "replace quietest")

    struct TrackState {
        std::vector<f64> fire_at; ///< per play event
        size_t next = 0;
        std::vector<std::unique_ptr<Voice>> voices;
    };
    std::vector<TrackState> tracks;
    std::vector<std::function<void()>> on_finished;
    bool ended = false;
};

// ---- WAV header synthesis ----

/// Build a minimal WAV file wrapping raw PCM or ADPCM data so miniaudio can decode it.
static std::vector<u8> build_wav(const WaveInfo& info, const std::vector<u8>& raw) {
    // For PCM (format_tag 0): RIFF/WAVE with fmt + data chunks
    // For ADPCM (format_tag 2): RIFF/WAVE with extended fmt + data chunks

    bool is_adpcm = (info.format_tag == 2);
    u16 wav_format_tag = is_adpcm ? 0x0002 : 0x0001;

    u32 channels = info.channels ? info.channels : 1;
    u32 sample_rate = info.sample_rate;
    u16 bits_per_sample = is_adpcm ? 4 : static_cast<u16>(info.bits_per_sample);
    u16 block_align = static_cast<u16>(info.block_align);
    u32 avg_bytes_per_sec;
    if (is_adpcm && block_align > 7 * channels) {
        u32 samples_per_block = (block_align - 7 * channels) * 2 / channels + 2;
        avg_bytes_per_sec = samples_per_block > 0
            ? (sample_rate / samples_per_block) * block_align
            : sample_rate;
    } else {
        avg_bytes_per_sec = sample_rate * channels * (info.bits_per_sample / 8);
    }

    // ADPCM needs extended fmt chunk with coefficient table
    // Standard MS-ADPCM has 7 coefficient pairs
    static const i16 adpcm_coeffs[7][2] = {
        {256, 0}, {512, -256}, {0, 0}, {192, 64},
        {240, 0}, {460, -208}, {392, -232}
    };

    u16 adpcm_samples_per_block = 0;
    if (is_adpcm && block_align > 0) {
        adpcm_samples_per_block = static_cast<u16>(
            (block_align - 7 * channels) * 2 / channels + 2);
    }

    // Calculate fmt chunk sizes
    u32 fmt_extra_size = is_adpcm ? (2 + 2 + 7 * 4) : 0; // cbSize data
    u32 fmt_chunk_size = 16 + (is_adpcm ? (2 + fmt_extra_size) : 0);
    u32 data_chunk_size = static_cast<u32>(raw.size());
    u32 riff_size = 4 + (8 + fmt_chunk_size) + (8 + data_chunk_size);

    std::vector<u8> wav;
    wav.reserve(12 + 8 + fmt_chunk_size + 8 + data_chunk_size);

    auto write_u16 = [&](u16 v) {
        wav.push_back(static_cast<u8>(v));
        wav.push_back(static_cast<u8>(v >> 8));
    };
    auto write_u32 = [&](u32 v) {
        wav.push_back(static_cast<u8>(v));
        wav.push_back(static_cast<u8>(v >> 8));
        wav.push_back(static_cast<u8>(v >> 16));
        wav.push_back(static_cast<u8>(v >> 24));
    };
    auto write_tag = [&](const char* tag) {
        wav.insert(wav.end(), tag, tag + 4);
    };
    auto write_i16 = [&](i16 v) {
        write_u16(static_cast<u16>(v));
    };

    // RIFF header
    write_tag("RIFF");
    write_u32(riff_size);
    write_tag("WAVE");

    // fmt chunk
    write_tag("fmt ");
    write_u32(fmt_chunk_size);
    write_u16(wav_format_tag);
    write_u16(static_cast<u16>(channels));
    write_u32(sample_rate);
    write_u32(avg_bytes_per_sec);
    write_u16(block_align);
    write_u16(bits_per_sample);

    if (is_adpcm) {
        write_u16(static_cast<u16>(fmt_extra_size)); // cbSize
        write_u16(adpcm_samples_per_block);
        write_u16(7); // num coefficients
        for (int i = 0; i < 7; i++) {
            write_i16(adpcm_coeffs[i][0]);
            write_i16(adpcm_coeffs[i][1]);
        }
    }

    // data chunk
    write_tag("data");
    write_u32(data_chunk_size);
    wav.insert(wav.end(), raw.begin(), raw.end());

    return wav;
}

// ---- SoundManager implementation ----

SoundManager::SoundManager(const fs::path& sounds_dir, bool output)
    : sounds_dir_(sounds_dir), engine_(std::make_unique<AudioEngine>()) {
    if (!fs::exists(sounds_dir_)) {
        spdlog::warn("Sound directory not found: {} -- no audio", sounds_dir_.string());
        return;
    }
    registry_ = std::make_unique<xact::BankRegistry>(sounds_dir_);
    if (const auto* gs = registry_->global_settings()) {
        globals_.resize(gs->variables.size());
        for (size_t i = 0; i < gs->variables.size(); ++i) globals_[i] = gs->variables[i].initial;
        user_volume_.assign(gs->categories.size(), 1.0f);
        world_category_ = gs->find_category("World");
        distance_variable_ = gs->find_variable("Distance");
        attack_variable_ = gs->find_variable("AttackTime");
        release_variable_ = gs->find_variable("ReleaseTime");
        cue_instances_variable_ = gs->find_variable("NumCueInstances");
    }
    if (!output) return;

    ma_engine_config cfg = ma_engine_config_init();
    cfg.listenerCount = 1;
    if (ma_engine_init(&cfg, &engine_->engine) != MA_SUCCESS) {
        spdlog::warn("No audio device -- sounds run silently");
        return;
    }
    engine_->initialized = true;
    output_ = true;
    ma_engine_listener_set_world_up(&engine_->engine, 0, 0, 1, 0);
    spdlog::info("Audio engine initialized");
}

SoundManager::~SoundManager() {
    instances_.clear(); // voices uninit before the engine
    if (engine_ && engine_->initialized) ma_engine_uninit(&engine_->engine);
}

std::shared_ptr<const SoundManager::WaveData> SoundManager::wave_data(const XwbParser& bank,
                                                                       u32 index) {
    const WaveKey key{&bank, index};
    if (auto it = waves_.find(key); it != waves_.end()) {
        wave_lru_.remove(key);
        wave_lru_.push_front(key);
        return it->second;
    }
    const WaveInfo& info = bank.entry(index);
    auto raw = bank.read_wave_data(index);
    if (raw.empty()) return nullptr;
    auto data = std::make_shared<WaveData>();
    u64 samples = info.duration_samples;
    if (info.format_tag == 0) {
        const u32 frame = std::max<u32>(1, info.channels * (info.bits_per_sample / 8));
        samples = raw.size() / frame;
    }
    data->seconds = info.sample_rate ? static_cast<f64>(samples) / info.sample_rate : 0.0;
    data->wav = build_wav(info, raw);

    wave_bytes_ += data->wav.size();
    waves_.emplace(key, data);
    wave_lru_.push_front(key);
    // Evict the least recent waves nothing is playing.
    for (auto it = wave_lru_.end(); wave_bytes_ > kWaveCacheBytes && it != wave_lru_.begin();) {
        --it;
        auto found = waves_.find(*it);
        if (found != waves_.end() && found->second.use_count() == 1) {
            wave_bytes_ -= found->second->wav.size();
            waves_.erase(found);
            it = wave_lru_.erase(it);
        }
    }
    return data;
}

u32 SoundManager::pick_wave(const xact::PlayEvent& ev) {
    const auto n = static_cast<u32>(ev.waves.size());
    if (n <= 1) return 0;
    VariationState& st = variation_[&ev];
    std::uniform_int_distribution<u32> any(0, n - 1);
    u32 pick = 0;
    switch (ev.variation) {
    case xact::VariationMode::Ordered:
        pick = st.last == 0xFFFFFFFF ? 0 : (st.last + 1) % n;
        break;
    case xact::VariationMode::OrderedFromRandom:
        pick = st.last == 0xFFFFFFFF ? any(rng_) : (st.last + 1) % n;
        break;
    case xact::VariationMode::Shuffle:
        if (st.order.empty()) {
            st.order.resize(n);
            for (u32 i = 0; i < n; ++i) st.order[i] = i;
            std::shuffle(st.order.begin(), st.order.end(), rng_);
        }
        pick = st.order.back();
        st.order.pop_back();
        break;
    case xact::VariationMode::Random:
    case xact::VariationMode::RandomNoImmediateRepeat:
    default: {
        // Weighted by each wave's weight range (all zero: even).
        u32 total = 0;
        for (const auto& w : ev.waves) total += static_cast<u32>(w.weight_max - std::min(w.weight_min, w.weight_max));
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (total == 0) {
                pick = any(rng_);
            } else {
                u32 roll = std::uniform_int_distribution<u32>(0, total - 1)(rng_);
                for (u32 i = 0; i < n; ++i) {
                    const u32 w = static_cast<u32>(ev.waves[i].weight_max -
                                                   std::min(ev.waves[i].weight_min, ev.waves[i].weight_max));
                    if (roll < w) {
                        pick = i;
                        break;
                    }
                    roll -= w;
                }
            }
            if (ev.variation != xact::VariationMode::RandomNoImmediateRepeat || pick != st.last) break;
        }
        break;
    }
    }
    st.last = pick;
    return pick;
}

int SoundManager::category_of(const CueInstance& inst) const {
    return inst.sound ? inst.sound->category : -1;
}

f32 SoundManager::category_gain(int category) const {
    const auto* gs = registry_ ? registry_->global_settings() : nullptr;
    if (!gs || category < 0 || static_cast<size_t>(category) >= gs->categories.size()) return 1.0f;
    f32 gain = 1.0f;
    int c = category;
    for (int depth = 0; depth < kMaxCategoryDepth && c >= 0 &&
                        static_cast<size_t>(c) < gs->categories.size();
         ++depth) {
        if (c == world_category_ && !world_enabled_) return 0.0f;
        gain *= xact::millibels_to_gain(gs->categories[static_cast<size_t>(c)].volume_mb) *
                user_volume_[static_cast<size_t>(c)];
        const u16 parent = gs->categories[static_cast<size_t>(c)].parent;
        c = parent == 0xFFFF ? -1 : parent;
    }
    return gain;
}

f32 SoundManager::distance(const CueInstance& inst) const {
    if (!inst.positional) return 0.0f;
    const f32 dx = inst.pos.x - listener_.x;
    const f32 dy = inst.pos.y - listener_.y;
    const f32 dz = inst.pos.z - listener_.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

f32 SoundManager::volume_mb(const CueInstance& inst, size_t track) const {
    const auto& t = inst.sound->tracks[track];
    f32 mb = inst.sound->volume_mb + t.volume_mb;
    const auto* gs = registry_->global_settings();
    if (!gs) return mb;
    auto variable = [&](u16 v) -> f32 {
        const int i = v;
        if (i == distance_variable_) return distance(inst);
        if (i == attack_variable_) return static_cast<f32>((clock_ - inst.started) * 1000.0);
        if (i == release_variable_)
            return inst.state == CueInstance::State::Releasing
                       ? static_cast<f32>((clock_ - inst.stop_started) * 1000.0)
                       : 0.0f;
        if (i == cue_instances_variable_) {
            f32 n = 0;
            for (const auto& [h, other] : instances_) n += other->cue == inst.cue ? 1.0f : 0.0f;
            return n;
        }
        if (v < gs->variables.size() && gs->variables[v].global()) return globals_[v];
        return v < gs->variables.size() ? gs->variables[v].initial : 0.0f;
    };
    auto add = [&](const std::vector<u32>& codes) {
        for (u32 code : codes) {
            const auto* rpc = gs->rpc(code);
            if (rpc && rpc->parameter == xact::RpcCurve::Parameter::Volume)
                mb += rpc->evaluate(variable(rpc->variable));
        }
    };
    add(inst.sound->rpc_codes);
    add(t.rpc_codes);
    return mb;
}

f32 SoundManager::pitch_cents(const CueInstance& inst, size_t /*track*/) const {
    return static_cast<f32>(inst.sound->pitch);
}

bool SoundManager::admit(const xact::SoundBank& /*sb*/, const xact::Cue& cue,
                         const xact::Sound& sound) {
    const auto* gs = registry_->global_settings();
    auto enforce = [&](u8 limit, xact::LimitBehavior behavior, auto&& same) {
        if (limit == 0xFF) return true;
        std::vector<CueInstance*> live;
        for (auto& [h, inst] : instances_)
            if (!inst->ended && inst->state == CueInstance::State::Playing && same(*inst))
                live.push_back(inst.get());
        if (live.size() < limit) return true;
        CueInstance* victim = nullptr;
        switch (behavior) {
        case xact::LimitBehavior::ReplaceOldest:
            for (auto* i : live)
                if (!victim || i->started < victim->started) victim = i;
            break;
        case xact::LimitBehavior::ReplaceQuietest:
            for (auto* i : live)
                if (!victim || i->gain < victim->gain) victim = i;
            break;
        case xact::LimitBehavior::ReplaceLowestPriority:
            // The smallest priority value is the lowest priority (as
            // FAudio's XACT reads it); it always gives way.
            for (auto* i : live)
                if (!victim || i->sound->priority < victim->sound->priority) victim = i;
            break;
        case xact::LimitBehavior::Fail:
        case xact::LimitBehavior::Queue: // no queue: a queued play is dropped
        default:
            return false;
        }
        if (!victim) return false;
        stop(victim->handle, false);
        return true;
    };
    if (!enforce(cue.instance_limit, cue.limit_behavior,
                 [&](const CueInstance& i) { return i.cue == &cue; }))
        return false;
    if (gs && sound.category < gs->categories.size()) {
        const auto& cat = gs->categories[sound.category];
        if (!enforce(cat.instance_limit, cat.limit_behavior,
                     [&](const CueInstance& i) { return i.sound->category == sound.category; }))
            return false;
    }
    return true;
}

SoundHandle SoundManager::play(const std::string& bank, const std::string& cue,
                               const sim::Vector3* pos, std::string_view lod_cutoff) {
    if (!registry_) return INVALID_SOUND;
    const xact::SoundBank* sb = registry_->sound_bank(bank);
    const xact::Cue* cue_def = sb ? sb->find_cue(cue) : nullptr;
    if (!cue_def) {
        spdlog::debug("Cue not found: {}/{}", bank, cue);
        return INVALID_SOUND;
    }
    const xact::Sound& sound = sb->sounds[cue_def->sound];

    auto inst = std::make_unique<CueInstance>();
    inst->bank = sb;
    inst->cue = cue_def;
    inst->sound = &sound;
    inst->bank_name = bank;
    inst->positional = pos != nullptr;
    if (pos) inst->pos = *pos;
    inst->started = clock_;

    // A LodCutoff variable culls a sound beyond its value (-1: never).
    if (pos && !lod_cutoff.empty()) {
        if (const auto* gs = registry_->global_settings()) {
            const int v = gs->find_variable(lod_cutoff);
            const f32 cutoff = v >= 0 ? globals_[static_cast<size_t>(v)] : -1.0f;
            if (cutoff >= 0 && distance(*inst) > cutoff) return INVALID_SOUND;
        }
    }
    if (!admit(*sb, *cue_def, sound)) return INVALID_SOUND;

    const auto* gs = registry_->global_settings();
    u16 fade_in_ms = cue_def->fade_in_ms;
    if (fade_in_ms == 0 && gs && sound.category < gs->categories.size())
        fade_in_ms = gs->categories[sound.category].fade_in_ms;
    inst->fade_in = fade_in_ms / 1000.0;

    inst->tracks.resize(sound.tracks.size());
    for (size_t t = 0; t < sound.tracks.size(); ++t) {
        for (const auto& ev : sound.tracks[t].plays) {
            f64 at = clock_ + ev.time_ms / 1000.0;
            if (ev.random_offset_ms > 0)
                at += std::uniform_int_distribution<u32>(0, ev.random_offset_ms)(rng_) / 1000.0;
            inst->tracks[t].fire_at.push_back(at);
        }
    }
    const SoundHandle handle = next_handle_++;
    if (next_handle_ == INVALID_SOUND) next_handle_ = 1;
    inst->handle = handle;
    CueInstance& ref = *inst;
    instances_.emplace(handle, std::move(inst));
    // Events at time 0 start now, so a one-shot is audible this frame.
    for (size_t t = 0; t < ref.tracks.size(); ++t) {
        auto& ts = ref.tracks[t];
        while (ts.next < ts.fire_at.size() && ts.fire_at[ts.next] <= clock_)
            start_event(ref, t, sound.tracks[t].plays[ts.next++]);
    }
    apply(ref);
    return handle;
}

SoundHandle SoundManager::play_loop(const std::string& bank, const std::string& cue,
                                    const sim::Vector3* pos) {
    const SoundHandle h = play(bank, cue, pos);
    if (auto it = instances_.find(h); it != instances_.end()) {
        it->second->force_loop = true;
        for (auto& ts : it->second->tracks)
            for (auto& v : ts.voices) {
                v->loops_left = kForever;
                if (v->sound_init) ma_sound_set_looping(&v->sound, MA_TRUE);
            }
    }
    return h;
}

void SoundManager::start_event(CueInstance& inst, size_t track, const xact::PlayEvent& ev) {
    if (ev.waves.empty()) return;
    const auto& choice = ev.waves[pick_wave(ev)];
    const auto wave = registry_->resolve(*inst.bank, choice);
    if (!wave) return;
    auto data = wave_data(*wave->bank, wave->index);
    if (!data) return;

    auto v = std::make_unique<Voice>();
    v->data = std::move(data);
    v->loops_left = (inst.force_loop || ev.loop_count == xact::PlayEvent::kLoopForever)
                        ? kForever
                        : ev.loop_count;
    if (ev.vary_pitch && ev.pitch_max > ev.pitch_min)
        v->variation_cents = static_cast<f32>(
            std::uniform_int_distribution<int>(ev.pitch_min, ev.pitch_max)(rng_));
    if (ev.vary_volume && ev.volume_max_mb > ev.volume_min_mb) {
        // Relative to unity: the event's range is in the same millibels.
        v->variation_mb = std::uniform_real_distribution<f32>(ev.volume_min_mb, ev.volume_max_mb)(rng_);
    }
    const f32 cents = pitch_cents(inst, track) + v->variation_cents;
    v->ends = clock_ + v->seconds_at(cents);

    if (output_) {
        ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, 0, 0);
        if (ma_decoder_init_memory(v->data->wav.data(), v->data->wav.size(), &dcfg, &v->decoder) ==
            MA_SUCCESS) {
            v->decoder_init = true;
            if (ma_sound_init_from_data_source(&engine_->engine, &v->decoder, 0, nullptr, &v->sound) ==
                MA_SUCCESS) {
                v->sound_init = true;
                ma_sound_set_spatialization_enabled(&v->sound, inst.positional ? MA_TRUE : MA_FALSE);
                // XACT's RPC curves do distance attenuation; miniaudio pans.
                ma_sound_set_attenuation_model(&v->sound, ma_attenuation_model_none);
                if (inst.positional) ma_sound_set_position(&v->sound, inst.pos.x, inst.pos.y, inst.pos.z);
                ma_sound_set_looping(&v->sound, v->loops_left == kForever ? MA_TRUE : MA_FALSE);
                ma_sound_set_pitch(&v->sound, static_cast<float>(std::pow(2.0, cents / 1200.0)));
                ma_sound_set_volume(&v->sound, 0.0f); // apply() sets it before the start
            }
        }
    }
    inst.tracks[track].voices.push_back(std::move(v));
    if (output_ && inst.tracks[track].voices.back()->sound_init) {
        apply(inst);
        ma_sound_start(&inst.tracks[track].voices.back()->sound);
    }
}

void SoundManager::apply(CueInstance& inst) {
    f32 fade = 1.0f;
    if (inst.fade_in > 0) fade = static_cast<f32>(std::min(1.0, (clock_ - inst.started) / inst.fade_in));
    if (inst.state == CueInstance::State::FadingOut && inst.stop_duration > 0)
        fade *= static_cast<f32>(std::max(0.0, 1.0 - (clock_ - inst.stop_started) / inst.stop_duration));
    const f32 cat = category_gain(category_of(inst));
    f32 loudest = 0;
    for (size_t t = 0; t < inst.tracks.size(); ++t) {
        const f32 base_mb = volume_mb(inst, t);
        for (auto& v : inst.tracks[t].voices) {
            const f32 gain = xact::millibels_to_gain(base_mb + v->variation_mb) * cat * fade;
            loudest = std::max(loudest, gain);
            if (!v->sound_init) continue;
            ma_sound_set_volume(&v->sound, gain);
            if (inst.positional) ma_sound_set_position(&v->sound, inst.pos.x, inst.pos.y, inst.pos.z);
        }
    }
    inst.gain = loudest;
}

void SoundManager::stop(SoundHandle handle, bool immediate) {
    auto it = instances_.find(handle);
    if (it == instances_.end()) return;
    CueInstance& inst = *it->second;
    if (inst.ended) return;
    if (!immediate && inst.state == CueInstance::State::Playing) {
        // A release curve (on ReleaseTime) fades the sound itself; else the
        // cue's or its category's fade-out.
        f64 release = 0;
        if (const auto* gs = registry_->global_settings()) {
            auto scan = [&](const std::vector<u32>& codes) {
                for (u32 code : codes) {
                    const auto* rpc = gs->rpc(code);
                    if (rpc && rpc->variable == release_variable_ && !rpc->points.empty())
                        release = std::max(release, static_cast<f64>(rpc->points.back().x) / 1000.0);
                }
            };
            scan(inst.sound->rpc_codes);
            for (const auto& t : inst.sound->tracks) scan(t.rpc_codes);
            if (release > 0) {
                inst.state = CueInstance::State::Releasing;
                inst.stop_started = clock_;
                inst.stop_duration = release;
                return;
            }
            u16 fade_ms = inst.cue->fade_out_ms;
            if (fade_ms == 0 && inst.sound->category < gs->categories.size())
                fade_ms = gs->categories[inst.sound->category].fade_out_ms;
            if (fade_ms > 0) {
                inst.state = CueInstance::State::FadingOut;
                inst.stop_started = clock_;
                inst.stop_duration = fade_ms / 1000.0;
                return;
            }
        }
    }
    end_instance(inst);
}

void SoundManager::end_instance(CueInstance& inst) {
    if (inst.ended) return;
    inst.ended = true;
    for (auto& ts : inst.tracks) ts.voices.clear(); // uninit, silence now
    // Retired (and its callbacks run) by the next update().
}

void SoundManager::stop_all() {
    for (auto& [h, inst] : instances_) end_instance(*inst);
    update(0.0f);
}

bool SoundManager::is_playing(SoundHandle handle) const {
    auto it = instances_.find(handle);
    return it != instances_.end() && !it->second->ended;
}

void SoundManager::on_finished(SoundHandle handle, std::function<void()> fn) {
    auto it = instances_.find(handle);
    if (it == instances_.end()) {
        fn(); // already over
        return;
    }
    it->second->on_finished.push_back(std::move(fn)); // runs when the sound is retired
}

void SoundManager::set_position(SoundHandle handle, const sim::Vector3& pos) {
    auto it = instances_.find(handle);
    if (it == instances_.end() || !it->second->positional) return;
    it->second->pos = pos;
}

bool SoundManager::position(SoundHandle handle, sim::Vector3& out) const {
    auto it = instances_.find(handle);
    if (it == instances_.end() || it->second->ended || !it->second->positional) return false;
    out = it->second->pos;
    return true;
}

void SoundManager::set_listener(const sim::Vector3& pos, const sim::Vector3& forward) {
    listener_ = pos;
    listener_forward_ = forward;
    if (!output_) return;
    ma_engine_listener_set_position(&engine_->engine, 0, pos.x, pos.y, pos.z);
    ma_engine_listener_set_direction(&engine_->engine, 0, forward.x, forward.y, forward.z);
}

void SoundManager::set_listener_position(const sim::Vector3& pos) {
    set_listener(pos, listener_forward_);
}

void SoundManager::set_global_variable(std::string_view name, f32 value) {
    const auto* gs = registry_ ? registry_->global_settings() : nullptr;
    if (!gs) return;
    const int v = gs->find_variable(name);
    if (v < 0 || !gs->variables[static_cast<size_t>(v)].global()) return;
    const auto& var = gs->variables[static_cast<size_t>(v)];
    globals_[static_cast<size_t>(v)] = std::clamp(value, var.min, var.max);
}

f32 SoundManager::global_variable(std::string_view name) const {
    const auto* gs = registry_ ? registry_->global_settings() : nullptr;
    const int v = gs ? gs->find_variable(name) : -1;
    return v >= 0 ? globals_[static_cast<size_t>(v)] : 0.0f;
}

void SoundManager::set_category_volume(std::string_view category, f32 volume) {
    const auto* gs = registry_ ? registry_->global_settings() : nullptr;
    const int c = gs ? gs->find_category(category) : -1;
    if (c >= 0) user_volume_[static_cast<size_t>(c)] = std::clamp(volume, 0.0f, 1.0f);
}

f32 SoundManager::category_volume(std::string_view category) const {
    const auto* gs = registry_ ? registry_->global_settings() : nullptr;
    const int c = gs ? gs->find_category(category) : -1;
    return c >= 0 ? user_volume_[static_cast<size_t>(c)] : 1.0f;
}

void SoundManager::set_world_enabled(bool enabled) {
    world_enabled_ = enabled;
}

f32 SoundManager::current_gain(SoundHandle handle) const {
    auto it = instances_.find(handle);
    return it == instances_.end() || it->second->ended ? 0.0f : it->second->gain;
}

bool SoundManager::is_cue_playing(std::string_view bank, std::string_view cue) const {
    for (const auto& [h, inst] : instances_)
        if (!inst->ended && iequals(inst->bank_name, bank) && inst->cue->name == cue) return true;
    return false;
}

void SoundManager::update(f32 dt) {
    clock_ += std::max(0.0f, dt);
    for (auto& [h, ptr] : instances_) {
        CueInstance& inst = *ptr;
        if (inst.ended) continue;
        bool pending = false;
        for (size_t t = 0; t < inst.tracks.size(); ++t) {
            auto& ts = inst.tracks[t];
            while (ts.next < ts.fire_at.size() && ts.fire_at[ts.next] <= clock_ &&
                   inst.state == CueInstance::State::Playing)
                start_event(inst, t, inst.sound->tracks[t].plays[ts.next++]);
            if (ts.next < ts.fire_at.size() && inst.state == CueInstance::State::Playing) pending = true;
            for (auto& v : ts.voices) {
                bool at_end;
                if (v->sound_init) {
                    at_end = v->loops_left != kForever && ma_sound_at_end(&v->sound);
                } else {
                    at_end = v->loops_left != kForever && clock_ >= v->ends;
                }
                if (!at_end) continue;
                if (v->loops_left > 0) {
                    --v->loops_left;
                    v->ends += v->seconds_at(pitch_cents(inst, t) + v->variation_cents);
                    if (v->sound_init) {
                        ma_sound_seek_to_pcm_frame(&v->sound, 0);
                        ma_sound_start(&v->sound);
                    }
                } else {
                    v->done = true;
                }
            }
            std::erase_if(ts.voices, [](const std::unique_ptr<Voice>& v) { return v->done; });
            if (!ts.voices.empty()) pending = true;
        }
        const bool stop_over = inst.state != CueInstance::State::Playing &&
                               clock_ - inst.stop_started >= inst.stop_duration;
        if (!pending || stop_over) {
            end_instance(inst);
            continue;
        }
        apply(inst);
    }
    // Retire ended sounds, then run their callbacks (which may play more).
    std::vector<std::function<void()>> callbacks;
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (it->second->ended) {
            for (auto& fn : it->second->on_finished) callbacks.push_back(std::move(fn));
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& fn : callbacks) fn();
}

} // namespace osc::audio
