// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
// =============================================================================
//  MAGE v1 Editor - docked ImGui viewport + live entity / physics inspector
//
//  Panels: Viewport (scene rendered to texture, click-to-select via physics raycast),
//  Hierarchy, Inspector, Physics, Vehicle Tuning, Streaming, Renderer, Profiler, Console.
// =============================================================================
#include "animation/AnimationSystem.h"
#include "assets/AssetManager.h"
#include "audio/AudioEngine.h"
#include "core/Engine.h"
#include "navigation/NavWorld.h"
#include "core/Log.h"
#include "physics/PhysicsWorld.h"
#include "renderer/Renderer.h"
#include "scene/Testbed.h"
#include "scripting/ScriptHost.h"
#include "streaming/SectorStreamer.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace mage;

namespace {

class Editor {
public:
    Editor(Engine& engine, Testbed& testbed) : m_engine(engine), m_testbed(testbed) {}

    void Update(float dt) {
        // Game input only goes to the scene while the viewport is focused.
        m_engine.GetInput().SetGameInputEnabled(m_viewportFocused);
        if (m_viewportFocused && m_engine.GetInput().IsKeyPressedRaw(GLFW_KEY_DELETE)) DeleteSelected();
        m_testbed.Update(m_engine, dt, m_viewportHovered);
        if (m_previewFont) m_engine.DrawLabel("The quick brown fox 0123456789", {20.0f, 40.0f}, 32.0f, {1, 1, 1, 1}, m_previewFont);

        m_frameTimes[m_frameTimeOffset] = static_cast<float>(m_engine.GetFrameStats().frameMs);
        m_frameTimeOffset = (m_frameTimeOffset + 1) % m_frameTimes.size();
    }

    // Files dragged onto the window are copied into the asset folder.
    void ImportFiles(const std::vector<std::filesystem::path>& files) {
        for (const auto& f : files) {
            std::string error;
            const std::string path = m_engine.GetAssets().Import(f, error);
            if (path.empty()) Log::Error("Import of '{}' failed: {}", PathToUtf8(f.filename()), error);
            m_importStatus = path.empty() ? "Failed: " + error : "Imported " + path;
        }
        m_assetsDirty = true;
    }

    void Draw() {
        DrawMenuBar();
        const ImGuiID dockspace = ImGui::GetID("MAGE_DockSpace");
        if (!m_layoutChecked) {
            m_layoutChecked = true;
            if (ImGui::DockBuilderGetNode(dockspace) == nullptr) BuildDefaultLayout(dockspace);
        }
        ImGui::DockSpaceOverViewport(dockspace, ImGui::GetMainViewport());

        DrawViewport();
        if (m_showHierarchy) DrawHierarchy();
        if (m_showInspector) DrawInspector();
        if (m_showPhysics) DrawPhysicsPanel();
        if (m_showTuning) m_testbed.DrawTuningWindow(m_engine, &m_showTuning);
        if (m_showStreaming) DrawStreamingPanel();
        if (m_showRenderer) DrawRendererPanel();
        if (m_showPathTracer) m_testbed.DrawPathTracerWindow(m_engine, &m_showPathTracer);
        if (m_showProfiler) DrawProfiler();
        if (m_showConsole) DrawConsole();
        if (m_showAssets) DrawAssets();
        if (m_showDemo) ImGui::ShowDemoWindow(&m_showDemo);
    }

private:
    // ------------------------------------------------------------------ layout
    void BuildDefaultLayout(ImGuiID dockspace) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::DockBuilderRemoveNode(dockspace);
        ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspace, vp->WorkSize);

