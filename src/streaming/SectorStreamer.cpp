// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "streaming/SectorStreamer.h"
#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

namespace mage {

using json = nlohmann::json;

namespace {

// Deterministic per-sector RNG (SplitMix64).
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed) {}
    uint64_t Next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float Float() { return static_cast<float>(Next() >> 40) / static_cast<float>(1ull << 24); }
    float Range(float a, float b) { return a + (b - a) * Float(); }
    int Int(int a, int b) { return a + static_cast<int>(Next() % static_cast<uint64_t>(b - a + 1)); }
};

glm::vec3 ReadVec3(const json& j, const char* key, const glm::vec3& fallback) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 3) return fallback;
    return glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
}

json WriteVec3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

SectorPhysics ParsePhysics(const std::string& s) {
    if (s == "dynamic") return SectorPhysics::Dynamic;
    if (s == "none") return SectorPhysics::None;
    return SectorPhysics::Static;
}

const char* PhysicsName(SectorPhysics p) {
    switch (p) {
        case SectorPhysics::Dynamic: return "dynamic";
        case SectorPhysics::None: return "none";
        default: return "static";
    }
}

int Chebyshev(const SectorCoord& a, const SectorCoord& b) {
    return std::max(std::abs(a.x - b.x), std::abs(a.z - b.z));
}

} // namespace

// ============================================================================ Lifetime
SectorStreamer::~SectorStreamer() { Stop(); }

void SectorStreamer::Start(const StreamerConfig& config, LoadedCallback onLoaded, UnloadedCallback onUnloaded) {
    Stop();
    m_config = config;
    m_config.unloadRadius = std::max(m_config.unloadRadius, m_config.loadRadius + 1);
    m_onLoaded = std::move(onLoaded);
    m_onUnloaded = std::move(onUnloaded);
    m_worker = std::jthread([this](std::stop_token st) { WorkerMain(st); });
    Log::Info("SectorStreamer started: {:.0f} m sectors, load radius {}, dir '{}'", m_config.sectorSize,
              m_config.loadRadius, m_config.sectorDirectory.string());
}

void SectorStreamer::Stop() {
    if (m_worker.joinable()) {
        m_worker.request_stop();
        m_cv.notify_all();
        m_worker.join();
    }
    std::lock_guard lock(m_mutex);
    m_requests.clear();
    m_completed.clear();
}

// ============================================================================ Worker thread
void SectorStreamer::WorkerMain(std::stop_token stop) {
    while (!stop.stop_requested()) {
        SectorCoord coord;
        {
            std::unique_lock lock(m_mutex);
            if (!m_cv.wait(lock, stop, [this] { return !m_requests.empty(); })) return; // stop requested
            coord = m_requests.front();
            m_requests.pop_front();
        }
        SectorData data = LoadOrGenerate(coord);
        {
            std::lock_guard lock(m_mutex);
            m_completed.push_back(std::move(data));
        }
    }
}

SectorData SectorStreamer::LoadOrGenerate(const SectorCoord& coord) const {
    const auto start = std::chrono::steady_clock::now();
    SectorData data;
    data.coord = coord;

    std::error_code ec;
    const std::filesystem::path file = m_config.sectorDirectory / std::format("sector_{}_{}.json", coord.x, coord.z);
    if (!m_config.sectorDirectory.empty() && std::filesystem::exists(file, ec)) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string error;
        if (ParseSectorJson(ss.str(), data, &error)) {
            data.source = SectorSource::File;
        } else {
            Log::Error("Sector {} parse error: {}", file.string(), error);
        }
    } else if (m_config.proceduralFallback) {
        GenerateProcedural(coord, data);
    }
    data.coord = coord;
    data.loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return data;
}

