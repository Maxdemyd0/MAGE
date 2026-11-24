// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
//
// MAGE path tracer - shared bindings and helpers.
// !!! Binding layout and the Frame/Instance/Light structs mirror src/raytracing/PathTracer.cpp !!!

#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require

const float PI = 3.14159265359;

// ---------------------------------------------------------------------------- scene
layout(set = 0, binding = 0) uniform accelerationStructureEXT uTLAS;
layout(std430, set = 0, binding = 1) readonly buffer Vertices { float vertices[]; };  // 8 floats: pos, normal, uv
layout(std430, set = 0, binding = 2) readonly buffer Indices { uint indices[]; };
layout(std430, set = 0, binding = 3) readonly buffer Meshes { uvec4 meshes[]; };      // firstIndex, vertexOffset, indexCount, -

struct Instance {
    vec4 albedoMetallic;    // rgb base color, a metallic
    vec4 emissionRoughness; // rgb emitted radiance, a roughness
    vec4 layer;             // x checker scale (m), y clearcoat, z clearcoat roughness, w transmission
    vec4 misc;              // x ior, y light selection probability (0 = not a light), zw -
    uvec4 ids;              // x mesh index, y triangle count, z flags (1 transmissive, 2 emissive)
    vec4 row0, row1, row2;  // object -> world (rows of the 3x4 matrix)
    uvec4 textures;         // base color, metallic-roughness, normal, emissive (0 = none)
    vec4 surface;           // x normal scale, y alpha, z alpha cutoff
};
layout(std430, set = 0, binding = 4) readonly buffer Instances { Instance instances[]; };

const uint INSTANCE_TRANSMISSIVE = 1u;
const uint INSTANCE_EMISSIVE = 2u;
const uint INSTANCE_MASKED = 4u;

layout(std140, set = 0, binding = 5) uniform Frame {
    mat4 invViewProj;
    mat4 prevViewProj;
    vec4 cameraPos;      // xyz world (m), w delta time (s)
    vec4 cameraBasis;    // xyz camera forward, w focus distance (m)
    vec4 sunDir;         // xyz towards the sun, w = cos(angular radius)
    vec4 solarIrradiance;// rgb top-of-atmosphere irradiance, w sun angular radius (rad)
    vec4 rayleigh;       // rgb scattering (1/km), w scale height (km)
    vec4 mie;            // x scattering, y extinction (1/km), z phase g, w scale height (km)
    vec4 ozone;          // rgb absorption (1/km), w ground albedo
    vec4 planet;         // x bottom radius, y top radius (km), z camera altitude (km), w haze multiplier
    vec4 camera;         // x EV compensation, y manual EV100, z bloom intensity, w lens aperture radius (m)
    vec4 night;          // rgb night sky radiance, w firefly clamp
    uvec4 params0;       // x frame index, y max bounces, z samples per pixel, w flags
    vec4 params1;        // x luminance scale, y max history, z color sigma, w moving min alpha
    uvec4 size;          // x width, y height, z instance count, w atrous iterations
    uvec4 counts;        // x light count, y bloom levels, z camera flags, w pixel spread angle (float bits)
} ubo;

const uint FLAG_STATIC = 1u;
const uint FLAG_RESET = 2u;
const uint FLAG_ACCUMULATE = 4u;
const uint CAMERA_AUTO_EXPOSURE = 1u;
const uint CAMERA_LIGHT_SAMPLING = 2u;
const uint CAMERA_AERIAL = 4u;

// ---------------------------------------------------------------------------- images
layout(set = 0, binding = 6, rgba16f) uniform image2D noisyIllum;
layout(set = 0, binding = 7, rgba16f) uniform image2D gAlbedo;
layout(set = 0, binding = 8, rgba16f) uniform image2D gEmission;
layout(set = 0, binding = 9, rgba16f) uniform image2D gNormal[2];      // xyz normal, a roughness
layout(set = 0, binding = 10, rgba32f) uniform image2D gPos[2];        // xyz world position, a = 1 hit / 0 sky
layout(set = 0, binding = 11, rgba16f) uniform image2D historyColor[2];
layout(set = 0, binding = 12, rgba32f) uniform image2D historyMoments[2]; // x m1, y m2, z history length
layout(set = 0, binding = 13, rgba16f) uniform image2D filterTemp[2];  // rgb illumination, a variance
layout(set = 0, binding = 14, rgba8) uniform writeonly image2D outColor;
layout(set = 0, binding = 15, rgba32f) uniform image2D gGuide;      // xyz normal, w view distance (-1 = sky)

struct Light { uint instance; float cdf; float pdf; float pad; };
layout(std430, set = 0, binding = 16) readonly buffer Lights { Light lights[]; };

layout(std430, set = 0, binding = 17) buffer Exposure {
    uint histogram[256];
    float exposure;      // linear multiplier applied before tonemapping
    float ev100;         // current exposure value (for the UI)
    float averageLum;    // scene luminance (cd/m2)
    float pad;
} exposureData;

layout(set = 0, binding = 18, rgba16f) uniform image2D hdrColor;
layout(set = 0, binding = 19) uniform sampler2D hdrTex;
layout(set = 0, binding = 20, rgba16f) uniform image2D lutImage[3];   // 0 transmittance, 1 multiple scattering, 2 sky view
layout(set = 0, binding = 21) uniform sampler2D lutTex[3];
layout(set = 0, binding = 22, rgba16f) uniform image2D bloomImage[6];
layout(set = 0, binding = 23) uniform sampler2D bloomTex[6];

layout(set = 0, binding = 24) uniform sampler2D materialTex[1024];

layout(push_constant) uniform Push { ivec4 pass; } pc;

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

float Luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
