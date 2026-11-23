// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
// MAGE v1 - vertical-slice testbed scene: checkerboard ground, ramps, props and a drivable
// 4-wheel raycast vehicle controlled from C# (GameScripts.TestVehicleScript).
// Shared by MAGE_Runtime and MAGE_Editor.

#include "core/Camera.h"
#include "core/ECS.h"
#include "physics/PhysicsWorld.h"

namespace mage {

class Engine;

class Testbed {
public:
    static constexpr const char* kVehicleScript = "GameScripts.TestVehicleScript";

    void Build(Engine& engine);
    void Update(Engine& engine, float dt, bool cameraInputAllowed = true);
    void FixedUpdate(Engine& engine, float dt);   // native controller fallback (no .NET)

    void ResetVehicle(Engine& engine);
    void DrawTuningWindow(Engine& engine, bool* open = nullptr);
    void DrawHud(Engine& engine);

    Entity Vehicle() const { return m_vehicle; }
    bool UsingNativeController(Engine& engine) const;

    enum class CameraMode { Chase, Fly };
    CameraMode cameraMode = CameraMode::Chase;
    ChaseCameraController chaseCamera;
    FlyCameraController flyCamera;

private:
    void BuildGround(Engine& engine);
    void BuildRamps(Engine& engine);
    void BuildProps(Engine& engine);

    Entity m_vehicle = NullEntity;
    glm::vec3 m_spawnPosition{0.0f, 1.2f, -20.0f};
    float m_nativeSteer = 0.0f;
    float m_previousForward = 1.0f;
};

} // namespace mage
