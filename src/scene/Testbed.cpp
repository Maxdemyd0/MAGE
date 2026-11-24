// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "scene/Testbed.h"
#include "core/Engine.h"
#include "core/Log.h"
#include "core/Window.h"
#include "navigation/NavWorld.h"
#include "renderer/Renderer.h"
#include "raytracing/PathTracer.h"
#include "scripting/ScriptHost.h"
#include "streaming/SectorStreamer.h"

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mage {

namespace {

glm::quat YawPitch(float yawDeg, float pitchDeg) {
    return glm::angleAxis(glm::radians(yawDeg), glm::vec3(0, 1, 0)) * glm::angleAxis(glm::radians(-pitchDeg), glm::vec3(1, 0, 0));
}

// A tilted static slab whose top surface meets the ground at its low (-Z local) end.
void SpawnRamp(Engine& engine, const char* name, glm::vec2 centerXZ, float yawDeg, float angleDeg, float width,
               float length, glm::vec3 color, float thickness = 0.5f) {
    const float a = glm::radians(angleDeg);
    SpawnDesc d;
    d.name = name;
    d.mesh = "cube";
    d.position = glm::vec3(centerXZ.x, std::sin(a) * length * 0.5f - std::cos(a) * thickness * 0.5f + 0.02f, centerXZ.y);
    d.rotation = YawPitch(yawDeg, angleDeg);
    d.scale = glm::vec3(width, thickness, length);
    d.albedo = color;
    d.roughness = 0.75f;
    d.physics = SpawnPhysics::Static;
    d.friction = 0.9f;
    engine.SpawnPrimitive(d);
}

float MoveTowards(float current, float target, float maxDelta) {
    if (std::abs(target - current) <= maxDelta) return target;
    return current + (target > current ? maxDelta : -maxDelta);
}

} // namespace

// ============================================================================ Scene construction
void Testbed::Build(Engine& engine) {
    BuildGround(engine);
    BuildRamps(engine);
    BuildProps(engine);

    VehicleDesc desc;
    desc.position = m_spawnPosition;
    desc.rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    m_vehicle = engine.SpawnVehicle("Vehicle", desc, glm::vec3(0.78f, 0.07f, 0.05f), kVehicleScript);
    engine.SetCameraTarget(m_vehicle);

    // The player character; its script drives the car and walks around.
    SpawnDesc player;
    player.name = "Player";
    player.model = "models/mannequin/mannequin.glb";
    player.physics = SpawnPhysics::Character;
    player.position = m_spawnPosition + glm::vec3(-3.0f, -1.2f, 0.0f);
    m_player = engine.SpawnPrimitive(player);
    if (m_player != NullEntity) {
        if (!startOnFoot) engine.SetCharacterEnabled(m_player, false, {});
        engine.GetRegistry().Add<ScriptComponent>(m_player).className = kPlayerScript;
    }

    engine.GetPhysics().OptimizeBroadPhase();
    engine.StartScripts();

    if (UsingNativeController(engine))
        Log::Warn("Vehicle uses the native C++ fallback controller (C# scripting not available)");
    else
        Log::Info("Vehicle is driven by C# script '{}'", kVehicleScript);
    Log::Info("Controls: W/S throttle-brake, A/D steer, Space handbrake, R reset, E spawn crate, F enter/leave car, V mouse look, F1 physics wireframe, F2 camera");
    Log::Info("On foot: W/A/S/D run, Left Alt walk, Left Shift sprint, Space jump");
}

void Testbed::BuildGround(Engine& engine) {
    SpawnDesc ground;
    ground.name = "Ground";
    ground.mesh = "cube";
    ground.position = glm::vec3(0.0f, -0.5f, 0.0f);
    ground.scale = glm::vec3(3000.0f, 1.0f, 3000.0f);
    ground.albedo = glm::vec3(0.62f, 0.63f, 0.65f);
    ground.roughness = 0.92f;
    ground.checkerScale = 4.0f;
    ground.castShadows = false;
    ground.physics = SpawnPhysics::Static;
    ground.friction = 1.0f;
    engine.SpawnPrimitive(ground);
}

void Testbed::BuildRamps(Engine& engine) {
    const glm::vec3 orange(0.90f, 0.42f, 0.12f), concrete(0.66f, 0.64f, 0.60f), blue(0.15f, 0.35f, 0.75f);

    // Jump: take-off ramp + landing ramp with an 11 m gap.
    SpawnRamp(engine, "Ramp_Jump", {0.0f, 8.0f}, 0.0f, 14.0f, 7.0f, 14.0f, orange);
    SpawnRamp(engine, "Ramp_Landing", {0.0f, 34.0f}, 180.0f, 10.0f, 8.0f, 16.0f, orange);

    // Side kickers.
    SpawnRamp(engine, "Kicker_West", {-22.0f, 2.0f}, 90.0f, 20.0f, 4.0f, 6.0f, blue);
    SpawnRamp(engine, "Kicker_East", {-22.0f, 16.0f}, -90.0f, 20.0f, 4.0f, 6.0f, blue);

    // Plateau with an access ramp.
    SpawnDesc plateau;
    plateau.name = "Plateau";
    plateau.mesh = "cube";
    plateau.position = glm::vec3(24.0f, 1.0f, 10.0f);
    plateau.scale = glm::vec3(12.0f, 2.0f, 16.0f);
    plateau.albedo = concrete;
    plateau.roughness = 0.85f;
    plateau.physics = SpawnPhysics::Static;
    engine.SpawnPrimitive(plateau);
    SpawnRamp(engine, "Ramp_Plateau", {24.0f, -2.6f}, 0.0f, 12.0f, 8.0f, 9.7f, concrete);
    SpawnRamp(engine, "Ramp_PlateauDrop", {24.0f, 22.6f}, 180.0f, 12.0f, 8.0f, 9.7f, concrete);

    // Washboard road: static logs to exercise the suspension.
    for (int i = 0; i < 10; ++i) {
        SpawnDesc log;
        log.name = "Bump";
        log.mesh = "cylinder";
        log.position = glm::vec3(-12.0f, 0.12f, -34.0f + i * 1.6f);
        log.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
        log.scale = glm::vec3(0.35f, 6.0f, 0.35f);
        log.albedo = glm::vec3(0.35f, 0.25f, 0.15f);
        log.roughness = 0.9f;
        log.physics = SpawnPhysics::Static;
        engine.SpawnPrimitive(log);
    }
}

void Testbed::BuildProps(Engine& engine) {
    // Crate pyramid.
    const float s = 1.0f;
    for (int row = 0; row < 5; ++row) {
        for (int i = 0; i < 5 - row; ++i) {
            SpawnDesc crate;
            crate.name = "Crate";
            crate.mesh = "cube";
            crate.position = glm::vec3(12.0f + (i - (4 - row) * 0.5f) * s * 1.02f, s * 0.5f + row * s, -12.0f);
            crate.scale = glm::vec3(s);
            crate.albedo = glm::vec3(0.68f, 0.47f, 0.26f);
            crate.roughness = 0.8f;
            crate.physics = SpawnPhysics::Dynamic;
            crate.mass = 35.0f;
            engine.SpawnPrimitive(crate);
        }
    }

    // A wall of light boxes to plough through.
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 6; ++x) {
            SpawnDesc box;
            box.name = "WallBlock";
            box.mesh = "cube";
            box.position = glm::vec3(-10.0f + x * 1.22f + (y % 2) * 0.6f, 0.6f + y * 1.2f, 42.0f);
            box.scale = glm::vec3(1.2f);
            box.albedo = (x + y) % 2 ? glm::vec3(0.9f) : glm::vec3(0.85f, 0.2f, 0.15f);
            box.roughness = 0.5f;
            box.physics = SpawnPhysics::Dynamic;
            box.mass = 20.0f;
            engine.SpawnPrimitive(box);
        }
    }

    // Bowling balls.
    for (int i = 0; i < 6; ++i) {
        SpawnDesc ball;
        ball.name = "Ball";
        ball.mesh = "sphere";
        ball.position = glm::vec3(30.0f + (i % 3) * 2.0f, 1.0f, -20.0f + (i / 3) * 2.0f);
        ball.scale = glm::vec3(1.4f);
        ball.albedo = glm::vec3(0.1f, 0.4f + 0.1f * i, 0.9f);
        ball.metallic = 0.3f;
        ball.roughness = 0.2f;
        ball.physics = SpawnPhysics::Dynamic;
        ball.mass = 60.0f;
        ball.restitution = 0.4f;
        engine.SpawnPrimitive(ball);
    }
}

