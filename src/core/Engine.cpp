// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "core/Engine.h"
#include "animation/AnimationSystem.h"
#include "assets/AssetManager.h"
#include "audio/AudioEngine.h"
#include "core/Log.h"
#include "core/Window.h"
#include "physics/PhysicsWorld.h"
#include "renderer/GL.h"
#include "renderer/Renderer.h"
#include "scripting/ScriptHost.h"
#include "streaming/SectorStreamer.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>
#include <fstream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace mage {

namespace {

Engine* s_engine = nullptr;

std::filesystem::path ExecutableDirectory() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH * 4];
    const DWORD len = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::filesystem::path(std::wstring(buffer, len)).parent_path();
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    if (_NSGetExecutablePath(buffer, &size) == 0) return std::filesystem::path(buffer).parent_path();
    return std::filesystem::current_path();
#else
    char buffer[4096];
    const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) return std::filesystem::path(std::string(buffer, static_cast<size_t>(len))).parent_path();
    return std::filesystem::current_path();
#endif
}

uint64_t SectorKey(int32_t x, int32_t z) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z);
}

ShapeType ShapeForMesh(const std::string& mesh) {
    if (mesh == "sphere") return ShapeType::Sphere;
    if (mesh == "cylinder" || mesh == "cone") return ShapeType::Cylinder;
    return ShapeType::Box;
}

} // namespace

Engine::Engine() { s_engine = this; }

Engine::~Engine() {
    Shutdown();
    if (s_engine == this) s_engine = nullptr;
}

Engine* Engine::Get() { return s_engine; }

// ============================================================================ Init / Shutdown
bool Engine::Init(const EngineConfig& config) {
    if (m_initialized) return true;
    m_config = config;
    Log::Info("MAGE v1 - Maxdemyd0's Advanced Game Engine - starting ({})", config.editorMode ? "editor" : "runtime");

    const std::filesystem::path exeDir = ExecutableDirectory();
    std::error_code ec;
    if (!config.assetDirectory.empty()) m_assetDirectory = config.assetDirectory;
#ifdef MAGE_SOURCE_DIR
    // The editor works on the project's own assets, not on the copy next to the executable.
    else if (config.editorMode && std::filesystem::exists(std::filesystem::path(MAGE_SOURCE_DIR) / "assets", ec))
        m_assetDirectory = std::filesystem::path(MAGE_SOURCE_DIR) / "assets";
#endif
    else if (std::filesystem::exists(exeDir / "assets", ec)) m_assetDirectory = exeDir / "assets";
#ifdef MAGE_SOURCE_DIR
    else m_assetDirectory = std::filesystem::path(MAGE_SOURCE_DIR) / "assets";
#endif

    // Window + GL.
    m_window = std::make_unique<Window>();
    if (!m_window->Create(config.title, config.width, config.height, config.vsync, !config.background)) return false;
    if (!gl::LoadFunctions([](const char* name) { return reinterpret_cast<void*>(glfwGetProcAddress(name)); })) {
        Log::Error("Failed to load OpenGL functions");
        return false;
    }

    m_renderer = std::make_unique<Renderer>();
    if (!m_renderer->Init(config.enablePathTracing ? exeDir / "shaders" : std::filesystem::path{})) {
        Log::Error("Renderer initialization failed");
        return false;
    }
    // Creating the Vulkan device (driver layers / overlay hooks) can unbind the GL context on
    // this thread; make sure it is current again before anything else touches OpenGL.
    // GLFW only tracks its own bookkeeping, so query the real driver state where possible.
#if defined(_WIN32)
    const bool contextLost = wglGetCurrentContext() == nullptr;
#else
    const bool contextLost = glfwGetCurrentContext() != m_window->Handle();
#endif
    if (contextLost) Log::Warn("OpenGL context was unbound during Vulkan initialization - restoring it");
    glfwMakeContextCurrent(nullptr);
    glfwMakeContextCurrent(m_window->Handle());

    m_input.Attach(m_window->Handle());
    InitImGui();

    // Physics.
    m_physics = std::make_unique<PhysicsWorld>();
    if (!m_physics->Init()) return false;

#ifdef _WIN32
    timeBeginPeriod(1); // default is 15.6 ms, which makes the frame cap overshoot
#endif
    m_assets = std::make_unique<AssetManager>();
    m_assets->Init(m_renderer.get(), m_assetDirectory);
    m_animation = std::make_unique<AnimationSystem>();
    m_audio = std::make_unique<AudioEngine>();
    m_audio->Init(config.background); // automated runs stay silent

    // Scripting (optional: the engine keeps running without it).
    m_scripts = std::make_unique<ScriptHost>();
    if (config.enableScripting) {
        const std::filesystem::path scriptDir = config.scriptDirectory.empty() ? exeDir / "Scripts" : config.scriptDirectory;
        if (!m_scripts->Init(*this, scriptDir))
            Log::Warn("C# scripting unavailable: {}", m_scripts->LastError());
    }

    // World streaming.
    m_streamer = std::make_unique<SectorStreamer>();
    if (config.enableStreaming) {
        StreamerConfig sc;
        sc.sectorDirectory = m_assetDirectory / "sectors";
        m_streamer->Start(
            sc, [this](const SectorData& d) { OnSectorLoaded(d); }, [this](const SectorCoord& c) { OnSectorUnloaded(c); });
    }

    m_camera.position = glm::vec3(0.0f, 6.0f, -12.0f);
    m_camera.target = glm::vec3(0.0f, 1.0f, 0.0f);
    m_initialized = true;
    return true;
}

