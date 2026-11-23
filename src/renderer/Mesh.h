// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - GPU meshes and procedural primitives.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace mage {

using MeshHandle = uint32_t;
inline constexpr MeshHandle InvalidMesh = 0xFFFFFFFFu;

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

struct MAGE_API AABB {
    glm::vec3 min{0.0f};
    glm::vec3 max{0.0f};

    // World-space AABB of this box transformed by an affine matrix (Arvo's method).
    AABB Transformed(const glm::mat4& m) const;
};

struct Mesh {
    std::string name;
    uint32_t vao = 0, vbo = 0, ibo = 0;
    uint32_t indexCount = 0;
    AABB bounds;
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

namespace Primitives {
MAGE_API MeshData Cube();                         // unit cube, [-0.5, 0.5]^3
MAGE_API MeshData Plane();                        // unit quad on XZ, facing +Y
MAGE_API MeshData Sphere(int stacks = 24, int slices = 32);  // diameter 1
MAGE_API MeshData Cylinder(int segments = 32);    // radius 0.5, height 1, axis +Y
MAGE_API MeshData Cone(int segments = 32);        // base radius 0.5 at y=-0.5, apex at y=+0.5
} // namespace Primitives

class MAGE_API MeshLibrary {
public:
    ~MeshLibrary();
    void CreateDefaultPrimitives();
    MeshHandle Add(const std::string& name, const MeshData& data);
    MeshHandle Find(const std::string& name) const;
    const Mesh* Get(MeshHandle handle) const;
    // CPU copy of the geometry (used to build ray tracing acceleration structures).
    const MeshData* GetData(MeshHandle handle) const;
    const std::vector<Mesh>& All() const { return m_meshes; }
    void Destroy();

private:
    std::vector<Mesh> m_meshes;
    std::vector<MeshData> m_data;
    std::unordered_map<std::string, MeshHandle> m_byName;
};

} // namespace mage
