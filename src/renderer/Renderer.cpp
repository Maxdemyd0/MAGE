// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "renderer/Renderer.h"
#include "renderer/GL.h"
#include "core/Log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>

namespace mage {

using namespace gl;

// ============================================================================ Shaders
namespace {

// Shared GLSL helpers: sky gradient + tonemapping.
#define MAGE_GLSL_COMMON                                                                          \
    "uniform vec3 uSkyZenith;\n"                                                                  \
    "uniform vec3 uSkyHorizon;\n"                                                                 \
    "uniform vec3 uGroundColor;\n"                                                                \
    "uniform vec3 uSunDir;\n"                                                                     \
    "uniform vec3 uSunColor;\n"                                                                   \
    "uniform float uExposure;\n"                                                                  \
    "vec3 SkyColor(vec3 dir) {\n"                                                                 \
    "    float y = dir.y;\n"                                                                      \
    "    vec3 sky = mix(uSkyHorizon, uSkyZenith, pow(clamp(y, 0.0, 1.0), 0.55));\n"               \
    "    vec3 ground = mix(uSkyHorizon * 0.85, uGroundColor, clamp(-y * 3.0, 0.0, 1.0));\n"       \
    "    return y >= 0.0 ? sky : ground;\n"                                                       \
    "}\n"                                                                                         \
    "vec3 ACES(vec3 x) {\n"                                                                       \
    "    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;\n"                         \
    "    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);\n"                    \
    "}\n"                                                                                         \
    "vec3 Present(vec3 hdr) { return pow(ACES(hdr * uExposure), vec3(1.0 / 2.2)); }\n"

const char* kPbrVS = R"(#version 410 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
uniform mat4 uModel;
uniform mat3 uNormalMatrix;
uniform mat4 uViewProj;
uniform mat4 uLightViewProj;
out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vLightPos;
void main() {
    vec4 world = uModel * vec4(aPosition, 1.0);
    vWorldPos = world.xyz;
    vNormal = uNormalMatrix * aNormal;
    vUV = aUV;
    vLightPos = uLightViewProj * world;
    gl_Position = uViewProj * world;
}
)";

const char* kPbrFS = "#version 410 core\n" MAGE_GLSL_COMMON R"(
in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;
in vec4 vLightPos;
out vec4 FragColor;

uniform vec3 uAlbedo;
uniform float uMetallic;
uniform float uRoughness;
uniform float uChecker;
uniform vec3 uEmissive;
uniform vec3 uCameraPos;
uniform float uSunIntensity;
uniform float uAmbient;
uniform float uFogDensity;
uniform sampler2DShadow uShadowMap;
uniform int uShadowsEnabled;
uniform float uShadowTexel;

const float PI = 3.14159265359;

float DistributionGGX(float NdotH, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}
float GeometrySchlickGGX(float NdotX, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotX / (NdotX * (1.0 - k) + k);
}
vec3 FresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}
float ShadowFactor(vec3 N, vec3 L) {
    if (uShadowsEnabled == 0) return 1.0;
    vec3 p = vLightPos.xyz / vLightPos.w * 0.5 + 0.5;
    if (p.z > 1.0 || p.x < 0.0 || p.x > 1.0 || p.y < 0.0 || p.y > 1.0) return 1.0;
    float bias = max(0.0025 * (1.0 - dot(N, L)), 0.0007);
    float sum = 0.0;
    for (int x = -1; x <= 1; ++x)
        for (int y = -1; y <= 1; ++y)
            sum += texture(uShadowMap, vec3(p.xy + vec2(x, y) * uShadowTexel, p.z - bias));
    return sum / 9.0;
}