void Engine::InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Keyboard navigation would make ImGui claim WASD whenever a panel has focus; editor only.
    if (m_config.editorMode) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = m_config.editorMode ? "mage_editor.ini" : "mage_runtime.ini";

    const float scale = std::clamp(m_window->GetContentScale(), 1.0f, 3.0f);
    ImFontConfig fontConfig;
    fontConfig.SizePixels = 13.0f * scale;
    io.Fonts->AddFontDefault(&fontConfig);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.94f;
    style.Colors[ImGuiCol_Header] = ImVec4(0.55f, 0.20f, 0.18f, 0.55f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.70f, 0.26f, 0.22f, 0.80f);
    style.Colors[ImGuiCol_SliderGrab] = ImVec4(0.86f, 0.36f, 0.26f, 1.00f);
    style.Colors[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.45f, 0.30f, 1.00f);
    style.ScaleAllSizes(scale);

    if (m_config.background) io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
    ImGui_ImplGlfw_InitForOpenGL(m_window->Handle(), true);
    ImGui_ImplOpenGL3_Init("#version 410 core");
    m_imguiInitialized = true;
}

void Engine::ShutdownImGui() {
    if (!m_imguiInitialized) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    m_imguiInitialized = false;
}

void Engine::Shutdown() {
    if (!m_initialized) return;
    Log::Info("Engine shutting down");

    if (m_streamer) m_streamer->Stop();

    // Tear down every entity (scripts first, then vehicles/bodies) while subsystems still exist.
    std::vector<Entity> all;
    m_registry.ForEachEntity([&](Entity e) { all.push_back(e); });
    for (Entity e : all) DestroyEntityNow(e);
    m_registry.Clear();
    m_sectorEntities.clear();

    if (m_scripts) m_scripts->Shutdown();
#ifdef _WIN32
    if (m_assets) timeEndPeriod(1);
#endif
    if (m_audio) m_audio->Shutdown();
    if (m_physics) m_physics->Shutdown();
    ShutdownImGui();
    if (m_renderer) m_renderer->Shutdown();
    m_renderer.reset();
    if (m_window) m_window->Destroy();

    m_animation.reset();
    m_assets.reset();
    m_audio.reset();
    m_fonts.clear();
    m_streamer.reset();
    m_physics.reset();
    m_scripts.reset();
    m_window.reset();
    m_initialized = false;
}

void Engine::RequestQuit() { m_quitRequested = true; }

