#pragma once
// MAGE v1 - engine core: subsystem lifecycle + fixed-timestep simulation loop.
//
// Frame structure (Engine::Run):
//   poll input
//   accumulator += frameDt
//   while accumulator >= 1/60:                       <- fixed 60 Hz simulation
//       C# OnFixedUpdate -> app OnFixedUpdate -> vehicle inputs -> Jolt step -> capture poses
//   interpolate render transforms (alpha = accumulator / fixedDt)
//   C# OnUpdate -> app OnUpdate -> sector streaming -> deferred destruction
//   render scene (+ physics debug) -> ImGui -> swap  <- variable rate

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/Camera.h"
#include "core/ECS.h"
#include "core/Input.h"
#include "core/Profiler.h"

namespace mage {

class Window;
class Renderer;
class PhysicsWorld;
class SectorStreamer;
class ScriptHost;
struct SectorData;
struct SectorCoord;
struct VehicleDesc;

struct EngineConfig {
    std::string title = "MAGE v1";
    int width = 1600;
    int height = 900;
    bool vsync = true;
    double fixedHz = 60.0;
    int maxFixedStepsPerFrame = 5;
    bool editorMode = false;        // render the scene into an off-screen viewport texture
    bool enableScripting = true;
    bool enableStreaming = true;
    std::filesystem::path assetDirectory;   // empty = auto (<exe>/assets, then source tree)
    std::filesystem::path scriptDirectory;  // empty = <exe>/Scripts
};

enum class SpawnPhysics : uint8_t { None, Static, Kinematic, Dynamic };

struct SpawnDesc {
    std::string name = "Entity";
    std::string mesh = "cube";      // cube | sphere | cylinder | cone | plane
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    glm::vec3 albedo{0.8f};
    glm::vec3 emissive{0.0f};
    float metallic = 0.0f;
    float roughness = 0.6f;
    float checkerScale = 0.0f;
    bool castShadows = true;
    SpawnPhysics physics = SpawnPhysics::None;
    float mass = 0.0f;              // 0 = from volume
    float friction = 0.6f;
    float restitution = 0.1f;
};

struct FrameStats {
    double frameMs = 0.0;
    double fps = 0.0;
    int fixedStepsThisFrame = 0;
    float interpolationAlpha = 0.0f;
    uint64_t frameIndex = 0;
    double simulationTime = 0.0;
};

class Engine {
public:
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool Init(const EngineConfig& config);
    void Run();
    void Shutdown();
    void RequestQuit();
    static Engine* Get();

    // Application hooks.
    std::function<void(float)> OnUpdate;        // variable rate, after interpolation
    std::function<void(float)> OnFixedUpdate;   // 60 Hz, before the physics step
    std::function<void()> OnImGui;              // inside the ImGui frame

    // Entities.
    Entity CreateEntity(const std::string& name);
    void DestroyEntity(Entity entity);          // deferred to the end of the frame
    Entity SpawnPrimitive(const SpawnDesc& desc);
    Entity SpawnVehicle(const std::string& name, const VehicleDesc& desc, const glm::vec3& color,
                        const std::string& scriptClass = {});
    bool GetEntityTransform(Entity entity, glm::vec3& position, glm::quat& rotation) const;
    void SetEntityTransform(Entity entity, const glm::vec3& position, const glm::quat& rotation);
    void StartScripts();
    void SetStreamingFocus(Entity entity) { m_streamingFocus = entity; }
    Entity GetStreamingFocus() const { return m_streamingFocus; }

    // Subsystems.
    Registry& GetRegistry() { return m_registry; }
    const Registry& GetRegistry() const { return m_registry; }
    Window& GetWindow() { return *m_window; }
    Renderer& GetRenderer() { return *m_renderer; }
    PhysicsWorld& GetPhysics() { return *m_physics; }
    const PhysicsWorld& GetPhysics() const { return *m_physics; }
    SectorStreamer& GetStreamer() { return *m_streamer; }
    ScriptHost& GetScriptHost() { return *m_scripts; }
    Input& GetInput() { return m_input; }
    Camera& GetCamera() { return m_camera; }
    Profiler& GetProfiler() { return m_profiler; }
    const FrameStats& GetFrameStats() const { return m_stats; }
    const EngineConfig& GetConfig() const { return m_config; }
    const std::filesystem::path& AssetDirectory() const { return m_assetDirectory; }

    // Editor viewport (editorMode only).
    void SetViewportSize(int width, int height);
    int ViewportWidth() const { return m_viewportWidth; }
    int ViewportHeight() const { return m_viewportHeight; }
    uint32_t GetViewportTexture() const { return m_viewportTexture; }

    // Saves the next presented frame (scene + UI) as a 32-bit .bmp (also bound to F12).
    void RequestScreenshot(const std::filesystem::path& path) { m_screenshotPath = path; }

    // Simulation controls.
    bool simulationPaused = false;
    bool stepOnce = false;
    float timeScale = 1.0f;
    bool drawPhysicsDebug = false;
    bool drawPhysicsConstraints = true;
    bool drawPhysicsBounds = false;

private:
    void InitImGui();
    void ShutdownImGui();
    void FixedStep(float dt);
    void InterpolateTransforms(float alpha);
    void SubmitScene();
    void RenderFrame();
    void FlushDestroyQueue();
    void DestroyEntityNow(Entity entity);
    void OnSectorLoaded(const SectorData& data);
    void OnSectorUnloaded(const SectorCoord& coord);
    void WriteScreenshot(int width, int height);

    EngineConfig m_config;
    std::filesystem::path m_assetDirectory;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<Renderer> m_renderer;
    std::unique_ptr<PhysicsWorld> m_physics;
    std::unique_ptr<SectorStreamer> m_streamer;
    std::unique_ptr<ScriptHost> m_scripts;
    Registry m_registry;
    Input m_input;
    Camera m_camera;
    Profiler m_profiler;
    FrameStats m_stats;

    std::vector<Entity> m_destroyQueue;
    std::unordered_map<uint64_t, std::vector<Entity>> m_sectorEntities;
    Entity m_streamingFocus = NullEntity;
    std::filesystem::path m_screenshotPath;
    int m_screenshotCounter = 0;

    int m_viewportWidth = 1280;
    int m_viewportHeight = 720;
    uint32_t m_viewportTexture = 0;
    double m_accumulator = 0.0;
    bool m_imguiInitialized = false;
    bool m_initialized = false;
    bool m_quitRequested = false;
};

} // namespace mage
