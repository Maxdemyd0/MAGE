// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "physics/PhysicsWorld.h"
#include "core/Log.h"
#include "renderer/Renderer.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/RegisterTypes.h>

#include "renderer/PhysicsDebugRenderer.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <vector>

namespace mage {

// ============================================================================ Jolt glue
namespace {

void JoltTrace(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buffer[1024];
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    Log::Trace("[Jolt] {}", buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool JoltAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line) {
    Log::Error("[Jolt] Assert failed {}:{}: ({}) {}", file, line, expression, message ? message : "");
    return false; // don't break into the debugger, keep running
}
#endif

int s_joltRefCount = 0;

void AcquireJolt() {
    if (s_joltRefCount++ > 0) return;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = JoltTrace;
    JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = JoltAssertFailed;)
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void ReleaseJolt() {
    if (--s_joltRefCount > 0) return;
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

// Broadphase layers: static geometry lives in its own tree so it never needs re-balancing.
namespace BroadPhaseLayers {
constexpr JPH::BroadPhaseLayer NonMoving(0);
constexpr JPH::BroadPhaseLayer Moving(1);
constexpr JPH::uint Count = 2;
} // namespace BroadPhaseLayers

constexpr JPH::ObjectLayer ToLayer(ObjectLayer l) { return static_cast<JPH::ObjectLayer>(l); }

class BroadPhaseLayerMap final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::Count; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == ToLayer(ObjectLayer::StaticWorld) ? BroadPhaseLayers::NonMoving : BroadPhaseLayers::Moving;
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BroadPhaseLayers::NonMoving ? "NON_MOVING" : "MOVING";
    }
#endif
};

class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
        if (layer == ToLayer(ObjectLayer::StaticWorld)) return bp == BroadPhaseLayers::Moving;
        return true;
    }
};

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        const auto staticWorld = ToLayer(ObjectLayer::StaticWorld);
        if (a == staticWorld) return b != staticWorld; // STATIC_WORLD vs everything that moves
        return true;                                   // DYNAMIC_PROP / VEHICLE / RAGDOLL collide with all
    }
};

JPH::Vec3 ToJolt(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
JPH::RVec3 ToJoltR(const glm::vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }
JPH::Quat ToJolt(const glm::quat& q) {
    const glm::quat n = glm::normalize(q);
    return JPH::Quat(n.x, n.y, n.z, n.w);
}
glm::vec3 ToGlm(JPH::Vec3Arg v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }
#ifdef JPH_DOUBLE_PRECISION
glm::vec3 ToGlm(JPH::RVec3Arg v) {
    return glm::vec3(static_cast<float>(v.GetX()), static_cast<float>(v.GetY()), static_cast<float>(v.GetZ()));
}
#endif
glm::quat ToGlm(JPH::QuatArg q) { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }
glm::mat4 ToGlm(JPH::Mat44Arg m) {
    glm::mat4 r(1.0f);
    for (int c = 0; c < 4; ++c) {
        const JPH::Vec4 col = m.GetColumn4(static_cast<JPH::uint>(c));
        r[c] = glm::vec4(col.GetX(), col.GetY(), col.GetZ(), col.GetW());
    }
    return r;
}

JPH::BodyID ToBodyID(BodyHandle h) { return JPH::BodyID(h); }

} // namespace

// ============================================================================ Impl
struct Vehicle {
    JPH::Ref<JPH::VehicleConstraint> constraint;
    JPH::BodyID body;
    std::vector<JPH::Ref<JPH::WheelSettingsWV>> wheels;
    VehicleTuning tuning;
    float wheelRadius = 0.4f;
    float wheelWidth = 0.3f;
};

