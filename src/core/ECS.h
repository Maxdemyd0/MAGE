// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
// MAGE v1 - lightweight sparse-set Entity Component System.
//
// Entities are 32-bit handles: 20-bit index + 12-bit generation (stale handles are detected).
// Every component type gets its own densely packed pool (sparse set), so iteration over a
// component type is a linear walk over contiguous memory.

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace mage {

// ============================================================================ Entity handle
using Entity = uint32_t;
inline constexpr Entity NullEntity = 0xFFFFFFFFu;
inline constexpr uint32_t kEntityIndexBits = 20;
inline constexpr uint32_t kEntityIndexMask = (1u << kEntityIndexBits) - 1u;
inline constexpr uint32_t kEntityGenerationMask = 0xFFFu;

constexpr uint32_t EntityIndex(Entity e) { return e & kEntityIndexMask; }
constexpr uint32_t EntityGeneration(Entity e) { return (e >> kEntityIndexBits) & kEntityGenerationMask; }
constexpr Entity MakeEntity(uint32_t index, uint32_t generation) {
    return (index & kEntityIndexMask) | ((generation & kEntityGenerationMask) << kEntityIndexBits);
}

// ============================================================================ Components
struct NameComponent {
    std::string name;
};

struct TransformComponent {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // (w, x, y, z)
    glm::vec3 scale{1.0f};

    glm::mat4 Matrix() const;
};

struct MeshComponent {
    uint32_t mesh = 0;               // MeshHandle from the renderer's MeshLibrary
    glm::vec3 albedo{0.8f};
    float metallic = 0.0f;
    float roughness = 0.6f;
    float checkerScale = 0.0f;       // > 0 : world-space procedural checkerboard (meters per tile)
    glm::vec3 emissive{0.0f};
    bool visible = true;
    bool castShadows = true;
};

enum class BodyMotion : uint8_t { Static, Kinematic, Dynamic };

struct RigidBodyComponent {
    uint32_t body = 0xFFFFFFFFu;     // PhysicsWorld BodyHandle
    BodyMotion motion = BodyMotion::Static;
    // Fixed-step interpolation state (previous / current physics pose).
    glm::vec3 prevPosition{0.0f};
    glm::vec3 currPosition{0.0f};
    glm::quat prevRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::quat currRotation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct VehicleComponent {
    uint32_t vehicle = 0xFFFFFFFFu;  // PhysicsWorld VehicleHandle
    glm::vec3 bodyHalfExtents{0.9f, 0.3f, 2.2f};
    // Driver input (written by C# scripts or native controllers, consumed every fixed step).
    float throttle = 0.0f;           // -1..1 (negative = reverse)
    float steering = 0.0f;           // -1..1 (positive = right)
    float brake = 0.0f;              //  0..1
    float handBrake = 0.0f;          //  0..1
};

struct ScriptComponent {
    std::string className;           // fully-qualified C# type, e.g. "GameScripts.TestVehicleScript"
    bool instantiated = false;
};

struct SectorComponent {
    int32_t x = 0;
    int32_t z = 0;
};

// ============================================================================ Pools
namespace detail {
inline uint32_t NextComponentTypeId() {
    static uint32_t counter = 0;
    return counter++;
}
template <class T>
uint32_t ComponentTypeId() {
    static const uint32_t id = NextComponentTypeId();
    return id;
}
} // namespace detail

class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    virtual bool Contains(Entity e) const = 0;
    virtual void Remove(Entity e) = 0;
    virtual size_t Size() const = 0;
};

template <class T>
class ComponentPool final : public IComponentPool {
public:
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

    template <class... Args>
    T& Emplace(Entity e, Args&&... args) {
        const uint32_t idx = EntityIndex(e);
        if (idx >= m_sparse.size()) m_sparse.resize(static_cast<size_t>(idx) + 1, kInvalid);
        if (m_sparse[idx] != kInvalid) {
            const uint32_t slot = m_sparse[idx];
            m_dense[slot] = e;
            m_data[slot] = T(std::forward<Args>(args)...);
            return m_data[slot];
        }
        m_sparse[idx] = static_cast<uint32_t>(m_dense.size());
        m_dense.push_back(e);
        m_data.emplace_back(std::forward<Args>(args)...);
        return m_data.back();
    }

    bool Contains(Entity e) const override {
        const uint32_t idx = EntityIndex(e);
        return idx < m_sparse.size() && m_sparse[idx] != kInvalid && m_dense[m_sparse[idx]] == e;
    }

    T* TryGet(Entity e) { return Contains(e) ? &m_data[m_sparse[EntityIndex(e)]] : nullptr; }
    const T* TryGet(Entity e) const { return Contains(e) ? &m_data[m_sparse[EntityIndex(e)]] : nullptr; }

    void Remove(Entity e) override {
        if (!Contains(e)) return;
        const uint32_t idx = EntityIndex(e);
        const uint32_t slot = m_sparse[idx];
        const uint32_t last = static_cast<uint32_t>(m_dense.size() - 1);
        if (slot != last) {
            m_dense[slot] = m_dense[last];
            m_data[slot] = std::move(m_data[last]);
            m_sparse[EntityIndex(m_dense[slot])] = slot;
        }
        m_dense.pop_back();
        m_data.pop_back();
        m_sparse[idx] = kInvalid;
    }

    size_t Size() const override { return m_dense.size(); }
    Entity EntityAt(size_t i) const { return m_dense[i]; }
    T& DataAt(size_t i) { return m_data[i]; }

private:
    std::vector<uint32_t> m_sparse;
    std::vector<Entity> m_dense;
    std::vector<T> m_data;
};

// ============================================================================ Registry
class Registry {
public:
    Entity Create();
    void Destroy(Entity e);
    bool Valid(Entity e) const;
    size_t AliveCount() const { return m_aliveCount; }
    void Clear();

    template <class T, class... Args>
    T& Add(Entity e, Args&&... args) { return Pool<T>().Emplace(e, std::forward<Args>(args)...); }

    template <class T>
    void Remove(Entity e) {
        if (auto* pool = FindPool<T>()) pool->Remove(e);
    }

    template <class T>
    bool Has(Entity e) const {
        const auto* pool = FindPool<T>();
        return pool && pool->Contains(e);
    }

    template <class T>
    T* TryGet(Entity e) {
        auto* pool = FindPool<T>();
        return pool ? pool->TryGet(e) : nullptr;
    }
    template <class T>
    const T* TryGet(Entity e) const {
        const auto* pool = FindPool<T>();
        return pool ? pool->TryGet(e) : nullptr;
    }

    template <class T>
    T& Get(Entity e) { return *Pool<T>().TryGet(e); }

    // Iterate every entity owning all of <First, Rest...>: fn(Entity, First&, Rest&...).
    // Adding/removing *other* entities' components during iteration is safe.
    template <class First, class... Rest, class Fn>
    void Each(Fn&& fn) {
        auto& first = Pool<First>();
        for (size_t i = 0; i < first.Size(); ++i) {
            const Entity e = first.EntityAt(i);
            if constexpr (sizeof...(Rest) == 0) {
                fn(e, first.DataAt(i));
            } else {
                if (!(Has<Rest>(e) && ...)) continue;
                fn(e, first.DataAt(i), *Pool<Rest>().TryGet(e)...);
            }
        }
    }

    // Iterate all live entities: fn(Entity).
    template <class Fn>
    void ForEachEntity(Fn&& fn) const {
        for (uint32_t i = 0; i < m_generations.size(); ++i)
            if (m_alive[i]) fn(MakeEntity(i, m_generations[i]));
    }

    template <class T>
    ComponentPool<T>& Pool() {
        const uint32_t id = detail::ComponentTypeId<T>();
        if (id >= m_pools.size()) m_pools.resize(static_cast<size_t>(id) + 1);
        if (!m_pools[id]) m_pools[id] = std::make_unique<ComponentPool<T>>();
        return *static_cast<ComponentPool<T>*>(m_pools[id].get());
    }

private:
    template <class T>
    ComponentPool<T>* FindPool() const {
        const uint32_t id = detail::ComponentTypeId<T>();
        return id < m_pools.size() ? static_cast<ComponentPool<T>*>(m_pools[id].get()) : nullptr;
    }

    std::vector<uint32_t> m_generations;
    std::vector<uint8_t> m_alive;
    std::vector<uint32_t> m_freeList;
    std::vector<std::unique_ptr<IComponentPool>> m_pools;
    size_t m_aliveCount = 0;
};

} // namespace mage
