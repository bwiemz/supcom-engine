#define NOMINMAX
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "audio/sound_manager.hpp"
#include "audio/xact/bank_registry.hpp"
#include "audio/wave_stream.hpp"
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
constexpr u32 kStreamBytes = 2u << 20;         ///< PCM waves this long stream from their bank
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
    // A long PCM wave streams from its bank instead (retail's music and
    // movie voices run 34-43 MB): its WAV header, and where its samples sit.
    std::vector<u8> header;
    fs::path file;
    u64 offset = 0, length = 0;
    bool streamed() const { return length > 0; }
};

/// miniaudio's decoder callbacks over a WaveStream (a streamed wave).
static ma_result stream_read(ma_decoder* decoder, void* out, size_t bytes, size_t* got) {
    auto& stream = *static_cast<WaveStream*>(decoder->pUserData);
    if (got) *got = 0;
    if (stream.tell() >= stream.size()) return MA_AT_END;
    const size_t n = stream.read(out, bytes);
    if (got) *got = n;
    return n == 0 ? MA_AT_END : MA_SUCCESS;
}

static ma_result stream_seek(ma_decoder* decoder, ma_int64 offset, ma_seek_origin origin) {
    auto& stream = *static_cast<WaveStream*>(decoder->pUserData);
    stream.seek(offset, origin == ma_seek_origin_start     ? WaveStream::Origin::Start
                        : origin == ma_seek_origin_current ? WaveStream::Origin::Current
                                                           : WaveStream::Origin::End);
    return MA_SUCCESS;
}

/// One wave playing on a track. Not movable (miniaudio keeps pointers into
/// it), so voices live on the heap.
struct SoundManager::Voice {
    std::shared_ptr<const WaveData> data;
    std::unique_ptr<WaveStream> stream; ///< a streamed wave's reader (outlives the decoder)
    ma_decoder decoder{};
    ma_sound sound{};
    bool decoder_init = false;
    bool sound_init = false;
    u32 loops_left = 0; ///< plays after this one (kForever: until stopped)
    /// Its event picks a new wave for each loop: it doesn't loop itself,
    /// a fresh voice takes over.
    const xact::PlayEvent* repick = nullptr;
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
    bool paused = false;             ///< held by a paused category
    f32 pan_left = 1, pan_right = 1; ///< a positional sound's stereo gains
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
    /// Prepared, not started (XACT's preload-only play): silent and
    /// waiting, until start() begins it.
    bool prepared = false;
};

// ---- WAV header synthesis ----


namespace {

using xact::child_any_case;

fs::path voice_dir(const fs::path& sounds_dir, std::string_view la) {
    if (la.empty()) return {};
    const fs::path voice = child_any_case(sounds_dir, "voice");
    return voice.empty() ? fs::path{} : child_any_case(voice, la);
}

} // namespace

// ---- SoundManager implementation ----

bool SoundManager::has_voice_language(std::string_view la) const {
    std::error_code ec;
    const fs::path dir = voice_dir(sounds_dir_, la);
    return !dir.empty() && fs::is_directory(dir, ec);
}

