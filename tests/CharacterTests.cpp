// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
// Headless tests of the walking character controller.
#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

using namespace mage;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

constexpr float kDt = 1.0f / 60.0f;

struct Scene {
    PhysicsWorld world;
    Scene() {
        world.Init(512, 1);
        Box({0, -0.5f, 0}, {200, 0.5f, 200});
    }
    BodyHandle Box(const glm::vec3& p, const glm::vec3& half, MotionType motion = MotionType::Static, float mass = 0.0f, const glm::quat& q = glm::quat(1, 0, 0, 0)) {
        BodyDesc d;
        d.position = p;
        d.halfExtents = half;
        d.motion = motion;
        d.mass = mass;
        d.rotation = q;
        d.layer = motion == MotionType::Static ? ObjectLayer::StaticWorld : ObjectLayer::DynamicProp;
        d.userData = 77;
        return world.CreateBody(d);
    }
    CharacterHandle Character(const glm::vec3& p) {
        CharacterDesc d;
        d.position = p;
        return world.CreateCharacter(d, 42);
    }
    CharacterState Run(CharacterHandle c, const glm::vec3& velocity, float seconds, float jump = 0.0f) {
        CharacterState s;
        CharacterMove m;
        m.velocity = velocity;
        for (int i = 0; i < static_cast<int>(std::lround(seconds / kDt)); ++i) {
            m.jumpSpeed = i == 0 ? jump : 0.0f;
            s = world.UpdateCharacter(c, m, kDt);
            world.Step(kDt);
        }
        return s;
    }
};

static void TestFlat() {
    Scene s;
    const CharacterHandle c = s.Character({0, 0.02f, 0});
    CharacterState st = s.Run(c, {}, 1.0f);
    CHECK(st.grounded && std::abs(st.position.y) < 0.03f && glm::length(st.velocity) < 0.01f);
    st = s.Run(c, {0, 0, 3}, 2.0f);
    CHECK(st.grounded && std::abs(st.velocity.z - 3.0f) < 0.05f && std::abs(st.position.x) < 0.01f);
    CHECK(st.position.z > 5.5f && st.position.z < 6.0f);
    st = s.Run(c, {}, 0.5f);
    CHECK(glm::length(st.velocity) < 0.01f); // stops, no sliding
    const float nan = std::numeric_limits<float>::quiet_NaN();
    st = s.Run(c, {nan, 0, 0}, 0.2f);
    CHECK(std::isfinite(st.position.x) && st.grounded);
}

static void TestJumpAndFall() {
    Scene s;
    const CharacterHandle c = s.Character({0, 0.02f, 0});
    s.Run(c, {}, 0.5f);
    float apex = 0.0f, landing = 0.0f;
    int jumps = 0;
    CharacterMove m;
    m.jumpSpeed = 4.5f; // held: must not jump again in the air
    float airborne = 0.0f;
    for (int i = 0; i < 54; ++i) {
        const CharacterState st = s.world.UpdateCharacter(c, m, kDt);
        s.world.Step(kDt);
        apex = std::max(apex, st.position.y);
        landing = std::max(landing, st.landingSpeed);
        jumps += st.jumped;
        airborne += st.grounded ? 0.0f : kDt;
    }
    CHECK(jumps == 1 && airborne > 0.7f);
    CHECK(std::abs(apex - 4.5f * 4.5f / (2 * 9.81f)) < 0.08f);
    // Momentum is kept in the air without input.
    const CharacterHandle d = s.Character({20, 0.02f, 0});
    s.Run(d, {5, 0, 0}, 1.0f);
    s.Run(d, {5, 0, 0}, 0.1f, 4.5f);
    const CharacterState air = s.Run(d, {}, 0.3f);
    CHECK(!air.grounded && air.velocity.x > 4.5f);
    // Landing speed after a 5 m drop: sqrt(2 g h).
    const CharacterHandle e = s.Character({40, 5, 0});
    landing = 0.0f;
    for (int i = 0; i < 120; ++i) {
        landing = std::max(landing, s.world.UpdateCharacter(e, {}, kDt).landingSpeed);
        s.world.Step(kDt);
    }
    CHECK(std::abs(landing - std::sqrt(2 * 9.81f * 5)) < 0.4f);
    // Head hits a ceiling.
    s.Box({60, 2.4f, 0}, {2, 0.2f, 2});
    const CharacterHandle f = s.Character({60, 0.02f, 0});
    s.Run(f, {}, 0.3f);
    apex = 0.0f;
    for (int i = 0; i < 60; ++i) {
        m.jumpSpeed = i == 0 ? 6.0f : 0.0f;
        apex = std::max(apex, s.world.UpdateCharacter(f, m, kDt).position.y);
        s.world.Step(kDt);
    }
    CHECK(apex > 0.2f && apex < 0.45f);
}