// ============================================================================ Main loop
void Engine::Run() {
    if (!m_initialized) return;
    using Clock = std::chrono::steady_clock;
    const double fixedDt = 1.0 / m_config.fixedHz;
    auto last = Clock::now();
    double fpsTimer = 0.0;
    int fpsFrames = 0;

    while (!m_quitRequested && !m_window->ShouldClose()) {
        const auto now = Clock::now();
        double frameDt = std::chrono::duration<double>(now - last).count();
        last = now;
        frameDt = std::min(frameDt, 0.25); // avoid the spiral of death after a hitch
        m_stats.frameMs = frameDt * 1000.0;
        fpsTimer += frameDt;
        ++fpsFrames;
        if (fpsTimer >= 0.5) {
            m_stats.fps = fpsFrames / fpsTimer;
            fpsTimer = 0.0;
            fpsFrames = 0;
        }

        MAGE_PROFILE_SCOPE(m_profiler, "Frame");

        // ---- Input -------------------------------------------------------------------
        m_input.BeginFrame();
        m_window->PollEvents();
        m_input.EndFrame();
        // Runtime: keys reach the game unless the player is typing into an ImGui text field.
        if (!m_config.editorMode && m_imguiInitialized) m_input.SetGameInputEnabled(!ImGui::GetIO().WantTextInput);
        if (m_config.background) m_input.SetGameInputEnabled(false);

        // ---- Fixed-timestep simulation -----------------------------------------------
        int steps = 0;
        {
            MAGE_PROFILE_SCOPE(m_profiler, "Simulation");
            if (!simulationPaused) m_accumulator += frameDt * static_cast<double>(timeScale);
            else if (stepOnce) m_accumulator += fixedDt;
            stepOnce = false;

            while (m_accumulator >= fixedDt && steps < m_config.maxFixedStepsPerFrame) {
                FixedStep(static_cast<float>(fixedDt));
                m_accumulator -= fixedDt;
                m_stats.simulationTime += fixedDt;
                ++steps;
            }
            if (steps == m_config.maxFixedStepsPerFrame) m_accumulator = std::min(m_accumulator, fixedDt);
        }
        m_stats.fixedStepsThisFrame = steps;
        m_stats.interpolationAlpha = static_cast<float>(m_accumulator / fixedDt);
        InterpolateTransforms(m_stats.interpolationAlpha);

        // ---- Variable-rate update ----------------------------------------------------
        {
            MAGE_PROFILE_SCOPE(m_profiler, "Scripts.Update");
            if (m_scripts->IsReady()) m_scripts->Update(static_cast<float>(frameDt));
        }
        if (OnUpdate) OnUpdate(static_cast<float>(frameDt));

        {
            MAGE_PROFILE_SCOPE(m_profiler, "Streaming");
            glm::vec3 focus = m_camera.position;
            if (const auto* t = m_registry.TryGet<TransformComponent>(m_streamingFocus)) focus = t->position;
            m_streamer->Update(focus);
        }
        FlushDestroyQueue();
        {
            MAGE_PROFILE_SCOPE(m_profiler, "Animation");
            const float animDt = simulationPaused ? static_cast<float>(steps * fixedDt) : static_cast<float>(frameDt) * timeScale;
            m_animation->Update(m_registry, *m_assets, m_renderer->Meshes(), animDt);
        }
        m_audio->Update(m_camera.position, m_camera.Forward());
        if (auto dropped = m_window->TakeDroppedFiles(); !dropped.empty() && OnFilesDropped) OnFilesDropped(dropped);

        // ---- Render --------------------------------------------------------------------
        if (m_window->IsMinimized()) {
            m_labels.clear();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }
        RenderFrame();
        ++m_stats.frameIndex;

        // Frame cap: sleep coarsely, then spin for the last fraction of a millisecond.
        if (m_config.maxFps > 0) {
            const auto target = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / m_config.maxFps));
            while (Clock::now() < target) {
                if (target - Clock::now() > std::chrono::milliseconds(2)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                else std::this_thread::yield();
            }
        }
    }
}

void Engine::FixedStep(float dt) {
    {
        MAGE_PROFILE_SCOPE(m_profiler, "Scripts.FixedUpdate");
        if (m_scripts->IsReady()) m_scripts->FixedUpdate(dt);
    }
    if (OnFixedUpdate) OnFixedUpdate(dt);

    m_registry.Each<VehicleComponent>([&](Entity, VehicleComponent& v) {
        m_physics->SetVehicleInput(v.vehicle, v.throttle, v.steering, v.brake, v.handBrake);
    });

    {
        MAGE_PROFILE_SCOPE(m_profiler, "Physics.Step");
        m_physics->Step(dt);
    }

    m_registry.Each<RigidBodyComponent>([&](Entity, RigidBodyComponent& rb) {
        if (rb.motion == BodyMotion::Static) return;
        rb.prevPosition = rb.currPosition;
        rb.prevRotation = rb.currRotation;
        m_physics->GetBodyTransform(rb.body, rb.currPosition, rb.currRotation);
    });
}

void Engine::InterpolateTransforms(float alpha) {
    m_registry.Each<RigidBodyComponent, TransformComponent>([&](Entity, RigidBodyComponent& rb, TransformComponent& t) {
        if (rb.motion == BodyMotion::Static) return;
        t.position = glm::mix(rb.prevPosition, rb.currPosition, alpha);
        t.rotation = glm::slerp(rb.prevRotation, rb.currRotation, alpha);
    });
}

// ============================================================================ Rendering
void Engine::SetViewportSize(int width, int height) {
    m_viewportWidth = std::max(width, 1);
    m_viewportHeight = std::max(height, 1);
}