// Procedural "city block": roads on the sector borders + a centre cross street,
// four blocks with towers, plazas with loose crates, and street lamps.
void SectorStreamer::GenerateProcedural(const SectorCoord& coord, SectorData& out) const {
    out.source = SectorSource::Empty;
    if (coord.x == 0 && coord.z == 0) return; // the spawn plaza is hand-authored

    const uint64_t seed = (static_cast<uint64_t>(static_cast<uint32_t>(coord.x)) * 73856093ull) ^
                          (static_cast<uint64_t>(static_cast<uint32_t>(coord.z)) * 19349663ull) ^ m_config.seed;
    Rng rng(seed);

    // Generate as JSON and run it through the same parser the on-disk sectors use.
    json root;
    root["sector"] = json::array({coord.x, coord.z});
    json& entities = root["entities"];
    entities = json::array();

    const float half = m_config.sectorSize * 0.5f;
    const float road = 7.0f;          // half width of border roads
    const float street = 6.0f;        // half width of the centre street
    const float blockMin = street;
    const float blockMax = half - road;
    const float blockSize = blockMax - blockMin;
    const glm::vec3 palette[] = {{0.62f, 0.60f, 0.56f}, {0.48f, 0.52f, 0.58f}, {0.70f, 0.66f, 0.58f},
                                 {0.36f, 0.38f, 0.42f}, {0.58f, 0.46f, 0.40f}, {0.80f, 0.78f, 0.74f}};

    const float distanceFromSpawn = std::sqrt(static_cast<float>(coord.x * coord.x + coord.z * coord.z));
    const float heightScale = std::clamp(0.6f + distanceFromSpawn * 0.25f, 0.6f, 2.2f);

    for (int bx = 0; bx < 2; ++bx) {
        for (int bz = 0; bz < 2; ++bz) {
            const float sx = bx == 0 ? -1.0f : 1.0f;
            const float sz = bz == 0 ? -1.0f : 1.0f;
            const glm::vec2 blockCenter(sx * (blockMin + blockSize * 0.5f), sz * (blockMin + blockSize * 0.5f));

            // Sidewalk slab.
            entities.push_back({{"name", "Sidewalk"}, {"mesh", "cube"},
                                {"position", WriteVec3({blockCenter.x, 0.1f, blockCenter.y})},
                                {"scale", WriteVec3({blockSize, 0.2f, blockSize})},
                                {"color", WriteVec3({0.55f, 0.55f, 0.53f})}, {"roughness", 0.95f}, {"physics", "static"}});

            if (rng.Float() < 0.18f) {
                // Plaza: a pile of dynamic crates to smash through.
                const int crates = rng.Int(3, 7);
                for (int i = 0; i < crates; ++i) {
                    const float s = rng.Range(0.8f, 1.4f);
                    entities.push_back({{"name", "Crate"}, {"mesh", "cube"},
                                        {"position", WriteVec3({blockCenter.x + rng.Range(-8, 8), 0.2f + s * 0.5f + i * 0.05f,
                                                                blockCenter.y + rng.Range(-8, 8)})},
                                        {"rotation", WriteVec3({0, rng.Range(0, 90), 0})},
                                        {"scale", WriteVec3({s, s, s})}, {"color", WriteVec3({0.65f, 0.45f, 0.25f})},
                                        {"roughness", 0.8f}, {"physics", "dynamic"}, {"mass", 40.0f * s * s * s}});
                }
                continue;
            }

            // 2x2 lots per block, each with a tower.
            const float lot = blockSize * 0.5f;
            for (int lx = 0; lx < 2; ++lx) {
                for (int lz = 0; lz < 2; ++lz) {
                    if (rng.Float() < 0.15f) continue;
                    const glm::vec2 lotCenter = blockCenter + glm::vec2((lx - 0.5f) * lot, (lz - 0.5f) * lot);
                    const float w = rng.Range(lot * 0.55f, lot * 0.85f);
                    const float d = rng.Range(lot * 0.55f, lot * 0.85f);
                    const float h = rng.Range(6.0f, 28.0f) * heightScale;
                    const glm::vec3 color = palette[rng.Int(0, 5)] * rng.Range(0.85f, 1.1f);
                    entities.push_back({{"name", "Building"}, {"mesh", "cube"},
                                        {"position", WriteVec3({lotCenter.x, 0.2f + h * 0.5f, lotCenter.y})},
                                        {"scale", WriteVec3({w, h, d})}, {"color", WriteVec3(color)},
                                        {"metallic", rng.Float() < 0.2f ? 0.6f : 0.0f},
                                        {"roughness", rng.Range(0.35f, 0.9f)}, {"physics", "static"}});
                    if (h > 20.0f && rng.Float() < 0.5f) {
                        // Rooftop block.
                        entities.push_back({{"name", "Rooftop"}, {"mesh", "cube"},
                                            {"position", WriteVec3({lotCenter.x, 0.2f + h + 1.5f, lotCenter.y})},
                                            {"scale", WriteVec3({w * 0.5f, 3.0f, d * 0.5f})},
                                            {"color", WriteVec3(color * 0.8f)}, {"roughness", 0.8f}, {"physics", "static"}});
                    }
                }
            }
        }
    }

    // Street lamps along the centre street.
    for (int i = -1; i <= 1; i += 2) {
        for (int j = -1; j <= 1; j += 2) {
            const glm::vec3 p(i * (street + 0.8f), 3.0f, j * (half * 0.5f));
            entities.push_back({{"name", "LampPost"}, {"mesh", "cylinder"}, {"position", WriteVec3(p)},
                                {"scale", WriteVec3({0.25f, 6.0f, 0.25f})}, {"color", WriteVec3({0.2f, 0.2f, 0.22f})},
                                {"metallic", 0.8f}, {"roughness", 0.4f}, {"physics", "static"}});
            entities.push_back({{"name", "LampLight"}, {"mesh", "sphere"}, {"position", WriteVec3(p + glm::vec3(0, 3.2f, 0))},
                                {"scale", WriteVec3({0.6f, 0.6f, 0.6f})}, {"color", WriteVec3({1.0f, 0.9f, 0.7f})},
                                {"emissive", WriteVec3({1.2f, 1.0f, 0.7f})}, {"physics", "none"}});
        }
    }

    std::string error;
    if (ParseSectorJson(root.dump(), out, &error)) out.source = SectorSource::Procedural;
    else Log::Error("Procedural sector ({},{}) failed: {}", coord.x, coord.z, error);
}