static void TestObstacles() {
    Scene s;
    s.Box({0, 1, 5}, {3, 1, 0.5f}); // wall at z = 4.5
    CharacterHandle c = s.Character({0, 0.02f, 0});
    CharacterState st = s.Run(c, {0, 0, 5}, 3.0f);
    CHECK(st.position.z > 4.1f && st.position.z < 4.25f); // radius 0.3 plus padding
    st = s.Run(c, {3, 0, 3}, 0.5f);
    CHECK(st.velocity.x > 2.0f); // slides along it

    for (int i = 0; i < 5; ++i) s.Box({20, 0.15f * (i + 1) * 0.5f * 2 - 0.15f * 0.5f * (i + 1), 3.0f + i * 0.4f}, {1, 0.075f * (i + 1), 0.2f});
    s.Box({20, 0.375f, 8.0f}, {1, 0.375f, 3.2f}); // landing at 0.75 m
    c = s.Character({20, 0.02f, 0});
    st = s.Run(c, {0, 0, 2}, 4.0f);
    CHECK(st.position.z > 6.0f && std::abs(st.position.y - 0.75f) < 0.05f && st.grounded); // stairs

    s.Box({30, 0.175f, 5}, {1, 0.175f, 2});
    c = s.Character({30, 0.02f, 0});
    st = s.Run(c, {0, 0, 2}, 2.5f);
    CHECK(std::abs(st.position.y - 0.35f) < 0.05f); // 0.35 m step: walked up
    st = s.Run(c, {0, 0, 2}, 1.6f);
    CHECK(st.grounded && st.position.y < 0.05f); // and down again

    s.Box({40, 0.3f, 5}, {1, 0.3f, 2});
    c = s.Character({40, 0.02f, 0});
    st = s.Run(c, {0, 0, 2}, 3.0f);
    CHECK(st.position.y < 0.05f && st.position.z < 3.0f); // 0.6 m step: blocked

    auto slope = [&](float x, float deg) {
        s.Box({x, 0, 10}, {2, 0.1f, 8}, MotionType::Static, 0, glm::angleAxis(glm::radians(-deg), glm::vec3(1, 0, 0)));
        const CharacterHandle h = s.Character({x, 0.02f, 10 - 8 * std::cos(glm::radians(deg)) + 8 * std::cos(glm::radians(deg)) - 2.5f});
        return h;
    };
    c = slope(60, 35);
    s.world.SetCharacterPosition(c, {60, 0.5f, 10.5f});
    st = s.Run(c, {0, 0, 2}, 2.0f);
    CHECK(st.grounded && st.position.y > 1.8f); // 35 degrees: walks up
    const float held = st.position.y;
    st = s.Run(c, {}, 1.0f);
    CHECK(std::abs(st.position.y - held) < 0.05f); // and stands still on it
    c = slope(70, 60);
    s.world.SetCharacterPosition(c, {70, 1.5f, 10.6f});
    st = s.Run(c, {0, 0, 2}, 2.0f);
    CHECK(st.position.y < 1.2f); // 60 degrees: slides back down
}