void main() {
    vec3 albedo = uAlbedo;
    if (uChecker > 0.0) {
        vec2 cell = floor(vWorldPos.xz / uChecker);
        float checker = mod(cell.x + cell.y, 2.0);
        vec2 f = fract(vWorldPos.xz / (uChecker * 8.0));
        float line = step(0.995, max(f.x, f.y)) + step(max(f.x, f.y), 0.005);
        albedo *= mix(0.55, 1.0, checker) * (1.0 - 0.35 * clamp(line, 0.0, 1.0));
    }

    vec3 N = normalize(vNormal);
    vec3 V = normalize(uCameraPos - vWorldPos);
    vec3 L = normalize(uSunDir);
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float roughness = clamp(uRoughness, 0.04, 1.0);

    vec3 F0 = mix(vec3(0.04), albedo, uMetallic);
    vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);
    float D = DistributionGGX(NdotH, roughness);
    float G = GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
    vec3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 kD = (vec3(1.0) - F) * (1.0 - uMetallic);

    float shadow = ShadowFactor(N, L);
    vec3 direct = (kD * albedo / PI + specular) * uSunColor * uSunIntensity * NdotL * shadow;

    // Hemispheric ambient + cheap sky reflection for metals.
    vec3 hemi = mix(uGroundColor, mix(uSkyHorizon, uSkyZenith, 0.5), N.y * 0.5 + 0.5) * uAmbient;
    vec3 Fa = FresnelSchlick(NdotV, F0);
    vec3 R = reflect(-V, N);
    vec3 ambient = (1.0 - Fa) * (1.0 - uMetallic) * albedo * hemi
                 + Fa * SkyColor(R) * uAmbient * (1.0 - roughness * 0.85);

    vec3 color = direct + ambient + uEmissive;

    float dist = length(uCameraPos - vWorldPos);
    float fog = 1.0 - exp(-pow(dist * uFogDensity, 1.6));
    color = mix(color, SkyColor(-V), clamp(fog, 0.0, 1.0));

    FragColor = vec4(Present(color), 1.0);
}
)";

const char* kShadowVS = R"(#version 410 core
layout(location = 0) in vec3 aPosition;
uniform mat4 uLightViewProj;
uniform mat4 uModel;
void main() { gl_Position = uLightViewProj * uModel * vec4(aPosition, 1.0); }
)";

const char* kShadowFS = R"(#version 410 core
void main() {}
)";

const char* kSkyVS = R"(#version 410 core
out vec2 vNdc;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    vNdc = p;
    gl_Position = vec4(p, 1.0, 1.0);
}
)";

const char* kSkyFS = "#version 410 core\n" MAGE_GLSL_COMMON R"(
in vec2 vNdc;
out vec4 FragColor;
uniform mat4 uInvViewProj;
uniform vec3 uCameraPos;
uniform float uSunIntensity;
void main() {
    vec4 world = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(world.xyz / world.w - uCameraPos);
    vec3 color = SkyColor(dir);
    float sun = max(dot(dir, normalize(uSunDir)), 0.0);
    color += uSunColor * (pow(sun, 2000.0) * 30.0 + pow(sun, 12.0) * 0.25) * (uSunIntensity / 3.0);
    FragColor = vec4(Present(color), 1.0);
}
)";

const char* kLineVS = R"(#version 410 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aColor;
uniform mat4 uViewProj;
out vec3 vColor;
void main() { vColor = aColor; gl_Position = uViewProj * vec4(aPosition, 1.0); }
)";

const char* kLineFS = R"(#version 410 core
in vec3 vColor;
out vec4 FragColor;
void main() { FragColor = vec4(vColor, 1.0); }
)";

#undef MAGE_GLSL_COMMON

} // namespace

// ============================================================================ Lifetime
Renderer::~Renderer() { Shutdown(); }

bool Renderer::Init() {
    Log::Info("OpenGL renderer: {} | {} | {}",
              reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
              reinterpret_cast<const char*>(glGetString(GL_VERSION)));

#ifndef NDEBUG
    if (glDebugMessageCallback) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(
            [](GLenum, GLenum, GLuint, GLenum severity, GLsizei, const GLchar* message, const void*) {
                if (severity == GL_DEBUG_SEVERITY_HIGH) Log::Error("[GL] {}", message);
                else if (severity == GL_DEBUG_SEVERITY_MEDIUM) Log::Warn("[GL] {}", message);
            },
            nullptr);
    }