void Engine::SubmitScene() {
    MAGE_PROFILE_SCOPE(m_profiler, "Render.Submit");
    Renderer& r = *m_renderer;

    m_registry.Each<MeshComponent, TransformComponent>([&](Entity, MeshComponent& m, TransformComponent& t) {
        if (!m.visible) return;
        DrawCommand cmd;
        cmd.mesh = m.mesh;
        cmd.model = t.Matrix();
        cmd.material = Material{m.albedo, m.metallic, m.roughness, m.checkerScale, m.emissive,
                                m.clearcoat, m.clearcoatRoughness, m.transmission, m.ior,
                                m.baseColorTexture, m.metallicRoughnessTexture, m.normalTexture, m.emissiveTexture};
        cmd.castShadows = m.castShadows;
        r.Submit(cmd);
    });

    m_registry.Each<ModelComponent, TransformComponent>([&](Entity e, ModelComponent& m, TransformComponent& t) {
        const Model* model = m.visible ? m_assets->GetModel(m.model) : nullptr;
        if (!model) return;
        const glm::mat4 world = t.Matrix();
        const AnimationSystem::Instance* anim = m_animation->Get(e);
        if (anim && (anim->model != m.model || anim->geometry.size() != model->parts.size())) anim = nullptr;
        for (size_t i = 0; i < model->parts.size(); ++i) {
            const Model::Part& part = model->parts[i];
            DrawCommand cmd{part.mesh, world * part.transform, part.material, m.castShadows};
            if (anim && part.skin >= 0) cmd.skinned = &anim->geometry[i];
            else if (anim && part.node >= 0) cmd.model = world * anim->globals[static_cast<size_t>(part.node)];
            r.Submit(cmd);
        }
    });

    // Vehicles: wheels (from the Jolt suspension state) + a cabin on top of the chassis.
    const MeshHandle cylinder = r.Meshes().Find("cylinder");
    const MeshHandle cube = r.Meshes().Find("cube");
    m_registry.Each<VehicleComponent, TransformComponent>([&](Entity, VehicleComponent& v, TransformComponent& t) {
        const glm::mat4 body = glm::translate(glm::mat4(1.0f), t.position) * glm::mat4_cast(t.rotation);
        const float radius = m_physics->GetWheelRadius(v.vehicle);
        const float width = m_physics->GetWheelWidth(v.vehicle);
        for (int i = 0; i < m_physics->GetWheelCount(v.vehicle); ++i) {
            const glm::mat4 wheel = body * m_physics->GetWheelLocalTransform(v.vehicle, i);
            DrawCommand tire;
            tire.mesh = cylinder;
            tire.model = glm::scale(wheel, glm::vec3(radius * 2.0f, width, radius * 2.0f));
            tire.material = Material{glm::vec3(0.035f), 0.0f, 0.9f, 0.0f, glm::vec3(0.0f)};
            r.Submit(tire);
            DrawCommand hub;
            hub.mesh = cylinder;
            hub.model = glm::scale(wheel, glm::vec3(radius * 1.1f, width * 1.04f, radius * 1.1f));
            hub.material = Material{glm::vec3(0.91f, 0.92f, 0.92f), 1.0f, 0.18f, 0.0f, glm::vec3(0.0f)};
            r.Submit(hub);
        }
        const glm::vec3 he = v.bodyHalfExtents;
        DrawCommand cabin;
        cabin.mesh = cube;
        cabin.model = glm::scale(glm::translate(body, glm::vec3(0.0f, he.y + 0.27f, -0.25f)),
                                 glm::vec3(he.x * 1.7f, 0.55f, he.z * 1.0f));
        // Tinted thin-walled glass (path traced: see-through with reflections; raster: dark).
        cabin.material = Material{glm::vec3(0.42f, 0.5f, 0.52f), 0.0f, 0.03f, 0.0f, glm::vec3(0.0f), 0.0f, 0.05f, 0.9f, 1.52f};
        r.Submit(cabin);
        // Interior (seats / dash) seen through the tinted glass.
        DrawCommand interior;
        interior.mesh = cube;
        interior.model = glm::scale(glm::translate(body, glm::vec3(0.0f, he.y + 0.12f, -0.25f)),
                                    glm::vec3(he.x * 1.6f, 0.36f, he.z * 0.92f));
        interior.material = Material{glm::vec3(0.025f, 0.024f, 0.023f), 0.0f, 0.85f, 0.0f, glm::vec3(0.0f)};
        r.Submit(interior);
        // Tail lights.
        for (float side : {-1.0f, 1.0f}) {
            DrawCommand light;
            light.mesh = cube;
            light.model = glm::scale(glm::translate(body, glm::vec3(side * he.x * 0.7f, he.y * 0.3f, -he.z - 0.01f)),
                                     glm::vec3(0.35f, 0.12f, 0.04f));
            const bool braking = v.brake > 0.0f || v.throttle < 0.0f;
            light.material = Material{glm::vec3(0.6f, 0.02f, 0.02f), 0.0f, 0.3f, 0.0f,
                                      glm::vec3(braking ? 1.2f : 0.25f, braking ? 0.02f : 0.004f, 0.0f), 1.0f, 0.02f};
            light.castShadows = false;
            r.Submit(light);
        }
    });
}

