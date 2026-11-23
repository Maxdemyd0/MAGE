// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "core/ECS.h"

#include <glm/gtc/matrix_transform.hpp>

#include <mutex>
#include <string>
#include <unordered_map>

namespace mage {

uint32_t detail::ComponentTypeIdFromName(const char* uniqueTypeName) {
    static std::mutex mutex;
    static std::unordered_map<std::string, uint32_t> ids;
    std::lock_guard lock(mutex);
    auto [it, inserted] = ids.try_emplace(uniqueTypeName, static_cast<uint32_t>(ids.size()));
    return it->second;
}

glm::mat4 TransformComponent::Matrix() const {
    glm::mat4 m = glm::translate(glm::mat4(1.0f), position);
    m *= glm::mat4_cast(rotation);
    return glm::scale(m, scale);
}

Entity Registry::Create() {
    uint32_t index;
    if (!m_freeList.empty()) {
        index = m_freeList.back();
        m_freeList.pop_back();
    } else {
        index = static_cast<uint32_t>(m_generations.size());
        if (index > kEntityIndexMask) return NullEntity;
        m_generations.push_back(0);
        m_alive.push_back(0);
    }
    m_alive[index] = 1;
    ++m_aliveCount;
    return MakeEntity(index, m_generations[index]);
}

void Registry::Destroy(Entity e) {
    if (!Valid(e)) return;
    for (auto& pool : m_pools)
        if (pool) pool->Remove(e);
    const uint32_t index = EntityIndex(e);
    m_alive[index] = 0;
    m_generations[index] = (m_generations[index] + 1) & kEntityGenerationMask;
    m_freeList.push_back(index);
    --m_aliveCount;
}

bool Registry::Valid(Entity e) const {
    if (e == NullEntity) return false;
    const uint32_t index = EntityIndex(e);
    return index < m_generations.size() && m_alive[index] && m_generations[index] == EntityGeneration(e);
}

void Registry::Clear() {
    m_pools.clear();
    m_generations.clear();
    m_alive.clear();
    m_freeList.clear();
    m_aliveCount = 0;
}

} // namespace mage