#endif

    if (!m_pbrShader.Create("PBR", kPbrVS, kPbrFS) || !m_shadowShader.Create("Shadow", kShadowVS, kShadowFS) ||
        !m_skyShader.Create("Sky", kSkyVS, kSkyFS) || !m_lineShader.Create("Lines", kLineVS, kLineFS)) {
        return false;
    }

    m_meshes.CreateDefaultPrimitives();

    glGenVertexArrays(1, &m_emptyVao);

    glGenVertexArrays(1, &m_lineVao);
    glBindVertexArray(m_lineVao);
    glGenBuffers(1, &m_lineVbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_lineVbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), reinterpret_cast<const void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(LineVertex), reinterpret_cast<const void*>(sizeof(glm::vec3)));
    glBindVertexArray(0);

    if (!CreateShadowMap(m_shadowSize)) return false;

    m_initialized = true;
    return true;
}

void Renderer::Shutdown() {
    if (!m_initialized) return;
    m_meshes.Destroy();
    DestroyRenderTarget(m_viewport);
    if (m_shadowTex) glDeleteTextures(1, &m_shadowTex);
    if (m_shadowFbo) glDeleteFramebuffers(1, &m_shadowFbo);
    if (m_lineVbo) glDeleteBuffers(1, &m_lineVbo);
    if (m_lineVao) glDeleteVertexArrays(1, &m_lineVao);
    if (m_emptyVao) glDeleteVertexArrays(1, &m_emptyVao);
    m_shadowTex = m_shadowFbo = m_lineVbo = m_lineVao = m_emptyVao = 0;
    m_pbrShader.Destroy();
    m_shadowShader.Destroy();
    m_skyShader.Destroy();
    m_lineShader.Destroy();
    m_initialized = false;
}

bool Renderer::CreateShadowMap(int size) {
    glGenTextures(1, &m_shadowTex);
    glBindTexture(GL_TEXTURE_2D, m_shadowTex);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_DEPTH_COMPONENT32F), size, size, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

    glGenFramebuffers(1, &m_shadowFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_shadowTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!complete) Log::Error("Shadow map framebuffer incomplete");
    return complete;
}

RenderTarget& Renderer::GetViewportTarget(int width, int height) {
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (m_viewport.fbo && m_viewport.width == width && m_viewport.height == height) return m_viewport;

    DestroyRenderTarget(m_viewport);
    m_viewport.width = width;
    m_viewport.height = height;

    glGenTextures(1, &m_viewport.color);
    glBindTexture(GL_TEXTURE_2D, m_viewport.color);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &m_viewport.depth);
    glBindRenderbuffer(GL_RENDERBUFFER, m_viewport.depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);

    glGenFramebuffers(1, &m_viewport.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_viewport.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_viewport.color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_viewport.depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) Log::Error("Viewport framebuffer incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return m_viewport;
}

void Renderer::DestroyRenderTarget(RenderTarget& rt) {
    if (rt.fbo) glDeleteFramebuffers(1, &rt.fbo);
    if (rt.color) glDeleteTextures(1, &rt.color);
    if (rt.depth) glDeleteRenderbuffers(1, &rt.depth);
    rt = RenderTarget{};
}

// ============================================================================ Frame
void Renderer::BeginFrame(const Camera& camera, int width, int height) {
    m_camera = camera;
    m_width = std::max(width, 1);
    m_height = std::max(height, 1);
    m_view = camera.View();
    m_proj = camera.Projection(static_cast<float>(m_width) / static_cast<float>(m_height));
    m_viewProj = m_proj * m_view;
    m_frustum.Extract(m_viewProj);
    m_commands.clear();
    m_lines.clear();
    m_stats = RenderStats{};
}