void Engine::RenderFrame() {
    int fbWidth = 0, fbHeight = 0;
    m_window->GetFramebufferSize(fbWidth, fbHeight);
    if (fbWidth <= 0 || fbHeight <= 0) return;

    int width = fbWidth, height = fbHeight;
    uint32_t target = 0;
    if (m_config.editorMode) {
        RenderTarget& rt = m_renderer->GetViewportTarget(m_viewportWidth, m_viewportHeight);
        width = rt.width;
        height = rt.height;
        target = rt.fbo;
        m_viewportTexture = rt.color;
    }

    {
        MAGE_PROFILE_SCOPE(m_profiler, "Render.Scene");
        m_renderer->BeginFrame(m_camera, width, height);
        SubmitScene();
        if (OnRender) OnRender();
        if (drawPhysicsDebug) {
            MAGE_PROFILE_SCOPE(m_profiler, "Render.PhysicsDebug");
            PhysicsWorld::DebugDrawSettings dds;
            dds.constraints = drawPhysicsConstraints;
            dds.boundingBoxes = drawPhysicsBounds;
            m_physics->DebugDraw(*m_renderer, m_camera.position, dds);
        }
        m_renderer->EndFrame(target);
    }

    {
        MAGE_PROFILE_SCOPE(m_profiler, "Render.ImGui");
        using namespace gl;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, fbWidth, fbHeight);
        if (m_config.editorMode) {
            glClearColor(0.08f, 0.08f, 0.09f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        }
        BuildPendingFonts();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        if (OnImGui) OnImGui();
        DrawLabels();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    if (m_input.IsKeyPressedRaw(GLFW_KEY_F12))
        m_screenshotPath = std::format("mage_screenshot_{:03}.bmp", m_screenshotCounter++);
    if (!m_screenshotPath.empty()) WriteScreenshot(fbWidth, fbHeight);

    m_window->SwapBuffers();
}

// ============================================================================ Animation
bool Engine::PlayAnimation(Entity e, const std::string& clip, float fade, bool loop, float speed) {
    auto* a = m_registry.TryGet<AnimatorComponent>(e);
    const auto* mc = m_registry.TryGet<ModelComponent>(e);
    const Model* model = a && mc ? m_assets->GetModel(mc->model) : nullptr;
    return model && PlayClip(*a, model->rig, model->rig.FindClip(clip), fade, loop, speed);
}

int Engine::LoadAnimations(Entity e, const std::string& path) {
    const auto* mc = m_registry.TryGet<ModelComponent>(e);
    return mc ? m_assets->LoadAnimations(mc->model, path) : -1;
}

bool Engine::GetBoneTransform(Entity e, const std::string& bone, glm::mat4& world) const {
    const auto* mc = m_registry.TryGet<ModelComponent>(e);
    const auto* t = m_registry.TryGet<TransformComponent>(e);
    const Model* model = mc && t ? m_assets->GetModel(mc->model) : nullptr;
    const int node = model ? model->rig.FindNode(bone) : -1;
    if (node < 0) return false;
    const AnimationSystem::Instance* anim = m_animation->Get(e);
    if (anim && anim->model == mc->model) {
        world = t->Matrix() * anim->globals[static_cast<size_t>(node)];
    } else {
        std::vector<glm::mat4> globals;
        ComputeGlobals(model->rig, model->rig.RestPose(), globals);
        world = t->Matrix() * globals[static_cast<size_t>(node)];
    }
    return true;
}

// ============================================================================ Fonts & text
int Engine::LoadFont(const std::string& path, float size) {
    size = std::clamp(size, 4.0f, 256.0f);
    const std::filesystem::path file = m_assets->Resolve(path);
    const std::string key = std::format("{}@{:.1f}", PathToUtf8(file), size);
    for (size_t i = 0; i < m_fonts.size(); ++i)
        if (m_fonts[i].key == key) return static_cast<int>(i) + 1;
    std::string error;
    if (!ValidateFont(file, error)) {
        Log::Error("Font '{}': {}", path, error);
        return 0;
    }
    m_fonts.push_back({key, file, size, nullptr});
    m_fontsDirty = true;
    return static_cast<int>(m_fonts.size());
}

void Engine::DrawLabel(const std::string& text, const glm::vec2& position, float size, const glm::vec4& color, int font) {
    m_labels.push_back({text, position, size, color, font});
}

void Engine::BuildPendingFonts() { // must run outside an ImGui frame
    if (!m_fontsDirty) return;
    m_fontsDirty = false;
    // Latin, Greek, Cyrillic and general punctuation.
    static const ImWchar ranges[] = {0x0020, 0x024F, 0x0370, 0x03FF, 0x0400, 0x052F, 0x2010, 0x205E, 0x20A0, 0x20BF, 0};
    ImGuiIO& io = ImGui::GetIO();
    for (FontEntry& f : m_fonts) {
        if (f.font) continue;
        std::vector<uint8_t> bytes;
        if (!ReadFileBytes(f.file, bytes) || bytes.empty()) continue;
        void* copy = ImGui::MemAlloc(bytes.size()); // the atlas owns and frees this
        std::memcpy(copy, bytes.data(), bytes.size());
        f.font = io.Fonts->AddFontFromMemoryTTF(copy, static_cast<int>(bytes.size()), f.size, nullptr, ranges);
    }
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    if (!io.Fonts->Build() || !ImGui_ImplOpenGL3_CreateFontsTexture()) Log::Error("Font atlas could not be built");
}

void Engine::DrawLabels() {
    if (m_labels.empty()) return;
    ImDrawList* dl = m_config.editorMode ? ImGui::GetForegroundDrawList() : ImGui::GetBackgroundDrawList();
    const ImVec2 o = m_config.editorMode ? ImVec2(m_viewportOrigin.x, m_viewportOrigin.y) : ImGui::GetMainViewport()->Pos;
    if (m_config.editorMode) dl->PushClipRect(o, ImVec2(o.x + static_cast<float>(m_viewportWidth), o.y + static_cast<float>(m_viewportHeight)), true);
    for (const Label& l : m_labels) {
        auto* font = l.font > 0 && l.font <= static_cast<int>(m_fonts.size()) ? static_cast<ImFont*>(m_fonts[static_cast<size_t>(l.font) - 1].font) : nullptr;
        dl->AddText(font ? font : ImGui::GetFont(), l.size, ImVec2(o.x + l.position.x, o.y + l.position.y),
                    ImGui::ColorConvertFloat4ToU32(ImVec4(l.color.r, l.color.g, l.color.b, l.color.a)), l.text.c_str());
    }
    if (m_config.editorMode) dl->PopClipRect();
    m_labels.clear();
}

void Engine::WriteScreenshot(int width, int height) {
    using namespace gl;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    for (size_t i = 0; i < pixels.size(); i += 4) {
        std::swap(pixels[i], pixels[i + 2]); // RGBA -> BGRA
        pixels[i + 3] = 255;
    }

    // BMP rows are bottom-up, exactly like glReadPixels.
    const uint32_t dataSize = static_cast<uint32_t>(pixels.size());
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](int offset, uint32_t v) { for (int i = 0; i < 4; ++i) header[offset + i] = static_cast<uint8_t>(v >> (8 * i)); };
    put32(2, 54 + dataSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<uint32_t>(width));
    put32(22, static_cast<uint32_t>(height));
    header[26] = 1;
    header[28] = 32;
    put32(34, dataSize);

    std::ofstream out(m_screenshotPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(header), sizeof(header));
    out.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    if (out) Log::Info("Screenshot saved: {}", m_screenshotPath.string());
    else Log::Error("Could not write screenshot {}", m_screenshotPath.string());
    m_screenshotPath.clear();
}