// ============================================================================ Per-frame
bool Testbed::UsingNativeController(Engine& engine) const {
    if (!engine.GetScriptHost().IsReady()) return true;
    const auto* s = engine.GetRegistry().TryGet<ScriptComponent>(m_vehicle);
    return !s || !s->instantiated;
}

void Testbed::ResetVehicle(Engine& engine) {
    if (!engine.GetRegistry().Valid(m_vehicle)) return;
    engine.SetEntityTransform(m_vehicle, m_spawnPosition, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
    chaseCamera.Reset();
}

void Testbed::Update(Engine& engine, float dt, bool cameraInputAllowed) {
    Input& input = engine.GetInput();
    if (input.IsKeyPressed(GLFW_KEY_F1)) engine.drawPhysicsDebug = !engine.drawPhysicsDebug;
    if (input.IsKeyPressedRaw(GLFW_KEY_F3) && engine.GetRenderer().PathTracingAvailable())
        engine.GetRenderer().pathTracing = !engine.GetRenderer().pathTracing;
    if (input.IsKeyPressed(GLFW_KEY_F2)) {
        cameraMode = cameraMode == CameraMode::Chase ? CameraMode::Fly : CameraMode::Chase;
        if (cameraMode == CameraMode::Fly) flyCamera.SyncFrom(engine.GetCamera());
        else { chaseCamera.Reset(); thirdPersonCamera.Reset(); }
    }
    if (input.IsKeyPressed(GLFW_KEY_V)) engine.GetWindow().SetCursorCaptured(!engine.GetWindow().CursorCaptured());
    if (input.IsKeyPressed(GLFW_KEY_F4)) engine.drawNavMesh = !engine.drawNavMesh;
    if (UsingNativeController(engine) && input.IsKeyPressed(GLFW_KEY_R)) ResetVehicle(engine);

    const glm::vec3 plaza(-8.0f, 0.0f, -14.0f);
    if (m_spawnedPedestrians < pedestrians && engine.GetNavigation().IsReady(plaza) && engine.GetNavigation().Stats().pending == 0) {
        SpawnDesc d;
        d.name = "Pedestrian";
        d.model = "models/mannequin/mannequin.glb";
        d.physics = SpawnPhysics::Character;
        d.script = kPedestrianScript;
        // One try per frame; crowded or blocked spots are skipped.
        const float radius = std::max(30.0f, std::sqrt(static_cast<float>(pedestrians)) * 5.0f);
        if (engine.GetNavigation().RandomPoint(plaza, std::min(radius, 90.0f), d.position) &&
            engine.GetPhysics().CharacterFits(0.4f, 1.8f, d.position + glm::vec3(0.0f, 0.05f, 0.0f)) && engine.SpawnPrimitive(d) != NullEntity)
            ++m_spawnedPedestrians;
    }

    Camera& camera = engine.GetCamera();
    const Entity target = engine.GetRegistry().Valid(engine.GetCameraTarget()) ? engine.GetCameraTarget() : m_vehicle;
    if (target != m_lastTarget) {
        chaseCamera.Reset();
        thirdPersonCamera.Reset();
        m_lastTarget = target;
    }
    const auto* t = engine.GetRegistry().TryGet<TransformComponent>(target);
    const auto* c = engine.GetRegistry().TryGet<CharacterComponent>(target);
    if (cameraMode == CameraMode::Chase && t && c) {
        const PhysicsWorld& physics = engine.GetPhysics();
        const BodyHandle self = physics.GetCharacterBody(c->character);
        thirdPersonCamera.Update(camera, input, t->position - glm::vec3(0.0f, c->originHeight, 0.0f), c->velocity, c->heading, dt, cameraInputAllowed,
                                 engine.GetWindow().CursorCaptured(), [&](const glm::vec3& from, const glm::vec3& dir, float maxDistance) {
                                     // Several rays, so that the near plane does not cut into a wall edge.
                                     constexpr float kMargin = 0.25f;
                                     const glm::vec3 side = glm::normalize(glm::cross(dir, std::abs(dir.y) > 0.9f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0)));
                                     const glm::vec3 up = glm::cross(side, dir);
                                     float best = maxDistance;
                                     RaycastHit hit;
                                     for (const glm::vec3& o : {glm::vec3(0.0f), side * 0.12f, -side * 0.12f, up * 0.12f, -up * 0.12f})
                                         if (physics.Raycast(from + o, dir, maxDistance + kMargin, hit, self)) best = std::min(best, std::max(hit.distance - kMargin, 0.0f));
                                     return best;
                                 });
    } else if (cameraMode == CameraMode::Chase && t)
        chaseCamera.Update(camera, input, t->position, t->rotation, dt, cameraInputAllowed);
    else
        flyCamera.Update(camera, input, dt, cameraInputAllowed);
}

