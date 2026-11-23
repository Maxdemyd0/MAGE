#include "scene/Testbed.h"
#include "core/Engine.h"
#include "core/Log.h"
#include "renderer/Renderer.h"
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
    engine.SetStreamingFocus(m_vehicle);

    engine.GetPhysics().OptimizeBroadPhase();
    engine.StartScripts();

    if (UsingNativeController(engine))
        Log::Warn("Vehicle uses the native C++ fallback controller (C# scripting not available)");
    else
        Log::Info("Vehicle is driven by C# script '{}'", kVehicleScript);
    Log::Info("Controls: W/S throttle-brake, A/D steer, Space handbrake, R reset, E spawn crate, F1 physics wireframe, F2 camera");
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
    if (input.IsKeyPressed(GLFW_KEY_F2)) {
        cameraMode = cameraMode == CameraMode::Chase ? CameraMode::Fly : CameraMode::Chase;
        if (cameraMode == CameraMode::Fly) flyCamera.SyncFrom(engine.GetCamera());
        else chaseCamera.Reset();
    }
    if (UsingNativeController(engine) && input.IsKeyPressed(GLFW_KEY_R)) ResetVehicle(engine);

    Camera& camera = engine.GetCamera();
    const auto* t = engine.GetRegistry().TryGet<TransformComponent>(m_vehicle);
    if (cameraMode == CameraMode::Chase && t)
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
                        "R reset, E spawn crate (C#), F2 chase/fly camera, RMB orbit, wheel zoom");
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
        ImGui::Text("Draws    %u  culled %u  shadow %u", rs.drawn, rs.culled, rs.shadowCasters);
        ImGui::Text("Sectors  %zu loaded, %zu pending  @ (%d, %d)", ss.loaded, ss.pending, ss.focusSector.x, ss.focusSector.z);
        ImGui::Text("Entities %zu   Scripts %s", engine.GetRegistry().AliveCount(),
                    engine.GetScriptHost().IsReady() ? ".NET" : "off");
    }
    ImGui::End();
}

} // namespace mage