// ============================================================================ Entities
Entity Engine::CreateEntity(const std::string& name) {
    const Entity e = m_registry.Create();
    m_registry.Add<NameComponent>(e).name = name;
    return e;
}

void Engine::DestroyEntity(Entity entity) {
    if (m_registry.Valid(entity) && std::find(m_destroyQueue.begin(), m_destroyQueue.end(), entity) == m_destroyQueue.end())
        m_destroyQueue.push_back(entity);
}

void Engine::FlushDestroyQueue() {
    // Swap first: destroying a script may queue more destructions.
    while (!m_destroyQueue.empty()) {
        std::vector<Entity> queue;
        queue.swap(m_destroyQueue);
        for (Entity e : queue) DestroyEntityNow(e);
    }
}

void Engine::DestroyEntityNow(Entity e) {
    if (!m_registry.Valid(e)) return;
    if (auto* s = m_registry.TryGet<ScriptComponent>(e); s && s->instantiated) m_scripts->DestroyInstance(e);
    if (auto* v = m_registry.TryGet<VehicleComponent>(e)) m_physics->DestroyVehicle(v->vehicle);
    if (auto* rb = m_registry.TryGet<RigidBodyComponent>(e)) m_physics->DestroyBody(rb->body);
    if (m_streamingFocus == e) m_streamingFocus = NullEntity;
    m_registry.Destroy(e);
}