// Native C++ controller, used only when the C# runtime is not available.
void Testbed::FixedUpdate(Engine& engine, float dt) {
    if (!UsingNativeController(engine)) return;
    auto* v = engine.GetRegistry().TryGet<VehicleComponent>(m_vehicle);
    if (!v) return;
    const Input& in = engine.GetInput();

    float forward = 0.0f, right = 0.0f, brake = 0.0f, handBrake = 0.0f;
    if (in.IsKeyDown(GLFW_KEY_W) || in.IsKeyDown(GLFW_KEY_UP)) forward = 1.0f;
    else if (in.IsKeyDown(GLFW_KEY_S) || in.IsKeyDown(GLFW_KEY_DOWN)) forward = -1.0f;
    if (in.IsKeyDown(GLFW_KEY_D) || in.IsKeyDown(GLFW_KEY_RIGHT)) right += 1.0f;
    if (in.IsKeyDown(GLFW_KEY_A) || in.IsKeyDown(GLFW_KEY_LEFT)) right -= 1.0f;
    if (in.IsKeyDown(GLFW_KEY_SPACE)) handBrake = 1.0f;

    // Brake first when the requested direction opposes the current motion.
    const float speed = engine.GetPhysics().GetVehicleTelemetry(v->vehicle).forwardSpeed;
    if (m_previousForward * forward < 0.0f && ((forward > 0.0f && speed < -0.1f) || (forward < 0.0f && speed > 0.1f))) {
        forward = 0.0f;
        brake = 1.0f;
    } else if (forward != 0.0f) {
        m_previousForward = forward;
    }

    m_nativeSteer = MoveTowards(m_nativeSteer, right, 3.5f * dt);
    v->throttle = forward;
    v->steering = m_nativeSteer;
    v->brake = brake;
    v->handBrake = handBrake;
}

