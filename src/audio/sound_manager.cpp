#define NOMINMAX
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include "audio/sound_manager.hpp"
#include "audio/xwb_parser.hpp"
#include "audio/xsb_parser.hpp"

#include <spdlog/spdlog.h>

#include <cstring>

namespace osc::audio {

// ---- Pimpl structs (keep miniaudio.h out of header) ----

struct SoundManager::AudioEngine {
    ma_engine engine;
    bool initialized = false;
};

struct SoundManager::ActiveSound {
    ma_decoder decoder;
    ma_sound sound;
    std::vector<u8> wav_data; // must outlive decoder
    bool decoder_initialized = false;
    bool sound_initialized = false;
    bool looping = false;
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

SoundManager::SoundManager(const fs::path& sounds_dir)
    : sounds_dir_(sounds_dir)
    , engine_(std::make_unique<AudioEngine>())
{
    if (!fs::exists(sounds_dir_)) {
        spdlog::warn("Sound directory not found: {} — running headless",
                     sounds_dir_.string());
        headless_ = true;
        return;
    }

    ma_engine_config cfg = ma_engine_config_init();
    cfg.listenerCount = 1;

    ma_result result = ma_engine_init(&cfg, &engine_->engine);
    if (result != MA_SUCCESS) {
        spdlog::warn("Failed to initialize audio engine (error {}) — running headless",
                     static_cast<int>(result));
        headless_ = true;
        return;
    }
    engine_->initialized = true;
    spdlog::info("Audio engine initialized");
}

SoundManager::~SoundManager() {
    // Stop and clean up all active sounds
    for (auto& [handle, snd] : active_sounds_) {
        if (snd->sound_initialized) {
            ma_sound_uninit(&snd->sound);
        }
        if (snd->decoder_initialized) {
            ma_decoder_uninit(&snd->decoder);
        }
    }
    active_sounds_.clear();

    if (engine_ && engine_->initialized) {
        ma_engine_uninit(&engine_->engine);
    }
}

SoundManager::BankPair* SoundManager::ensure_bank(const std::string& bank_name) {
    auto it = banks_.find(bank_name);
    if (it != banks_.end()) {
        return it->second.get(); // may be nullptr (cached miss)
    }

    auto xwb_path = sounds_dir_ / (bank_name + ".xwb");
    auto xsb_path = sounds_dir_ / (bank_name + ".xsb");

    if (!fs::exists(xwb_path)) {
        spdlog::debug("Wave bank not found: {}", xwb_path.string());
        banks_[bank_name] = nullptr;
        return nullptr;
    }

    auto pair = std::make_unique<BankPair>();
    pair->xwb = std::make_unique<XwbParser>();
    pair->xsb = std::make_unique<XsbParser>();

    auto xwb_result = pair->xwb->parse(xwb_path);
    if (!xwb_result) {
        spdlog::warn("Failed to parse wave bank {}: {}",
                     bank_name, xwb_result.error().message);
        banks_[bank_name] = nullptr;
        return nullptr;
    }

    // XSB is optional — some banks might not have cue names
    if (fs::exists(xsb_path)) {
        auto xsb_result = pair->xsb->parse(xsb_path);
        if (!xsb_result) {
            spdlog::warn("Failed to parse sound bank {}: {}",
                         bank_name, xsb_result.error().message);
            // Continue without XSB — can still play by index
        }
    }

    spdlog::info("Loaded audio bank: {} ({} waves, {} cues)",
                 bank_name, pair->xwb->entry_count(), pair->xsb->cue_count());

    auto* ptr = pair.get();
    banks_[bank_name] = std::move(pair);
    return ptr;
}

std::vector<u8> SoundManager::wrap_as_wav(const WaveInfo& info,
                                           const std::vector<u8>& raw_data) {
    return build_wav(info, raw_data);
}

SoundHandle SoundManager::play(const std::string& bank, const std::string& cue,
                                const sim::Vector3* pos) {
    return play_internal(bank, cue, pos, false);
}

SoundHandle SoundManager::play_loop(const std::string& bank, const std::string& cue,
                                     const sim::Vector3* pos) {
    return play_internal(bank, cue, pos, true);
}

SoundHandle SoundManager::play_internal(const std::string& bank, const std::string& cue,
                                         const sim::Vector3* pos, bool looping) {
    if (headless_) return INVALID_SOUND;

    auto* bp = ensure_bank(bank);
    if (!bp) return INVALID_SOUND;

    // Look up cue → track index
    auto* mapping = bp->xsb->find_cue(cue);
    if (!mapping) {
        spdlog::debug("Cue not found: {}/{}", bank, cue);
        return INVALID_SOUND;
    }

    // If the cue references a different wave bank, load that
    const auto& wb_name = bp->xsb->wavebank_name(mapping->wave_bank_index);
    XwbParser* xwb = bp->xwb.get();
    BankPair* alt_bank = nullptr;
    if (wb_name != bp->xwb->bank_name()) {
        alt_bank = ensure_bank(wb_name);
        if (!alt_bank) return INVALID_SOUND;
        xwb = alt_bank->xwb.get();
    }

    spdlog::debug("Cue {}/{} → wb={} track={}", bank, cue, wb_name,
                  mapping->track_index);

    if (mapping->track_index >= xwb->entry_count()) {
        spdlog::debug("Track index {} out of range for bank {} ({} entries)",
                      mapping->track_index, wb_name, xwb->entry_count());
        return INVALID_SOUND;
    }

    // Read raw wave data from XWB
    auto raw = xwb->read_wave_data(mapping->track_index);
    if (raw.empty()) {
        spdlog::debug("Empty wave data for track {}", mapping->track_index);
        return INVALID_SOUND;
    }

    const auto& info = xwb->entry(mapping->track_index);

    // Wrap in WAV header for miniaudio decoding
    auto wav = wrap_as_wav(info, raw);

    // Create active sound
    auto active = std::make_unique<ActiveSound>();
    active->wav_data = std::move(wav);
    active->looping = looping;

    // Initialize decoder from memory
    ma_decoder_config dec_cfg = ma_decoder_config_init(
        ma_format_f32,
        0,  // use source channel count
        0   // use source sample rate
    );

    ma_result r = ma_decoder_init_memory(
        active->wav_data.data(),
        active->wav_data.size(),
        &dec_cfg,
        &active->decoder
    );
    if (r != MA_SUCCESS) {
        spdlog::debug("Failed to decode {}/{} (error {})", bank, cue, static_cast<int>(r));
        return INVALID_SOUND;
    }
    active->decoder_initialized = true;

    // Create sound from decoder data source
    r = ma_sound_init_from_data_source(
        &engine_->engine,
        &active->decoder,
        0, // flags
        nullptr, // group
        &active->sound
    );
    if (r != MA_SUCCESS) {
        spdlog::debug("Failed to create sound {}/{} (error {})", bank, cue, static_cast<int>(r));
        ma_decoder_uninit(&active->decoder);
        return INVALID_SOUND;
    }
    active->sound_initialized = true;

    // Configure 3D position
    if (pos) {
        ma_sound_set_position(&active->sound, pos->x, pos->y, pos->z);
        ma_sound_set_spatialization_enabled(&active->sound, MA_TRUE);
        ma_sound_set_min_distance(&active->sound, 5.0f);
        ma_sound_set_max_distance(&active->sound, 200.0f);
    }

    // Set looping
    if (looping) {
        ma_sound_set_looping(&active->sound, MA_TRUE);
    }

    // Start playback
    ma_sound_start(&active->sound);

    auto handle = next_handle_++;
    active_sounds_[handle] = std::move(active);
    return handle;
}

void SoundManager::stop(SoundHandle handle) {
    if (handle == INVALID_SOUND) return;

    auto it = active_sounds_.find(handle);
    if (it == active_sounds_.end()) return;

    auto& snd = it->second;
    if (snd->sound_initialized) {
        ma_sound_stop(&snd->sound);
        ma_sound_uninit(&snd->sound);
    }
    if (snd->decoder_initialized) {
        ma_decoder_uninit(&snd->decoder);
    }
    active_sounds_.erase(it);
}

void SoundManager::set_listener_position(const sim::Vector3& pos) {
    if (headless_ || !engine_->initialized) return;
    ma_engine_listener_set_position(&engine_->engine, 0, pos.x, pos.y, pos.z);
}

void SoundManager::gc() {
    if (headless_) return;

    for (auto it = active_sounds_.begin(); it != active_sounds_.end(); ) {
        auto& snd = it->second;
        if (!snd->looping && snd->sound_initialized &&
            !ma_sound_is_playing(&snd->sound)) {
            ma_sound_uninit(&snd->sound);
            if (snd->decoder_initialized) {
                ma_decoder_uninit(&snd->decoder);
            }
            it = active_sounds_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace osc::audio