Entity Engine::SpawnPrimitive(const SpawnDesc& d) {
    ModelHandle modelHandle = InvalidModel;
    if (!d.model.empty()) {
        modelHandle = m_assets->LoadModel(d.model);
        if (modelHandle == InvalidModel) return NullEntity;
    }
    const Model* model = m_assets->GetModel(modelHandle);

    const Entity e = CreateEntity(d.name);
    auto& t = m_registry.Add<TransformComponent>(e);
    t.position = d.position;
    t.rotation = glm::normalize(d.rotation);
    t.scale = d.scale;

    if (model) {
        auto& mc = m_registry.Add<ModelComponent>(e);
        mc.model = modelHandle;
        mc.castShadows = d.castShadows;
        for (const std::string& file : d.animationFiles) m_assets->LoadAnimations(modelHandle, file);
        if (!model->rig.Empty()) {
            auto& a = m_registry.Add<AnimatorComponent>(e);
            int clip = model->rig.FindClip(d.animation);
            if (d.animation.empty()) {
                clip = model->rig.FindClip("idle");
                if (clip < 0) clip = 0; // PlayClip ignores it when there are no clips
            } else if (clip < 0 && d.animation != "none") {
                Log::Warn("Model '{}' has no animation '{}'", d.model, d.animation);
            }
            PlayClip(a, model->rig, clip, 0.0f, true, 1.0f);
        }
    } else {
    auto& m = m_registry.Add<MeshComponent>(e);
    m.mesh = m_renderer->Meshes().Find(d.mesh);
    if (m.mesh == InvalidMesh) m.mesh = m_renderer->Meshes().Find("cube");
    m.albedo = d.albedo;
    m.emissive = d.emissive;
    m.metallic = d.metallic;
    m.roughness = d.roughness;
    m.checkerScale = d.checkerScale;
    m.clearcoat = d.clearcoat;
    m.clearcoatRoughness = d.clearcoatRoughness;
    m.transmission = d.transmission;
    m.ior = d.ior;
    m.castShadows = d.castShadows;
    if (!d.texture.empty()) m.baseColorTexture = m_assets->LoadTexture(d.texture);
    }

    if (d.physics != SpawnPhysics::None) {
        BodyDesc b;
        b.shape = ShapeForMesh(d.mesh);
        const glm::vec3 s = glm::abs(d.scale);
        if (model) {
            b.shape = d.physics == SpawnPhysics::Static ? ShapeType::Mesh : ShapeType::ConvexHull;
            b.points.reserve(model->collisionVertices.size());
            for (const glm::vec3& p : model->collisionVertices) b.points.push_back(p * d.scale);
            if (b.shape == ShapeType::Mesh) b.triangles = model->collisionIndices;
        } else if (b.shape == ShapeType::Sphere) b.halfExtents = glm::vec3(std::max({s.x, s.y, s.z}) * 0.5f);
        else if (b.shape == ShapeType::Cylinder) b.halfExtents = glm::vec3(std::max(s.x, s.z) * 0.5f, s.y * 0.5f, 0.0f);
        else b.halfExtents = s * 0.5f;
        b.position = t.position;
        b.rotation = t.rotation;
        b.mass = d.mass;
        b.friction = d.friction;
        b.restitution = d.restitution;
        b.userData = e;
        BodyMotion motion = BodyMotion::Static;
        switch (d.physics) {
            case SpawnPhysics::Dynamic: b.motion = MotionType::Dynamic; b.layer = ObjectLayer::DynamicProp; motion = BodyMotion::Dynamic; break;
            case SpawnPhysics::Kinematic: b.motion = MotionType::Kinematic; b.layer = ObjectLayer::DynamicProp; motion = BodyMotion::Kinematic; break;
            default: b.motion = MotionType::Static; b.layer = ObjectLayer::StaticWorld; break;
        }
        auto& rb = m_registry.Add<RigidBodyComponent>(e);
        rb.body = m_physics->CreateBody(b);
        rb.motion = motion;
        rb.prevPosition = rb.currPosition = t.position;
        rb.prevRotation = rb.currRotation = t.rotation;
    }
    return e;
}