bool SoundManager::set_voice_language(std::string_view la) {
    std::string wanted(la);
    std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!registry_ || !has_voice_language(wanted)) return false;
    if (!voice_language_.empty()) {
        if (voice_language_ != wanted)
            spdlog::warn("Audio: voice language stays '{}' (asked for '{}')", voice_language_,
                         wanted);
        return voice_language_ == wanted;
    }
    const fs::path dir = voice_dir(sounds_dir_, wanted);
    registry_->add_directory(dir);
    if (const fs::path tutorials = child_any_case(dir, "tutorials"); !tutorials.empty())
        registry_->add_directory(tutorials);
    voice_language_ = std::move(wanted);
    return true;
}

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
        duck_variable_ = gs->find_variable("Duck");
        angle_variable_ = gs->find_variable("Angle");
        camera_distance_variable_ = gs->find_variable("CameraDistance");
        duck_length_variable_ = gs->find_variable("DuckLength");
        paused_.assign(gs->categories.size(), 0);
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
    if (info.format_tag == 0 && info.data_length >= kStreamBytes) {
        // Streamed: nothing is read now (a headless run never reads it).
        auto data = std::make_shared<WaveData>();
        const u32 frame = std::max<u32>(1, info.channels * (info.bits_per_sample / 8));
        const u64 frames = info.data_length / frame; // whole frames
        data->seconds = info.sample_rate ? static_cast<f64>(frames) / info.sample_rate : 0.0;
        data->header = wav_header(info, info.data_length);
        data->file = bank.path();
        data->offset = info.data_offset;
        data->length = info.data_length;
        waves_.emplace(key, data);
        wave_lru_.push_front(key);
        return data;
    }
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
        // Weighted by each wave's weight range (all zero: even). Without an
        // immediate repeat the last pick is left out, as FAudio excludes it.
        const bool exclude =
            ev.variation == xact::VariationMode::RandomNoImmediateRepeat && st.last < n;
        const auto weight = [&](u32 i) -> u32 {
            if (exclude && i == st.last) return 0;
            const auto& w = ev.waves[i];
            return static_cast<u32>(w.weight_max - std::min(w.weight_min, w.weight_max));
        };
        u32 total = 0;
        for (u32 i = 0; i < n; ++i) total += weight(i);
        if (total == 0) {
            pick = std::uniform_int_distribution<u32>(0, exclude ? n - 2 : n - 1)(rng_);
            if (exclude && pick >= st.last) ++pick;
        } else {
            u32 roll = std::uniform_int_distribution<u32>(0, total - 1)(rng_);
            for (u32 i = 0; i < n; ++i) {
                if (roll < weight(i)) {
                    pick = i;
                    break;
                }
                roll -= weight(i);
            }
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
        if (i == angle_variable_ && inst.positional) return cue_angle_degrees(inst.pos, listener_);
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
                         const xact::Sound& sound, f64& fade_in) {
    fade_in = 0;
    const auto* gs = registry_->global_settings();
    // A replacement fades the old sound out, and the new one in, over the
    // limit's own fades -- the cue's or the category's, whichever limited
    // (XACT's instance-limit crossfade; FAudio handle_instance_limit).
    auto enforce = [&](u8 limit, xact::LimitBehavior behavior, u16 fade_in_ms, u16 fade_out_ms,
                       auto&& same) {
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
        fade_out(*victim, fade_out_ms / 1000.0);
        fade_in = std::max(fade_in, fade_in_ms / 1000.0);
        return true;
    };
    if (!enforce(cue.instance_limit, cue.limit_behavior, cue.fade_in_ms, cue.fade_out_ms,
                 [&](const CueInstance& i) { return i.cue == &cue; }))
        return false;
    if (gs && sound.category < gs->categories.size()) {
        const auto& cat = gs->categories[sound.category];
        if (!enforce(cat.instance_limit, cat.limit_behavior, cat.fade_in_ms, cat.fade_out_ms,
                     [&](const CueInstance& i) { return i.sound->category == sound.category; }))
            return false;
    }
    return true;
}

void SoundManager::fade_out(CueInstance& inst, f64 seconds) {
    // A paused sound is silent and its clock held: nothing to fade.
    if (seconds <= 0 || inst.prepared || inst.paused) {
        end_instance(inst);
        return;
    }
    inst.state = CueInstance::State::FadingOut;
    inst.stop_started = clock_;
    inst.stop_duration = seconds;
}

