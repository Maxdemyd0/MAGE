#include "renderer/Mesh.h"
#include "renderer/GL.h"

#include <cmath>
#include <cstddef>

namespace mage {

using namespace gl;

namespace {
constexpr float kPi = 3.14159265358979f;
}

// ----------------------------------------------------------------------------- AABB
AABB AABB::Transformed(const glm::mat4& m) const {
    const glm::vec3 center = (min + max) * 0.5f;
    const glm::vec3 extent = (max - min) * 0.5f;
    const glm::vec3 newCenter = glm::vec3(m * glm::vec4(center, 1.0f));
    glm::vec3 newExtent(0.0f);
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            newExtent[i] += std::abs(m[j][i]) * extent[j];
    return AABB{newCenter - newExtent, newCenter + newExtent};
}

// ----------------------------------------------------------------------------- Primitives
namespace Primitives {

MeshData Cube() {
    MeshData d;
    const glm::vec3 normals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const glm::vec3& n : normals) {
        // u x v == n guarantees counter-clockwise winding seen from outside.
        const glm::vec3 u = std::abs(n.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
        const glm::vec3 v = glm::cross(n, u);
        const uint32_t base = static_cast<uint32_t>(d.vertices.size());
        const glm::vec3 c = n * 0.5f;
        d.vertices.push_back({c - u * 0.5f - v * 0.5f, n, {0, 0}});
        d.vertices.push_back({c + u * 0.5f - v * 0.5f, n, {1, 0}});
        d.vertices.push_back({c + u * 0.5f + v * 0.5f, n, {1, 1}});
        d.vertices.push_back({c - u * 0.5f + v * 0.5f, n, {0, 1}});
        d.indices.insert(d.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return d;
}

MeshData Plane() {
    MeshData d;
    const glm::vec3 n(0, 1, 0);
    d.vertices = {{{-0.5f, 0, -0.5f}, n, {0, 0}}, {{0.5f, 0, -0.5f}, n, {1, 0}},
                  {{0.5f, 0, 0.5f}, n, {1, 1}},   {{-0.5f, 0, 0.5f}, n, {0, 1}}};
    d.indices = {0, 2, 1, 0, 3, 2};
    return d;
}

MeshData Sphere(int stacks, int slices) {
    MeshData d;
    for (int i = 0; i <= stacks; ++i) {
        const float phi = kPi * static_cast<float>(i) / static_cast<float>(stacks);
        for (int j = 0; j <= slices; ++j) {
            const float theta = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(slices);
            const glm::vec3 n(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
            d.vertices.push_back({n * 0.5f, n, {static_cast<float>(j) / slices, static_cast<float>(i) / stacks}});
        }
    }
    const uint32_t ring = static_cast<uint32_t>(slices + 1);
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            const uint32_t a = static_cast<uint32_t>(i) * ring + static_cast<uint32_t>(j);
            const uint32_t b = a + ring;
            d.indices.insert(d.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }
    return d;
}

namespace {
void AddCap(MeshData& d, int segments, float y, bool top) {
    const glm::vec3 n(0, top ? 1.0f : -1.0f, 0);
    const uint32_t center = static_cast<uint32_t>(d.vertices.size());
    d.vertices.push_back({{0, y, 0}, n, {0.5f, 0.5f}});
    for (int j = 0; j <= segments; ++j) {
        const float t = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(segments);
        const float c = std::cos(t), s = std::sin(t);
        d.vertices.push_back({{0.5f * c, y, 0.5f * s}, n, {0.5f + 0.5f * c, 0.5f + 0.5f * s}});
    }
    for (int j = 0; j < segments; ++j) {
        const uint32_t p0 = center + 1 + static_cast<uint32_t>(j);
        const uint32_t p1 = p0 + 1;
        if (top) d.indices.insert(d.indices.end(), {center, p1, p0});
        else d.indices.insert(d.indices.end(), {center, p0, p1});
    }
}
} // namespace

MeshData Cylinder(int segments) {
    MeshData d;
    for (int j = 0; j <= segments; ++j) {
        const float t = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(segments);
        const float c = std::cos(t), s = std::sin(t);
        const glm::vec3 n(c, 0, s);
        const float u = static_cast<float>(j) / segments;
        d.vertices.push_back({{0.5f * c, -0.5f, 0.5f * s}, n, {u, 0}});
        d.vertices.push_back({{0.5f * c, 0.5f, 0.5f * s}, n, {u, 1}});
    }
    for (int j = 0; j < segments; ++j) {
        const uint32_t b0 = static_cast<uint32_t>(j) * 2, t0 = b0 + 1, b1 = b0 + 2, t1 = b0 + 3;
        d.indices.insert(d.indices.end(), {b0, t0, b1, b1, t0, t1});
    }
    AddCap(d, segments, 0.5f, true);
    AddCap(d, segments, -0.5f, false);
    return d;
}

MeshData Cone(int segments) {
    MeshData d;
    for (int j = 0; j <= segments; ++j) {
        const float t = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(segments);
        const float c = std::cos(t), s = std::sin(t);
        const glm::vec3 n = glm::normalize(glm::vec3(c, 0.5f, s));
        const float u = static_cast<float>(j) / segments;
        d.vertices.push_back({{0.5f * c, -0.5f, 0.5f * s}, n, {u, 0}});
        d.vertices.push_back({{0.0f, 0.5f, 0.0f}, n, {u, 1}});
    }
    for (int j = 0; j < segments; ++j) {
        const uint32_t b0 = static_cast<uint32_t>(j) * 2, t0 = b0 + 1, b1 = b0 + 2;
        d.indices.insert(d.indices.end(), {b0, t0, b1});
    }
    AddCap(d, segments, -0.5f, false);
    return d;
}

} // namespace Primitives

// ----------------------------------------------------------------------------- MeshLibrary
MeshLibrary::~MeshLibrary() = default;

void MeshLibrary::CreateDefaultPrimitives() {
    Add("cube", Primitives::Cube());
    Add("plane", Primitives::Plane());
    Add("sphere", Primitives::Sphere());
    Add("cylinder", Primitives::Cylinder());
    Add("cone", Primitives::Cone());
}

MeshHandle MeshLibrary::Add(const std::string& name, const MeshData& data) {
    Mesh mesh;
    mesh.name = name;
    mesh.indexCount = static_cast<uint32_t>(data.indices.size());

    glm::vec3 mn(1e30f), mx(-1e30f);
    for (const Vertex& v : data.vertices) {
        mn = glm::min(mn, v.position);
        mx = glm::max(mx, v.position);
    }
    mesh.bounds = AABB{mn, mx};

    glGenVertexArrays(1, &mesh.vao);
    glBindVertexArray(mesh.vao);
    glGenBuffers(1, &mesh.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.vertices.size() * sizeof(Vertex)), data.vertices.data(), GL_STATIC_DRAW);
    glGenBuffers(1, &mesh.ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, mesh.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(data.indices.size() * sizeof(uint32_t)), data.indices.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, normal)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<const void*>(offsetof(Vertex, uv)));
    glBindVertexArray(0);

    const MeshHandle handle = static_cast<MeshHandle>(m_meshes.size());
    m_meshes.push_back(mesh);
    m_byName[name] = handle;
    return handle;
}

MeshHandle MeshLibrary::Find(const std::string& name) const {
    auto it = m_byName.find(name);
    return it == m_byName.end() ? InvalidMesh : it->second;
}

const Mesh* MeshLibrary::Get(MeshHandle handle) const {
    return handle < m_meshes.size() ? &m_meshes[handle] : nullptr;
}

void MeshLibrary::Destroy() {
    for (Mesh& m : m_meshes) {
        if (m.ibo) glDeleteBuffers(1, &m.ibo);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
    }
    m_meshes.clear();
    m_byName.clear();
}

} // namespace mage