struct PhysicsWorld::Impl {
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
    BroadPhaseLayerMap broadPhaseLayers;
    ObjectVsBroadPhaseFilter objectVsBroadPhase;
    ObjectLayerPairFilter objectPairs;
    std::unique_ptr<JPH::PhysicsSystem> system;
    std::vector<std::unique_ptr<Vehicle>> vehicles; // index == VehicleHandle (nullptr = free slot)
#ifdef JPH_DEBUG_RENDERER
    std::unique_ptr<PhysicsDebugRenderer> debugRenderer;
#endif
    PhysicsStats stats;
    bool initialized = false;

    JPH::BodyInterface& Bodies() { return system->GetBodyInterface(); }
    const JPH::BodyInterface& Bodies() const { return system->GetBodyInterface(); }

    Vehicle* GetVehicle(VehicleHandle h) const {
        return h < vehicles.size() ? vehicles[h].get() : nullptr;
    }
};

PhysicsWorld::PhysicsWorld() : m(std::make_unique<Impl>()) {}
PhysicsWorld::~PhysicsWorld() { Shutdown(); }

bool PhysicsWorld::Init(uint32_t maxBodies, int workerThreads) {
    if (m->initialized) return true;
    AcquireJolt();

    const int hw = static_cast<int>(std::thread::hardware_concurrency());
    const int threads = workerThreads >= 0 ? workerThreads : std::max(1, hw - 1);
    m->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(32 * 1024 * 1024);
    m->jobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);

    m->system = std::make_unique<JPH::PhysicsSystem>();
    m->system->Init(maxBodies, 0, 65536, 20480, m->broadPhaseLayers, m->objectVsBroadPhase, m->objectPairs);
    m->system->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));

#ifdef JPH_DEBUG_RENDERER
    m->debugRenderer = std::make_unique<PhysicsDebugRenderer>();
#endif

    m->stats.workerThreads = threads;
    m->initialized = true;
    Log::Info("Jolt Physics initialized: {} worker threads, max {} bodies", threads, maxBodies);
    return true;
}

void PhysicsWorld::Shutdown() {
    if (!m->initialized) return;
    for (VehicleHandle h = 0; h < m->vehicles.size(); ++h) DestroyVehicle(h);
    m->vehicles.clear();

    // Remove any remaining bodies.
    JPH::BodyIDVector ids;
    m->system->GetBodies(ids);
    if (!ids.empty()) {
        std::vector<JPH::BodyID> added;
        for (const JPH::BodyID& id : ids)
            if (m->Bodies().IsAdded(id)) added.push_back(id);
        if (!added.empty()) m->Bodies().RemoveBodies(added.data(), static_cast<int>(added.size()));
        m->Bodies().DestroyBodies(ids.data(), static_cast<int>(ids.size()));
    }

#ifdef JPH_DEBUG_RENDERER
    m->debugRenderer.reset();
#endif
    m->system.reset();
    m->jobSystem.reset();
    m->tempAllocator.reset();
    m->initialized = false;
    ReleaseJolt();
}

void PhysicsWorld::Step(float dt) {
    const auto start = std::chrono::steady_clock::now();
    const JPH::EPhysicsUpdateError err = m->system->Update(dt, 1, m->tempAllocator.get(), m->jobSystem.get());
    if (err != JPH::EPhysicsUpdateError::None) Log::Warn("Jolt update reported error flags {}", static_cast<int>(err));
    m->stats.lastStepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    ++m->stats.steps;
}

void PhysicsWorld::OptimizeBroadPhase() { m->system->OptimizeBroadPhase(); }

