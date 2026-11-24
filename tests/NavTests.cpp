// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
// Headless tests: navmesh from collision, paths, rebuilding, and agents that drive characters.
#include "navigation/NavWorld.h"
#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace mage;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

constexpr float kDt = 1.0f / 60.0f;

struct Scene {
    PhysicsWorld world;
    NavWorld nav;
    explicit Scene(float radius = 40.0f) {
        world.Init(1024, 1);
        Box({0, -0.5f, 0}, {1500, 0.5f, 1500});
        NavSettings s;
        s.activeRadius = radius;
        nav.Init(world, s);
    }
    ~Scene() { nav.Shutdown(); }
    BodyHandle Box(const glm::vec3& p, const glm::vec3& half, const glm::quat& q = glm::quat(1, 0, 0, 0)) {
        BodyDesc d;
        d.position = p;
        d.halfExtents = half;
        d.rotation = q;
        return world.CreateBody(d);
    }
    NavPathResult Path(const glm::vec3& a, const glm::vec3& b) { return nav.FindPath(a, b, points); }
    float Length() const {
        float l = 0.0f;
        for (size_t i = 1; i < points.size(); ++i) l += glm::distance(points[i - 1], points[i]);
        return l;
    }
    std::vector<glm::vec3> points;
};

struct Walker {
    CharacterHandle character;
    NavAgentHandle agent;
    CharacterState state;
};
static Walker MakeWalker(Scene& s, const glm::vec3& p) {
    CharacterDesc d;
    d.position = p;
    Walker w{s.world.CreateCharacter(d, 0), s.nav.AddAgent(p, 0.35f, 1.8f), {}};
    w.state.position = p;
    return w;
}
static void Step(Scene& s, std::vector<Walker>& walkers) {
    for (Walker& w : walkers) s.nav.SetAgentBody(w.agent, w.state.position, w.state.velocity);
    s.nav.UpdateAgents(kDt);
    for (Walker& w : walkers) {
        CharacterMove m;
        m.velocity = s.nav.AgentVelocity(w.agent);
        w.state = s.world.UpdateCharacter(w.character, m, kDt);
    }
    s.world.Step(kDt);
}

static void TestPaths() {
    Scene s;
    s.Box({0, 1.5f, 0}, {8, 1.5f, 0.5f});                      // wall, 16 m wide
    s.Box({20, 5, 20}, {4, 5, 4}, glm::angleAxis(0.6f, glm::vec3(0, 1, 0))); // building
    s.nav.WaitForBuilds({0, 0, 0});
    CHECK(s.nav.IsReady({0, 0, 0}) && !s.nav.IsReady({500, 0, 0}) && s.nav.Stats().tiles > 4);
    CHECK(s.Path({-5, 0, -20}, {5, 0, -20}) == NavPathResult::Complete && std::abs(s.Length() - 10.0f) < 0.2f);
    CHECK(s.Path({0, 0, -5}, {0, 0, 5}) == NavPathResult::Complete && s.points.size() >= 4);
    CHECK(s.Length() > 18.0f && s.Length() < 22.0f); // around the wall's end
    bool clear = true;
    for (const glm::vec3& p : s.points) clear &= !(std::abs(p.x) < 8.3f && std::abs(p.z) < 0.8f);
    CHECK(clear); // keeps the agent radius
    CHECK(s.Path({-30, 0, -30}, {30, 0, 30}) == NavPathResult::Complete); // across several tiles
    CHECK(s.Path({0, 0, 0}, {400, 0, 400}) == NavPathResult::None);
    CHECK(s.Path({NAN, 0, 0}, {1, 0, 0}) == NavPathResult::None);

    glm::vec3 p, hit;
    CHECK(s.nav.NearestPoint({10, 0.3f, 10}, p) && glm::distance(p, glm::vec3(10, 0, 10)) < 0.3f);
    CHECK(!s.nav.NearestPoint({20, 0.3f, 20}, p)); // the inside of a building is not walkable
    CHECK(s.nav.Raycast({0, 0, -5}, {0, 0, 5}, hit) && hit.z < -0.7f && hit.z > -1.3f);
    CHECK(!s.nav.Raycast({-5, 0, -20}, {5, 0, -20}, hit));
    for (int i = 0; i < 50; ++i) {
        CHECK(s.nav.RandomPoint({20, 0, 12}, 10, p) && glm::distance(glm::vec2(p.x, p.z), glm::vec2(20, 12)) < 10.1f);
        CHECK(std::abs(p.y) < 0.3f); // not on the roof
    }
}

