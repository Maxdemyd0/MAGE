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
class TextureLibrary;
struct DrawCommand;
struct LightingSettings;

struct PathTracerSettings {
    // Light transport
    int maxBounces = 6;
    int samplesPerPixel = 1;
    float renderScale = 1.0f;          // fraction of the output resolution that is traced
    bool lightSampling = true;         // next-event estimation of emissive surfaces (+ MIS)
    float fireflyClamp = 64.0f;        // max luminance of an indirect sample (radiance units)

    // Denoiser
    bool accumulate = true;            // temporal accumulation (converges while static)
    int maxHistory = 4096;             // frames of accumulation while the scene is static
    float movingMinAlpha = 0.12f;      // history blend while moving (lower = smoother, more ghosting)
    bool denoise = true;
    int atrousIterations = 5;
    float colorSigma = 4.0f;           // luminance edge-stopping strength of the denoiser
    float staticTolerance = 0.002f;    // movement (m) below which the scene still counts as static

    // Sun & atmosphere (physically based; units: km, 1/km)
    float sunAngularDiameterDeg = 0.545f; // the real sun: ~0.53 deg
    float sunIntensity = 1.0f;            // scales the top-of-atmosphere solar irradiance
    float rayleighScale = 1.0f;           // air density
    float hazeDensity = 30.0f;            // aerosol (Mie) density multiplier; 30 ~ typical clear day (turbidity ~3, AOD ~0.15)
    float mieAnisotropy = 0.8f;
    float ozoneScale = 1.0f;
    float groundAlbedo = 0.3f;
    float groundAltitudeKm = 0.05f;
    bool aerialPerspective = true;
    float nightSkyLuminance = 0.05f;      // cd/m^2 (moonlit sky); 0 = pitch black

    // Camera
    bool autoExposure = true;
    float exposureCompensation = 0.5f;    // EV (auto exposure); +0.5 like a photographer on bright scenes
    float manualEV100 = 14.0f;            // EV100 (manual exposure)
    float bloomIntensity = 0.04f;
    bool depthOfField = false;
    float fStop = 2.8f;
    float focusDistance = 0.0f;           // m; 0 = autofocus on the camera target
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
    uint32_t lights = 0;               // emissive instances sampled by next-event estimation
    float ev100 = 0.0f;                // current camera exposure
    float sceneLuminance = 0.0f;       // metered average luminance (cd/m^2)
};

// Radiance unit -> luminance (cd/m^2). The solar irradiance in the atmosphere model is
// normalised; this maps it so that a sunlit scene meters at a realistic EV100 of ~14-15.
inline constexpr float kRadianceToLuminance = 25000.0f;

class MAGE_API PathTracer {
public:
    PathTracer();
    ~PathTracer();
    PathTracer(const PathTracer&) = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    // Builds acceleration structures for every mesh. Meshes and textures added later are picked up
    // automatically; both libraries must outlive the path tracer.
    bool Init(const std::filesystem::path& shaderDirectory, const MeshLibrary& meshes, const TextureLibrary& textures);
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
