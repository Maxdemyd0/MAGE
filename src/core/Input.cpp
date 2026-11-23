// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "core/Input.h"

#include <GLFW/glfw3.h>

namespace mage {

namespace {
Input* s_input = nullptr;
}

void Input::Attach(GLFWwindow* window) {
    m_window = window;
    s_input = this;
    // Installed before ImGui so the ImGui GLFW backend chains to these callbacks.
    glfwSetKeyCallback(window, &Input::KeyCallback);
    glfwSetScrollCallback(window, &Input::ScrollCallback);
}

void Input::KeyCallback(GLFWwindow*, int key, int, int action, int) {
    if (!s_input || key < 0 || key >= kMaxKeys) return;
    if (action == GLFW_PRESS) s_input->m_keys[key] = 1;
    else if (action == GLFW_RELEASE) s_input->m_keys[key] = 0;
}

void Input::ScrollCallback(GLFWwindow*, double, double dy) {
    if (s_input) s_input->m_scrollAccum += static_cast<float>(dy);
}

void Input::BeginFrame() {
    m_prevKeys = m_keys;
    m_prevInjected = m_injected;
    m_prevButtons = m_buttons;
    m_scrollAccum = 0.0f;
}

void Input::EndFrame() {
    if (!m_window) return;
    for (int b = 0; b < kMaxButtons; ++b)
        m_buttons[b] = glfwGetMouseButton(m_window, b) == GLFW_PRESS ? 1 : 0;

    double x = 0.0, y = 0.0;
    glfwGetCursorPos(m_window, &x, &y);
    const glm::vec2 pos(static_cast<float>(x), static_cast<float>(y));
    m_mouseDelta = m_firstMouse ? glm::vec2(0.0f) : pos - m_mousePos;
    m_mousePos = pos;
    m_firstMouse = false;
    m_scrollDelta = m_scrollAccum;

    if (glfwGetWindowAttrib(m_window, GLFW_FOCUSED) == 0) m_keys.fill(0);
}

bool Input::IsKeyDown(int key) const { return m_gameInput && IsKeyDownRaw(key); }
bool Input::IsKeyPressed(int key) const { return m_gameInput && IsKeyPressedRaw(key); }
bool Input::IsKeyReleased(int key) const {
    if (!m_gameInput || key < 0 || key >= kMaxKeys) return false;
    return !(m_keys[key] || m_injected[key]) && (m_prevKeys[key] || m_prevInjected[key]);
}

bool Input::IsKeyDownRaw(int key) const {
    return key >= 0 && key < kMaxKeys && (m_keys[key] || m_injected[key]);
}
bool Input::IsKeyPressedRaw(int key) const {
    if (key < 0 || key >= kMaxKeys) return false;
    return (m_keys[key] || m_injected[key]) && !(m_prevKeys[key] || m_prevInjected[key]);
}

bool Input::IsMouseDown(int button) const { return button >= 0 && button < kMaxButtons && m_buttons[button]; }
bool Input::IsMousePressed(int button) const {
    return button >= 0 && button < kMaxButtons && m_buttons[button] && !m_prevButtons[button];
}

} // namespace mage
