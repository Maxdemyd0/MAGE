// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - forward PBR renderer (OpenGL 4.x core).
//
//   * Cook-Torrance GGX directional sun + hemispheric ambient, ACES tonemapping
//   * Directional shadow map (camera-following, texel-snapped, 3x3 PCF)
//   * Procedural sky dome with sun disc + distance fog
//   * Camera frustum culling of every draw command (world AABB vs 6 planes)
//   * Immediate-mode debug lines (used by the Jolt physics debug renderer)

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "core/Camera.h"
#include "renderer/Mesh.h"
#include "renderer/Shader.h"
#include "raytracing/PathTracer.h"

namespace mage {

// Physically based material (metallic/roughness). Emissive is radiance in path tracer units
// (1 unit ~ 25 000 cd/m^2). Clearcoat and transmission are honoured by the path tracer;
// the raster fallback renders transmissive surfaces as opaque.
struct Material {
    glm::vec3 albedo{0.8f};
    float metallic = 0.0f;
    float roughness = 0.6f;
    float checkerScale = 0.0f;
    glm::vec3 emissive{0.0f};
    float clearcoat = 0.0f;            // 0..1 strength of a glossy dielectric coat (car paint)
    float clearcoatRoughness = 0.05f;
    float transmission = 0.0f;         // 0..1 thin-walled glass (albedo = tint)
    float ior = 1.5f;
};

struct DrawCommand {
    MeshHandle mesh = InvalidMesh;
    glm::mat4 model{1.0f};
    Material material;
    bool castShadows = true;
};

struct LightingSettings {
    glm::vec3 sunDirection = glm::normalize(glm::vec3(0.45f, 0.75f, 0.35f)); // points *towards* the sun
    glm::vec3 sunColor{1.0f, 0.95f, 0.88f};
    float sunIntensity = 3.4f;
    glm::vec3 skyZenith{0.16f, 0.34f, 0.70f};
    glm::vec3 skyHorizon{0.68f, 0.78f, 0.90f};
    glm::vec3 groundColor{0.30f, 0.28f, 0.26f};
    float ambientIntensity = 0.65f;
    float fogDensity = 0.0028f;
    float exposure = 1.0f;
    bool shadows = true;
    float shadowDistance = 70.0f;
};

struct RenderStats {
    uint32_t submitted = 0;
    uint32_t drawn = 0;
    uint32_t culled = 0;
    uint32_t shadowCasters = 0;
    uint32_t lineVertices = 0;
    bool pathTraced = false;
};

struct RenderTarget {
    uint32_t fbo = 0;
    uint32_t color = 0;
    uint32_t depth = 0;
    int width = 0;
    int height = 0;
};

class MAGE_API Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // shaderDirectory: compiled SPIR-V for the Vulkan path tracer (optional; raster works without it).
    bool Init(const std::filesystem::path& shaderDirectory = {});
    void Shutdown();

    MeshLibrary& Meshes() { return m_meshes; }

    void BeginFrame(const Camera& camera, int width, int height);
    void Submit(const DrawCommand& command);
    void DrawLine(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color);
    void DrawWireBox(const AABB& box, const glm::vec3& color);
    void EndFrame(uint32_t targetFramebuffer);

    // Off-screen colour target used by the editor viewport (created / resized on demand).
    RenderTarget& GetViewportTarget(int width, int height);

    const RenderStats& Stats() const { return m_stats; }
    const glm::mat4& ViewProjection() const { return m_viewProj; }

    // Hardware path tracer (unavailable when the GPU or the build lacks Vulkan ray queries).
    PathTracer* GetPathTracer() { return m_pathTracer.get(); }
    bool PathTracingAvailable() const;
    bool pathTracing = true; // use the path tracer when available, otherwise the raster pipeline

    LightingSettings lighting;
    bool frustumCulling = true;
    glm::vec3 clearColor{0.1f, 0.1f, 0.12f};

private:
    struct LineVertex {
        glm::vec3 position;
        glm::vec3 color;
    };

    bool CreateShadowMap(int size);
    void DestroyRenderTarget(RenderTarget& rt);
    glm::mat4 ComputeLightViewProjection() const;
    void DrawMesh(const Mesh& mesh) const;
    void DrawLines();

    MeshLibrary m_meshes;
    Shader m_pbrShader;
    Shader m_shadowShader;
    Shader m_skyShader;
    Shader m_lineShader;
    Shader m_blitShader;
    std::unique_ptr<PathTracer> m_pathTracer;

    uint32_t m_emptyVao = 0;
    uint32_t m_lineVao = 0;
    uint32_t m_lineVbo = 0;
    size_t m_lineVboCapacity = 0;

    uint32_t m_shadowFbo = 0;
    uint32_t m_shadowTex = 0;
    int m_shadowSize = 2048;

    RenderTarget m_viewport;

    std::vector<DrawCommand> m_commands;
    std::vector<uint32_t> m_visible;
    std::vector<LineVertex> m_lines;

    Camera m_camera;
    glm::mat4 m_view{1.0f};
    glm::mat4 m_proj{1.0f};
    glm::mat4 m_viewProj{1.0f};
    Frustum m_frustum;
    int m_width = 1;
    int m_height = 1;
    RenderStats m_stats;
    bool m_initialized = false;
};

} // namespace mage
