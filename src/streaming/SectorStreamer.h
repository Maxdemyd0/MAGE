// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - asynchronous 2D grid world streamer.
//
// The world is cut into square sectors on the XZ plane. Every frame the main thread calls
// Update(focus) with the player's position; sectors inside the load radius are queued for a
// std::jthread worker that reads + parses sector_<x>_<z>.json (or procedurally generates a
// city block when no file exists). Parsed SectorData comes back through a lock-protected queue
// and is integrated on the main thread (a few sectors per frame), so disk I/O and JSON parsing
// never stall rendering. Sectors outside the unload radius are released.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace mage {

struct SectorCoord {
    int32_t x = 0;
    int32_t z = 0;
    bool operator==(const SectorCoord&) const = default;
};

struct SectorCoordHash {
    size_t operator()(const SectorCoord& c) const noexcept {
        return std::hash<uint64_t>()((static_cast<uint64_t>(static_cast<uint32_t>(c.x)) << 32) | static_cast<uint32_t>(c.z));
    }
};

enum class SectorPhysics : uint8_t { None, Static, Dynamic };

struct SectorEntityDesc {
    std::string name = "Prop";
    std::string mesh = "cube";
    glm::vec3 position{0.0f};      // relative to the sector centre
    glm::vec3 rotationDeg{0.0f};   // euler pitch/yaw/roll (degrees)
    glm::vec3 scale{1.0f};
    glm::vec3 color{0.8f};
    glm::vec3 emissive{0.0f};
    float metallic = 0.0f;
    float roughness = 0.7f;
    float clearcoat = 0.0f;
    float transmission = 0.0f;
    float ior = 1.5f;
    std::string model;             // imported model, replaces 'mesh'
    std::string animation;         // clip name (see SpawnDesc)
    std::vector<std::string> animationFiles;
    std::string texture;
    SectorPhysics physics = SectorPhysics::Static;
    float mass = 0.0f;
};

enum class SectorSource : uint8_t { File, Procedural, Empty };

struct SectorData {
    SectorCoord coord;
    SectorSource source = SectorSource::Empty;
    std::vector<SectorEntityDesc> entities;
    double loadMs = 0.0;
};

struct StreamerConfig {
    float sectorSize = 96.0f;
    int loadRadius = 2;        // in sectors (Chebyshev distance)
    int unloadRadius = 3;      // must be > loadRadius (hysteresis)
    int maxIntegrationsPerFrame = 2;
    std::filesystem::path sectorDirectory;
    bool proceduralFallback = true;
    uint32_t seed = 1337;
};

struct StreamerStats {
    size_t loaded = 0;
    size_t pending = 0;
    uint64_t totalLoaded = 0;
    uint64_t totalUnloaded = 0;
    double lastLoadMs = 0.0;
    SectorCoord focusSector;
};

class MAGE_API SectorStreamer {
public:
    using LoadedCallback = std::function<void(const SectorData&)>;
    using UnloadedCallback = std::function<void(const SectorCoord&)>;

    SectorStreamer() = default;
    ~SectorStreamer();
    SectorStreamer(const SectorStreamer&) = delete;
    SectorStreamer& operator=(const SectorStreamer&) = delete;

    void Start(const StreamerConfig& config, LoadedCallback onLoaded, UnloadedCallback onUnloaded);
    void Stop();
    bool Running() const { return m_worker.joinable(); }

    // Main thread, once per frame.
    void Update(const glm::vec3& focusWorldPosition);
    // Unload everything (e.g. before shutting the scene down).
    void UnloadAll();

    SectorCoord WorldToSector(const glm::vec3& p) const;
    glm::vec3 SectorCenter(const SectorCoord& c) const;
    std::vector<SectorCoord> LoadedSectors() const;
    const StreamerStats& Stats() const { return m_stats; }
    StreamerConfig& Config() { return m_config; }

    // Serialization helpers (sector JSON format).
    static bool ParseSectorJson(const std::string& text, SectorData& out, std::string* error = nullptr);
    static std::string SectorToJson(const SectorData& data);

private:
    enum class State : uint8_t { Requested, Loaded };

    void WorkerMain(std::stop_token stop);
    SectorData LoadOrGenerate(const SectorCoord& coord) const;
    void GenerateProcedural(const SectorCoord& coord, SectorData& out) const;

    StreamerConfig m_config;
    LoadedCallback m_onLoaded;
    UnloadedCallback m_onUnloaded;

    // Main-thread state.
    std::unordered_map<SectorCoord, State, SectorCoordHash> m_states;
    std::deque<SectorData> m_ready;   // received from worker, waiting for integration
    StreamerStats m_stats;
    bool m_haveFocus = false;

    // Shared with the worker.
    std::mutex m_mutex;
    std::condition_variable_any m_cv;
    std::deque<SectorCoord> m_requests;
    std::deque<SectorData> m_completed;
    std::jthread m_worker;
};

} // namespace mage
