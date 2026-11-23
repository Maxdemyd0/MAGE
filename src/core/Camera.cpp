#include "core/Camera.h"
#include "core/Input.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace mage {

// ----------------------------------------------------------------------------- Frustum
void Frustum::Extract(const glm::mat4& m) {
    // Gribb/Hartmann plane extraction. GLM is column-major: m[col][row].
    auto row = [&](int r) { return glm::vec4(m[0][r], m[1][r], m[2][r], m[3][r]); };
    const glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    planes[0] = r3 + r0; // left
    planes[1] = r3 - r0; // right
    planes[2] = r3 + r1; // bottom
    planes[3] = r3 - r1; // top
    planes[4] = r3 + r2; // near
    planes[5] = r3 - r2; // far
    for (glm::vec4& p : planes) {
        const float len = glm::length(glm::vec3(p));
        if (len > 0.0f) p /= len;
    }
}

bool Frustum::IntersectsAABB(const glm::vec3& mn, const glm::vec3& mx) const {
    for (const glm::vec4& p : planes) {
        // "Positive vertex": corner furthest along the plane normal.
        const glm::vec3 v(p.x >= 0.0f ? mx.x : mn.x, p.y >= 0.0f ? mx.y : mn.y, p.z >= 0.0f ? mx.z : mn.z);
        if (glm::dot(glm::vec3(p), v) + p.w < 0.0f) return false;
    }
    return true;
}

// ----------------------------------------------------------------------------- Camera
glm::mat4 Camera::View() const { return glm::lookAt(position, target, up); }

glm::mat4 Camera::Projection(float aspect) const {
    return glm::perspective(glm::radians(fovDegrees), std::max(aspect, 0.01f), nearPlane, farPlane);
}

glm::vec3 Camera::Forward() const {
    const glm::vec3 d = target - position;
    const float len = glm::length(d);
    return len > 1e-5f ? d / len : glm::vec3(0.0f, 0.0f, 1.0f);
}

// ----------------------------------------------------------------------------- Chase camera
namespace {
float WrapAngle(float a) {
    constexpr float kPi = 3.14159265358979f;
    while (a > kPi) a -= 2.0f * kPi;
    while (a < -kPi) a += 2.0f * kPi;
    return a;
}
float Damp(float stiffness, float dt) { return 1.0f - std::exp(-stiffness * dt); }
} // namespace

void ChaseCameraController::Update(Camera& camera, const Input& input, const glm::vec3& targetPos,
                                   const glm::quat& targetRot, float dt, bool allowMouse) {
    const glm::vec3 forward = targetRot * glm::vec3(0.0f, 0.0f, 1.0f);
    const float targetHeading = std::atan2(forward.x, forward.z);

    if (!m_initialized) {
        m_heading = targetHeading;
        m_orbitYaw = 0.0f;
        m_orbitPitch = 0.0f;
    }

    // Heading follows the vehicle with lag; ignore pitch/roll so jumps don't whip the camera.
    m_heading = WrapAngle(m_heading + WrapAngle(targetHeading - m_heading) * Damp(headingStiffness, dt));

    if (allowMouse && input.IsMouseDown(GLFW_MOUSE_BUTTON_RIGHT)) {
        m_orbitYaw -= input.MouseDelta().x * 0.006f;
        m_orbitPitch = std::clamp(m_orbitPitch + input.MouseDelta().y * 0.004f, -0.25f, 1.2f);
    } else {
        m_orbitYaw *= 1.0f - Damp(1.5f, dt);
        m_orbitPitch *= 1.0f - Damp(1.5f, dt);
    }
    if (allowMouse && input.ScrollDelta() != 0.0f)
        distance = std::clamp(distance - input.ScrollDelta() * 0.8f, 3.0f, 40.0f);

    const float yaw = m_heading + m_orbitYaw;
    const glm::vec3 back(-std::sin(yaw), 0.0f, -std::cos(yaw));
    const float pitch = std::atan2(height, distance) + m_orbitPitch;
    const glm::vec3 lookAt = targetPos + glm::vec3(0.0f, lookHeight, 0.0f);
    const glm::vec3 desired = lookAt + back * (distance * std::cos(pitch)) + glm::vec3(0.0f, distance * std::sin(pitch), 0.0f);

    if (!m_initialized) {
        m_position = desired;
        m_initialized = true;
    } else {
        m_position = glm::mix(m_position, desired, Damp(positionStiffness, dt));
    }

    camera.position = m_position;
    camera.target = lookAt;
    camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
}

// ----------------------------------------------------------------------------- Fly camera
void FlyCameraController::SyncFrom(const Camera& camera) {
    const glm::vec3 f = camera.Forward();
    m_yaw = std::atan2(f.x, f.z);
    m_pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
    m_synced = true;
}

void FlyCameraController::Update(Camera& camera, const Input& input, float dt, bool active) {
    if (!m_synced) SyncFrom(camera);

    if (active && input.IsMouseDown(GLFW_MOUSE_BUTTON_RIGHT)) {
        m_yaw -= input.MouseDelta().x * sensitivity;
        m_pitch = std::clamp(m_pitch - input.MouseDelta().y * sensitivity, -1.55f, 1.55f);

        const glm::vec3 forward(std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch), std::cos(m_pitch) * std::cos(m_yaw));
        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));
        glm::vec3 move(0.0f);
        if (input.IsKeyDownRaw(GLFW_KEY_W)) move += forward;
        if (input.IsKeyDownRaw(GLFW_KEY_S)) move -= forward;
        if (input.IsKeyDownRaw(GLFW_KEY_D)) move += right;
        if (input.IsKeyDownRaw(GLFW_KEY_A)) move -= right;
        if (input.IsKeyDownRaw(GLFW_KEY_E)) move.y += 1.0f;
        if (input.IsKeyDownRaw(GLFW_KEY_Q)) move.y -= 1.0f;
        if (input.ScrollDelta() != 0.0f) speed = std::clamp(speed * (1.0f + input.ScrollDelta() * 0.1f), 1.0f, 500.0f);
        const float boost = input.IsKeyDownRaw(GLFW_KEY_LEFT_SHIFT) ? 4.0f : 1.0f;
        if (glm::dot(move, move) > 0.0f) camera.position += glm::normalize(move) * speed * boost * dt;
    }

    const glm::vec3 forward(std::cos(m_pitch) * std::sin(m_yaw), std::sin(m_pitch), std::cos(m_pitch) * std::cos(m_yaw));
    camera.target = camera.position + forward;
    camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
}

} // namespace mage