        ImGuiID main = dockspace;
        const ImGuiID left = ImGui::DockBuilderSplitNode(main, ImGuiDir_Left, 0.17f, nullptr, &main);
        const ImGuiID right = ImGui::DockBuilderSplitNode(main, ImGuiDir_Right, 0.26f, nullptr, &main);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(main, ImGuiDir_Down, 0.26f, nullptr, &main);
        ImGuiID rightBottom = 0;
        const ImGuiID rightTop = ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.55f, nullptr, &rightBottom);

        ImGui::DockBuilderDockWindow("Viewport", main);
        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", rightTop);
        ImGui::DockBuilderDockWindow("Vehicle Tuning", rightBottom);
        ImGui::DockBuilderDockWindow("Physics", rightBottom);
        ImGui::DockBuilderDockWindow("Path Tracer", rightBottom);
        ImGui::DockBuilderDockWindow("Assets", bottom);
        ImGui::DockBuilderDockWindow("Console", bottom);
        ImGui::DockBuilderDockWindow("Profiler", bottom);
        ImGui::DockBuilderDockWindow("Streaming", bottom);
        ImGui::DockBuilderDockWindow("Renderer", bottom);
        ImGui::DockBuilderFinish(dockspace);
    }

    void DrawMenuBar() {
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Quit", "Alt+F4")) m_engine.RequestQuit();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Simulation")) {
            ImGui::MenuItem("Paused", "P", &m_engine.simulationPaused);
            if (ImGui::MenuItem("Step one tick", nullptr, false, m_engine.simulationPaused)) m_engine.stepOnce = true;
            ImGui::SliderFloat("Time scale", &m_engine.timeScale, 0.05f, 2.0f, "%.2fx");
            ImGui::Separator();
            if (ImGui::MenuItem("Reset vehicle")) m_testbed.ResetVehicle(m_engine);
            if (ImGui::MenuItem("Spawn crate at camera")) SpawnAtCamera("cube");
            if (ImGui::MenuItem("Spawn ball at camera")) SpawnAtCamera("sphere");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Hierarchy", nullptr, &m_showHierarchy);
            ImGui::MenuItem("Inspector", nullptr, &m_showInspector);
            ImGui::MenuItem("Physics", nullptr, &m_showPhysics);
            ImGui::MenuItem("Vehicle Tuning", nullptr, &m_showTuning);
            ImGui::MenuItem("Streaming", nullptr, &m_showStreaming);
            ImGui::MenuItem("Renderer", nullptr, &m_showRenderer);
            ImGui::MenuItem("Path Tracer", nullptr, &m_showPathTracer);
            ImGui::MenuItem("Profiler", nullptr, &m_showProfiler);
            ImGui::MenuItem("Console", nullptr, &m_showConsole);
            ImGui::MenuItem("Assets", nullptr, &m_showAssets);
            ImGui::Separator();
            ImGui::MenuItem("Physics wireframe", "F1", &m_engine.drawPhysicsDebug);
            ImGui::MenuItem("Navigation mesh", "F4", &m_engine.drawNavMesh);
            bool fly = m_testbed.cameraMode == Testbed::CameraMode::Fly;
            if (ImGui::MenuItem("Free-fly camera", "F2", &fly)) {
                m_testbed.cameraMode = fly ? Testbed::CameraMode::Fly : Testbed::CameraMode::Chase;
                if (fly) m_testbed.flyCamera.SyncFrom(m_engine.GetCamera());
            }
            ImGui::MenuItem("ImGui demo", nullptr, &m_showDemo);
            ImGui::EndMenu();
        }
        ImGui::TextDisabled("  |  %.0f FPS  |  %s  |  %s", m_engine.GetFrameStats().fps,
                            m_engine.simulationPaused ? "PAUSED" : "Simulating",
                            m_engine.GetScriptHost().IsReady() ? "C# .NET scripting" : "native controllers");
        ImGui::EndMainMenuBar();
    }

    // ------------------------------------------------------------------ viewport
    void DrawViewport() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Viewport");
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        m_engine.SetViewportSize(static_cast<int>(std::max(avail.x, 1.0f)), static_cast<int>(std::max(avail.y, 1.0f)));

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        m_engine.SetViewportOrigin({origin.x, origin.y});
        if (const uint32_t tex = m_engine.GetViewportTexture())
            ImGui::Image((ImTextureID)(uintptr_t)tex, avail, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        m_viewportHovered = ImGui::IsItemHovered();
        if (m_viewportHovered && (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)))
            ImGui::SetWindowFocus();
        m_viewportFocused = ImGui::IsWindowFocused();

        // Click-to-select: ray from the camera through the mouse cursor.
        if (m_viewportHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && avail.x > 0 && avail.y > 0) {
            const ImVec2 mouse = ImGui::GetMousePos();
            const float nx = (mouse.x - origin.x) / avail.x * 2.0f - 1.0f;
            const float ny = 1.0f - (mouse.y - origin.y) / avail.y * 2.0f;
            const glm::mat4 inv = glm::inverse(m_engine.GetRenderer().ViewProjection());
            glm::vec4 a = inv * glm::vec4(nx, ny, -1.0f, 1.0f);
            glm::vec4 b = inv * glm::vec4(nx, ny, 1.0f, 1.0f);
            const glm::vec3 from = glm::vec3(a) / a.w, to = glm::vec3(b) / b.w;
            RaycastHit hit;
            if (m_engine.GetPhysics().Raycast(from, glm::normalize(to - from), 2000.0f, hit))
                m_selected = static_cast<Entity>(hit.userData);
        }

        // Overlay.
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.0f, origin.y + 8.0f));
        ImGui::BeginGroup();
        if (ImGui::Button(m_engine.simulationPaused ? "Play" : "Pause")) m_engine.simulationPaused = !m_engine.simulationPaused;
        ImGui::SameLine();
        if (ImGui::Button("Step")) {
            m_engine.simulationPaused = true;
            m_engine.stepOnce = true;
        }
        ImGui::SameLine();
        ImGui::Checkbox("Wireframe", &m_engine.drawPhysicsDebug);
        ImGui::SameLine();
        const RenderStats& rs = m_engine.GetRenderer().Stats();
        ImGui::Text("%dx%d  draws %u  culled %u  %s", m_engine.ViewportWidth(), m_engine.ViewportHeight(), rs.drawn, rs.culled,
                    m_viewportFocused ? "[input captured: WASD drive, RMB orbit]" : "[click to focus]");
        ImGui::EndGroup();

        ImGui::End();
        ImGui::PopStyleVar();
    }

    // ------------------------------------------------------------------ hierarchy
    void DrawHierarchy() {
        if (!ImGui::Begin("Hierarchy", &m_showHierarchy)) {
            ImGui::End();
            return;
        }
        Registry& reg = m_engine.GetRegistry();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##filter", "Filter entities...", m_filter, sizeof(m_filter));
        ImGui::Checkbox("Hide streamed sector entities", &m_hideSectorEntities);
        ImGui::TextDisabled("%zu entities", reg.AliveCount());
        ImGui::Separator();

        ImGui::BeginChild("##entities");
        reg.ForEachEntity([&](Entity e) {
            if (m_hideSectorEntities && reg.Has<SectorComponent>(e)) return;
            const auto* name = reg.TryGet<NameComponent>(e);
            const char* label = name ? name->name.c_str() : "Entity";
            if (m_filter[0] && !ContainsCaseInsensitive(label, m_filter)) return;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s%s##%u", reg.Has<ScriptComponent>(e) ? "[C#] " : "", label, e);
            if (ImGui::Selectable(buf, m_selected == e)) m_selected = e;
        });
        ImGui::EndChild();
        ImGui::End();
    }

    // ------------------------------------------------------------------ inspector
    void DrawInspector() {
        if (!ImGui::Begin("Inspector", &m_showInspector)) {
            ImGui::End();
            return;
        }
        Registry& reg = m_engine.GetRegistry();
        if (!reg.Valid(m_selected)) {
            ImGui::TextDisabled("Select an entity in the Hierarchy or click it in the Viewport.");
            ImGui::End();
            return;
        }
        const Entity e = m_selected;
        ImGui::Text("Entity %u  (index %u, gen %u)", e, EntityIndex(e), EntityGeneration(e));

        if (auto* name = reg.TryGet<NameComponent>(e)) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", name->name.c_str());
            if (ImGui::InputText("Name", buf, sizeof(buf))) name->name = buf;
        }

        auto* rb = reg.TryGet<RigidBodyComponent>(e);
        if (auto* t = reg.TryGet<TransformComponent>(e); t && ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
            glm::vec3 pos = t->position;
            glm::vec3 euler = glm::degrees(glm::eulerAngles(t->rotation));
            bool moved = ImGui::DragFloat3("Position", &pos.x, 0.05f);
            moved |= ImGui::DragFloat3("Rotation", &euler.x, 0.5f);
            if (moved) m_engine.SetEntityTransform(e, pos, glm::quat(glm::radians(euler)));
            if (!rb) ImGui::DragFloat3("Scale", &t->scale.x, 0.02f, 0.01f, 1000.0f);
            else ImGui::TextDisabled("Scale %.2f %.2f %.2f (fixed by physics shape)", t->scale.x, t->scale.y, t->scale.z);
        }

        if (auto* m = reg.TryGet<MeshComponent>(e); m && ImGui::CollapsingHeader("Mesh / PBR Material", ImGuiTreeNodeFlags_DefaultOpen)) {
            const auto& meshes = m_engine.GetRenderer().Meshes().All();
            if (ImGui::BeginCombo("Mesh", m->mesh < meshes.size() ? meshes[m->mesh].name.c_str() : "?")) {
                for (uint32_t i = 0; i < meshes.size(); ++i)
                    if (ImGui::Selectable(meshes[i].name.c_str(), m->mesh == i)) m->mesh = i;
                ImGui::EndCombo();
            }
            ImGui::ColorEdit3("Albedo", &m->albedo.x);
            ImGui::SliderFloat("Metallic", &m->metallic, 0.0f, 1.0f);
            ImGui::SliderFloat("Roughness", &m->roughness, 0.02f, 1.0f);
            ImGui::ColorEdit3("Emissive", &m->emissive.x, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
            ImGui::SliderFloat("Clearcoat", &m->clearcoat, 0.0f, 1.0f);
            ImGui::SliderFloat("Clearcoat roughness", &m->clearcoatRoughness, 0.02f, 1.0f);
            ImGui::SliderFloat("Transmission (glass)", &m->transmission, 0.0f, 1.0f);
            ImGui::SliderFloat("IOR", &m->ior, 1.0f, 2.5f);
            ImGui::DragFloat("Checker (m)", &m->checkerScale, 0.1f, 0.0f, 100.0f);
            ImGui::Checkbox("Visible", &m->visible);
            ImGui::SameLine();
            ImGui::Checkbox("Cast shadows", &m->castShadows);
        }

        if (auto* mc = reg.TryGet<ModelComponent>(e); mc && ImGui::CollapsingHeader("Model", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (const Model* model = m_engine.GetAssets().GetModel(mc->model)) {
                const glm::vec3 size = model->bounds.max - model->bounds.min;
                ImGui::Text("%s: %zu parts, %zu triangles", model->name.c_str(), model->parts.size(), model->triangles);
                ImGui::Text("Size %.2f x %.2f x %.2f m", size.x, size.y, size.z);
            }
            ImGui::Checkbox("Visible##model", &mc->visible);
            ImGui::SameLine();
            ImGui::Checkbox("Cast shadows##model", &mc->castShadows);
        }

        if (auto* c = reg.TryGet<CharacterComponent>(e); c && ImGui::CollapsingHeader("Character", ImGuiTreeNodeFlags_DefaultOpen)) {
            const glm::vec3 v = c->velocity;
            ImGui::Text("%s  %.2f m/s  (%.2f x %.2f m)", c->character == InvalidCharacter ? "disabled" : c->knockedDown > 0.0f ? "knocked down" : c->grounded ? "grounded" : c->sliding ? "sliding" : "in the air",
                        glm::length(v), c->radius * 2.0f, c->height);
            ImGui::SliderFloat("Jump speed", &c->jumpSpeed, 0.0f, 12.0f);
            ImGui::SliderFloat("Acceleration", &c->acceleration, 1.0f, 80.0f);
            ImGui::SliderFloat("Air acceleration", &c->airAcceleration, 0.0f, 30.0f);
            ImGui::SliderFloat("Knockdown (m/s)", &c->knockdownSpeed, 1.0f, 30.0f);
            ImGui::Checkbox("Animate from movement", &c->autoAnimate);
        }

        if (auto* a = reg.TryGet<AnimatorComponent>(e); a && ImGui::CollapsingHeader("Animator", ImGuiTreeNodeFlags_DefaultOpen)) DrawAnimator(e, *a);

        if (rb && ImGui::CollapsingHeader("Rigid Body (Jolt)", ImGuiTreeNodeFlags_DefaultOpen)) {
            PhysicsWorld& physics = m_engine.GetPhysics();
            static const char* kMotion[] = {"Static", "Kinematic", "Dynamic"};
            ImGui::Text("Body 0x%08X  motion %s  %s", rb->body, kMotion[static_cast<int>(rb->motion)],
                        physics.IsActive(rb->body) ? "ACTIVE" : "sleeping");
            if (rb->motion == BodyMotion::Dynamic) {
                const glm::vec3 v = physics.GetLinearVelocity(rb->body);
                const glm::vec3 w = physics.GetAngularVelocity(rb->body);
                ImGui::Text("Mass %.1f kg", physics.GetMass(rb->body));
                ImGui::Text("Linear  %.2f %.2f %.2f  (%.1f m/s)", v.x, v.y, v.z, glm::length(v));
                ImGui::Text("Angular %.2f %.2f %.2f", w.x, w.y, w.z);
                if (ImGui::Button("Kick up")) physics.AddImpulse(rb->body, glm::vec3(0.0f, 8.0f, 0.0f) * physics.GetMass(rb->body));
                ImGui::SameLine();
                if (ImGui::Button("Stop")) physics.SetBodyTransform(rb->body, rb->currPosition, rb->currRotation, true);
            }
        }

        if (auto* v = reg.TryGet<VehicleComponent>(e); v && ImGui::CollapsingHeader("Vehicle", ImGuiTreeNodeFlags_DefaultOpen)) {
            const VehicleTelemetry t = m_engine.GetPhysics().GetVehicleTelemetry(v->vehicle);
            ImGui::Text("Handle %u   %.1f km/h   gear %d   %.0f rpm", v->vehicle, t.speedKmh, t.gear, t.engineRPM);
            ImGui::Text("Input: throttle %+.2f steer %+.2f brake %.2f hand %.2f", v->throttle, v->steering, v->brake, v->handBrake);
            ImGui::TextDisabled("Tune live in the 'Vehicle Tuning' panel.");
        }

        if (auto* s = reg.TryGet<ScriptComponent>(e); s && ImGui::CollapsingHeader("C# Script", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("Class: %s", s->className.c_str());
            ImGui::Text("State: %s", s->instantiated ? "running (OnCreate called)" : "not instantiated");
        }

        if (auto* sc = reg.TryGet<SectorComponent>(e)) ImGui::TextDisabled("Streamed from sector (%d, %d)", sc->x, sc->z);

        ImGui::Separator();
        if (ImGui::Button("Delete entity")) DeleteSelected();
        ImGui::End();
    }

    // ------------------------------------------------------------------ physics
    void DrawPhysicsPanel() {
        if (!ImGui::Begin("Physics", &m_showPhysics)) {
            ImGui::End();
            return;
        }
        PhysicsWorld& physics = m_engine.GetPhysics();
        const PhysicsStats s = physics.GetStats();
        ImGui::Text("Jolt Physics - %d worker threads, fixed 60 Hz", s.workerThreads);
        ImGui::Text("Bodies %u (active %u), vehicles %u", s.numBodies, s.numActiveBodies, s.numVehicles);
        ImGui::Text("Step %.3f ms, total steps %llu", s.lastStepMs, static_cast<unsigned long long>(s.steps));
        ImGui::Text("Fixed steps this frame %d, alpha %.2f", m_engine.GetFrameStats().fixedStepsThisFrame,
                    m_engine.GetFrameStats().interpolationAlpha);
        ImGui::Separator();
        glm::vec3 g = physics.GetGravity();
        if (ImGui::DragFloat3("Gravity", &g.x, 0.05f, -50.0f, 50.0f)) physics.SetGravity(g);
        ImGui::Checkbox("Paused", &m_engine.simulationPaused);
        ImGui::SameLine();
        if (ImGui::Button("Step")) m_engine.stepOnce = true;
        ImGui::SliderFloat("Time scale", &m_engine.timeScale, 0.05f, 2.0f, "%.2fx");
        ImGui::SeparatorText("Debug draw (JPH::DebugRenderer)");
        ImGui::Checkbox("Body wireframes", &m_engine.drawPhysicsDebug);
        ImGui::Checkbox("Constraints / suspension", &m_engine.drawPhysicsConstraints);
        ImGui::Checkbox("Bounding boxes", &m_engine.drawPhysicsBounds);
        ImGui::SeparatorText("Navigation");
        const NavStats nav = m_engine.GetNavigation().Stats();
        ImGui::Text("Tiles %d (%d pending), %llu builds, last %.1f ms, agents %d", nav.tiles, nav.pending, static_cast<unsigned long long>(nav.builds), nav.lastBuildMs, nav.agents);
        ImGui::Checkbox("Show navigation mesh", &m_engine.drawNavMesh);
        ImGui::SliderInt("Pedestrians", &m_testbed.pedestrians, 0, 100);
        ImGui::SeparatorText("Collision layers");
        ImGui::BulletText("STATIC_WORLD  - ground, ramps, buildings");
        ImGui::BulletText("DYNAMIC_PROP  - crates, balls, cones");
        ImGui::BulletText("VEHICLE       - chassis (wheels are raycasts)");
        ImGui::BulletText("RAGDOLL       - reserved");
        ImGui::End();
    }

    // ------------------------------------------------------------------ streaming
    void DrawStreamingPanel() {
        if (!ImGui::Begin("Streaming", &m_showStreaming)) {
            ImGui::End();
            return;
        }
        SectorStreamer& st = m_engine.GetStreamer();
        const StreamerStats& s = st.Stats();
        ImGui::Text("Focus sector (%d, %d)   loaded %zu   pending %zu", s.focusSector.x, s.focusSector.z, s.loaded, s.pending);
        ImGui::Text("Total loaded %llu, unloaded %llu, last load %.2f ms (worker thread)",
                    static_cast<unsigned long long>(s.totalLoaded), static_cast<unsigned long long>(s.totalUnloaded), s.lastLoadMs);
        StreamerConfig& cfg = st.Config();
        ImGui::SliderInt("Load radius", &cfg.loadRadius, 0, 4);
        cfg.unloadRadius = std::max(cfg.unloadRadius, cfg.loadRadius + 1);
        ImGui::SliderInt("Unload radius", &cfg.unloadRadius, cfg.loadRadius + 1, 6);
        ImGui::SliderInt("Integrations / frame", &cfg.maxIntegrationsPerFrame, 1, 8);

        // Mini map of loaded sectors.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float cell = 14.0f;
        const int range = 6;
        const ImVec2 center(p.x + cell * (range + 0.5f), p.y + cell * (range + 0.5f));
        for (const SectorCoord& c : st.LoadedSectors()) {
            const float x = center.x + (c.x - s.focusSector.x) * cell - cell * 0.5f;
            const float y = center.y - (c.z - s.focusSector.z) * cell - cell * 0.5f;
            dl->AddRectFilled(ImVec2(x + 1, y + 1), ImVec2(x + cell - 1, y + cell - 1), IM_COL32(80, 170, 110, 255));
        }
        dl->AddRect(ImVec2(center.x - cell * 0.5f, center.y - cell * 0.5f), ImVec2(center.x + cell * 0.5f, center.y + cell * 0.5f),
                    IM_COL32(255, 120, 60, 255), 0.0f, 0, 2.0f);
        ImGui::Dummy(ImVec2(cell * (2 * range + 1), cell * (2 * range + 1)));
        ImGui::End();
    }

    // ------------------------------------------------------------------ renderer
    void DrawRendererPanel() {
        if (!ImGui::Begin("Renderer", &m_showRenderer)) {
            ImGui::End();
            return;
        }
        Renderer& r = m_engine.GetRenderer();
        const RenderStats& s = r.Stats();
        ImGui::Text("Submitted %u, drawn %u, frustum-culled %u, shadow casters %u, debug line verts %u", s.submitted, s.drawn,
                    s.culled, s.shadowCasters, s.lineVertices);
        ImGui::Checkbox("Frustum culling", &r.frustumCulling);
        ImGui::SameLine();
        ImGui::Checkbox("Shadows", &r.lighting.shadows);
        LightingSettings& l = r.lighting;
        if (ImGui::DragFloat3("Sun direction", &l.sunDirection.x, 0.01f, -1.0f, 1.0f)) {
            if (glm::length(l.sunDirection) < 1e-3f) l.sunDirection = glm::vec3(0, 1, 0);
            l.sunDirection = glm::normalize(l.sunDirection);
        }
        ImGui::ColorEdit3("Sun color", &l.sunColor.x);
        ImGui::SliderFloat("Sun intensity", &l.sunIntensity, 0.0f, 10.0f);
        ImGui::ColorEdit3("Sky zenith", &l.skyZenith.x);
        ImGui::ColorEdit3("Sky horizon", &l.skyHorizon.x);
        ImGui::SliderFloat("Ambient", &l.ambientIntensity, 0.0f, 2.0f);
        ImGui::SliderFloat("Fog density", &l.fogDensity, 0.0f, 0.02f, "%.4f");
        ImGui::SliderFloat("Exposure", &l.exposure, 0.1f, 4.0f);
        ImGui::SliderFloat("Shadow distance", &l.shadowDistance, 20.0f, 250.0f);
        ImGui::SliderFloat("Camera FOV", &m_engine.GetCamera().fovDegrees, 30.0f, 110.0f);
        ImGui::End();
    }

    // ------------------------------------------------------------------ profiler
    void DrawProfiler() {
        if (!ImGui::Begin("Profiler", &m_showProfiler)) {
            ImGui::End();
            return;
        }
        const FrameStats& fs = m_engine.GetFrameStats();
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%.2f ms (%.0f FPS)", fs.frameMs, fs.fps);
        ImGui::PlotLines("##frametimes", m_frameTimes.data(), static_cast<int>(m_frameTimes.size()),
                         static_cast<int>(m_frameTimeOffset), overlay, 0.0f, 33.3f, ImVec2(-1.0f, 70.0f));
        if (ImGui::BeginTable("##scopes", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Scope");
            ImGui::TableSetupColumn("Last (ms)");
            ImGui::TableSetupColumn("Avg (ms)");
            ImGui::TableSetupColumn("Peak (ms)");
            ImGui::TableHeadersRow();
            for (const Profiler::Sample& s : m_engine.GetProfiler().Samples()) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s.name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.3f", s.lastMs);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", s.avgMs);
                ImGui::TableNextColumn(); ImGui::Text("%.3f", s.maxMs);
            }
            ImGui::EndTable();
        }
        ImGui::End();
    }

    // ------------------------------------------------------------------ console
    void DrawConsole() {
        if (!ImGui::Begin("Console", &m_showConsole)) {
            ImGui::End();
            return;
        }
        if (ImGui::Button("Clear")) Log::Clear();
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_autoScroll);
        ImGui::Separator();
        const uint64_t revision = Log::Revision();
        if (revision != m_logRevision) {
            m_logCache = Log::Snapshot(1000);
            m_logRevision = revision;
        }
        ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        for (const LogEntry& entry : m_logCache) {
            ImVec4 color(0.85f, 0.85f, 0.85f, 1.0f);
            if (entry.level == LogLevel::Warn) color = ImVec4(1.0f, 0.8f, 0.35f, 1.0f);
            else if (entry.level == LogLevel::Error) color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
            else if (entry.level == LogLevel::Trace) color = ImVec4(0.55f, 0.55f, 0.6f, 1.0f);
            ImGui::TextColored(color, "[%8.2f] %s", entry.timeSeconds, entry.message.c_str());
        }
        if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::End();
    }

    // ------------------------------------------------------------------ animation
    void DrawAnimator(Entity e, AnimatorComponent& a) {
        const auto* mc = m_engine.GetRegistry().TryGet<ModelComponent>(e);
        const Model* model = mc ? m_engine.GetAssets().GetModel(mc->model) : nullptr;
        if (!model) return;
        const Rig& rig = model->rig;
        ImGui::Text("%zu nodes, %zu skins, %zu clips", rig.nodes.size(), rig.skins.size(), rig.clips.size());
        ImGui::SliderFloat("Fade (s)", &m_fade, 0.0f, 1.0f);
        const bool valid = a.clip >= 0 && a.clip < static_cast<int>(rig.clips.size());
        if (ImGui::BeginCombo("Clip", valid ? rig.clips[static_cast<size_t>(a.clip)].name.c_str() : "(rest pose)")) {
            for (int i = 0; i < static_cast<int>(rig.clips.size()); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(rig.clips[static_cast<size_t>(i)].name.c_str(), i == a.clip)) PlayClip(a, rig, i, m_fade, a.loop, a.speed);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (valid) {
            const float duration = rig.clips[static_cast<size_t>(a.clip)].duration;
            if (ImGui::Button(a.playing ? "Pause##anim" : "Play##anim")) {
                a.playing = !a.playing;
                if (a.playing && !a.loop && a.time >= duration) a.time = 0.0f;
            }
            ImGui::SameLine();
            ImGui::SliderFloat("Time", &a.time, 0.0f, duration, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
        }
        ImGui::SliderFloat("Speed", &a.speed, -2.0f, 3.0f);
        ImGui::Checkbox("Loop", &a.loop);
        ImGui::SameLine();
        ImGui::Checkbox("In place", &a.inPlace);
        ImGui::SameLine();
        ImGui::Checkbox("Show skeleton", &m_showSkeleton);
    }

public:
    // World-space lines; called from OnRender.
    void DrawSkeleton() {
        const auto* t = m_engine.GetRegistry().TryGet<TransformComponent>(m_selected);
        const auto* mc = m_engine.GetRegistry().TryGet<ModelComponent>(m_selected);
        const auto* anim = m_engine.GetAnimation().Get(m_selected);
        const Model* model = t && mc && anim ? m_engine.GetAssets().GetModel(mc->model) : nullptr;
        if (!m_showSkeleton || !model || anim->model != mc->model) return;
        const glm::mat4 world = t->Matrix();
        for (size_t i = 0; i < model->rig.nodes.size(); ++i) {
            const int p = model->rig.nodes[i].parent;
            if (p < 0) continue;
            m_engine.GetRenderer().DrawLine(glm::vec3(world * anim->globals[static_cast<size_t>(p)][3]), glm::vec3(world * anim->globals[i][3]), {1.0f, 0.85f, 0.1f});
        }
    }

private:
    // ------------------------------------------------------------------ assets
    void DrawAssets() {
        // Saved layouts from before this panel existed: join the Console's tab group.
        if (const ImGuiWindow* console = ImGui::FindWindowByName("Console"); console && console->DockId)
            ImGui::SetNextWindowDockID(console->DockId, ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Assets", &m_showAssets)) {
            ImGui::End();
            return;
        }
        AssetManager& assets = m_engine.GetAssets();
        ImGui::TextDisabled("Drop .gltf .glb .fbx .obj / images / fonts / sounds onto the window, or paste a path:");
        ImGui::SetNextItemWidth(-160.0f);
        ImGui::InputText("##importpath", m_importPath, sizeof(m_importPath));
        ImGui::SameLine();
        if (ImGui::Button("Import") && m_importPath[0]) ImportFiles({Utf8ToPath(m_importPath)});
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) m_assetsDirty = true;
        if (!m_importStatus.empty()) ImGui::TextUnformatted(m_importStatus.c_str());
        if (m_assetsDirty) {
            m_assetList = assets.List();
            m_assetsDirty = false;
        }
        static const char* kSlots[] = {"Base color", "Metal/rough", "Normal", "Emissive"};
        ImGui::SetNextItemWidth(130.0f);
        ImGui::Combo("Texture slot", &m_textureSlot, kSlots, 4);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130.0f);
        static const char* kPhysics[] = {"No collision", "Static mesh", "Dynamic hull", "Walking character"};
        ImGui::Combo("Model physics", &m_modelPhysics, kPhysics, 4);
        ImGui::Separator();
        ImGui::BeginChild("##assetlist");
        for (size_t i = 0; i < m_assetList.size(); ++i) {
            const AssetManager::Entry& a = m_assetList[i];
            ImGui::PushID(static_cast<int>(i));
            const char* action = a.type == AssetType::Model ? "Spawn" : a.type == AssetType::Image ? "Apply" : a.type == AssetType::Sound ? "Play" : "Load";
            if (ImGui::SmallButton(action)) UseAsset(a);
            ImGui::SameLine();
            if (a.type == AssetType::Model && m_engine.GetRegistry().TryGet<AnimatorComponent>(m_selected)) {
                if (ImGui::SmallButton("Add clips")) {
                    const int n = m_engine.LoadAnimations(m_selected, a.path);
                    m_importStatus = n < 0 ? "No usable clips in " + a.path + " (see Console)" : n == 0 ? "Clips were already added" : "Added clips from " + a.path;
                }
                ImGui::SetItemTooltip("Use this file's animations on the selected character");
                ImGui::SameLine();
            }
            ImGui::Text("[%s] %s", AssetTypeName(a.type), a.path.c_str());
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::End();
    }

    void UseAsset(const AssetManager::Entry& a) {
        AssetManager& assets = m_engine.GetAssets();
        if (a.type == AssetType::Model) {
            SpawnDesc d;
            d.name = PathToUtf8(Utf8ToPath(a.path).stem());
            d.model = a.path;
            const Camera& cam = m_engine.GetCamera();
            d.position = cam.position + cam.Forward() * 8.0f;
            d.physics = m_modelPhysics == 3 ? SpawnPhysics::Character : m_modelPhysics == 2 ? SpawnPhysics::Dynamic : m_modelPhysics == 1 ? SpawnPhysics::Static : SpawnPhysics::None;
            const Entity e = m_engine.SpawnPrimitive(d);
            if (e != NullEntity) m_selected = e;
            else m_importStatus = "Could not load " + a.path + " (see Console)";
        } else if (a.type == AssetType::Image) {
            auto* m = m_engine.GetRegistry().TryGet<MeshComponent>(m_selected);
            if (!m) {
                m_importStatus = "Select a primitive (not an imported model) to apply a texture";
                return;
            }
            uint32_t* slots[] = {&m->baseColorTexture, &m->metallicRoughnessTexture, &m->normalTexture, &m->emissiveTexture};
            *slots[m_textureSlot] = assets.LoadTexture(a.path, m_textureSlot == 0 || m_textureSlot == 3);
        } else if (a.type == AssetType::Sound) {
            if (m_previewSound) m_engine.GetAudio().Stop(m_previewSound);
            m_previewSound = m_engine.GetAudio().Play(assets.Resolve(a.path));
        } else if (a.type == AssetType::Font) {
            m_previewFont = m_engine.LoadFont(a.path, 32.0f);
        }
    }

    // ------------------------------------------------------------------ helpers
    void SpawnAtCamera(const char* mesh) {
        const Camera& cam = m_engine.GetCamera();
        SpawnDesc d;
        d.name = mesh == std::string("sphere") ? "Ball" : "Crate";
        d.mesh = mesh;
        d.position = cam.position + cam.Forward() * 6.0f;
        d.scale = glm::vec3(1.0f);
        d.albedo = glm::vec3(0.2f, 0.6f, 0.9f);
        d.physics = SpawnPhysics::Dynamic;
        d.mass = 30.0f;
        m_selected = m_engine.SpawnPrimitive(d);
    }

    void DeleteSelected() {
        if (!m_engine.GetRegistry().Valid(m_selected) || m_selected == m_testbed.Vehicle()) return;
        m_engine.DestroyEntity(m_selected);
        m_selected = NullEntity;
    }

    static bool ContainsCaseInsensitive(const char* haystack, const char* needle) {
        const std::string h(haystack), n(needle);
        return std::search(h.begin(), h.end(), n.begin(), n.end(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
               }) != h.end();
    }

    Engine& m_engine;
    Testbed& m_testbed;
    Entity m_selected = NullEntity;
    bool m_viewportHovered = false;
    bool m_viewportFocused = false;
    bool m_layoutChecked = false;
    bool m_showHierarchy = true, m_showInspector = true, m_showPhysics = true, m_showTuning = true;
    bool m_showStreaming = true, m_showRenderer = true, m_showProfiler = true, m_showConsole = true, m_showDemo = false;
    bool m_showPathTracer = true, m_showAssets = true, m_assetsDirty = true;
    std::vector<AssetManager::Entry> m_assetList;
    std::string m_importStatus;
    char m_importPath[512] = {};
    float m_fade = 0.2f;
    bool m_showSkeleton = false;
    int m_textureSlot = 0, m_modelPhysics = 1, m_previewFont = 0;
    SoundHandle m_previewSound = 0;
    bool m_hideSectorEntities = true;
    bool m_autoScroll = true;
    char m_filter[64] = {};
    std::array<float, 240> m_frameTimes{};
    size_t m_frameTimeOffset = 0;
    std::vector<LogEntry> m_logCache;
    uint64_t m_logRevision = ~0ull;
};

} // namespace

