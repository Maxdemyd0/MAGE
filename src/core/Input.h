#pragma once
// MAGE v1 - keyboard / mouse input. Key codes are GLFW key codes (also used by C# KeyCode).

#include <array>
#include <cstdint>
#include <glm/vec2.hpp>

struct GLFWwindow;

namespace mage {

class Input {
public:
    static constexpr int kMaxKeys = 512;
    static constexpr int kMaxButtons = 8;

    void Attach(GLFWwindow* window);
    void BeginFrame();   // call before glfwPollEvents()
    void EndFrame();     // call after glfwPollEvents()

    // "Game" queries: return false while game input is disabled (e.g. ImGui has keyboard focus).
    bool IsKeyDown(int key) const;
    bool IsKeyPressed(int key) const;
    bool IsKeyReleased(int key) const;

    // Raw queries ignore the game-input gate (used by editor cameras, hotkeys).
    bool IsKeyDownRaw(int key) const;
    bool IsKeyPressedRaw(int key) const;
    bool IsMouseDown(int button) const;
    bool IsMousePressed(int button) const;

    glm::vec2 MousePosition() const { return m_mousePos; }
    glm::vec2 MouseDelta() const { return m_mouseDelta; }
    float ScrollDelta() const { return m_scrollDelta; }

    void SetGameInputEnabled(bool enabled) { m_gameInput = enabled; }
    bool GameInputEnabled() const { return m_gameInput; }

    // Synthetic key state (demo / autopilot / automated tests). OR-ed with the real keyboard.
    void InjectKey(int key, bool down) {
        if (key >= 0 && key < kMaxKeys) m_injected[key] = down ? 1 : 0;
    }
    void ClearInjectedKeys() { m_injected.fill(0); }

private:
    static void KeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods);
    static void ScrollCallback(GLFWwindow* window, double dx, double dy);

    GLFWwindow* m_window = nullptr;
    std::array<uint8_t, kMaxKeys> m_keys{};
    std::array<uint8_t, kMaxKeys> m_prevKeys{};
    std::array<uint8_t, kMaxKeys> m_injected{};
    std::array<uint8_t, kMaxKeys> m_prevInjected{};
    std::array<uint8_t, kMaxButtons> m_buttons{};
    std::array<uint8_t, kMaxButtons> m_prevButtons{};
    glm::vec2 m_mousePos{0.0f};
    glm::vec2 m_mouseDelta{0.0f};
    float m_scrollAccum = 0.0f;
    float m_scrollDelta = 0.0f;
    bool m_gameInput = true;
    bool m_firstMouse = true;
};

} // namespace mage
