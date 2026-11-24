// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once

#include <cstdint>
#include <glm/glm.hpp>

namespace mage {

using TextureHandle = uint32_t; // 0 = no texture

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
    // Textures multiply the factors above (glTF conventions).
    TextureHandle baseColorTexture = 0;         // sRGB, alpha used with alphaCutoff
    TextureHandle metallicRoughnessTexture = 0; // G roughness, B metallic
    TextureHandle normalTexture = 0;            // tangent space, +Y up
    TextureHandle emissiveTexture = 0;          // sRGB
    float normalScale = 1.0f;
    float alpha = 1.0f;
    float alphaCutoff = 0.0f;                   // > 0: cut out texels below this alpha
    bool doubleSided = false;
};

} // namespace mage