// ============================================================================ UI
void Testbed::DrawTuningWindow(Engine& engine, bool* open) {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + ImGui::GetMainViewport()->WorkSize.x - 480.0f,
                                   ImGui::GetMainViewport()->WorkPos.y + 20.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Vehicle Tuning", open)) {
        ImGui::End();
        return;
    }

    const auto* v = engine.GetRegistry().TryGet<VehicleComponent>(m_vehicle);
    if (!v) {
        ImGui::TextDisabled("No vehicle in the scene.");
        ImGui::End();
        return;
    }
    PhysicsWorld& physics = engine.GetPhysics();
    VehicleTuning tuning = physics.GetVehicleTuning(v->vehicle);
    const VehicleTelemetry telemetry = physics.GetVehicleTelemetry(v->vehicle);

    ImGui::SeparatorText("Telemetry");
    ImGui::Text("Speed  %6.1f km/h   Gear %d   Wheels on ground %d/4", telemetry.speedKmh, telemetry.gear, telemetry.wheelsInContact);
    const float rpmFraction = std::clamp(telemetry.engineRPM / std::max(tuning.engineMaxRPM, 1.0f), 0.0f, 1.0f);
    char rpmLabel[32];
    std::snprintf(rpmLabel, sizeof(rpmLabel), "%.0f rpm", telemetry.engineRPM);
    ImGui::ProgressBar(rpmFraction, ImVec2(-1.0f, 0.0f), rpmLabel);
    ImGui::Text("Input  throttle %+.2f  steer %+.2f  brake %.2f  hand %.2f", v->throttle, v->steering, v->brake, v->handBrake);
    ImGui::TextColored(UsingNativeController(engine) ? ImVec4(1.0f, 0.7f, 0.3f, 1.0f) : ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                       "Controller: %s", UsingNativeController(engine) ? "native C++ fallback" : "C# TestVehicleScript");

    bool changed = false;
    ImGui::SeparatorText("Suspension");
    changed |= ImGui::SliderFloat("Stiffness (N/m)", &tuning.suspensionStiffness, 5000.0f, 150000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderFloat("Damping (N*s/m)", &tuning.suspensionDamping, 200.0f, 20000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    const float sprungMass = tuning.mass * 0.25f;
    ImGui::TextDisabled("  natural freq %.2f Hz, damping ratio %.2f",
                        std::sqrt(tuning.suspensionStiffness / sprungMass) / 6.2831853f,
                        tuning.suspensionDamping / (2.0f * std::sqrt(tuning.suspensionStiffness * sprungMass)));

    ImGui::SeparatorText("Engine & Drivetrain");
    changed |= ImGui::SliderFloat("Engine torque (Nm)", &tuning.engineMaxTorque, 50.0f, 2500.0f, "%.0f");
    changed |= ImGui::SliderFloat("Max RPM", &tuning.engineMaxRPM, 3000.0f, 10000.0f, "%.0f");
    changed |= ImGui::SliderFloat("Max steer (deg)", &tuning.maxSteerAngleDeg, 5.0f, 50.0f, "%.1f");
    changed |= ImGui::SliderFloat("Brake torque (Nm)", &tuning.maxBrakeTorque, 200.0f, 8000.0f, "%.0f");
    changed |= ImGui::SliderFloat("Handbrake torque (Nm)", &tuning.maxHandBrakeTorque, 500.0f, 12000.0f, "%.0f");
    if (changed) physics.ApplyVehicleTuning(v->vehicle, tuning);

    if (ImGui::Button("Defaults")) physics.ApplyVehicleTuning(v->vehicle, VehicleTuning{});
    ImGui::SameLine();
    if (ImGui::Button("Reset vehicle (R)")) ResetVehicle(engine);
    ImGui::SameLine();
    ImGui::Checkbox("Physics wireframe (F1)", &engine.drawPhysicsDebug);

    ImGui::SeparatorText("Controls");
    ImGui::TextDisabled("W/S throttle & brake/reverse, A/D steer, Space handbrake\n"
                        "R reset, E spawn crate (C#), F2 chase/fly camera, F3 path tracing,\n"
                        "F enter/leave car, V mouse look, F4 navigation mesh; on foot: WASD, Shift sprint, Alt walk, Space jump\n"
                        "RMB orbit, wheel zoom");
    ImGui::End();
}

void Testbed::DrawPathTracerWindow(Engine& engine, bool* open) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 480.0f, vp->WorkPos.y + 470.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Path Tracer", open)) {
        ImGui::End();
        return;
    }
    Renderer& renderer = engine.GetRenderer();
    PathTracer* pt = renderer.GetPathTracer();
    if (!pt || !pt->Available()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Hardware path tracing unavailable");
        if (pt) ImGui::TextWrapped("%s", pt->Stats().error.c_str());
        ImGui::End();
        return;
    }
    const PathTracerStats& st = pt->Stats();
    ImGui::Checkbox("Path tracing (F3)", &renderer.pathTracing);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", st.device.c_str());
    ImGui::Text("%ux%u  %.0f traced FPS  GPU %.2f ms  latency %.1f ms", st.width, st.height, st.tracedFps, st.gpuMs, st.latencyMs);
    const uint32_t converged = std::min<uint32_t>(st.staticFrames + 1, static_cast<uint32_t>(pt->settings.maxHistory));
    ImGui::Text("Accumulated %u frames%s  |  %u instances, %u lights", converged,
                st.staticFrames > 0 ? " (static)" : " (moving)", st.instances, st.lights);
    ImGui::Text("Camera EV100 %.1f   scene %.0f cd/m^2", st.ev100, st.sceneLuminance);

    PathTracerSettings s = pt->settings;
    LightingSettings& l = renderer.lighting;

    // ---- Time of day: sun elevation / azimuth drive the whole atmosphere ----------------------
    if (ImGui::CollapsingHeader("Sun & time of day", ImGuiTreeNodeFlags_DefaultOpen)) {
        glm::vec3 d = glm::normalize(l.sunDirection);
        float elevation = glm::degrees(std::asin(std::clamp(d.y, -1.0f, 1.0f)));
        float azimuth = glm::degrees(std::atan2(d.x, d.z));
        bool sunChanged = ImGui::SliderFloat("Sun elevation", &elevation, -18.0f, 90.0f, "%.1f deg");
        sunChanged |= ImGui::SliderFloat("Sun azimuth", &azimuth, -180.0f, 180.0f, "%.0f deg");
        if (sunChanged) {
            const float e = glm::radians(elevation), a = glm::radians(azimuth);
            l.sunDirection = glm::vec3(std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a));
        }
        const struct { const char* name; float elevation; } presets[] = {
            {"Noon", 62.0f}, {"Afternoon", 28.0f}, {"Golden hour", 6.0f}, {"Sunset", 0.6f}, {"Dusk", -4.0f}, {"Night", -16.0f}};
        for (int i = 0; i < 6; ++i) {
            if (i > 0) ImGui::SameLine();
            if (ImGui::SmallButton(presets[i].name)) {
                const float e = glm::radians(presets[i].elevation), a = glm::radians(azimuth);
                l.sunDirection = glm::vec3(std::cos(e) * std::sin(a), std::sin(e), std::cos(e) * std::cos(a));
            }
        }
        ImGui::SliderFloat("Sun intensity", &s.sunIntensity, 0.0f, 4.0f);
        ImGui::SliderFloat("Sun diameter", &s.sunAngularDiameterDeg, 0.05f, 10.0f, "%.2f deg", ImGuiSliderFlags_Logarithmic);
    }
    if (ImGui::CollapsingHeader("Atmosphere")) {
        ImGui::SliderFloat("Air density (Rayleigh)", &s.rayleighScale, 0.0f, 4.0f);
        ImGui::SliderFloat("Haze (aerosols)", &s.hazeDensity, 0.0f, 120.0f, "%.1f");
        ImGui::SliderFloat("Haze anisotropy", &s.mieAnisotropy, 0.0f, 0.95f);
        ImGui::SliderFloat("Ozone", &s.ozoneScale, 0.0f, 3.0f);
        ImGui::SliderFloat("Ground albedo", &s.groundAlbedo, 0.0f, 1.0f);
        ImGui::SliderFloat("Altitude (km)", &s.groundAltitudeKm, 0.0f, 8.0f);
        ImGui::SliderFloat("Night sky (cd/m^2)", &s.nightSkyLuminance, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
        ImGui::Checkbox("Aerial perspective", &s.aerialPerspective);
    }
    if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Auto exposure", &s.autoExposure);
        if (s.autoExposure) ImGui::SliderFloat("Exposure compensation", &s.exposureCompensation, -4.0f, 4.0f, "%+.1f EV");
        else ImGui::SliderFloat("EV100", &s.manualEV100, -6.0f, 18.0f, "%.1f");
        ImGui::SliderFloat("Bloom", &s.bloomIntensity, 0.0f, 0.3f, "%.3f");
        ImGui::Checkbox("Depth of field", &s.depthOfField);
        if (s.depthOfField) {
            ImGui::SliderFloat("f-stop", &s.fStop, 0.95f, 22.0f, "f/%.1f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Focus (m, 0 = auto)", &s.focusDistance, 0.0f, 200.0f, "%.1f");
        }
    }
    if (ImGui::CollapsingHeader("Light transport")) {
        ImGui::SliderInt("Max bounces", &s.maxBounces, 0, 16);
        ImGui::SliderInt("Samples / pixel / frame", &s.samplesPerPixel, 1, 16);
        ImGui::SliderFloat("Render scale", &s.renderScale, 0.25f, 1.0f, "%.2f");
        ImGui::Checkbox("Emissive light sampling (NEE + MIS)", &s.lightSampling);
        ImGui::SliderFloat("Firefly clamp", &s.fireflyClamp, 1.0f, 1000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
    }
    if (ImGui::CollapsingHeader("Denoiser (SVGF-style)")) {
        ImGui::Checkbox("Temporal accumulation", &s.accumulate);
        ImGui::SameLine();
        ImGui::Checkbox("Spatial denoise", &s.denoise);
        ImGui::SliderInt("A-trous iterations", &s.atrousIterations, 0, 8);
        ImGui::SliderFloat("Color sigma", &s.colorSigma, 0.5f, 16.0f);
        ImGui::SliderFloat("Moving history alpha", &s.movingMinAlpha, 0.02f, 1.0f);
        ImGui::SliderInt("Max accumulation", &s.maxHistory, 16, 16384);
        ImGui::SliderFloat("Static tolerance (m)", &s.staticTolerance, 0.0f, 0.05f, "%.4f");
        if (ImGui::Button("Restart accumulation")) pt->ResetAccumulation();
    }
    pt->settings = s;
    ImGui::End();
}