int main(int argc, char** argv) {
    double quitAfterSeconds = 0.0; // --quit-after <s> [--screenshot <file.bmp>] for automated checks
    std::string screenshotPath;
    bool background = false;
    for (int i = 1; i < argc; ++i) background |= std::string(argv[i]) == "--background";
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--quit-after") quitAfterSeconds = std::atof(argv[++i]);
        else if (arg == "--screenshot") screenshotPath = argv[++i];
    }

    EngineConfig config;
    config.title = "MAGE v1 Editor";
    config.width = 1720;
    config.height = 980;
    config.editorMode = true;
    config.background = background;

    Engine engine;
    if (!engine.Init(config)) {
        Log::Error("Engine failed to initialize");
        return 1;
    }

    Testbed testbed;
    testbed.Build(engine);
    Editor editor(engine, testbed);

    engine.OnFixedUpdate = [&](float dt) { testbed.FixedUpdate(engine, dt); };
    double elapsed = 0.0;
    int quitStage = 0;
    engine.OnUpdate = [&](float dt) {
        elapsed += dt;
        if (quitAfterSeconds > 0.0 && elapsed >= quitAfterSeconds) {
            if (quitStage == 0 && !screenshotPath.empty()) engine.RequestScreenshot(screenshotPath);
            if (quitStage++ > 0) engine.RequestQuit();
        }
        editor.Update(dt);
    };
    engine.OnRender = [&] { editor.DrawSkeleton(); };
    engine.OnImGui = [&] { editor.Draw(); };
    engine.OnFilesDropped = [&](const std::vector<std::filesystem::path>& files) { editor.ImportFiles(files); };

    engine.Run();
    engine.Shutdown();
    return 0;
}