// ============================================================================ Bodies
BodyHandle PhysicsWorld::CreateBody(const BodyDesc& desc) {
    JPH::RefConst<JPH::Shape> shape;
    switch (desc.shape) {
        case ShapeType::Box: {
            const glm::vec3 h = glm::max(desc.halfExtents, glm::vec3(0.01f));
            const float convex = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({h.x, h.y, h.z}));
            shape = new JPH::BoxShape(ToJolt(h), convex);
            break;
        }
        case ShapeType::Sphere:
            shape = new JPH::SphereShape(std::max(desc.halfExtents.x, 0.01f));
            break;
        case ShapeType::Cylinder: {
            const float radius = std::max(desc.halfExtents.x, 0.02f);
            const float halfHeight = std::max(desc.halfExtents.y, 0.02f);
            const float convex = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min(radius, halfHeight));
            shape = new JPH::CylinderShape(halfHeight, radius, convex);
            break;
        }
        case ShapeType::Capsule:
            shape = new JPH::CapsuleShape(std::max(desc.halfExtents.y, 0.01f), std::max(desc.halfExtents.x, 0.01f));
            break;
    }

    JPH::EMotionType motion = JPH::EMotionType::Static;
    if (desc.motion == MotionType::Dynamic) motion = JPH::EMotionType::Dynamic;
    else if (desc.motion == MotionType::Kinematic) motion = JPH::EMotionType::Kinematic;

    ObjectLayer layer = desc.layer;
    if (motion != JPH::EMotionType::Static && layer == ObjectLayer::StaticWorld) layer = ObjectLayer::DynamicProp;
    if (motion == JPH::EMotionType::Static) layer = ObjectLayer::StaticWorld;

    JPH::BodyCreationSettings settings(shape, ToJoltR(desc.position), ToJolt(desc.rotation), motion, ToLayer(layer));
    settings.mFriction = desc.friction;
    settings.mRestitution = desc.restitution;
    settings.mUserData = desc.userData;
    if (motion == JPH::EMotionType::Dynamic && desc.mass > 0.0f) {
        settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        settings.mMassPropertiesOverride.mMass = desc.mass;
    }

    const JPH::BodyID id = m->Bodies().CreateAndAddBody(
        settings, motion == JPH::EMotionType::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (id.IsInvalid()) {
        Log::Error("PhysicsWorld: out of bodies");
        return InvalidBody;
    }
    return id.GetIndexAndSequenceNumber();
}

void PhysicsWorld::DestroyBody(BodyHandle body) {
    if (body == InvalidBody) return;
    const JPH::BodyID id = ToBodyID(body);
    JPH::BodyInterface& bi = m->Bodies();
    if (bi.IsAdded(id)) bi.RemoveBody(id);
    bi.DestroyBody(id);
}

bool PhysicsWorld::IsValid(BodyHandle body) const {
    return body != InvalidBody && m->Bodies().IsAdded(ToBodyID(body));
}

void PhysicsWorld::GetBodyTransform(BodyHandle body, glm::vec3& position, glm::quat& rotation) const {
    if (body == InvalidBody) return;
    JPH::RVec3 p;
    JPH::Quat q;
    m->Bodies().GetPositionAndRotation(ToBodyID(body), p, q);
    position = ToGlm(p);
    rotation = ToGlm(q);
}