// ============================================================================ Main thread
SectorCoord SectorStreamer::WorldToSector(const glm::vec3& p) const {
    const float s = m_config.sectorSize;
    return SectorCoord{static_cast<int32_t>(std::floor((p.x + s * 0.5f) / s)),
                       static_cast<int32_t>(std::floor((p.z + s * 0.5f) / s))};
}

glm::vec3 SectorStreamer::SectorCenter(const SectorCoord& c) const {
    return glm::vec3(static_cast<float>(c.x) * m_config.sectorSize, 0.0f, static_cast<float>(c.z) * m_config.sectorSize);
}

void SectorStreamer::Update(const glm::vec3& focusWorldPosition) {
    if (!Running()) return;
    const SectorCoord center = WorldToSector(focusWorldPosition);
    m_stats.focusSector = center;

    // 1) Collect finished loads from the worker.
    {
        std::lock_guard lock(m_mutex);
        while (!m_completed.empty()) {
            m_ready.push_back(std::move(m_completed.front()));
            m_completed.pop_front();
        }
    }

    // 2) Integrate a bounded number per frame (entity + physics body creation is main-thread work).
    int integrated = 0;
    while (!m_ready.empty() && integrated < m_config.maxIntegrationsPerFrame) {
        SectorData data = std::move(m_ready.front());
        m_ready.pop_front();
        auto it = m_states.find(data.coord);
        if (it == m_states.end() || it->second != State::Requested) continue; // cancelled meanwhile
        if (Chebyshev(data.coord, center) > m_config.unloadRadius) {
            m_states.erase(it);
            continue;
        }
        it->second = State::Loaded;
        if (m_onLoaded) m_onLoaded(data);
        m_stats.lastLoadMs = data.loadMs;
        ++m_stats.totalLoaded;
        ++integrated;
    }

    // 3) Unload / cancel sectors that fell out of range.
    std::vector<SectorCoord> cancelled;
    for (auto it = m_states.begin(); it != m_states.end();) {
        if (Chebyshev(it->first, center) <= m_config.unloadRadius) {
            ++it;
            continue;
        }
        if (it->second == State::Loaded) {
            if (m_onUnloaded) m_onUnloaded(it->first);
            ++m_stats.totalUnloaded;
        } else {
            cancelled.push_back(it->first);
        }
        it = m_states.erase(it);
    }

    // 4) Request missing sectors in range, nearest first.
    std::vector<SectorCoord> wanted;
    const int r = m_config.loadRadius;
    for (int dz = -r; dz <= r; ++dz)
        for (int dx = -r; dx <= r; ++dx) {
            const SectorCoord c{center.x + dx, center.z + dz};
            if (!m_states.contains(c)) wanted.push_back(c);
        }
    std::sort(wanted.begin(), wanted.end(), [&](const SectorCoord& a, const SectorCoord& b) {
        const int da = (a.x - center.x) * (a.x - center.x) + (a.z - center.z) * (a.z - center.z);
        const int db = (b.x - center.x) * (b.x - center.x) + (b.z - center.z) * (b.z - center.z);
        return da < db;
    });

    if (!wanted.empty() || !cancelled.empty()) {
        std::lock_guard lock(m_mutex);
        if (!cancelled.empty()) {
            std::erase_if(m_requests, [&](const SectorCoord& c) {
                return std::find(cancelled.begin(), cancelled.end(), c) != cancelled.end();
            });
        }
        for (const SectorCoord& c : wanted) {
            m_requests.push_back(c);
            m_states[c] = State::Requested;
        }
    }
    if (!wanted.empty()) m_cv.notify_one();

    size_t loaded = 0;
    for (const auto& [c, s] : m_states)
        if (s == State::Loaded) ++loaded;
    m_stats.loaded = loaded;
    m_stats.pending = m_states.size() - loaded;
    m_haveFocus = true;
}

