// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - AI navigation on Recast/Detour. Their types never leak through this header.
//
//   static Jolt collision -> Recast tiles (worker threads) -> Detour navmesh -> paths and crowd steering
//
// Tiles exist around a focus point only. They are rebuilt automatically when static bodies appear,
// move or disappear (for example when a sector is streamed). Rigid bodies that move are not part of
// the mesh; agents steer around each other and around registered obstacles instead.

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

namespace mage {

class PhysicsWorld;

struct NavSettings {
    float tileSize = 32.0f;
    float cellSize = 0.2f;
    float cellHeight = 0.1f;
    float agentRadius = 0.4f;       // keep in step with the character controller
    float agentHeight = 1.8f;
    float agentClimb = 0.4f;
    float agentSlopeDegrees = 45.0f;
    float activeRadius = 130.0f;    // around the focus
    int workerThreads = 2;
    glm::vec2 origin{-48.0f, -48.0f}; // of the tile grid (sector corner)
};

struct NavStats {
    int tiles = 0;
    int pending = 0;                // waiting for or in a build
    uint64_t builds = 0;
    double lastBuildMs = 0.0;
    int agents = 0;
};

enum class NavPathResult : uint8_t { None, Partial, Complete };
enum class NavAgentStatus : uint8_t {
    Idle,       // no target
    Moving,
    Arrived,
    Blocked,    // went as far as possible; the target cannot be reached
    Waiting     // no navmesh here or at the target (yet)
};

using NavAgentHandle = uint32_t;
inline constexpr NavAgentHandle InvalidNavAgent = 0xFFFFFFFFu;

class MAGE_API NavWorld {
public:
    NavWorld();
    ~NavWorld();
    NavWorld(const NavWorld&) = delete;
    NavWorld& operator=(const NavWorld&) = delete;

    bool Init(PhysicsWorld& physics, const NavSettings& settings = {});
    void Shutdown();

    // Streams tiles in and out, applies finished builds. Call once per frame on the main thread.
    void Update(const glm::vec3& focus);
    void WaitForBuilds(const glm::vec3& focus); // blocks until everything around the focus is built
    bool IsReady(const glm::vec3& point) const; // the tile there exists and is up to date

    // Queries. Points are snapped to the mesh within 2 m horizontally and 4 m vertically.
    NavPathResult FindPath(const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>& outPoints) const;
    bool NearestPoint(const glm::vec3& point, glm::vec3& out) const;
    bool RandomPoint(const glm::vec3& center, float radius, glm::vec3& out) const; // reachable from center
    bool Raycast(const glm::vec3& from, const glm::vec3& to, glm::vec3& hit) const; // along the mesh; true if a border is hit

    // Agents. The owner moves the body: feed its position and velocity in, take the steering out.
    NavAgentHandle AddAgent(const glm::vec3& feet, float radius, float height);
    // Something agents should walk around (a car, the player); never steered itself.
    NavAgentHandle AddObstacle(const glm::vec3& feet, float radius, float height);
    void RemoveAgent(NavAgentHandle agent);
    void SetAgentBody(NavAgentHandle agent, const glm::vec3& feet, const glm::vec3& velocity);
    bool MoveAgent(NavAgentHandle agent, const glm::vec3& target, float speed, float stopDistance);
    void StopAgent(NavAgentHandle agent);
    void PauseAgent(NavAgentHandle agent, bool paused); // keeps the target (for example while knocked down)
    NavAgentStatus AgentStatus(NavAgentHandle agent) const;
    glm::vec3 AgentVelocity(NavAgentHandle agent) const; // desired, after avoidance
    void UpdateAgents(float dt);                         // once per fixed step

    // Edges of the mesh around a point, as pairs, for debug drawing.
    void DebugEdges(const glm::vec3& center, float radius, std::vector<glm::vec3>& outLines) const;
    NavStats Stats() const;
    const NavSettings& Settings() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace mage