static void TestHeights() {
    Scene s;
    s.Box({0, 1, 10}, {4, 1, 4});                                                   // platform at 2 m
    s.Box({0, 0.89f, 4.15f}, {2, 0.1f, 2.15f}, glm::angleAxis(glm::radians(-28.0f), glm::vec3(1, 0, 0))); // ramp up to it
    s.Box({20, 1, 10}, {4, 1, 4});                                                  // platform without a ramp
    for (int i = 0; i < 5; ++i) s.Box({-20, 0.1f * (i + 1), 5.0f + i * 0.6f}, {2, 0.1f * (i + 1), 0.3f}); // stairs, 20 cm steps
    s.Box({-20, 0.5f, 10.7f}, {2, 0.5f, 3.0f});
    s.nav.WaitForBuilds({0, 0, 0});
    CHECK(s.Path({0, 0, -8}, {0, 2, 11}) == NavPathResult::Complete && std::abs(s.points.back().y - 2.0f) < 0.3f);
    CHECK(s.Path({20, 0, -8}, {20, 2, 11}) == NavPathResult::Partial);
    CHECK(s.Path({-20, 0, -8}, {-20, 1, 11}) == NavPathResult::Complete && std::abs(s.points.back().y - 1.0f) < 0.3f);

    // Characters really can follow those paths.
    std::vector<Walker> w = {MakeWalker(s, {0, 0.02f, -8}), MakeWalker(s, {-20, 0.02f, -8}), MakeWalker(s, {20, 0.02f, -4})};
    s.nav.MoveAgent(w[0].agent, {0, 2, 11}, 2.0f, 0.4f);
    s.nav.MoveAgent(w[1].agent, {-20, 1, 11}, 2.0f, 0.4f);
    s.nav.MoveAgent(w[2].agent, {20, 2, 11}, 2.0f, 0.4f);
    for (int i = 0; i < 60 * 25; ++i) Step(s, w);
    CHECK(s.nav.AgentStatus(w[0].agent) == NavAgentStatus::Arrived && std::abs(w[0].state.position.y - 2.0f) < 0.1f);
    CHECK(s.nav.AgentStatus(w[1].agent) == NavAgentStatus::Arrived && std::abs(w[1].state.position.y - 1.0f) < 0.1f);
    CHECK(s.nav.AgentStatus(w[2].agent) == NavAgentStatus::Blocked && w[2].state.position.y < 0.1f);
    CHECK(glm::length(w[2].state.velocity) < 0.1f);
}

static void TestRebuild() {
    Scene s;
    // A closed room with a door that gets blocked and opened.
    s.Box({-5, 1.5f, 0}, {0.3f, 1.5f, 5});
    s.Box({5, 1.5f, 0}, {0.3f, 1.5f, 5});
    s.Box({0, 1.5f, 5}, {5, 1.5f, 0.3f});
    s.Box({-3.2f, 1.5f, -5}, {2.0f, 1.5f, 0.3f});
    s.Box({3.2f, 1.5f, -5}, {2.0f, 1.5f, 0.3f});
    s.nav.WaitForBuilds({});
    CHECK(s.Path({0, 0, -10}, {0, 0, 0}) == NavPathResult::Complete);
    const uint64_t before = s.nav.Stats().builds;
    std::vector<Walker> w = {MakeWalker(s, {0, 0.02f, -20})};
    s.nav.MoveAgent(w[0].agent, {0, 0, 0}, 3.0f, 0.4f);
    for (int i = 0; i < 60; ++i) Step(s, w);
    const BodyHandle door = s.Box({0, 1.5f, -5}, {1.5f, 1.5f, 0.3f});
    s.nav.Update({});
    CHECK(!s.nav.IsReady({0, 0, -5}));
    s.nav.WaitForBuilds({});
    CHECK(s.nav.Stats().builds - before <= 4); // only the tiles that touch the door
    CHECK(s.Path({0, 0, -10}, {0, 0, 0}) == NavPathResult::Partial);
    for (int i = 0; i < 60 * 14; ++i) Step(s, w);
    CHECK(glm::length(w[0].state.position) > 4.9f && s.nav.AgentStatus(w[0].agent) == NavAgentStatus::Blocked);
    s.world.DestroyBody(door);
    s.nav.WaitForBuilds({});
    CHECK(s.Path({0, 0, -10}, {0, 0, 0}) == NavPathResult::Complete);
    s.nav.MoveAgent(w[0].agent, {0, 0, 0}, 3.0f, 0.4f);
    for (int i = 0; i < 60 * 10; ++i) Step(s, w);
    CHECK(s.nav.AgentStatus(w[0].agent) == NavAgentStatus::Arrived);

    // Streaming: tiles follow the focus, agents left behind wait and recover.
    s.nav.WaitForBuilds({400, 0, 0});
    CHECK(!s.nav.IsReady({0, 0, 0}) && s.nav.IsReady({400, 0, 0}));
    s.nav.MoveAgent(w[0].agent, {0, 0, -10}, 3.0f, 0.4f);
    for (int i = 0; i < 90; ++i) Step(s, w);
    CHECK(s.nav.AgentStatus(w[0].agent) == NavAgentStatus::Waiting);
    s.nav.WaitForBuilds({});
    for (int i = 0; i < 60 * 10; ++i) Step(s, w);
    CHECK(s.nav.AgentStatus(w[0].agent) == NavAgentStatus::Arrived && std::abs(w[0].state.position.z + 10.0f) < 0.6f);
}