Entity Engine::SpawnVehicle(const std::string& name, const VehicleDesc& desc, const glm::vec3& color,
                            const std::string& scriptClass) {
    const Entity e = CreateEntity(name);
    auto& t = m_registry.Add<TransformComponent>(e);
    t.position = desc.position;
    t.rotation = glm::normalize(desc.rotation);
    t.scale = desc.halfExtents * 2.0f;

    auto& m = m_registry.Add<MeshComponent>(e);
    m.mesh = m_renderer->Meshes().Find("cube");
    m.albedo = color;
    // Metallic-flake paint under a glossy clearcoat.
    m.metallic = 0.6f;
    m.roughness = 0.38f;
    m.clearcoat = 1.0f;
    m.clearcoatRoughness = 0.03f;

    BodyHandle body = InvalidBody;
    const VehicleHandle vh = m_physics->CreateRaycastVehicle(desc, e, body);
    auto& rb = m_registry.Add<RigidBodyComponent>(e);
    rb.body = body;
    rb.motion = BodyMotion::Dynamic;
    rb.prevPosition = rb.currPosition = t.position;
    rb.prevRotation = rb.currRotation = t.rotation;

    auto& v = m_registry.Add<VehicleComponent>(e);
    v.vehicle = vh;
    v.bodyHalfExtents = desc.halfExtents;

    if (!scriptClass.empty()) m_registry.Add<ScriptComponent>(e).className = scriptClass;
    return e;
}

bool Engine::GetEntityTransform(Entity e, glm::vec3& position, glm::quat& rotation) const {
    if (const auto* rb = m_registry.TryGet<RigidBodyComponent>(e); rb && rb->motion != BodyMotion::Static) {
        position = rb->currPosition; // latest simulated pose (not the render interpolation)
        rotation = rb->currRotation;
        return true;
    }
    if (const auto* t = m_registry.TryGet<TransformComponent>(e)) {
        position = t->position;
        rotation = t->rotation;
        return true;
    }
    return false;
}

void Engine::SetEntityTransform(Entity e, const glm::vec3& position, const glm::quat& rotation) {
    const glm::quat r = glm::normalize(rotation);
    if (auto* t = m_registry.TryGet<TransformComponent>(e)) {
        t->position = position;
        t->rotation = r;
    }
    if (auto* rb = m_registry.TryGet<RigidBodyComponent>(e)) {
        m_physics->SetBodyTransform(rb->body, position, r, true);
        rb->prevPosition = rb->currPosition = position;
        rb->prevRotation = rb->currRotation = r;
    }
}

void Engine::StartScripts() {
    m_registry.Each<ScriptComponent>([&](Entity e, ScriptComponent& s) {
        if (s.instantiated || !m_scripts->IsReady()) return;
        s.instantiated = m_scripts->CreateInstance(e, s.className);
    });
}

// ============================================================================ Streaming
void Engine::OnSectorLoaded(const SectorData& data) {
    MAGE_PROFILE_SCOPE(m_profiler, "Streaming.Integrate");
    auto& list = m_sectorEntities[SectorKey(data.coord.x, data.coord.z)];
    const glm::vec3 origin = m_streamer->SectorCenter(data.coord);
    for (const SectorEntityDesc& d : data.entities) {
        SpawnDesc s;
        s.name = d.name;
        s.mesh = d.mesh;
        s.position = origin + d.position;
        s.rotation = glm::quat(glm::radians(d.rotationDeg));
        s.scale = d.scale;
        s.albedo = d.color;
        s.emissive = d.emissive;
        s.metallic = d.metallic;
        s.roughness = d.roughness;
        s.clearcoat = d.clearcoat;
        s.transmission = d.transmission;
        s.ior = d.ior;
        s.model = d.model;
        s.texture = d.texture;
        s.animation = d.animation;
        s.animationFiles = d.animationFiles;
        s.mass = d.mass;
        s.physics = d.physics == SectorPhysics::Dynamic ? SpawnPhysics::Dynamic
                  : d.physics == SectorPhysics::Static  ? SpawnPhysics::Static
                                                        : SpawnPhysics::None;
        const Entity e = SpawnPrimitive(s);
        if (e == NullEntity) continue;
        m_registry.Add<SectorComponent>(e, SectorComponent{data.coord.x, data.coord.z});
        list.push_back(e);
    }
}

void Engine::OnSectorUnloaded(const SectorCoord& coord) {
    auto it = m_sectorEntities.find(SectorKey(coord.x, coord.z));
    if (it == m_sectorEntities.end()) return;
    for (Entity e : it->second) DestroyEntity(e);
    m_sectorEntities.erase(it);
}

} // namespace mage
