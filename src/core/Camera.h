// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
// MAGE v1 - camera, view frustum and camera controllers.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace mage {

class Input;

// Six planes (ax + by + cz + d >= 0 means inside) extracted from a view-projection matrix.
struct Frustum {
    glm::vec4 planes[6]{};

    void Extract(const glm::mat4& viewProjection);
    bool IntersectsAABB(const glm::vec3& min, const glm::vec3& max) const;
};

class Camera {
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
class ChaseCameraController {
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

// Free-fly camera (editor): hold RMB + WASD/QE, Shift to go fast.
class FlyCameraController {
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