static void TestCrowd() {
    Scene s;
    s.nav.WaitForBuilds({});
    // Two groups swap sides through each other.
    std::vector<Walker> w;
    std::vector<glm::vec3> goals;
    for (int i = 0; i < 10; ++i) {
        const float x = (static_cast<float>(i % 5) - 2.0f) * 1.2f, z = 10.0f + static_cast<float>(i / 5) * 1.2f;
        const float side = i % 2 ? 1.0f : -1.0f;
        w.push_back(MakeWalker(s, {x, 0.02f, z * side}));
        goals.push_back({x, 0, -z * side});
    }
    for (size_t i = 0; i < w.size(); ++i) s.nav.MoveAgent(w[i].agent, goals[i], 1.5f, 0.6f);
    float closest = 10.0f;
    for (int i = 0; i < 60 * 25; ++i) {
        Step(s, w);
        for (size_t a = 0; a < w.size(); ++a)
            for (size_t b = a + 1; b < w.size(); ++b) closest = std::min(closest, glm::distance(w[a].state.position, w[b].state.position));
    }
    int arrived = 0;
    for (const Walker& k : w) arrived += s.nav.AgentStatus(k.agent) == NavAgentStatus::Arrived;
    CHECK(arrived == 10);
    CHECK(closest > 0.5f);

}

static void TestObstacles() {
    // Walks around a parked car (registered as an obstacle, not part of the mesh).
    Scene t;
    t.nav.WaitForBuilds({});
    BodyDesc car;
    car.position = {0, 0.8f, 0};
    car.halfExtents = {0.9f, 0.7f, 2.2f};
    car.motion = MotionType::Dynamic;
    car.mass = 1400;
    car.layer = ObjectLayer::Vehicle;
    t.world.CreateBody(car);
    for (float z : {-1.2f, 1.2f}) t.nav.AddObstacle({0, 0, z}, 1.25f, 1.6f);
    std::vector<Walker> v = {MakeWalker(t, {0.1f, 0.02f, -10})};
    t.nav.MoveAgent(v[0].agent, {0, 0, 10}, 1.5f, 0.5f);
    for (int i = 0; i < 60 * 25; ++i) Step(t, v);
    CHECK(t.nav.AgentStatus(v[0].agent) == NavAgentStatus::Arrived);

    // Pause, stop, remove.
    t.nav.MoveAgent(v[0].agent, {0, 0, 20}, 1.5f, 0.5f);
    for (int i = 0; i < 60; ++i) Step(t, v);
    t.nav.PauseAgent(v[0].agent, true);
    for (int i = 0; i < 30; ++i) Step(t, v);
    const glm::vec3 held = v[0].state.position;
    for (int i = 0; i < 60; ++i) Step(t, v);
    CHECK(glm::distance(held, v[0].state.position) < 0.05f && t.nav.AgentStatus(v[0].agent) == NavAgentStatus::Moving);
    t.nav.PauseAgent(v[0].agent, false);
    for (int i = 0; i < 60; ++i) Step(t, v);
    CHECK(glm::distance(held, v[0].state.position) > 0.8f);
    t.nav.StopAgent(v[0].agent);
    Step(t, v);
    CHECK(t.nav.AgentStatus(v[0].agent) == NavAgentStatus::Idle && t.nav.AgentVelocity(v[0].agent) == glm::vec3(0.0f));
    t.nav.RemoveAgent(v[0].agent);
    t.nav.RemoveAgent(v[0].agent);
    CHECK(!t.nav.MoveAgent(v[0].agent, {}, 1, 1) && !t.nav.MoveAgent(InvalidNavAgent, {}, 1, 1));
    CHECK(t.nav.AddAgent({}, 0.35f, 1.8f) == v[0].agent);
}

int main() {
    TestPaths();
    TestHeights();
    TestRebuild();
    TestCrowd();
    TestObstacles();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