SoundManager::CueInstance* SoundManager::create(const std::string& bank, const std::string& cue,
                                                const sim::Vector3* pos) {
    if (!registry_) return nullptr;
    const xact::SoundBank* sb = registry_->sound_bank(bank);
    const xact::Cue* cue_def = sb ? sb->find_cue(cue) : nullptr;
    if (!cue_def) {
        spdlog::debug("Cue not found: {}/{}", bank, cue);
        return nullptr;
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

    // Only a replacement fades in: a play otherwise starts at full volume
    // (retail's Ambient category's 1 s fade-in is for its crossfades).
    if (!admit(*sb, *cue_def, sound, inst->fade_in)) return nullptr;
    inst->tracks.resize(sound.tracks.size());

    const SoundHandle handle = next_handle_++;
    if (next_handle_ == INVALID_SOUND) next_handle_ = 1;
    inst->handle = handle;
    CueInstance& ref = *inst;
    instances_.emplace(handle, std::move(inst));
    return &ref;
}

void SoundManager::begin(CueInstance& inst) {
    inst.prepared = false;
    inst.started = clock_;
    inst.paused = category_paused(category_of(inst));
    const xact::Sound& sound = *inst.sound;
    for (size_t t = 0; t < sound.tracks.size(); ++t) {
        for (const auto& ev : sound.tracks[t].plays) {
            f64 at = clock_ + ev.time_ms / 1000.0;
            if (ev.random_offset_ms > 0)
                at += std::uniform_int_distribution<u32>(0, ev.random_offset_ms)(rng_) / 1000.0;
            inst.tracks[t].fire_at.push_back(at);
        }
    }
    // Events at time 0 start now, so a one-shot is audible this frame.
    for (size_t t = 0; t < inst.tracks.size(); ++t) {
        auto& ts = inst.tracks[t];
        while (ts.next < ts.fire_at.size() && ts.fire_at[ts.next] <= clock_)
            start_event(inst, t, sound.tracks[t].plays[ts.next++]);
    }
    apply(inst);
}

SoundHandle SoundManager::play(const std::string& bank, const std::string& cue,
                               const sim::Vector3* pos) {
    CueInstance* inst = create(bank, cue, pos);
    if (!inst) return INVALID_SOUND;
    begin(*inst);
    return inst->handle;
}

SoundHandle SoundManager::prepare(const std::string& bank, const std::string& cue) {
    CueInstance* inst = create(bank, cue, nullptr);
    if (!inst) return INVALID_SOUND;
    inst->prepared = true;
    return inst->handle;
}

void SoundManager::start(SoundHandle handle) {
    auto it = instances_.find(handle);
    if (it == instances_.end() || it->second->ended || !it->second->prepared) return;
    begin(*it->second);
}

SoundManager::Voice* SoundManager::start_event(CueInstance& inst, size_t track,
                                               const xact::PlayEvent& ev) {
    if (ev.waves.empty()) return nullptr;
    const auto& choice = ev.waves[pick_wave(ev)];
    const auto wave = registry_->resolve(*inst.bank, choice);
    if (!wave) return nullptr;
    auto data = wave_data(*wave->bank, wave->index);
    if (!data) return nullptr;

    auto v = std::make_unique<Voice>();
    v->data = std::move(data);
    v->loops_left = ev.loop_count == xact::PlayEvent::kLoopForever ? kForever : ev.loop_count;
    if (ev.new_variation_on_loop && ev.waves.size() > 1 && v->loops_left > 0) v->repick = &ev;
    // A range of one value is a fixed offset: retail's pitched UI stacks
    // (UI_Menu_Rollover's tracks at +800 and +1200 cents) are authored so.
    if (ev.vary_pitch && ev.pitch_max >= ev.pitch_min)
        v->variation_cents = static_cast<f32>(
            std::uniform_int_distribution<int>(ev.pitch_min, ev.pitch_max)(rng_));
    if (ev.vary_volume && ev.volume_max_mb >= ev.volume_min_mb) {
        // Relative to unity: the event's range is in the same millibels.
        v->variation_mb =
            ev.volume_max_mb > ev.volume_min_mb
                ? std::uniform_real_distribution<f32>(ev.volume_min_mb, ev.volume_max_mb)(rng_)
                : ev.volume_min_mb;
    }
    const f32 cents = pitch_cents(inst, track) + v->variation_cents;
    v->ends = clock_ + v->seconds_at(cents);

    if (output_) {
        ma_decoder_config dcfg = ma_decoder_config_init(ma_format_f32, 0, 0);
        ma_result opened = MA_ERROR;
        if (v->data->streamed()) {
            v->stream = std::make_unique<WaveStream>(v->data->header, v->data->file,
                                                     v->data->offset, v->data->length);
            if (v->stream->ok())
                opened = ma_decoder_init(&stream_read, &stream_seek, v->stream.get(), &dcfg,
                                         &v->decoder);
        } else {
            opened = ma_decoder_init_memory(v->data->wav.data(), v->data->wav.size(), &dcfg,
                                            &v->decoder);
        }
        if (opened == MA_SUCCESS) {
            v->decoder_init = true;
            if (ma_sound_init_from_data_source(&engine_->engine, &v->decoder, 0, nullptr, &v->sound) ==
                MA_SUCCESS) {
                v->sound_init = true;
                // XACT's RPC curves attenuate and apply() pans, as X3DAudio's
                // matrix does; miniaudio's spatializer is off (its model
                // without attenuation doesn't pan either).
                ma_sound_set_spatialization_enabled(&v->sound, MA_FALSE);
                ma_sound_set_pan_mode(&v->sound, ma_pan_mode_balance);
                ma_sound_set_looping(&v->sound,
                                     v->loops_left == kForever && !v->repick ? MA_TRUE : MA_FALSE);
                ma_sound_set_pitch(&v->sound, static_cast<float>(std::pow(2.0, cents / 1200.0)));
                ma_sound_set_volume(&v->sound, 0.0f); // apply() sets it before the start
            }
        }
    }
    Voice* started = v.get();
    inst.tracks[track].voices.push_back(std::move(v));
    if (output_ && started->sound_init && !inst.paused) {
        apply(inst);
        ma_sound_start(&started->sound);
    }
    return started;
}

void SoundManager::apply(CueInstance& inst) {
    f32 fade = 1.0f;
    if (inst.fade_in > 0) fade = static_cast<f32>(std::min(1.0, (clock_ - inst.started) / inst.fade_in));
    if (inst.state == CueInstance::State::FadingOut && inst.stop_duration > 0)
        fade *= static_cast<f32>(std::max(0.0, 1.0 - (clock_ - inst.stop_started) / inst.stop_duration));
    const f32 cat = category_gain(category_of(inst));
    // A positional sound's stereo matrix, as miniaudio's balance pan (which
    // scales one side) times the louder side's gain. A 2D mono sound plays
    // at [1, 1], XAudio2's default.
    f32 pan = 0;
    f32 side = 1;
    if (inst.positional) {
        const auto [l, r] = stereo_gains(
            {inst.pos.x - listener_.x, inst.pos.y - listener_.y, inst.pos.z - listener_.z},
            listener_forward_, listener_right_);
        inst.pan_left = l;
        inst.pan_right = r;
        side = std::max(l, r);
        pan = side <= 0 ? 0.0f : r >= l ? 1.0f - l / r : r / l - 1.0f;
    }
    f32 loudest = 0;
    for (size_t t = 0; t < inst.tracks.size(); ++t) {
        const f32 base_mb = volume_mb(inst, t);
        for (auto& v : inst.tracks[t].voices) {
            const f32 gain = xact::millibels_to_gain(base_mb + v->variation_mb) * cat * fade * side;
            loudest = std::max(loudest, gain);
            if (!v->sound_init) continue;
            ma_sound_set_volume(&v->sound, gain);
            ma_sound_set_pan(&v->sound, pan);
        }
    }
    inst.gain = loudest;
}

void SoundManager::stop(SoundHandle handle, bool immediate) {
    auto it = instances_.find(handle);
    if (it == instances_.end()) return;
    CueInstance& inst = *it->second;
    if (inst.ended) return;
    // A paused sound is silent and its clock held: it ends now.
    if (!immediate && !inst.prepared && !inst.paused && inst.state == CueInstance::State::Playing) {
        // A release curve (on ReleaseTime) fades the sound itself; else the
        // cue's fade-out. Which comes first for a cue with both (retail's
        // Music: 200 ms fade, 6 s release) is open (M216b design doc); the
        // release keeps UserMusic's peace transition as retail times it.
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
            if (inst.cue->fade_out_ms > 0) {
                fade_out(inst, inst.cue->fade_out_ms / 1000.0);
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
    for (auto& [h, inst] : instances_) stop(h, false);
    reset_duck();
    update(0.0f);
}

bool SoundManager::category_paused(int category) const {
    const auto* gs = registry_ ? registry_->global_settings() : nullptr;
    for (int depth = 0; gs && depth < kMaxCategoryDepth && category >= 0 &&
                        static_cast<size_t>(category) < gs->categories.size();
         ++depth) {
        if (paused_[static_cast<size_t>(category)] != 0) return true;
        const u16 parent = gs->categories[static_cast<size_t>(category)].parent;
        category = parent == 0xFFFF ? -1 : parent;
    }
    return false;
}

void SoundManager::pause_category(std::string_view category, bool paused) {
    const auto* gs = registry_ ? registry_->global_settings() : nullptr;
    const int c = gs ? gs->find_category(category) : -1;
    if (c < 0) return;
    paused_[static_cast<size_t>(c)] = paused ? 1 : 0;
    for (auto& [h, inst] : instances_) {
        if (inst->ended || inst->prepared) continue;
        const bool now = category_paused(category_of(*inst));
        if (now == inst->paused) continue;
        inst->paused = now;
        set_voices_running(*inst, !now);
    }
}

void SoundManager::resume_all() {
    std::fill(paused_.begin(), paused_.end(), u8{0});
    for (auto& [h, inst] : instances_) {
        if (!inst->paused) continue;
        inst->paused = false;
        if (!inst->ended && !inst->prepared) set_voices_running(*inst, true);
    }
}

bool SoundManager::is_paused(SoundHandle handle) const {
    auto it = instances_.find(handle);
    return it != instances_.end() && !it->second->ended && it->second->paused;
}

void SoundManager::set_voices_running(CueInstance& inst, bool running) {
    if (!output_) return;
    if (running) apply(inst);
    for (auto& ts : inst.tracks)
        for (auto& v : ts.voices) {
            if (!v->sound_init) continue;
            // Stopped, a miniaudio sound keeps its cursor: it resumes there.
            if (running) {
                if (!ma_sound_at_end(&v->sound)) ma_sound_start(&v->sound);
            } else {
                ma_sound_stop(&v->sound);
            }
        }
}

void SoundManager::push_duck() {
    if (duck_count_++ == 0 && duck_length_variable_ >= 0) {
        duck_elapsed_ = 0;
        duck_mode_ = DuckMode::Up;
    }
}

void SoundManager::pop_duck() {
    if (duck_count_ == 0) return;
    if (--duck_count_ == 0 && duck_length_variable_ >= 0) {
        duck_elapsed_ = 0;
        duck_mode_ = DuckMode::Down;
    }
}

void SoundManager::reset_duck() {
    duck_count_ = 0;
    duck_mode_ = DuckMode::None;
    ++duck_generation_;
    if (duck_variable_ >= 0) globals_[static_cast<size_t>(duck_variable_)] = 0.0f;
}

void SoundManager::update_duck(f32 dt) {
    if (duck_mode_ == DuckMode::None || duck_variable_ < 0 || duck_length_variable_ < 0) return;
    const f32 length = globals_[static_cast<size_t>(duck_length_variable_)];
    if (length <= 0) {
        globals_[static_cast<size_t>(duck_variable_)] = duck_mode_ == DuckMode::Up ? 1.0f : 0.0f;
        duck_mode_ = DuckMode::None;
        return;
    }
    duck_elapsed_ = std::min(duck_elapsed_ + dt, length);
    // Moho's UpdateDuck: down runs from 1 whatever the way up had reached.
    const f32 t = duck_elapsed_ / length;
    globals_[static_cast<size_t>(duck_variable_)] = duck_mode_ == DuckMode::Up ? t : 1.0f - t;
    if (duck_elapsed_ >= length) duck_mode_ = DuckMode::None;
}

bool SoundManager::is_playing(SoundHandle handle) const {
    auto it = instances_.find(handle);
    return it != instances_.end() && !it->second->ended && !it->second->prepared;
}

bool SoundManager::is_prepared(SoundHandle handle) const {
    auto it = instances_.find(handle);
    return it != instances_.end() && !it->second->ended && it->second->prepared;
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

void SoundManager::set_listener(const sim::Vector3& pos, const sim::Vector3& forward,
                                const sim::Vector3& right) {
    listener_ = pos;
    listener_forward_ = forward;
    if (right.x != 0 || right.y != 0 || right.z != 0) {
        listener_right_ = right;
    } else { // forward x up
        listener_right_ = {-forward.z, 0.0f, forward.x};
    }
}

std::array<f32, 2> SoundManager::stereo_gains(const sim::Vector3& dir, const sim::Vector3& forward,
                                              const sim::Vector3& right) {
    const f32 x = dir.x * right.x + dir.y * right.y + dir.z * right.z;
    const f32 z = dir.x * forward.x + dir.y * forward.y + dir.z * forward.z;
    if (std::abs(x) < 1e-6f && std::abs(z) < 1e-6f) return {0.5f, 0.5f};
    // Azimuth from straight ahead, right positive; right's share runs
    // linearly from 0 at -90 degrees to 1 at +90, and back behind.
    constexpr f32 kPi = 3.14159265358979f;
    f32 az = std::atan2(x, z); // [-pi, pi]
    f32 r = 0;
    if (az >= -kPi / 2 && az <= kPi / 2) {
        r = (az + kPi / 2) / kPi;
    } else {
        if (az < 0) az += 2 * kPi; // (pi/2, 3pi/2)
        r = (1.5f * kPi - az) / kPi;
    }
    r = std::clamp(r, 0.0f, 1.0f);
    return {1.0f - r, r};
}

f32 SoundManager::cue_angle_degrees(const sim::Vector3& emitter, const sim::Vector3& listener) {
    const f32 dx = emitter.x - listener.x;
    const f32 dy = emitter.y - listener.y;
    const f32 dz = emitter.z - listener.z;
    // faf-re's ComputeCueAngleDegrees is 90 - pitch, its pitch the
    // vertical over the horizontal; Moho's world is Y-up.
    const f32 pitch = std::atan2(dy, std::sqrt(dx * dx + dz * dz));
    return 90.0f - pitch * (180.0f / 3.14159265358979f);
}

bool SoundManager::stereo(SoundHandle handle, f32& left, f32& right) const {
    auto it = instances_.find(handle);
    if (it == instances_.end() || it->second->ended || !it->second->positional) return false;
    left = it->second->pan_left;
    right = it->second->pan_right;
    return true;
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
    reset_duck(); // Moho's SetVolume drops the duck
}

f32 SoundManager::category_volume(std::string_view category) const {
    const auto* gs = registry_ ? registry_->global_settings() : nullptr;
    const int c = gs ? gs->find_category(category) : -1;
    return c >= 0 ? user_volume_[static_cast<size_t>(c)] : 1.0f;
}

void SoundManager::set_world_enabled(bool enabled) {
    world_enabled_ = enabled;
}

f32 SoundManager::camera_distance() const {
    return camera_distance_variable_ >= 0 ? globals_[static_cast<size_t>(camera_distance_variable_)]
                                          : 0.0f;
}

SoundManager::Filter SoundManager::filter(std::string_view lod_cutoff, const sim::Vector3& pos,
                                          bool underwater, const Hearing& hears) const {
    if (!lod_cutoff.empty()) {
        const auto* gs = registry_->global_settings();
        const int v = gs ? gs->find_variable(lod_cutoff) : -1;
        if (v >= 0) {
            const f32 cutoff = globals_[static_cast<size_t>(v)];
            if (cutoff > -1.0f && camera_distance() > cutoff) return Filter::Distance;
        }
    }
    if (hears && !hears(pos, underwater)) return Filter::Hearing;
    return Filter::Pass;
}

SoundHandle SoundManager::play_world(const WorldSound& sound, const Hearing& hears) {
    if (!registry_ || !world_enabled_) return INVALID_SOUND;
    if (filter(sound.lod_cutoff, sound.pos, sound.underwater, hears) != Filter::Pass)
        return INVALID_SOUND;
    // One of each cue a beat (Moho's recent one-shot keys).
    if (sound.beat != dedupe_beat_) {
        dedupe_beat_ = sound.beat;
        dedupe_.clear();
    }
    for (const auto& [bank, cue] : dedupe_)
        if (iequals(bank, sound.bank) && cue == sound.cue) return INVALID_SOUND;
    dedupe_.emplace_back(sound.bank, sound.cue);
    return play(sound.bank, sound.cue, &sound.pos);
}

void SoundManager::sync_entity_loops(const std::vector<EntityLoop>& wanted, const Hearing& hears) {
    if (!registry_ || !world_enabled_) return;
    // The one kept for a key: its first entry.
    std::map<u64, const EntityLoop*> by_key;
    for (const auto& w : wanted) by_key.emplace(w.key, &w);
    for (auto it = entity_loops_.begin(); it != entity_loops_.end();) {
        const auto want = by_key.find(it->first);
        const bool same = want != by_key.end() && iequals(want->second->bank, it->second.bank) &&
                          want->second->cue == it->second.cue;
        if (!same) {
            stop(it->second.handle, false); // Moho's StopLoop: the release
            it = entity_loops_.erase(it);
            continue;
        }
        if (!is_playing(it->second.handle)) { // its cue ended: forgotten
            it = entity_loops_.erase(it);
            continue;
        }
        const EntityLoop& w = *want->second;
        switch (filter(w.lod_cutoff, w.pos, w.underwater, hears)) {
        case Filter::Distance:
            stop(it->second.handle, true);
            it = entity_loops_.erase(it);
            continue;
        case Filter::Hearing:
            stop(it->second.handle, false);
            it = entity_loops_.erase(it);
            continue;
        case Filter::Pass: break;
        }
        set_position(it->second.handle, w.pos);
        ++it;
    }
    // New loops start only near and in view (Moho's frustum pass, its 200
    // CameraDistance cutoff).
    constexpr f32 kLoopStartCameraDistance = 200.0f;
    if (camera_distance() > kLoopStartCameraDistance) return;
    for (const auto& [key, w] : by_key) {
        if (!w->in_view || entity_loops_.count(key) != 0) continue;
        if (filter(w->lod_cutoff, w->pos, w->underwater, hears) != Filter::Pass) continue;
        const SoundHandle h = play(w->bank, w->cue, &w->pos);
        if (h != INVALID_SOUND) entity_loops_.emplace(key, PlayingLoop{w->bank, w->cue, h});
    }
}

SoundHandle SoundManager::entity_loop(u64 key) const {
    const auto it = entity_loops_.find(key);
    return it != entity_loops_.end() && is_playing(it->second.handle) ? it->second.handle
                                                                      : INVALID_SOUND;
}

f32 SoundManager::current_gain(SoundHandle handle) const {
    auto it = instances_.find(handle);
    return it == instances_.end() || it->second->ended ? 0.0f : it->second->gain;
}

bool SoundManager::is_cue_playing(std::string_view bank, std::string_view cue) const {
    for (const auto& [h, inst] : instances_)
        if (!inst->ended && !inst->prepared && iequals(inst->bank_name, bank) &&
            inst->cue->name == cue)
            return true;
    return false;
}

void SoundManager::update(f32 dt) {
    dt = std::max(0.0f, dt);
    clock_ += dt;
    update_duck(dt);
    for (auto& [h, ptr] : instances_) {
        CueInstance& inst = *ptr;
        if (inst.ended || inst.prepared) continue;
        if (inst.paused) {
            // Held: its clocks wait with it, so nothing fires, ends or fades.
            inst.started += dt;
            inst.stop_started += dt;
            for (auto& ts : inst.tracks) {
                for (size_t i = ts.next; i < ts.fire_at.size(); ++i) ts.fire_at[i] += dt;
                for (auto& v : ts.voices) v->ends += dt;
            }
            continue;
        }
        bool pending = false;
        for (size_t t = 0; t < inst.tracks.size(); ++t) {
            auto& ts = inst.tracks[t];
            while (ts.next < ts.fire_at.size() && ts.fire_at[ts.next] <= clock_ &&
                   inst.state == CueInstance::State::Playing)
                start_event(inst, t, inst.sound->tracks[t].plays[ts.next++]);
            if (ts.next < ts.fire_at.size() && inst.state == CueInstance::State::Playing) pending = true;
            // Loops that pick a new wave: (event, plays left after the next).
            std::vector<std::pair<const xact::PlayEvent*, u32>> repicks;
            for (auto& v : ts.voices) {
                const bool counts_end = v->loops_left != kForever || v->repick;
                bool at_end;
                if (v->sound_init) {
                    at_end = counts_end && ma_sound_at_end(&v->sound);
                } else {
                    at_end = counts_end && clock_ >= v->ends;
                }
                if (!at_end) continue;
                if (v->repick && v->loops_left > 0) {
                    repicks.emplace_back(v->repick,
                                         v->loops_left == kForever ? kForever : v->loops_left - 1);
                    v->done = true;
                } else if (v->loops_left > 0) {
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
            // A loop goes on through a fade or release, as a native one does;
            // the stop's end ends it.
            for (const auto& [ev, left] : repicks)
                if (Voice* next = start_event(inst, t, *ev)) {
                    next->loops_left = left;
                    if (left == 0) next->repick = nullptr;
                }
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
