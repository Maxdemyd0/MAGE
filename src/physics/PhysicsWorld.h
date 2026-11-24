// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - physics world built on Jolt Physics (JPH::PhysicsSystem).
//
// Jolt types never leak through this header; all engine code talks in glm + handles.
//
// Collision layers:
//   STATIC_WORLD  - ground, ramps, buildings (never moves, broadphase NON_MOVING)
//   DYNAMIC_PROP  - crates, cones, spheres
//   VEHICLE       - vehicle chassis (wheels are ray-cast, not bodies)
//   CHARACTER     - query body of walking characters: raycasts see it, rigid bodies do not (the
//                   character itself collides with them, so a car is not stopped by a pedestrian)
//   RAGDOLL       - reserved for ragdoll parts (collide with everything but STATIC-vs-STATIC)

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace mage {

class Renderer;

enum class ObjectLayer : uint16_t { StaticWorld = 0, DynamicProp = 1, Vehicle = 2, Ragdoll = 3, Character = 4, Count = 5 };
enum class MotionType : uint8_t { Static, Kinematic, Dynamic };
enum class ShapeType : uint8_t { Box, Sphere, Cylinder, Capsule, ConvexHull, Mesh };

using BodyHandle = uint32_t;
inline constexpr BodyHandle InvalidBody = 0xFFFFFFFFu;
using VehicleHandle = uint32_t;
inline constexpr VehicleHandle InvalidVehicle = 0xFFFFFFFFu;

struct BodyDesc {
    ShapeType shape = ShapeType::Box;
    glm::vec3 halfExtents{0.5f};   // Box: half extents | Sphere: x = radius | Cylinder/Capsule: x = radius, y = half height
    std::vector<glm::vec3> points;   // ConvexHull / Mesh vertices, already scaled
    std::vector<uint32_t> triangles; // Mesh only (static bodies): 3 indices per triangle
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    MotionType motion = MotionType::Static;
    ObjectLayer layer = ObjectLayer::StaticWorld;
    float mass = 0.0f;             // 0 = derive from shape volume (density 1000 kg/m^3)
    float friction = 0.6f;
    float restitution = 0.1f;
    uint64_t userData = 0;         // MAGE stores the owning Entity here
};

struct RaycastHit {
    glm::vec3 point{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
    BodyHandle body = InvalidBody;
    uint64_t userData = 0;
};

// Live-tunable parameters of a raycast vehicle (Jolt WheeledVehicleController).
struct VehicleTuning {
    float suspensionStiffness = 32000.0f;  // N/m   (spring k)
    float suspensionDamping = 3200.0f;     // N*s/m (damper c)
    float suspensionMinLength = 0.20f;     // m (creation only)
    float suspensionMaxLength = 0.50f;     // m (creation only)
    float engineMaxTorque = 550.0f;        // N*m
    float engineMaxRPM = 6500.0f;
    float engineMinRPM = 1000.0f;
    float maxSteerAngleDeg = 32.0f;
    float maxBrakeTorque = 2500.0f;        // N*m per wheel
    float maxHandBrakeTorque = 5000.0f;    // N*m per rear wheel
    float tireFriction = 1.0f;             // longitudinal/lateral friction scale (creation only)
    float mass = 1400.0f;                  // kg (creation only)
};

struct VehicleDesc {
    glm::vec3 position{0.0f, 2.0f, 0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 halfExtents{0.9f, 0.3f, 2.2f};  // chassis box half extents (x = width, z = length)
    float wheelRadius = 0.38f;
    float wheelWidth = 0.28f;
    bool fourWheelDrive = true;
    VehicleTuning tuning;
};

struct VehicleTelemetry {
    float speedKmh = 0.0f;
    float forwardSpeed = 0.0f;   // signed m/s along the chassis forward axis
    float engineRPM = 0.0f;
    int gear = 0;
    int wheelsInContact = 0;
};

using CharacterHandle = uint32_t;
inline constexpr CharacterHandle InvalidCharacter = 0xFFFFFFFFu;

struct CharacterDesc {
    glm::vec3 position{0.0f};        // feet
    float radius = 0.3f;
    float height = 1.8f;             // total, including the caps
    float mass = 75.0f;
    float maxSlopeDegrees = 48.0f;   // steeper ground makes the character slide
    float stepHeight = 0.4f;         // stairs and curbs up to this height are walked over
    float pushForce = 250.0f;        // N, pushing rigid bodies
};

struct CharacterMove {
    glm::vec3 velocity{0.0f};        // desired horizontal velocity (m/s), relative to the ground
    float jumpSpeed = 0.0f;          // > 0: jump now if standing on the ground
    float acceleration = 25.0f;      // m/s^2 on the ground
    float airAcceleration = 6.0f;
    bool passThroughVehicles = false; // knocked down: cars run over it instead of pushing it along
};

struct CharacterState {
    glm::vec3 position{0.0f};        // feet
    glm::vec3 velocity{0.0f};        // actual movement during the last update
    glm::vec3 groundVelocity{0.0f};  // of the surface under the feet (moving platforms, vehicles)
    glm::vec3 groundNormal{0.0f, 1.0f, 0.0f};
    bool grounded = false;
    bool sliding = false;            // on ground that is too steep
    bool jumped = false;             // left the ground by jumping in this update
    float landingSpeed = 0.0f;       // > 0 in the update that touches down (m/s)
    float impactSpeed = 0.0f;        // velocity change (m/s) a moving body caused, 0 = not hit
    glm::vec3 impactDirection{0.0f};
    uint64_t impactUserData = 0;
};

// Static collision as triangles (for building navigation meshes).
struct StaticGeometry {
    struct Box { // upright boxes, half axes
        glm::vec3 center, axisX, axisZ;
        float halfHeight;
    };
    std::vector<glm::vec3> triangles; // 3 per triangle, outward winding
    std::vector<Box> boxes;
};
struct StaticChange { glm::vec3 min, max; };

struct PhysicsStats {
    uint32_t numBodies = 0;
    uint32_t numActiveBodies = 0;
    uint32_t numVehicles = 0;
    double lastStepMs = 0.0;
    uint64_t steps = 0;
    int workerThreads = 0;
};

class MAGE_API PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    bool Init(uint32_t maxBodies = 20000, int workerThreads = -1);
    void Shutdown();
    void Step(float dt);
    void OptimizeBroadPhase();

    // Bodies
    BodyHandle CreateBody(const BodyDesc& desc);
    void DestroyBody(BodyHandle body);
    bool IsValid(BodyHandle body) const;
    void GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const;
    void SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation, bool resetVelocity);
    glm::vec3 GetLinearVelocity(BodyHandle body) const;
    glm::vec3 GetAngularVelocity(BodyHandle body) const;
    void SetLinearVelocity(BodyHandle body, const glm::vec3& velocity);
    void AddImpulse(BodyHandle body, const glm::vec3& impulse);
    bool IsActive(BodyHandle body) const;
    float GetMass(BodyHandle body) const;

    // Queries: dir must be normalized. Returns the closest hit within maxDistance.
    bool Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDistance, RaycastHit& outHit,
                 BodyHandle ignoreBody = InvalidBody) const;

    // Vehicles (Jolt VehicleConstraint + WheeledVehicleController + ray collision tester)
    VehicleHandle CreateRaycastVehicle(const VehicleDesc& desc, uint64_t userData, BodyHandle& outBody);
    void DestroyVehicle(VehicleHandle vehicle);
    void SetVehicleInput(VehicleHandle vehicle, float forward, float right, float brake, float handBrake);
    void ApplyVehicleTuning(VehicleHandle vehicle, const VehicleTuning& tuning);
    VehicleTuning GetVehicleTuning(VehicleHandle vehicle) const;
    VehicleTelemetry GetVehicleTelemetry(VehicleHandle vehicle) const;
    BodyHandle GetVehicleBody(VehicleHandle vehicle) const;
    int GetWheelCount(VehicleHandle vehicle) const;
    // Wheel transform relative to the chassis body frame, for a unit cylinder whose axis is +Y.
    glm::mat4 GetWheelLocalTransform(VehicleHandle vehicle, int wheel) const;
    float GetWheelRadius(VehicleHandle vehicle) const;
    float GetWheelWidth(VehicleHandle vehicle) const;

    // Walking characters (capsule, stairs, slopes, moving ground). Call UpdateCharacter once per fixed
    // step, before Step().
    CharacterHandle CreateCharacter(const CharacterDesc& desc, uint64_t userData);
    void DestroyCharacter(CharacterHandle character);
    CharacterState UpdateCharacter(CharacterHandle character, const CharacterMove& move, float dt);
    void SetCharacterPosition(CharacterHandle character, const glm::vec3& feet); // teleport
    void SetCharacterVelocity(CharacterHandle character, const glm::vec3& velocity);
    BodyHandle GetCharacterBody(CharacterHandle character) const; // for Raycast's ignoreBody
    bool CharacterFits(float radius, float height, const glm::vec3& feet, BodyHandle ignore = InvalidBody) const;

    void CollectStatic(const glm::vec3& min, const glm::vec3& max, StaticGeometry& out) const;
    std::vector<StaticChange> TakeStaticChanges(); // where static bodies appeared, moved or vanished since the last call

    void SetGravity(const glm::vec3& gravity);
    glm::vec3 GetGravity() const;

    struct DebugDrawSettings {
        bool shapes = true;
        bool constraints = true;
        bool boundingBoxes = false;
        bool velocities = false;
    };
    void DebugDraw(Renderer& renderer, const glm::vec3& cameraPosition, const DebugDrawSettings& settings);

    PhysicsStats GetStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace mage
