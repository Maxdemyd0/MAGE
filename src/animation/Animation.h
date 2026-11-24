// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - skeletal animation: rig data, clip sampling, blending, retargeting and CPU skinning.
// Pure functions without GPU or engine state, so the test suite runs them headless.

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "renderer/Mesh.h"

namespace mage {

struct JointPose {
    glm::vec3 translation{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    MAGE_API glm::mat4 Matrix() const;
    MAGE_API static JointPose FromMatrix(const glm::mat4& m); // shear is dropped
};
using Pose = std::vector<JointPose>; // local transforms, one per rig node

struct RigNode {
    std::string name;
    int parent = -1; // always smaller than the node's own index
    JointPose rest;
};

struct Skin {
    std::vector<int> nodes;              // rig node of every joint
    std::vector<glm::mat4> inverseBind;  // mesh space -> joint space
};

struct SkinInfluence { // sorted by descending weight, weights sum to 1
    uint16_t joints[4] = {0, 0, 0, 0};   // into Skin::nodes
    float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
};

template <class T>
struct AnimationKeys {
    std::vector<float> times; // ascending seconds
    std::vector<T> values;
    bool step = false;
};

struct AnimationTrack {
    int node = -1;
    AnimationKeys<glm::vec3> translation, scale;
    AnimationKeys<glm::quat> rotation;
};

struct AnimationClip {
    std::string name;
    float duration = 0.0f;
    int motionNode = -1; // topmost node whose translation moves (usually the hips)
    std::vector<AnimationTrack> tracks;
};

struct Rig {
    std::vector<RigNode> nodes;
    std::vector<Skin> skins;
    std::vector<AnimationClip> clips;

    bool Empty() const { return nodes.empty(); }
    MAGE_API int FindNode(const std::string& name) const; // exact, then ignoring case and namespace prefix
    MAGE_API int FindClip(const std::string& name) const; // exact, then ignoring case
    MAGE_API Pose RestPose() const;
};

// Sampling. Nodes without a track keep their rest transform. 'inPlace' removes the horizontal
// travel of the clip's motion node so that gameplay code can move the character.
MAGE_API void SamplePose(const Rig& rig, const AnimationClip& clip, float time, bool inPlace, Pose& out);
MAGE_API void BlendPoses(const Pose& a, const Pose& b, float t, Pose& out); // out may alias a or b
MAGE_API void ComputeGlobals(const Rig& rig, const Pose& pose, std::vector<glm::mat4>& out);

// Skins into model space. 'bind' and 'influences' have one entry per vertex.
MAGE_API void SkinMesh(const Skin& skin, const std::vector<glm::mat4>& globals, const std::vector<Vertex>& bind,
                       const std::vector<SkinInfluence>& influences, std::vector<Vertex>& out, AABB& bounds);

// Adds clips made for another file of the same skeleton (bones are matched by name; a different
// orientation of the root bone's parent and a different size are compensated). Returns the number
// of clips added; 'matched' receives the largest number of animated bones that exist in the target.
MAGE_API int RetargetClips(const Rig& source, Rig& target, const std::string& fallbackName, int* matched = nullptr);

// Fills duration and motionNode, drops empty tracks. Called by the importers.
MAGE_API void FinalizeClip(AnimationClip& clip);

} // namespace mage
