// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
// =============================================================================
//  MAGE v1 Runtime - vertical slice boot scene
//
//  Checkerboard ground + ramps + props, a 4-wheel Jolt raycast vehicle driven from
//  C# (GameScripts.TestVehicleScript), streamed city sectors around the player, and
//  live ImGui sliders for suspension stiffness / damping / engine torque.
//
//  Command line:
//    --no-vsync           uncapped frame rate (also removes the --max-fps cap)
//    --max-fps <n>        frame cap, default 240 (0 = unlimited)
//    --no-scripts         skip .NET (native C++ controller drives the car)
//    --no-streaming       disable the sector streamer
//    --raster             skip the Vulkan path tracer (OpenGL raster only; F3 toggles at runtime)
//    --on-foot            start beside the car;  --autowalk plays a short on-foot demo
//    --load-model <file>  spawn an imported model next to the car
//    --animation <clip>   clip it plays;  --load-animations <file> adds clips from another file
//    --model-count <n>    spawn it n times (performance tests)
//    --font <file>        draw a line of text with a font file
//    --background         do not take window focus; ignore the real keyboard (automated runs)
//    --sun-elevation <d>  time of day as sun elevation in degrees (-18 night .. 90 noon)
//    --autodrive          demo autopilot: injects W/D key presses (exercises C++ -> C# -> Jolt)
//    --quit-after <s>     exit after <s> seconds (smoke tests / CI)
//    --screenshot <file>  save a .bmp of the last frame before quitting
// =============================================================================
#include "core/Engine.h"
#include "core/Log.h"
#include "physics/PhysicsWorld.h"
#include "raytracing/PathTracer.h"
#include "renderer/Renderer.h"
#include "scene/Testbed.h"
#include "streaming/SectorStreamer.h"

