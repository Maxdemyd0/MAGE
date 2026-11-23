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

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <cmath>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    mage::EngineConfig config;
    config.title = "MAGE v1 Runtime - Vehicle Testbed";
    double quitAfterSeconds = 0.0;
    bool autodrive = false;
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
        else if (arg == "--raster") config.enablePathTracing = false;
    }

    mage::Engine engine;
    if (!engine.Init(config)) {
        mage::Log::Error("Engine failed to initialize");
        return 1;
    }

    mage::Testbed testbed;
    testbed.Build(engine);

    bool showTuning = true;
    bool showPathTracer = true;
    double elapsed = 0.0;
    bool quitting = false;
    float maxSpeedKmh = 0.0f;

    engine.OnFixedUpdate = [&](float dt) { testbed.FixedUpdate(engine, dt); };
    engine.OnUpdate = [&](float dt) {
        elapsed += dt;
        mage::Input& input = engine.GetInput();

        if (autodrive) {
            // Accelerate, then sweep the steering: a figure of eight around the spawn plaza.
            const double t = elapsed;
            input.InjectKey(GLFW_KEY_W, t > 0.5);
            input.InjectKey(GLFW_KEY_D, t > 4.0 && std::fmod(t, 8.0) < 3.0);
            input.InjectKey(GLFW_KEY_A, t > 4.0 && std::fmod(t, 8.0) >= 5.0);
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