void Renderer::Submit(const DrawCommand& command) {
    if (m_meshes.Get(command.mesh)) m_commands.push_back(command);
}

void Renderer::DrawLine(const glm::vec3& a, const glm::vec3& b, const glm::vec3& color) {
    m_lines.push_back({a, color});
    m_lines.push_back({b, color});
}

void Renderer::DrawWireBox(const AABB& box, const glm::vec3& color) {
    const glm::vec3& a = box.min;
    const glm::vec3& b = box.max;
    const glm::vec3 c[8] = {{a.x, a.y, a.z}, {b.x, a.y, a.z}, {b.x, b.y, a.z}, {a.x, b.y, a.z},
                            {a.x, a.y, b.z}, {b.x, a.y, b.z}, {b.x, b.y, b.z}, {a.x, b.y, b.z}};
    const int e[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6}, {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& edge : e) DrawLine(c[edge[0]], c[edge[1]], color);
}

glm::mat4 Renderer::ComputeLightViewProjection() const {
    const float radius = lighting.shadowDistance;
    // Centre the shadow box in front of the camera so the visible foreground gets the texels.
    glm::vec3 forward = m_camera.Forward();
    forward.y = 0.0f;
    if (glm::dot(forward, forward) < 1e-4f) forward = glm::vec3(0, 0, 1);
    forward = glm::normalize(forward);
    const glm::vec3 center = m_camera.target + forward * (radius * 0.35f);

    const glm::vec3 L = glm::normalize(lighting.sunDirection);
    const glm::vec3 up = std::abs(L.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
    const glm::mat4 view = glm::lookAt(center + L * 250.0f, center, up);
    glm::mat4 proj = glm::ortho(-radius, radius, -radius, radius, 1.0f, 600.0f);

    // Snap to shadow-map texels to stop shimmering while the camera moves.
    const glm::mat4 vp = proj * view;
    glm::vec4 origin = vp * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    origin *= static_cast<float>(m_shadowSize) * 0.5f;
    const glm::vec4 rounded = glm::round(origin);
    const glm::vec4 offset = (rounded - origin) * (2.0f / static_cast<float>(m_shadowSize));
    proj[3][0] += offset.x;
    proj[3][1] += offset.y;
    return proj * view;
}

void Renderer::DrawMesh(const Mesh& mesh) const {
    glBindVertexArray(mesh.vao);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mesh.indexCount), GL_UNSIGNED_INT, nullptr);
}