#include <vector>
#include <algorithm>
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <cmath>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    mage::EngineConfig config;
    config.title = "MAGE v1 Runtime - Vehicle Testbed";
    double quitAfterSeconds = 0.0;
    float sunElevation = -1000.0f; // --sun-elevation <deg>: time of day for tests / screenshots
    bool autodrive = false, onFoot = false, autowalk = false;
    std::string animation;                   // --animation <clip>
    std::vector<std::string> animationFiles; // --load-animations <file>
    int modelCount = 1;                      // --model-count <n>
    std::string loadModel, fontPath; // --load-model <file>, --font <file>: preview imported assets
    std::string screenshotPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--quit-after" && i + 1 < argc) quitAfterSeconds = std::atof(argv[++i]);
        else if (arg == "--screenshot" && i + 1 < argc) screenshotPath = argv[++i];
        else if (arg == "--autodrive") autodrive = true;
        else if (arg == "--no-vsync") { config.vsync = false; config.maxFps = 0; }
        else if (arg == "--max-fps" && i + 1 < argc) config.maxFps = std::atoi(argv[++i]);
        else if (arg == "--no-scripts") config.enableScripting = false;
        else if (arg == "--no-streaming") config.enableStreaming = false;
        else if (arg == "--on-foot") onFoot = true;
        else if (arg == "--autowalk") onFoot = autowalk = true;
        else if (arg == "--load-model" && i + 1 < argc) loadModel = argv[++i];
        else if (arg == "--animation" && i + 1 < argc) animation = argv[++i];
        else if (arg == "--load-animations" && i + 1 < argc) animationFiles.emplace_back(argv[++i]);
        else if (arg == "--model-count" && i + 1 < argc) modelCount = std::clamp(std::atoi(argv[++i]), 1, 10000);
        else if (arg == "--font" && i + 1 < argc) fontPath = argv[++i];
        else if (arg == "--raster") config.enablePathTracing = false;
        else if (arg == "--background") config.background = true;
        else if (arg == "--sun-elevation" && i + 1 < argc) sunElevation = static_cast<float>(std::atof(argv[++i]));
    }

    mage::Engine engine;
    if (!engine.Init(config)) {
        mage::Log::Error("Engine failed to initialize");
        return 1;
    }

    mage::Testbed testbed;
    testbed.startOnFoot = onFoot;
    testbed.Build(engine);
    if (!loadModel.empty()) {
        mage::SpawnDesc d;
        d.name = "ImportedModel";
        d.model = loadModel;
        d.position = glm::vec3(2.6f, 0.0f, -15.0f);
        d.animation = animation;
        d.animationFiles = animationFiles;
        d.physics = modelCount == 1 ? mage::SpawnPhysics::Static : mage::SpawnPhysics::None;
        for (int i = 0; i < modelCount; ++i) { // more than one: a grid in front of the car
            if (modelCount > 1) d.position = glm::vec3(static_cast<float>(i % 10) * 1.5f - 6.75f, 0.0f, -14.0f + static_cast<float>(i / 10) * 1.5f);
            if (engine.SpawnPrimitive(d) == mage::NullEntity) {
                mage::Log::Error("--load-model failed");
                break;
            }
        }
    }
    const int previewFont = fontPath.empty() ? 0 : engine.LoadFont(fontPath, 40.0f);
    if (sunElevation > -90.0f) {
        auto& sun = engine.GetRenderer().lighting.sunDirection;
        const float azimuth = std::atan2(sun.x, sun.z), e = sunElevation * 3.14159265f / 180.0f;
        sun = glm::vec3(std::cos(e) * std::sin(azimuth), std::sin(e), std::cos(e) * std::cos(azimuth));
    }

    bool showTuning = true;
    bool showPathTracer = true;
    double elapsed = 0.0;
    bool quitting = false;
    float maxSpeedKmh = 0.0f;

    engine.OnFixedUpdate = [&](float dt) { testbed.FixedUpdate(engine, dt); };
    engine.OnUpdate = [&](float dt) {
        elapsed += dt;
        if (previewFont) engine.DrawLabel("MAGE font import \xC3\x9C \xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD1\x96\xD1\x82", {430.0f, 20.0f}, 40.0f, {1, 1, 1, 1}, previewFont);
        mage::Input& input = engine.GetInput();

        if (autodrive) {
            // Accelerate, then sweep the steering: a figure of eight around the spawn plaza.
            const double t = elapsed;
            input.InjectKey(GLFW_KEY_W, t > 0.5);
            input.InjectKey(GLFW_KEY_D, t > 4.0 && std::fmod(t, 8.0) < 3.0);
            input.InjectKey(GLFW_KEY_A, t > 4.0 && std::fmod(t, 8.0) >= 5.0);
        }
        if (autowalk) {
            // Walk around, jump, then get into the car, drive, and get out again.
            const double t = elapsed;
            input.InjectKey(GLFW_KEY_W, (t > 0.5 && t < 3.0) || (t > 8.0 && t < 10.0));
            input.InjectKey(GLFW_KEY_S, (t > 3.5 && t < 6.9) || (t > 10.0 && t < 12.5));
            input.InjectKey(GLFW_KEY_LEFT_SHIFT, t > 2.0 && t < 3.0);
            input.InjectKey(GLFW_KEY_SPACE, (t > 1.2 && t < 1.3) || (t > 12.5 && t < 14.0));
            input.InjectKey(GLFW_KEY_F, (t > 7.0 && t < 7.1) || (t > 14.0 && t < 14.1));
            if (const auto* c = engine.GetRegistry().TryGet<mage::CharacterComponent>(testbed.Player()); c && std::floor(t * 4.0) != std::floor((t - dt) * 4.0))
                mage::Log::Info("Player t={:.2f} pos ({:.2f}, {:.2f}, {:.2f}) {:.2f} m/s {} target {}", t, c->currPosition.x, c->currPosition.y, c->currPosition.z,
                                glm::length(c->velocity), c->character == mage::InvalidCharacter ? "in car" : c->grounded ? "grounded" : "air", engine.GetCameraTarget());
        }
        if (const auto* v = engine.GetRegistry().TryGet<mage::VehicleComponent>(testbed.Vehicle()))
            maxSpeedKmh = std::max(maxSpeedKmh, engine.GetPhysics().GetVehicleTelemetry(v->vehicle).speedKmh);

        if (quitAfterSeconds > 0.0 && elapsed >= quitAfterSeconds && !quitting) {
            quitting = true;
            if (!screenshotPath.empty()) engine.RequestScreenshot(screenshotPath);
        } else if (quitting) {
            const auto& fs = engine.GetFrameStats();
            const auto& ss = engine.GetStreamer().Stats();
            const auto ps = engine.GetPhysics().GetStats();
            mage::Log::Info("--quit-after: {} frames, {:.1f} s simulated, {:.0f} FPS, {} entities, {} bodies, {} sectors loaded",
                            fs.frameIndex, fs.simulationTime, fs.fps, engine.GetRegistry().AliveCount(), ps.numBodies, ss.loaded);
            if (const auto* v = engine.GetRegistry().TryGet<mage::VehicleComponent>(testbed.Vehicle())) {
                const auto tel = engine.GetPhysics().GetVehicleTelemetry(v->vehicle);
                const auto* t = engine.GetRegistry().TryGet<mage::TransformComponent>(testbed.Vehicle());
                mage::Log::Info("Vehicle: pos ({:.1f}, {:.2f}, {:.1f}), {:.1f} km/h (max {:.1f}), gear {}, {:.0f} rpm, wheels down {}/4, input thr {:+.2f} steer {:+.2f}",
                                t->position.x, t->position.y, t->position.z, tel.speedKmh, maxSpeedKmh, tel.gear, tel.engineRPM,
                                tel.wheelsInContact, v->throttle, v->steering);
            }
            if (const auto* pt = engine.GetRenderer().GetPathTracer(); pt && engine.GetRenderer().Stats().pathTraced) {
                const mage::PathTracerStats& p = pt->Stats();
                mage::Log::Info("Path tracer: {}x{}, {} instances, {:.0f} traced FPS ({} frames), GPU {:.2f} ms, latency {:.1f} ms, "
                                "upload {:.2f} ms, static frames {}, restarts camera {} / scene {}",
                                p.width, p.height, p.instances, p.tracedFps, p.framesCompleted, p.gpuMs, p.latencyMs, p.uploadMs,
                                p.staticFrames, p.cameraChanges, p.sceneChanges);
            }
            for (const auto& sample : engine.GetProfiler().Samples())
                mage::Log::Trace("  profile {:<22} avg {:7.3f} ms  peak {:7.3f} ms", sample.name, sample.avgMs, sample.maxMs);
            engine.RequestQuit();
        }

        if (input.IsKeyPressedRaw(GLFW_KEY_ESCAPE)) engine.RequestQuit();
        if (input.IsKeyPressed(GLFW_KEY_TAB)) showTuning = !showTuning;
        testbed.Update(engine, dt, !ImGui::GetIO().WantCaptureMouse);
    };
    engine.OnImGui = [&] {
        testbed.DrawHud(engine);
        if (showTuning) testbed.DrawTuningWindow(engine, &showTuning);
        if (showTuning && showPathTracer) testbed.DrawPathTracerWindow(engine, &showPathTracer);
    };

    engine.Run();
    engine.Shutdown();
    return 0;
}