static void TestBodies() {
    Scene s;
    const BodyHandle crate = s.Box({0, 0.5f, 2}, {0.5f, 0.5f, 0.5f}, MotionType::Dynamic, 20);
    CharacterHandle c = s.Character({0, 0.02f, 0});
    CharacterState st = s.Run(c, {0, 0, 3}, 2.0f);
    glm::vec3 p;
    glm::quat q;
    s.world.GetBodyTransform(crate, p, q);
    CHECK(p.z > 2.5f && st.impactSpeed == 0.0f); // pushed, and pushing is not being hit
    const BodyHandle heavy = s.Box({10, 1, 2}, {1, 1, 1}, MotionType::Dynamic, 2000);
    c = s.Character({10, 0.02f, 0});
    s.Run(c, {0, 0, 3}, 2.0f);
    s.world.GetBodyTransform(heavy, p, q);
    CHECK(std::abs(p.z - 2.0f) < 0.05f);

    // Raycasts see the character, and can ignore it.
    RaycastHit hit;
    CHECK(s.world.Raycast({10, 1, -5}, {0, 0, 1}, 20, hit) && hit.userData == 42);
    CHECK(s.world.Raycast({10, 1, -5}, {0, 0, 1}, 20, hit, s.world.GetCharacterBody(c)) && hit.userData == 77);

    // Hit by something heavy and fast.
    const BodyHandle car = s.Box({20, 0.8f, -6}, {0.9f, 0.4f, 2}, MotionType::Dynamic, 1400);
    c = s.Character({20, 0.02f, 0});
    float impact = 0.0f;
    glm::vec3 direction(0.0f);
    for (int i = 0; i < 60; ++i) {
        s.world.SetLinearVelocity(car, {0, 0, 12});
        st = s.world.UpdateCharacter(c, {}, kDt);
        if (st.impactSpeed > impact) {
            impact = st.impactSpeed;
            direction = st.impactDirection;
            CHECK(st.impactUserData == 77);
        }
        s.world.Step(kDt);
    }
    CHECK(impact > 9.0f && impact < 12.5f && direction.z > 0.9f);
    CHECK(glm::length(s.world.GetLinearVelocity(car)) > 10.0f); // the car is not stopped dead
    // Knocked down, it passes through vehicles instead of being pushed along.
    VehicleDesc vd;
    vd.position = {-30, 1.2f, 0};
    BodyHandle chassis;
    s.world.CreateRaycastVehicle(vd, 99, chassis);
    for (int i = 0; i < 90; ++i) s.world.Step(kDt);
    for (bool pass : {false, true}) {
        c = s.Character({-30, 0.02f, -6});
        CharacterMove m;
        m.velocity = {0, 0, 5};
        m.passThroughVehicles = pass;
        for (int i = 0; i < 150; ++i) {
            st = s.world.UpdateCharacter(c, m, kDt);
            s.world.Step(kDt);
        }
        CHECK(pass ? st.position.z > 5.0f : st.position.z < -1.5f);
        s.world.DestroyCharacter(c);
    }
    // A light box at walking speed is harmless.
    const BodyHandle light = s.Box({30, 0.8f, -2}, {0.2f, 0.2f, 0.2f}, MotionType::Dynamic, 3);
    c = s.Character({30, 0.02f, 0});
    impact = 0.0f;
    for (int i = 0; i < 60; ++i) {
        s.world.SetLinearVelocity(light, {0, 0, 4});
        impact = std::max(impact, s.world.UpdateCharacter(c, {}, kDt).impactSpeed);
        s.world.Step(kDt);
    }
    CHECK(impact < 0.5f);

    // Riding a moving platform.
    const BodyHandle platform = s.Box({50, 0.5f, 0}, {2, 0.5f, 2}, MotionType::Kinematic);
    c = s.Character({50, 1.02f, 0});
    for (int i = 0; i < 120; ++i) {
        s.world.SetLinearVelocity(platform, {1.5f, 0, 0});
        st = s.world.UpdateCharacter(c, {}, kDt);
        s.world.Step(kDt);
    }
    s.world.GetBodyTransform(platform, p, q);
    CHECK(st.grounded && std::abs(p.x - st.position.x) < 0.15f && p.x > 52.5f);
}

static void TestCharacters() {
    Scene s;
    const CharacterHandle a = s.Character({0, 0.02f, 0}), b = s.Character({0, 0.02f, 3});
    CharacterState sa, sb;
    CharacterMove ma, mb;
    ma.velocity = {0, 0, 2};
    mb.velocity = {0, 0, -2};
    for (int i = 0; i < 180; ++i) {
        sa = s.world.UpdateCharacter(a, ma, kDt);
        sb = s.world.UpdateCharacter(b, mb, kDt);
        s.world.Step(kDt);
    }
    CHECK(glm::distance(sa.position, sb.position) > 0.55f && sa.grounded && sb.grounded);

    CHECK(s.world.CharacterFits(0.3f, 1.8f, {30, 0, 0}));
    s.Box({30, 1, 0}, {0.5f, 1, 0.5f});
    CHECK(!s.world.CharacterFits(0.3f, 1.8f, {30, 0, 0.6f}));
    CHECK(s.world.CharacterFits(0.3f, 1.8f, {30, 0, 0.85f})); // next to it
    CHECK(!s.world.CharacterFits(0.3f, 1.8f, sa.position) && s.world.CharacterFits(0.3f, 1.8f, sa.position, s.world.GetCharacterBody(a)));

    const uint32_t bodies = s.world.GetStats().numBodies;
    s.world.DestroyCharacter(a);
    s.world.DestroyCharacter(a); // twice is harmless
    CHECK(s.world.GetStats().numBodies == bodies - 1);
    CHECK(s.world.UpdateCharacter(a, ma, kDt).position == glm::vec3(0.0f));
    CHECK(s.Character({60, 0.02f, 0}) == a); // slot is reused
    s.world.SetCharacterPosition(b, {80, 10, 0});
    sb = s.Run(b, {}, 0.1f);
    CHECK(!sb.grounded && sb.position.y > 9.8f && sb.position.x == 80.0f);
    s.world.SetCharacterPosition(b, {80, 0.02f, 0});
    s.Run(b, {}, 0.3f);
    s.world.SetCharacterVelocity(b, {0, 5, 3}); // launched although it stands on the ground
    sb = s.Run(b, {}, 0.2f);
    CHECK(sb.position.y > 0.5f && sb.velocity.z > 2.5f);
}

int main() {
    TestFlat();
    TestJumpAndFall();
    TestObstacles();
    TestBodies();
    TestCharacters();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
