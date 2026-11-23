// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
//
// MAGE path tracer - shared bindings and helpers.
// !!! Binding layout and the Frame/Instance structs mirror src/raytracing/PathTracer.cpp !!!

#extension GL_EXT_ray_query : require

const float PI = 3.14159265359;

// ---------------------------------------------------------------------------- resources
layout(set = 0, binding = 0) uniform accelerationStructureEXT uTLAS;
layout(std430, set = 0, binding = 1) readonly buffer Vertices { float vertices[]; };  // 8 floats: pos, normal, uv
layout(std430, set = 0, binding = 2) readonly buffer Indices { uint indices[]; };
layout(std430, set = 0, binding = 3) readonly buffer Meshes { uvec4 meshes[]; };      // firstIndex, vertexOffset, indexCount, -

struct Instance {
    vec4 albedoMetallic;    // rgb albedo, a metallic
    vec4 emissionRoughness; // rgb emission, a roughness
    vec4 misc;              // x checker scale (m), yzw unused
    uvec4 ids;              // x mesh index
};
layout(std430, set = 0, binding = 4) readonly buffer Instances { Instance instances[]; };

layout(std140, set = 0, binding = 5) uniform Frame {
    mat4 invViewProj;
    mat4 prevViewProj;
    vec4 cameraPos;     // xyz
    vec4 sunDir;        // xyz towards the sun, w = cos(angular radius)
    vec4 sunColor;      // rgb irradiance (color * intensity)
    vec4 skyZenith;     // rgb, a = sky intensity
    vec4 skyHorizon;    // rgb, a = exposure
    vec4 groundColor;   // rgb, a = fog density
    uvec4 params0;      // x frame index, y max bounces, z samples per pixel, w flags
    vec4 params1;       // x firefly clamp, y max history, z color sigma, w moving min alpha
    uvec4 size;         // x width, y height, z instance count, w atrous iterations
} ubo;

const uint FLAG_STATIC = 1u;
const uint FLAG_RESET = 2u;
const uint FLAG_ACCUMULATE = 4u;

layout(set = 0, binding = 6, rgba16f) uniform image2D noisyIllum;
layout(set = 0, binding = 7, rgba16f) uniform image2D gAlbedo;
layout(set = 0, binding = 8, rgba16f) uniform image2D gEmission;
layout(set = 0, binding = 9, rgba16f) uniform image2D gNormal[2];      // xyz normal, a roughness
layout(set = 0, binding = 10, rgba32f) uniform image2D gPos[2];        // xyz world position, a = 1 hit / 0 sky
layout(set = 0, binding = 11, rgba16f) uniform image2D historyColor[2];
layout(set = 0, binding = 12, rgba32f) uniform image2D historyMoments[2]; // x m1, y m2, z history length
layout(set = 0, binding = 13, rgba16f) uniform image2D filterTemp[2];  // rgb illumination, a variance
layout(set = 0, binding = 14, rgba8) uniform writeonly image2D outColor;
layout(set = 0, binding = 15, rgba32f) uniform image2D gGuide;      // denoiser guide: xyz normal, w view distance (-1 = sky)

layout(push_constant) uniform Push { ivec4 pass; } pc; // atrous: x step, y src, z dst | composite: y src

ivec2 ImageSize() { return ivec2(ubo.size.xy); }
uint CurrentIndex() { return ubo.params0.x & 1u; }
uint PreviousIndex() { return (ubo.params0.x & 1u) ^ 1u; }

// ---------------------------------------------------------------------------- random
uint PcgHash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}
uint InitRng(ivec2 px, uint frame) { return PcgHash(uint(px.x) + PcgHash(uint(px.y) + PcgHash(frame))); }
float Rand(inout uint state) {
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return float((word >> 22u) ^ word) * (1.0 / 4294967296.0);
}

// ---------------------------------------------------------------------------- color / sky
float Luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

vec3 SkyGradient(vec3 dir) {
    float y = dir.y;
    vec3 sky = mix(ubo.skyHorizon.rgb, ubo.skyZenith.rgb, pow(clamp(y, 0.0, 1.0), 0.55));
    vec3 ground = mix(ubo.skyHorizon.rgb * 0.85, ubo.groundColor.rgb, clamp(-y * 3.0, 0.0, 1.0));
    return (y >= 0.0 ? sky : ground) * ubo.skyZenith.a;
}

// Visible sun disc (only for camera rays; lighting from the sun is handled by next-event estimation).
vec3 SunDisc(vec3 dir) {
    float c = dot(dir, ubo.sunDir.xyz);
    float edge = ubo.sunDir.w;
    float disc = smoothstep(edge - 0.00002, edge + 0.00002, c);
    float solidAngle = 2.0 * PI * (1.0 - edge);
    return ubo.sunColor.rgb * disc / max(solidAngle, 1e-6) + ubo.sunColor.rgb * pow(max(c, 0.0), 12.0) * 0.02;
}

vec3 ACES(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
