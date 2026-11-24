// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - per-entity animation playback. Advances every AnimatorComponent, evaluates poses and
// skins the meshes on worker threads, and hands the result to the renderer.

#include <memory>
#include <unordered_map>
#include <vector>

#include "animation/Animation.h"
#include "assets/AssetManager.h"
#include "core/ECS.h"
#include "renderer/Renderer.h"

namespace mage {

// Crossfades from whatever is playing to a clip. Returns false if the clip index is invalid.
// Calling it again for a looping clip that is already playing does not restart it.
MAGE_API bool PlayClip(AnimatorComponent& a, const Rig& rig, int clip, float fadeSeconds, bool loop, float speed);

class MAGE_API AnimationSystem {
public:
    struct Instance {
        uint32_t model = 0xFFFFFFFFu;
        Pose pose;
        std::vector<glm::mat4> globals;        // model space, one per rig node
        std::vector<SkinnedGeometry> geometry; // one per model part (id 0 = rigid part)
        std::vector<std::vector<Vertex>> vertices;
        AABB bounds;

        // Internal evaluation state.
        Pose from, snapshot;
        AnimatorComponent evaluated; // inputs of the current pose, to skip unchanged work
        bool valid = false;
        uint64_t touched = 0;
    };

    AnimationSystem();
    ~AnimationSystem();
    AnimationSystem(const AnimationSystem&) = delete;
    AnimationSystem& operator=(const AnimationSystem&) = delete;

    void Update(Registry& registry, const AssetManager& assets, const MeshLibrary& meshes, float dt);
    const Instance* Get(Entity e) const; // null if the entity is not animated (yet)
    size_t Count() const { return m_instances.size(); }

private:
    struct Workers;
    struct Job {
        Instance* instance;
        const Model* model;
        AnimatorComponent animator;
    };
    static void Evaluate(const Job& job, const MeshLibrary& meshes);

    std::unique_ptr<Workers> m_workers;
    std::unordered_map<Entity, std::unique_ptr<Instance>> m_instances;
    std::vector<Job> m_jobs;
    uint64_t m_frame = 0;
    uint64_t m_nextGeometryId = 1;
};

} // namespace mage
