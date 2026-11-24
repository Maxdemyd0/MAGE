// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
// MAGE v1 - C++ <-> C# interop ABI.
//
// !!! Every struct here is mirrored 1:1 in scripting/Mage.Core/InternalCalls.cs !!!
// Field order, types and sizes must match exactly (sequential layout, natural alignment,
// 64-bit function pointers). Bump kInteropVersion whenever the layout changes.

#include <cstdint>

namespace mage::interop {

inline constexpr uint32_t kInteropVersion = 5;

struct NVec3 { float x, y, z; };
struct NQuat { float x, y, z, w; };

struct NRaycastHit {
    NVec3 point;
    NVec3 normal;
    float distance;
    uint32_t entity;   // 0xFFFFFFFF when the hit body has no entity
};

struct NVehicleState {
    float speedKmh;
    float forwardSpeed;
    float engineRPM;
    int32_t gear;
    int32_t wheelsInContact;
};

struct NCharacterState {
    NVec3 velocity;
    NVec3 groundNormal;
    float landingSpeed;
    float impactSpeed;
    uint32_t impactEntity;
    int32_t flags;     // 1 enabled, 2 grounded, 4 sliding, 8 knocked down
};

// ---- Native functions exposed to C# (called through `delegate* unmanaged<...>`) ----
using FnLog                = void (*)(int32_t level, const char* utf8Message);
using FnGetEntityTransform = void (*)(uint32_t entity, NVec3* outPosition, NQuat* outRotation);
using FnSetEntityTransform = void (*)(uint32_t entity, const NVec3* position, const NQuat* rotation);
using FnApplyVehicleInput  = void (*)(uint32_t entity, float throttle, float steering, float brake, float handBrake);
using FnGetVehicleState    = int32_t (*)(uint32_t entity, NVehicleState* outState);
using FnRaycastWorld       = int32_t (*)(const NVec3* origin, const NVec3* direction, float maxDistance,
                                         uint32_t ignoreEntity, NRaycastHit* outHit);
using FnSpawnEntity        = uint32_t (*)(const char* utf8Mesh, const NVec3* position, const NQuat* rotation,
                                          const NVec3* scale, int32_t dynamic);
using FnDestroyEntity      = void (*)(uint32_t entity);
using FnIsKeyDown          = int32_t (*)(int32_t key);
using FnIsKeyPressed       = int32_t (*)(int32_t key);
using FnEntityExists       = int32_t (*)(uint32_t entity);

using FnSpawnModel         = uint32_t (*)(const char* utf8Path, const NVec3* position, const NQuat* rotation, const NVec3* scale,
                                          int32_t physics); // 0 none, 1 static, 2 dynamic
using FnSetEntityTexture   = int32_t (*)(uint32_t entity, int32_t slot, const char* utf8Path); // base, metal-rough, normal, emissive
using FnPlayAudio          = uint32_t (*)(const char* utf8Path, float volume, float pitch, int32_t loop, const NVec3* position);
using FnStopAudio          = void (*)(uint32_t sound);
using FnLoadFont           = int32_t (*)(const char* utf8Path, float size);
using FnDrawLabel          = void (*)(const char* utf8Text, float x, float y, float size, uint32_t rgba, int32_t font);
using FnPlayAnimation      = int32_t (*)(uint32_t entity, const char* utf8Clip, float fade, int32_t loop, float speed);
using FnStopAnimation      = void (*)(uint32_t entity);   // freezes the current pose
using FnLoadAnimations     = int32_t (*)(uint32_t entity, const char* utf8Path);
using FnAnimationState     = int32_t (*)(uint32_t entity, float* time, float* duration); // 1 playing, 0 stopped / finished, -1 no animator
using FnSetAnimation       = void (*)(uint32_t entity, float speed, int32_t inPlace);
using FnBonePosition       = int32_t (*)(uint32_t entity, const char* utf8Bone, NVec3* position, NQuat* rotation);
using FnSetCharacterMove    = void (*)(uint32_t entity, const NVec3* velocity, int32_t jump);
using FnGetCharacterState   = int32_t (*)(uint32_t entity, NCharacterState* outState);
using FnSetCharacterEnabled = int32_t (*)(uint32_t entity, int32_t enabled, const NVec3* position);
using FnSetCharacterOptions = void (*)(uint32_t entity, float jumpSpeed, float knockdownSpeed, int32_t autoAnimate); // negative = keep
using FnKnockDown           = void (*)(uint32_t entity, const NVec3* velocity, float seconds);
using FnGetCamera           = uint32_t (*)(NVec3* position, NVec3* forward); // returns the camera target
using FnSetCameraTarget     = void (*)(uint32_t entity);
using FnNavMoveTo       = int32_t (*)(uint32_t entity, const NVec3* target, float speed, float stopDistance);
using FnNavStop         = void (*)(uint32_t entity);
using FnNavStatus       = int32_t (*)(uint32_t entity); // NavAgentStatus, -1 = never navigated
using FnNavFindPath     = int32_t (*)(const NVec3* from, const NVec3* to, NVec3* outPoints, int32_t maxPoints, int32_t* result); // NavPathResult
using FnNavRandomPoint  = int32_t (*)(const NVec3* center, float radius, NVec3* outPoint);
using FnNavNearestPoint = int32_t (*)(const NVec3* point, NVec3* outPoint);
using FnAttachScript    = int32_t (*)(uint32_t entity, const char* utf8Class);

struct NativeBindings {
    uint32_t structSize;
    uint32_t version;
    FnLog Log;
    FnGetEntityTransform GetEntityTransform;
    FnSetEntityTransform SetEntityTransform;
    FnApplyVehicleInput ApplyVehicleInput;
    FnGetVehicleState GetVehicleState;
    FnRaycastWorld RaycastWorld;
    FnSpawnEntity SpawnEntity;
    FnDestroyEntity DestroyEntity;
    FnIsKeyDown IsKeyDown;
    FnIsKeyPressed IsKeyPressed;
    FnEntityExists EntityExists;
    FnSpawnModel SpawnModel;
    FnSetEntityTexture SetEntityTexture;
    FnPlayAudio PlayAudio;
    FnStopAudio StopAudio;
    FnLoadFont LoadFont;
    FnDrawLabel DrawLabel;
    FnPlayAnimation PlayAnimation;
    FnStopAnimation StopAnimation;
    FnLoadAnimations LoadAnimations;
    FnAnimationState AnimationState;
    FnSetAnimation SetAnimation;
    FnBonePosition BonePosition;
    FnSetCharacterMove SetCharacterMove;
    FnGetCharacterState GetCharacterState;
    FnSetCharacterEnabled SetCharacterEnabled;
    FnSetCharacterOptions SetCharacterOptions;
    FnKnockDown KnockDown;
    FnGetCamera GetCamera;
    FnSetCameraTarget SetCameraTarget;
    FnNavMoveTo NavMoveTo;
    FnNavStop NavStop;
    FnNavStatus NavStatus;
    FnNavFindPath NavFindPath;
    FnNavRandomPoint NavRandomPoint;
    FnNavNearestPoint NavNearestPoint;
    FnAttachScript AttachScript;
};

// ---- Managed entry points filled in by C# ScriptEngine.Initialize ----
struct ManagedCallbacks {
    int32_t (*CreateScript)(uint32_t entity, const char* utf8ClassName);
    void (*DestroyScript)(uint32_t entity);
    void (*OnUpdate)(float dt);
    void (*OnFixedUpdate)(float fixedDt);
    void (*Shutdown)();
};

// int Mage.Interop.ScriptEngine.Initialize(NativeBindings*, ManagedCallbacks*)
using FnManagedInitialize = int32_t (*)(const NativeBindings* bindings, ManagedCallbacks* outCallbacks);

static_assert(sizeof(NVec3) == 12);
static_assert(sizeof(NQuat) == 16);
static_assert(sizeof(NRaycastHit) == 32);
static_assert(sizeof(NVehicleState) == 20);
static_assert(sizeof(NCharacterState) == 40);
static_assert(sizeof(NativeBindings) == 8 + 37 * sizeof(void*));
static_assert(sizeof(ManagedCallbacks) == 5 * sizeof(void*));

} // namespace mage::interop
