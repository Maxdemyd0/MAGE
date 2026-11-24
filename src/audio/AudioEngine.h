// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - sound playback (miniaudio): .wav .mp3 .flac .ogg, optional 3D positioning.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <glm/glm.hpp>

namespace mage {

using SoundHandle = uint32_t; // 0 = invalid

class MAGE_API AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // silent = decode and mix without opening an output device (automated runs, tests).
    bool Init(bool silent);
    void Shutdown();
    bool Ready() const;
    static bool Validate(const std::filesystem::path& file, std::string& error);

    // position == nullptr plays the sound flat (music, UI).
    SoundHandle Play(const std::filesystem::path& file, float volume = 1.0f, float pitch = 1.0f, bool loop = false,
                     const glm::vec3* position = nullptr);
    void Stop(SoundHandle sound);
    bool IsPlaying(SoundHandle sound) const;
    void Update(const glm::vec3& listener, const glm::vec3& forward); // frees finished sounds
    size_t ActiveCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace mage