void SectorStreamer::UnloadAll() {
    for (const auto& [c, s] : m_states)
        if (s == State::Loaded && m_onUnloaded) m_onUnloaded(c);
    m_states.clear();
    m_ready.clear();
    std::lock_guard lock(m_mutex);
    m_requests.clear();
    m_completed.clear();
}

std::vector<SectorCoord> SectorStreamer::LoadedSectors() const {
    std::vector<SectorCoord> out;
    for (const auto& [c, s] : m_states)
        if (s == State::Loaded) out.push_back(c);
    return out;
}

// ============================================================================ Serialization
bool SectorStreamer::ParseSectorJson(const std::string& text, SectorData& out, std::string* error) {
    try {
        const json root = json::parse(text);
        if (auto it = root.find("sector"); it != root.end() && it->is_array() && it->size() >= 2) {
            out.coord.x = (*it)[0].get<int32_t>();
            out.coord.z = (*it)[1].get<int32_t>();
        }
        out.entities.clear();
        if (auto it = root.find("entities"); it != root.end() && it->is_array()) {
            out.entities.reserve(it->size());
            for (const json& e : *it) {
                SectorEntityDesc d;
                d.name = e.value("name", std::string("Prop"));
                d.mesh = e.value("mesh", std::string("cube"));
                d.position = ReadVec3(e, "position", glm::vec3(0.0f));
                d.rotationDeg = ReadVec3(e, "rotation", glm::vec3(0.0f));
                d.scale = ReadVec3(e, "scale", glm::vec3(1.0f));
                d.color = ReadVec3(e, "color", glm::vec3(0.8f));
                d.emissive = ReadVec3(e, "emissive", glm::vec3(0.0f));
                d.metallic = e.value("metallic", 0.0f);
                d.roughness = e.value("roughness", 0.7f);
                d.physics = ParsePhysics(e.value("physics", std::string("static")));
                d.mass = e.value("mass", 0.0f);
                out.entities.push_back(std::move(d));
            }
        }
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
}

std::string SectorStreamer::SectorToJson(const SectorData& data) {
    json root;
    root["sector"] = json::array({data.coord.x, data.coord.z});
    json entities = json::array();
    for (const SectorEntityDesc& d : data.entities) {
        entities.push_back({{"name", d.name}, {"mesh", d.mesh}, {"position", WriteVec3(d.position)},
                            {"rotation", WriteVec3(d.rotationDeg)}, {"scale", WriteVec3(d.scale)},
                            {"color", WriteVec3(d.color)}, {"emissive", WriteVec3(d.emissive)},
                            {"metallic", d.metallic}, {"roughness", d.roughness},
                            {"physics", PhysicsName(d.physics)}, {"mass", d.mass}});
    }
    root["entities"] = std::move(entities);
    return root.dump(2);
}

} // namespace mage
