// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "audio/AudioEngine.h"
#include "core/Log.h"

#include <miniaudio.h>
#include <unordered_map>

namespace mage {

struct AudioEngine::Impl {
    ma_engine engine{};
    bool ready = false;
    SoundHandle next = 1;
    std::unordered_map<SoundHandle, std::unique_ptr<ma_sound>> sounds; // ma_sound must not move
};

AudioEngine::AudioEngine() : m(std::make_unique<Impl>()) {}
AudioEngine::~AudioEngine() { Shutdown(); }

bool AudioEngine::Init(bool silent) {
    if (m->ready) return true;
    ma_engine_config config = ma_engine_config_init();
    if (silent) {
        config.noDevice = MA_TRUE;
        config.channels = 2;
        config.sampleRate = 48000;
    }
    const ma_result r = ma_engine_init(&config, &m->engine);
    if (r != MA_SUCCESS) {
        Log::Warn("Audio unavailable: {}", ma_result_description(r));
        return false;
    }
    m->ready = true;
    return true;
}

void AudioEngine::Shutdown() {
    if (!m->ready) return;
    for (auto& [h, s] : m->sounds) ma_sound_uninit(s.get());
    m->sounds.clear();
    ma_engine_uninit(&m->engine);
    m->ready = false;
}

bool AudioEngine::Ready() const { return m->ready; }
size_t AudioEngine::ActiveCount() const { return m->sounds.size(); }

bool AudioEngine::Validate(const std::filesystem::path& file, std::string& error) {
    ma_decoder decoder;
#if defined(_WIN32)
    const ma_result r = ma_decoder_init_file_w(file.c_str(), nullptr, &decoder);
#else
    const ma_result r = ma_decoder_init_file(file.c_str(), nullptr, &decoder);
#endif
    if (r != MA_SUCCESS) { error = ma_result_description(r); return false; }
    ma_decoder_uninit(&decoder);
    return true;
}

SoundHandle AudioEngine::Play(const std::filesystem::path& file, float volume, float pitch, bool loop, const glm::vec3* position) {
    if (!m->ready) return 0;
    auto sound = std::make_unique<ma_sound>();
    const ma_uint32 flags = MA_SOUND_FLAG_DECODE | (position ? 0u : static_cast<ma_uint32>(MA_SOUND_FLAG_NO_SPATIALIZATION));
#if defined(_WIN32)
    const ma_result r = ma_sound_init_from_file_w(&m->engine, file.c_str(), flags, nullptr, nullptr, sound.get());
#else
    const ma_result r = ma_sound_init_from_file(&m->engine, file.c_str(), flags, nullptr, nullptr, sound.get());
#endif
    if (r != MA_SUCCESS) {
        Log::Error("Could not play sound: {}", ma_result_description(r));
        return 0;
    }
    ma_sound_set_volume(sound.get(), glm::clamp(volume, 0.0f, 4.0f));
    ma_sound_set_pitch(sound.get(), glm::clamp(pitch, 0.05f, 8.0f));
    ma_sound_set_looping(sound.get(), loop ? MA_TRUE : MA_FALSE);
    if (position) {
        ma_sound_set_position(sound.get(), position->x, position->y, position->z);
        ma_sound_set_min_distance(sound.get(), 2.0f);
    }
    ma_sound_start(sound.get());
    const SoundHandle h = m->next++;
    m->sounds.emplace(h, std::move(sound));
    return h;
}

void AudioEngine::Stop(SoundHandle h) {
    auto it = m->sounds.find(h);
    if (it == m->sounds.end()) return;
    ma_sound_uninit(it->second.get());
    m->sounds.erase(it);
}

bool AudioEngine::IsPlaying(SoundHandle h) const {
    auto it = m->sounds.find(h);
    return it != m->sounds.end() && !ma_sound_at_end(it->second.get());
}

void AudioEngine::Update(const glm::vec3& p, const glm::vec3& f) {
    if (!m->ready) return;
    ma_engine_listener_set_position(&m->engine, 0, p.x, p.y, p.z);
    ma_engine_listener_set_direction(&m->engine, 0, f.x, f.y, f.z);
    for (auto it = m->sounds.begin(); it != m->sounds.end();) {
        if (ma_sound_at_end(it->second.get())) {
            ma_sound_uninit(it->second.get());
            it = m->sounds.erase(it);
        } else ++it;
    }
}

} // namespace mage