void PhysicsWorld::SetBodyTransform(BodyHandle body, const glm::vec3& position, const glm::quat& rotation, bool resetVelocity) {
    if (body == InvalidBody) return;
    const JPH::BodyID id = ToBodyID(body);
    JPH::BodyInterface& bi = m->Bodies();
    const bool isStatic = bi.GetMotionType(id) == JPH::EMotionType::Static;
    bi.SetPositionAndRotation(id, ToJoltR(position), ToJolt(rotation),
                              isStatic ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (resetVelocity && !isStatic) bi.SetLinearAndAngularVelocity(id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
}

glm::vec3 PhysicsWorld::GetLinearVelocity(BodyHandle body) const {
    return body == InvalidBody ? glm::vec3(0.0f) : ToGlm(m->Bodies().GetLinearVelocity(ToBodyID(body)));
}

glm::vec3 PhysicsWorld::GetAngularVelocity(BodyHandle body) const {
    return body == InvalidBody ? glm::vec3(0.0f) : ToGlm(m->Bodies().GetAngularVelocity(ToBodyID(body)));
}

void PhysicsWorld::SetLinearVelocity(BodyHandle body, const glm::vec3& velocity) {
    if (body != InvalidBody) m->Bodies().SetLinearVelocity(ToBodyID(body), ToJolt(velocity));
}

void PhysicsWorld::AddImpulse(BodyHandle body, const glm::vec3& impulse) {
    if (body != InvalidBody) m->Bodies().AddImpulse(ToBodyID(body), ToJolt(impulse));
}

bool PhysicsWorld::IsActive(BodyHandle body) const {
    return body != InvalidBody && m->Bodies().IsActive(ToBodyID(body));
}

float PhysicsWorld::GetMass(BodyHandle body) const {
    if (body == InvalidBody) return 0.0f;
    JPH::BodyLockRead lock(m->system->GetBodyLockInterface(), ToBodyID(body));
    if (!lock.Succeeded()) return 0.0f;
    const JPH::Body& b = lock.GetBody();
    if (!b.IsDynamic()) return 0.0f;
    const float inv = b.GetMotionProperties()->GetInverseMass();
    return inv > 0.0f ? 1.0f / inv : 0.0f;
}

// ============================================================================ Queries
bool PhysicsWorld::Raycast(const glm::vec3& origin, const glm::vec3& dir, float maxDistance, RaycastHit& outHit,
                           BodyHandle ignoreBody) const {
    const JPH::RRayCast ray{ToJoltR(origin), ToJolt(dir * maxDistance)};
    JPH::RayCastResult result;
    const JPH::IgnoreSingleBodyFilter bodyFilter(ToBodyID(ignoreBody));
    if (!m->system->GetNarrowPhaseQuery().CastRay(ray, result, {}, {}, bodyFilter)) return false;

    const JPH::RVec3 point = ray.GetPointOnRay(result.mFraction);
    outHit.point = ToGlm(point);
    outHit.distance = result.mFraction * maxDistance;
    outHit.body = result.mBodyID.GetIndexAndSequenceNumber();

    JPH::BodyLockRead lock(m->system->GetBodyLockInterface(), result.mBodyID);
    if (lock.Succeeded()) {
        const JPH::Body& body = lock.GetBody();
        outHit.normal = ToGlm(body.GetWorldSpaceSurfaceNormal(result.mSubShapeID2, point));
        outHit.userData = body.GetUserData();
    }
    return true;
}

// ============================================================================ Vehicles
VehicleHandle PhysicsWorld::CreateRaycastVehicle(const VehicleDesc& desc, uint64_t userData, BodyHandle& outBody) {
    outBody = InvalidBody;
    const VehicleTuning& t = desc.tuning;
    const glm::vec3 he = desc.halfExtents;
    JPH::BodyInterface& bi = m->Bodies();

    // Chassis: a box with the centre of mass lowered for stability.
    JPH::ShapeSettings::ShapeResult shapeResult =
        JPH::OffsetCenterOfMassShapeSettings(JPH::Vec3(0.0f, -he.y, 0.0f), new JPH::BoxShape(ToJolt(he))).Create();
    if (shapeResult.HasError()) {
        Log::Error("Vehicle chassis shape error: {}", shapeResult.GetError().c_str());
        return InvalidVehicle;
    }
    JPH::BodyCreationSettings bodySettings(shapeResult.Get(), ToJoltR(desc.position), ToJolt(desc.rotation),
                                           JPH::EMotionType::Dynamic, ToLayer(ObjectLayer::Vehicle));
    bodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bodySettings.mMassPropertiesOverride.mMass = t.mass;
    bodySettings.mUserData = userData;
    JPH::Body* body = bi.CreateBody(bodySettings);
    if (!body) {
        Log::Error("PhysicsWorld: could not create vehicle body");
        return InvalidVehicle;
    }
    bi.AddBody(body->GetID(), JPH::EActivation::Activate);

    auto vehicle = std::make_unique<Vehicle>();
    vehicle->body = body->GetID();
    vehicle->tuning = t;
    vehicle->wheelRadius = desc.wheelRadius;
    vehicle->wheelWidth = desc.wheelWidth;

    // Wheels: 0 = front-left, 1 = front-right, 2 = rear-left, 3 = rear-right.
    // Vehicle space: +Z forward, +Y up, +X left.
    const float wheelY = -0.35f * he.y;
    const float axleZ = he.z - 1.6f * desc.wheelRadius;
    const glm::vec3 wheelPositions[4] = {{he.x, wheelY, axleZ}, {-he.x, wheelY, axleZ}, {he.x, wheelY, -axleZ}, {-he.x, wheelY, -axleZ}};

    JPH::VehicleConstraintSettings settings;
    settings.mMaxPitchRollAngle = JPH::DegreesToRadians(60.0f);
    for (int i = 0; i < 4; ++i) {
        JPH::Ref<JPH::WheelSettingsWV> w = new JPH::WheelSettingsWV;
        w->mPosition = ToJolt(wheelPositions[i]);
        w->mRadius = desc.wheelRadius;
        w->mWidth = desc.wheelWidth;
        w->mSuspensionMinLength = t.suspensionMinLength;
        w->mSuspensionMaxLength = t.suspensionMaxLength;

        // Tire friction curves (Jolt defaults scaled by the grip factor).
        w->mLongitudinalFriction.Clear();
        w->mLongitudinalFriction.AddPoint(0.0f, 0.0f);
        w->mLongitudinalFriction.AddPoint(0.06f, 1.2f * t.tireFriction);
        w->mLongitudinalFriction.AddPoint(0.2f, 1.0f * t.tireFriction);
        w->mLateralFriction.Clear();
        w->mLateralFriction.AddPoint(0.0f, 0.0f);
        w->mLateralFriction.AddPoint(3.0f, 1.2f * t.tireFriction);
        w->mLateralFriction.AddPoint(20.0f, 1.0f * t.tireFriction);

        vehicle->wheels.push_back(w);
        settings.mWheels.push_back(JPH::Ref<JPH::WheelSettings>(w.GetPtr()));
    }

    // Anti-roll bars front + rear keep the car from tipping in fast corners.
    settings.mAntiRollBars.resize(2);
    settings.mAntiRollBars[0].mLeftWheel = 0;
    settings.mAntiRollBars[0].mRightWheel = 1;
    settings.mAntiRollBars[1].mLeftWheel = 2;
    settings.mAntiRollBars[1].mRightWheel = 3;

    auto* controller = new JPH::WheeledVehicleControllerSettings;
    controller->mEngine.mMaxTorque = t.engineMaxTorque;
    controller->mEngine.mMaxRPM = t.engineMaxRPM;
    controller->mEngine.mMinRPM = t.engineMinRPM;
    if (desc.fourWheelDrive) {
        controller->mDifferentials.resize(2);
        controller->mDifferentials[0].mLeftWheel = 0;
        controller->mDifferentials[0].mRightWheel = 1;
        controller->mDifferentials[0].mEngineTorqueRatio = 0.4f;
        controller->mDifferentials[1].mLeftWheel = 2;
        controller->mDifferentials[1].mRightWheel = 3;
        controller->mDifferentials[1].mEngineTorqueRatio = 0.6f;
    } else {
        controller->mDifferentials.resize(1);
        controller->mDifferentials[0].mLeftWheel = 2;
        controller->mDifferentials[0].mRightWheel = 3;
    }
    settings.mController = controller;

    vehicle->constraint = new JPH::VehicleConstraint(*body, settings);
    vehicle->constraint->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterRay(ToLayer(ObjectLayer::Vehicle)));
    m->system->AddConstraint(vehicle->constraint);
    m->system->AddStepListener(vehicle->constraint);

    // Find a free slot.
    VehicleHandle handle = InvalidVehicle;
    for (VehicleHandle i = 0; i < m->vehicles.size(); ++i)
        if (!m->vehicles[i]) { handle = i; break; }
    if (handle == InvalidVehicle) {
        handle = static_cast<VehicleHandle>(m->vehicles.size());
        m->vehicles.emplace_back();
    }
    m->vehicles[handle] = std::move(vehicle);

    ApplyVehicleTuning(handle, t); // spring mode, steering, brakes
    outBody = body->GetID().GetIndexAndSequenceNumber();
    Log::Info("Raycast vehicle #{} created ({} kg, {} Nm, {})", handle, t.mass, t.engineMaxTorque,
              desc.fourWheelDrive ? "AWD" : "RWD");
    return handle;
}

void PhysicsWorld::DestroyVehicle(VehicleHandle handle) {
    Vehicle* v = m->GetVehicle(handle);
    if (!v) return;
    m->system->RemoveStepListener(v->constraint);
    m->system->RemoveConstraint(v->constraint);
    m->vehicles[handle].reset(); // chassis body is owned by the entity's RigidBodyComponent
}

void PhysicsWorld::SetVehicleInput(VehicleHandle handle, float forward, float right, float brake, float handBrake) {
    Vehicle* v = m->GetVehicle(handle);
    if (!v) return;
    auto* controller = static_cast<JPH::WheeledVehicleController*>(v->constraint->GetController());
    controller->SetDriverInput(std::clamp(forward, -1.0f, 1.0f), std::clamp(right, -1.0f, 1.0f),
                               std::clamp(brake, 0.0f, 1.0f), std::clamp(handBrake, 0.0f, 1.0f));
    if (forward != 0.0f || right != 0.0f || brake != 0.0f || handBrake != 0.0f) m->Bodies().ActivateBody(v->body);
}

void PhysicsWorld::ApplyVehicleTuning(VehicleHandle handle, const VehicleTuning& t) {
    Vehicle* v = m->GetVehicle(handle);
    if (!v) return;
    // WheelSettings are shared (ref-counted) with the live wheels, so edits apply next step.
    for (size_t i = 0; i < v->wheels.size(); ++i) {
        JPH::WheelSettingsWV& w = *v->wheels[i];
        w.mSuspensionSpring.mMode = JPH::ESpringMode::StiffnessAndDamping;
        w.mSuspensionSpring.mStiffness = std::max(t.suspensionStiffness, 1.0f);
        w.mSuspensionSpring.mDamping = std::max(t.suspensionDamping, 0.0f);
        w.mMaxBrakeTorque = t.maxBrakeTorque;
        const bool front = i < 2;
        w.mMaxSteerAngle = front ? JPH::DegreesToRadians(t.maxSteerAngleDeg) : 0.0f;
        w.mMaxHandBrakeTorque = front ? 0.0f : t.maxHandBrakeTorque;
    }
    auto* controller = static_cast<JPH::WheeledVehicleController*>(v->constraint->GetController());
    controller->GetEngine().mMaxTorque = t.engineMaxTorque;
    controller->GetEngine().mMaxRPM = std::max(t.engineMaxRPM, t.engineMinRPM + 500.0f);
    controller->GetEngine().mMinRPM = t.engineMinRPM;

    // Keep the creation-only values from the original vehicle.
    const VehicleTuning old = v->tuning;
    v->tuning = t;
    v->tuning.suspensionMinLength = old.suspensionMinLength;
    v->tuning.suspensionMaxLength = old.suspensionMaxLength;
    v->tuning.tireFriction = old.tireFriction;
    v->tuning.mass = old.mass;
    m->Bodies().ActivateBody(v->body);
}

VehicleTuning PhysicsWorld::GetVehicleTuning(VehicleHandle handle) const {
    const Vehicle* v = m->GetVehicle(handle);
    return v ? v->tuning : VehicleTuning{};
}

VehicleTelemetry PhysicsWorld::GetVehicleTelemetry(VehicleHandle handle) const {
    VehicleTelemetry out;
    const Vehicle* v = m->GetVehicle(handle);
    if (!v) return out;
    const auto* controller = static_cast<const JPH::WheeledVehicleController*>(
        static_cast<const JPH::VehicleConstraint*>(v->constraint.GetPtr())->GetController());
    out.engineRPM = controller->GetEngine().GetCurrentRPM();
    out.gear = controller->GetTransmission().GetCurrentGear();

    const JPH::Vec3 velocity = m->Bodies().GetLinearVelocity(v->body);
    const JPH::Quat rotation = m->Bodies().GetRotation(v->body);
    out.speedKmh = velocity.Length() * 3.6f;
    out.forwardSpeed = (rotation.Conjugated() * velocity).GetZ();
    for (const JPH::Wheel* w : v->constraint->GetWheels())
        if (w->HasContact()) ++out.wheelsInContact;
    return out;
}

BodyHandle PhysicsWorld::GetVehicleBody(VehicleHandle handle) const {
    const Vehicle* v = m->GetVehicle(handle);
    return v ? v->body.GetIndexAndSequenceNumber() : InvalidBody;
}

int PhysicsWorld::GetWheelCount(VehicleHandle handle) const {
    const Vehicle* v = m->GetVehicle(handle);
    return v ? static_cast<int>(v->wheels.size()) : 0;
}

glm::mat4 PhysicsWorld::GetWheelLocalTransform(VehicleHandle handle, int wheel) const {
    const Vehicle* v = m->GetVehicle(handle);
    if (!v || wheel < 0 || wheel >= static_cast<int>(v->wheels.size())) return glm::mat4(1.0f);
    // Model space of our wheel mesh: axle along +Y, "up" along +X (cylinder primitive).
    return ToGlm(v->constraint->GetWheelLocalTransform(static_cast<JPH::uint>(wheel), JPH::Vec3::sAxisY(), JPH::Vec3::sAxisX()));
}

float PhysicsWorld::GetWheelRadius(VehicleHandle handle) const {
    const Vehicle* v = m->GetVehicle(handle);
    return v ? v->wheelRadius : 0.0f;
}

float PhysicsWorld::GetWheelWidth(VehicleHandle handle) const {
    const Vehicle* v = m->GetVehicle(handle);
    return v ? v->wheelWidth : 0.0f;
}

// ============================================================================ Misc
void PhysicsWorld::SetGravity(const glm::vec3& gravity) { m->system->SetGravity(ToJolt(gravity)); }
glm::vec3 PhysicsWorld::GetGravity() const { return ToGlm(m->system->GetGravity()); }

void PhysicsWorld::DebugDraw(Renderer& renderer, const glm::vec3& cameraPosition, const DebugDrawSettings& s) {
#ifdef JPH_DEBUG_RENDERER
    PhysicsDebugRenderer& dbg = *m->debugRenderer;
    dbg.SetTarget(&renderer);
    dbg.SetCameraPos(ToJoltR(cameraPosition));
    JPH::BodyManager::DrawSettings ds;
    ds.mDrawShape = s.shapes;
    ds.mDrawShapeWireframe = true;
    ds.mDrawBoundingBox = s.boundingBoxes;
    ds.mDrawVelocity = s.velocities;
    ds.mDrawShapeColor = JPH::BodyManager::EShapeColor::MotionTypeColor;
    m->system->DrawBodies(ds, &dbg);
    if (s.constraints) m->system->DrawConstraints(&dbg);
    dbg.SetTarget(nullptr);
#else
    (void)renderer;
    (void)cameraPosition;
    (void)s;
#endif
}

PhysicsStats PhysicsWorld::GetStats() const {
    PhysicsStats s = m->stats;
    if (m->system) {
        s.numBodies = m->system->GetNumBodies();
        s.numActiveBodies = m->system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
    }
    s.numVehicles = 0;
    for (const auto& v : m->vehicles)
        if (v) ++s.numVehicles;
    return s;
}

} // namespace mage