void Renderer::EndFrame(uint32_t targetFramebuffer) {
    m_stats.submitted = static_cast<uint32_t>(m_commands.size());

    // ---- Frustum culling ------------------------------------------------------------
    m_visible.clear();
    for (uint32_t i = 0; i < m_commands.size(); ++i) {
        const DrawCommand& cmd = m_commands[i];
        const Mesh* mesh = m_meshes.Get(cmd.mesh);
        if (frustumCulling) {
            const AABB world = mesh->bounds.Transformed(cmd.model);
            if (!m_frustum.IntersectsAABB(world.min, world.max)) {
                ++m_stats.culled;
                continue;
            }
        }
        m_visible.push_back(i);
    }
    m_stats.drawn = static_cast<uint32_t>(m_visible.size());

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glDisable(GL_BLEND);

    // ---- Shadow pass -----------------------------------------------------------------
    const glm::mat4 lightVP = ComputeLightViewProjection();
    if (lighting.shadows) {
        Frustum lightFrustum;
        lightFrustum.Extract(lightVP);
        glBindFramebuffer(GL_FRAMEBUFFER, m_shadowFbo);
        glViewport(0, 0, m_shadowSize, m_shadowSize);
        glClear(GL_DEPTH_BUFFER_BIT);
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(1.5f, 3.0f);
        m_shadowShader.Bind();
        m_shadowShader.SetMat4("uLightViewProj", lightVP);
        for (const DrawCommand& cmd : m_commands) {
            if (!cmd.castShadows) continue;
            const Mesh* mesh = m_meshes.Get(cmd.mesh);
            const AABB world = mesh->bounds.Transformed(cmd.model);
            if (!lightFrustum.IntersectsAABB(world.min, world.max)) continue;
            m_shadowShader.SetMat4("uModel", cmd.model);
            DrawMesh(*mesh);
            ++m_stats.shadowCasters;
        }
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    // ---- Main pass -------------------------------------------------------------------
    glBindFramebuffer(GL_FRAMEBUFFER, targetFramebuffer);
    glViewport(0, 0, m_width, m_height);
    glClearColor(clearColor.r, clearColor.g, clearColor.b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_MULTISAMPLE);

    auto setCommonUniforms = [&](Shader& s) {
        s.SetVec3("uSkyZenith", lighting.skyZenith);
        s.SetVec3("uSkyHorizon", lighting.skyHorizon);
        s.SetVec3("uGroundColor", lighting.groundColor);
        s.SetVec3("uSunDir", glm::normalize(lighting.sunDirection));
        s.SetVec3("uSunColor", lighting.sunColor);
        s.SetFloat("uExposure", lighting.exposure);
        s.SetFloat("uSunIntensity", lighting.sunIntensity);
        s.SetVec3("uCameraPos", m_camera.position);
    };

    m_pbrShader.Bind();
    setCommonUniforms(m_pbrShader);
    m_pbrShader.SetMat4("uViewProj", m_viewProj);
    m_pbrShader.SetMat4("uLightViewProj", lightVP);
    m_pbrShader.SetFloat("uAmbient", lighting.ambientIntensity);
    m_pbrShader.SetFloat("uFogDensity", lighting.fogDensity);
    m_pbrShader.SetInt("uShadowsEnabled", lighting.shadows ? 1 : 0);
    m_pbrShader.SetFloat("uShadowTexel", 1.0f / static_cast<float>(m_shadowSize));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_shadowTex);
    m_pbrShader.SetInt("uShadowMap", 0);

    for (uint32_t index : m_visible) {
        const DrawCommand& cmd = m_commands[index];
        m_pbrShader.SetMat4("uModel", cmd.model);
        m_pbrShader.SetMat3("uNormalMatrix", glm::inverseTranspose(glm::mat3(cmd.model)));
        m_pbrShader.SetVec3("uAlbedo", cmd.material.albedo);
        m_pbrShader.SetFloat("uMetallic", cmd.material.metallic);
        m_pbrShader.SetFloat("uRoughness", cmd.material.roughness);
        m_pbrShader.SetFloat("uChecker", cmd.material.checkerScale);
        m_pbrShader.SetVec3("uEmissive", cmd.material.emissive);
        DrawMesh(*m_meshes.Get(cmd.mesh));
    }

    // ---- Sky (drawn last at far depth, only where nothing was rendered) ----------------
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    m_skyShader.Bind();
    setCommonUniforms(m_skyShader);
    m_skyShader.SetMat4("uInvViewProj", glm::inverse(m_viewProj));
    glBindVertexArray(m_emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);

    // ---- Debug lines -------------------------------------------------------------------
    m_stats.lineVertices = static_cast<uint32_t>(m_lines.size());
    if (!m_lines.empty()) {
        glBindVertexArray(m_lineVao);
        glBindBuffer(GL_ARRAY_BUFFER, m_lineVbo);
        const size_t bytes = m_lines.size() * sizeof(LineVertex);
        if (bytes > m_lineVboCapacity) {
            m_lineVboCapacity = bytes * 2;
            glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_lineVboCapacity), nullptr, GL_STREAM_DRAW);
        }
        glBufferSubData(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(bytes), m_lines.data());
        m_lineShader.Bind();
        m_lineShader.SetMat4("uViewProj", m_viewProj);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_lines.size()));
    }

    glBindVertexArray(0);
    glEnable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace mage
