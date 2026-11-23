// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - real-time hardware path tracer (Vulkan, VK_KHR_ray_query on RT cores).
//
// Runs on its own headless Vulkan device next to the OpenGL renderer:
//   draw list -> TLAS over per-mesh BLAS -> pathtrace.comp (GGX/Lambert, sun NEE, sky, emission)
//   -> temporal.comp (reprojection, unbounded accumulation while static)
//   -> atrous.comp x N (SVGF-style edge-aware denoise) -> composite.comp (ACES)
//   -> readback -> OpenGL texture displayed by the Renderer.
// Two frames are kept in flight and the CPU never blocks on the GPU: the image on screen is the
// most recently finished path traced frame (typically 1-2 frames behind the simulation).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mage {

class Camera;
class MeshLibrary;
struct DrawCommand;
struct LightingSettings;

struct PathTracerSettings {
    int maxBounces = 4;
    int samplesPerPixel = 1;
    float renderScale = 1.0f;          // fraction of the output resolution that is traced
    bool accumulate = true;            // temporal accumulation (converges while static)
    int maxHistory = 4096;             // frames of accumulation while the scene is static
    float movingMinAlpha = 0.12f;      // history blend while moving (lower = smoother, more ghosting)
    bool denoise = true;
    int atrousIterations = 5;
    float colorSigma = 4.0f;           // luminance edge-stopping strength of the denoiser
    float fireflyClamp = 12.0f;        // max luminance of an indirect sample
    float sunAngularDiameterDeg = 0.6f;
    float skyIntensity = 0.6f;
    float exposure = 0.8f;             // multiplied with the shared LightingSettings exposure
    float staticTolerance = 0.002f;    // movement (m) below which the scene still counts as static
};

struct PathTracerStats {
    bool available = false;
    std::string device;
    std::string error;
    uint32_t width = 0, height = 0;
    uint32_t instances = 0;
    uint32_t staticFrames = 0;         // consecutive traced frames with an unchanged scene + camera
    uint64_t cameraChanges = 0;        // accumulation restarts caused by camera movement
    uint64_t sceneChanges = 0;         // ... by instances / materials / lights changing
    uint64_t framesCompleted = 0;      // path traced frames handed to OpenGL
    double tracedFps = 0.0;            // completed path traced frames per second
    double gpuMs = 0.0;                // GPU execution time of the last frame (timestamps)
    double latencyMs = 0.0;            // submit -> result available on the CPU
    double uploadMs = 0.0;             // readback -> OpenGL texture
};

class MAGE_API PathTracer {
public:
    PathTracer();
    ~PathTracer();
    PathTracer(const PathTracer&) = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    // Builds bottom-level acceleration structures for every mesh in the library.
    bool Init(const std::filesystem::path& shaderDirectory, const MeshLibrary& meshes);
    void Shutdown();
    bool Available() const;

    // Queues a path traced frame of the draw list (if a frame slot is free) and returns an OpenGL
    // texture (GL_RGBA8, display-ready) holding the newest finished frame, or 0 if none is ready yet.
    uint32_t Render(const std::vector<DrawCommand>& commands, const Camera& camera, const LightingSettings& lighting,
                    int outputWidth, int outputHeight);
    void ResetAccumulation();

    PathTracerSettings settings;
    const PathTracerStats& Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace mage
