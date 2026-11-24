// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "core/Window.h"
#include "core/Log.h"

#include <GLFW/glfw3.h>
#include <utility>

namespace mage {

namespace {
std::vector<std::filesystem::path> s_dropped;
void DropCallback(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; ++i) s_dropped.emplace_back(std::u8string(reinterpret_cast<const char8_t*>(paths[i])));
}
void GlfwErrorCallback(int code, const char* description) {
    Log::Error("[GLFW] ({}) {}", code, description ? description : "");
}
} // namespace

Window::~Window() { Destroy(); }

bool Window::Create(const std::string& title, int width, int height, bool vsync, bool takeFocus) {
    glfwSetErrorCallback(GlfwErrorCallback);
    if (!glfwInit()) {
        Log::Error("glfwInit failed");
        return false;
    }

    // Prefer OpenGL 4.6 Core, fall back to 4.1 Core (macOS / older drivers).
    const int versions[][2] = {{4, 6}, {4, 5}, {4, 3}, {4, 1}};
    for (const auto& v : versions) {
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, v[0]);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, v[1]);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
        glfwWindowHint(GLFW_SAMPLES, 4);
        glfwWindowHint(GLFW_FOCUSED, takeFocus ? GLFW_TRUE : GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, takeFocus ? GLFW_TRUE : GLFW_FALSE);
#ifndef NDEBUG
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif
        m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
        if (m_window) {
            m_glMajor = v[0];
            m_glMinor = v[1];
            break;
        }
    }
    if (!m_window) {
        Log::Error("Failed to create a GLFW window with an OpenGL 4.x core context");
        glfwTerminate();
        return false;
    }

    glfwSetDropCallback(m_window, DropCallback);
    glfwMakeContextCurrent(m_window);
    SetVSync(vsync);
    Log::Info("Window created ({}x{}), OpenGL {}.{} core context", width, height, m_glMajor, m_glMinor);
    return true;
}

std::vector<std::filesystem::path> Window::TakeDroppedFiles() { return std::exchange(s_dropped, {}); }

void Window::Destroy() {
    if (m_window) {
        glfwDestroyWindow(m_window);
        m_window = nullptr;
        glfwTerminate();
    }
}

bool Window::ShouldClose() const { return m_window == nullptr || glfwWindowShouldClose(m_window); }
void Window::RequestClose() { if (m_window) glfwSetWindowShouldClose(m_window, GLFW_TRUE); }
void Window::PollEvents() { glfwPollEvents(); }
void Window::SwapBuffers() { glfwSwapBuffers(m_window); }

void Window::GetFramebufferSize(int& width, int& height) const {
    glfwGetFramebufferSize(m_window, &width, &height);
}

float Window::GetContentScale() const {
    float sx = 1.0f, sy = 1.0f;
    glfwGetWindowContentScale(m_window, &sx, &sy);
    return sx > 0.0f ? sx : 1.0f;
}

bool Window::IsMinimized() const {
    return glfwGetWindowAttrib(m_window, GLFW_ICONIFIED) != 0;
}

void Window::SetVSync(bool enabled) {
    m_vsync = enabled;
    glfwSwapInterval(enabled ? 1 : 0);
}

void Window::SetTitle(const std::string& title) { glfwSetWindowTitle(m_window, title.c_str()); }

} // namespace mage
