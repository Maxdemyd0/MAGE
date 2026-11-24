// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - GLFW window + OpenGL context.

#include <filesystem>
#include <string>
#include <vector>

struct GLFWwindow;

namespace mage {

class MAGE_API Window {
public:
    Window() = default;
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool Create(const std::string& title, int width, int height, bool vsync, bool takeFocus = true);
    void Destroy();

    bool ShouldClose() const;
    void RequestClose();
    void PollEvents();
    void SwapBuffers();

    void GetFramebufferSize(int& width, int& height) const;
    float GetContentScale() const;
    bool IsMinimized() const;

    void SetVSync(bool enabled);
    bool VSync() const { return m_vsync; }
    void SetTitle(const std::string& title);
    void SetCursorCaptured(bool captured); // hidden and locked, for mouse look
    bool CursorCaptured() const;

    std::vector<std::filesystem::path> TakeDroppedFiles(); // files dragged onto the window

    int GLMajor() const { return m_glMajor; }
    int GLMinor() const { return m_glMinor; }
    GLFWwindow* Handle() const { return m_window; }

private:
    GLFWwindow* m_window = nullptr;
    bool m_vsync = true;
    int m_glMajor = 0;
    int m_glMinor = 0;
};

} // namespace mage