void Testbed::DrawHud(Engine& engine) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 12.0f, vp->WorkPos.y + 12.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##hud", nullptr, flags)) {
        const FrameStats& fs = engine.GetFrameStats();
        const PhysicsStats ps = engine.GetPhysics().GetStats();
        const RenderStats& rs = engine.GetRenderer().Stats();
        const StreamerStats& ss = engine.GetStreamer().Stats();
        ImGui::Text("MAGE v1");
        ImGui::Separator();
        ImGui::Text("%.0f FPS  (%.2f ms)", fs.fps, fs.frameMs);
        ImGui::Text("Physics  %.2f ms  bodies %u (active %u)", ps.lastStepMs, ps.numBodies, ps.numActiveBodies);
        if (rs.pathTraced) {
            const PathTracerStats& pts = engine.GetRenderer().GetPathTracer()->Stats();
            ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f), "Path traced  %ux%u  %.0f fps  GPU %.2f ms  %u inst.", pts.width,
                               pts.height, pts.tracedFps, pts.gpuMs, pts.instances);
        } else {
            ImGui::Text("Raster   draws %u  culled %u  shadow %u", rs.drawn, rs.culled, rs.shadowCasters);
        }
        ImGui::Text("Sectors  %zu loaded, %zu pending  @ (%d, %d)", ss.loaded, ss.pending, ss.focusSector.x, ss.focusSector.z);
        ImGui::Text("Entities %zu   Scripts %s", engine.GetRegistry().AliveCount(),
                    engine.GetScriptHost().IsReady() ? ".NET" : "off");
    }
    ImGui::End();
}

} // namespace mage
