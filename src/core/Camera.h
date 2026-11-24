// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - camera, view frustum and camera controllers.

#include <functional>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace mage {

class Input;

// Six planes (ax + by + cz + d >= 0 means inside) extracted from a view-projection matrix.
struct MAGE_API Frustum {
    glm::vec4 planes[6]{};

    void Extract(const glm::mat4& viewProjection);
    bool IntersectsAABB(const glm::vec3& min, const glm::vec3& max) const;
};

class MAGE_API Camera {
public:
    glm::vec3 position{0.0f, 5.0f, -10.0f};
    glm::vec3 target{0.0f, 0.0f, 0.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
    float fovDegrees = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 1500.0f;

    glm::mat4 View() const;
    glm::mat4 Projection(float aspect) const;
    glm::vec3 Forward() const;
};

// Third-person chase camera for vehicles: smooth follow, right-mouse orbit, scroll zoom.
class MAGE_API ChaseCameraController {
public:
    float distance = 8.0f;
    float height = 2.4f;
    float lookHeight = 1.0f;
    float positionStiffness = 7.0f;
    float headingStiffness = 4.0f;

    void Update(Camera& camera, const Input& input, const glm::vec3& targetPos, const glm::quat& targetRot,
                float dt, bool allowMouse);
    void Reset() { m_initialized = false; }

private:
    bool m_initialized = false;
    float m_heading = 0.0f;
    float m_orbitYaw = 0.0f;
    float m_orbitPitch = 0.0f;
    glm::vec3 m_position{0.0f};
};

// Third-person camera for characters: orbit with the mouse (right button, or always with a captured
// cursor), drifts behind the character while it moves, and stays in front of walls.
class MAGE_API ThirdPersonCameraController {
public:
    float distance = 4.2f;
    float pivotHeight = 1.5f;        // above the feet
    float shoulder = 0.35f;
    float sensitivity = 0.0035f;

    // clearance(origin, direction, maxDistance) returns how far the camera may move.
    using ClearanceFn = std::function<float(const glm::vec3&, const glm::vec3&, float)>;
    void Update(Camera& camera, const Input& input, const glm::vec3& feet, const glm::vec3& velocity, float heading, float dt,
                bool allowMouse, bool cursorCaptured, const ClearanceFn& clearance);
    void Reset() { m_initialized = false; }

private:
    bool m_initialized = false;
    float m_yaw = 0.0f, m_pitch = 0.25f, m_current = 4.0f, m_idleMouse = 0.0f;
    glm::vec3 m_pivot{0.0f};
};

// Free-fly camera (editor): hold RMB + WASD/QE, Shift to go fast.
class MAGE_API FlyCameraController {
public:
    float speed = 20.0f;
    float sensitivity = 0.0025f;

    void Update(Camera& camera, const Input& input, float dt, bool active);
    void SyncFrom(const Camera& camera);

private:
    float m_yaw = 0.0f;
    float m_pitch = 0.0f;
    bool m_synced = false;
};

} // namespace mage
