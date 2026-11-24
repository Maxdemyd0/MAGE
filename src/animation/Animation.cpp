// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "animation/Animation.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace mage {

namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
// "mixamorig:Hips" and "Hips" are the same bone.
std::string BaseName(const std::string& s) {
    const size_t p = s.find_last_of(':');
    return Lower(p == std::string::npos ? s : s.substr(p + 1));
}

glm::vec3 Mix(const glm::vec3& a, const glm::vec3& b, float t) { return glm::mix(a, b, t); }
glm::quat Mix(const glm::quat& a, const glm::quat& b, float t) { return glm::normalize(glm::slerp(a, b, t)); } // shortest arc

template <class T>
T SampleKeys(const AnimationKeys<T>& k, float time) {
    if (time <= k.times.front()) return k.values.front();
    if (time >= k.times.back()) return k.values.back();
    const size_t i = static_cast<size_t>(std::upper_bound(k.times.begin(), k.times.end(), time) - k.times.begin());
    if (k.step) return k.values[i - 1];
    const float span = k.times[i] - k.times[i - 1];
    return Mix(k.values[i - 1], k.values[i], span > 1e-8f ? (time - k.times[i - 1]) / span : 1.0f);
}

const AnimationTrack* FindTrack(const AnimationClip& clip, int node) {
    for (const AnimationTrack& t : clip.tracks)
        if (t.node == node) return &t;
    return nullptr;
}

} // namespace

glm::mat4 JointPose::Matrix() const {
    const glm::mat3 r = glm::mat3_cast(rotation);
    return glm::mat4(glm::vec4(r[0] * scale.x, 0.0f), glm::vec4(r[1] * scale.y, 0.0f), glm::vec4(r[2] * scale.z, 0.0f), glm::vec4(translation, 1.0f));
}

JointPose JointPose::FromMatrix(const glm::mat4& m) {
    JointPose p;
    p.translation = glm::vec3(m[3]);
    glm::mat3 r(m);
    p.scale = glm::vec3(glm::length(r[0]), glm::length(r[1]), glm::length(r[2]));
    if (glm::determinant(r) < 0.0f) p.scale.x = -p.scale.x;
    for (int i = 0; i < 3; ++i) {
        if (std::abs(p.scale[i]) < 1e-12f) return p; // degenerate: keep the identity rotation
        r[i] /= p.scale[i];
    }
    p.rotation = glm::normalize(glm::quat_cast(r));
    return p;
}

int Rig::FindNode(const std::string& name) const {
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].name == name) return static_cast<int>(i);
    const std::string base = BaseName(name);
    if (base.empty()) return -1;
    for (size_t i = 0; i < nodes.size(); ++i)
        if (BaseName(nodes[i].name) == base) return static_cast<int>(i);
    return -1;
}

int Rig::FindClip(const std::string& name) const {
    for (size_t i = 0; i < clips.size(); ++i)
        if (clips[i].name == name) return static_cast<int>(i);
    const std::string lower = Lower(name);
    for (size_t i = 0; i < clips.size(); ++i)
        if (Lower(clips[i].name) == lower) return static_cast<int>(i);
    return -1;
}

Pose Rig::RestPose() const {
    Pose p;
    p.reserve(nodes.size());
    for (const RigNode& n : nodes) p.push_back(n.rest);
    return p;
}

void SamplePose(const Rig& rig, const AnimationClip& clip, float time, bool inPlace, Pose& out) {
    out.resize(rig.nodes.size());
    for (size_t i = 0; i < out.size(); ++i) out[i] = rig.nodes[i].rest;
    for (const AnimationTrack& t : clip.tracks) {
        JointPose& j = out[static_cast<size_t>(t.node)];
        if (!t.translation.times.empty()) j.translation = SampleKeys(t.translation, time);
        if (!t.rotation.times.empty()) j.rotation = SampleKeys(t.rotation, time);
        if (!t.scale.times.empty()) j.scale = SampleKeys(t.scale, time);
    }
    if (!inPlace || clip.motionNode < 0) return;
    const AnimationTrack* track = FindTrack(clip, clip.motionNode);
    if (!track || track->translation.times.empty()) return;
    // Model-space displacement since the first key; only the vertical part is kept.
    glm::mat4 parent(1.0f);
    for (int n = rig.nodes[static_cast<size_t>(clip.motionNode)].parent; n >= 0; n = rig.nodes[static_cast<size_t>(n)].parent)
        parent = out[static_cast<size_t>(n)].Matrix() * parent;
    const glm::mat3 toModel(parent);
    if (std::abs(glm::determinant(toModel)) < 1e-18f) return;
    JointPose& j = out[static_cast<size_t>(clip.motionNode)];
    glm::vec3 moved = toModel * (j.translation - track->translation.values.front());
    moved.x = moved.z = 0.0f;
    j.translation = track->translation.values.front() + glm::inverse(toModel) * moved;
}

void BlendPoses(const Pose& a, const Pose& b, float t, Pose& out) {
    const size_t n = std::min(a.size(), b.size());
    out.resize(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = JointPose{Mix(a[i].translation, b[i].translation, t), Mix(a[i].rotation, b[i].rotation, t), Mix(a[i].scale, b[i].scale, t)};
}

void ComputeGlobals(const Rig& rig, const Pose& pose, std::vector<glm::mat4>& out) {
    out.resize(rig.nodes.size());
    for (size_t i = 0; i < rig.nodes.size(); ++i) {
        const glm::mat4 local = pose[i].Matrix();
        const int p = rig.nodes[i].parent;
        out[i] = p >= 0 ? out[static_cast<size_t>(p)] * local : local;
    }
}

void SkinMesh(const Skin& skin, const std::vector<glm::mat4>& globals, const std::vector<Vertex>& bind,
              const std::vector<SkinInfluence>& influences, std::vector<Vertex>& out, AABB& bounds) {
    std::vector<glm::mat4> joints(skin.nodes.size());
    for (size_t i = 0; i < joints.size(); ++i) joints[i] = globals[static_cast<size_t>(skin.nodes[i])] * skin.inverseBind[i];
    out.resize(bind.size());
    glm::vec3 mn(std::numeric_limits<float>::max()), mx(-std::numeric_limits<float>::max());
    for (size_t i = 0; i < bind.size(); ++i) {
        const SkinInfluence& w = influences[i];
        glm::mat4 m = joints[w.joints[0]] * w.weights[0];
        for (int k = 1; k < 4 && w.weights[k] > 0.0f; ++k) m += joints[w.joints[k]] * w.weights[k];
        const Vertex& v = bind[i];
        Vertex& o = out[i];
        o.position = glm::vec3(m[0]) * v.position.x + glm::vec3(m[1]) * v.position.y + glm::vec3(m[2]) * v.position.z + glm::vec3(m[3]);
        const glm::vec3 n = glm::vec3(m[0]) * v.normal.x + glm::vec3(m[1]) * v.normal.y + glm::vec3(m[2]) * v.normal.z;
        const float len2 = glm::dot(n, n);
        o.normal = len2 > 1e-20f ? n / std::sqrt(len2) : glm::vec3(0, 1, 0);
        o.uv = v.uv;
        mn = glm::min(mn, o.position);
        mx = glm::max(mx, o.position);
    }
    if (!bind.empty()) bounds = AABB{mn, mx};
}

void FinalizeClip(AnimationClip& clip) {
    std::erase_if(clip.tracks, [](const AnimationTrack& t) {
        return t.node < 0 || (t.translation.times.empty() && t.rotation.times.empty() && t.scale.times.empty());
    });
    clip.duration = 0.0f;
    clip.motionNode = -1;
    for (const AnimationTrack& t : clip.tracks) {
        for (const auto* times : {&t.translation.times, &t.rotation.times, &t.scale.times})
            if (!times->empty()) clip.duration = std::max(clip.duration, times->back());
        // Exporters bake constant translations on every node; only one that moves is body motion.
        bool moves = false;
        for (const glm::vec3& v : t.translation.values) moves |= glm::distance(v, t.translation.values.front()) > 1e-5f;
        if (moves && (clip.motionNode < 0 || t.node < clip.motionNode)) clip.motionNode = t.node;
    }
}

int RetargetClips(const Rig& source, Rig& target, const std::string& fallbackName, int* matched) {
    std::vector<int> map(source.nodes.size(), -1);
    for (size_t i = 0; i < map.size(); ++i) map[i] = target.FindNode(source.nodes[i].name);
    std::vector<glm::mat4> srcGlobal, dstGlobal;
    ComputeGlobals(source, source.RestPose(), srcGlobal);
    ComputeGlobals(target, target.RestPose(), dstGlobal);
    auto parentOf = [](const Rig& rig, const std::vector<glm::mat4>& globals, int node) {
        const int p = rig.nodes[static_cast<size_t>(node)].parent;
        return p >= 0 ? globals[static_cast<size_t>(p)] : glm::mat4(1.0f);
    };
    // Only real bones count when the target is skinned (an "Armature" helper node is not a bone).
    std::vector<bool> bone(target.nodes.size(), target.skins.empty());
    for (const Skin& s : target.skins)
        for (int n : s.nodes) bone[static_cast<size_t>(n)] = true;

    // Relative size from bone lengths: unlike the hip height they do not depend on the pose, and the
    // "rest pose" of an animation-only FBX is really its first frame.
    float srcSize = 0.0f, dstSize = 0.0f;
    for (size_t i = 0; i < map.size(); ++i) {
        const int sp = source.nodes[i].parent, dst = map[i];
        if (dst < 0 || sp < 0 || map[static_cast<size_t>(sp)] < 0 || target.nodes[static_cast<size_t>(dst)].parent != map[static_cast<size_t>(sp)]) continue;
        if (!bone[static_cast<size_t>(dst)] || !bone[static_cast<size_t>(map[static_cast<size_t>(sp)])]) continue;
        srcSize += glm::distance(glm::vec3(srcGlobal[i][3]), glm::vec3(srcGlobal[static_cast<size_t>(sp)][3]));
        dstSize += glm::distance(glm::vec3(dstGlobal[static_cast<size_t>(dst)][3]), glm::vec3(dstGlobal[static_cast<size_t>(map[static_cast<size_t>(sp)])][3]));
    }
    const float ratio = srcSize > 1e-4f && dstSize > 1e-4f ? dstSize / srcSize : 1.0f;

    int added = 0, best = 0;
    for (const AnimationClip& clip : source.clips) {
        // The topmost animated bone that exists in both rigs carries the body's motion.
        int root = -1;
        for (const AnimationTrack& t : clip.tracks) {
            const int dst = map[static_cast<size_t>(t.node)];
            if (dst >= 0 && bone[static_cast<size_t>(dst)] && (root < 0 || t.node < root)) root = t.node;
        }
        if (root < 0) continue;
        AnimationClip out;
        // Generic exporter names say nothing: use the file's name instead.
        const std::string lower = Lower(clip.name);
        const bool generic = lower.empty() || lower.ends_with("mixamo.com") || lower.starts_with("take 0") || lower == "scene";
        out.name = generic ? fallbackName : clip.name;
        const std::string baseName = out.name;
        for (int n = 2; target.FindClip(out.name) >= 0; ++n) out.name = baseName + "_" + std::to_string(n);

        for (const AnimationTrack& t : clip.tracks) {
            const int dst = map[static_cast<size_t>(t.node)];
            if (dst < 0 || !bone[static_cast<size_t>(dst)]) continue;
            if (FindTrack(out, dst)) continue; // two source bones with the same base name
            AnimationTrack nt;
            nt.node = dst;
            nt.rotation = t.rotation;
            if (t.node == root) {
                // Files may hang the same skeleton below differently oriented / scaled parents.
                const glm::mat4 sp = parentOf(source, srcGlobal, t.node), dp = parentOf(target, dstGlobal, dst);
                const glm::quat fix = glm::inverse(JointPose::FromMatrix(dp).rotation) * JointPose::FromMatrix(sp).rotation;
                for (glm::quat& q : nt.rotation.values) q = glm::normalize(fix * q);
                nt.translation = t.translation;
                const glm::mat4 toDst = glm::inverse(dp) * glm::scale(glm::mat4(1.0f), glm::vec3(ratio)) * sp;
                for (glm::vec3& v : nt.translation.values) v = glm::vec3(toDst * glm::vec4(v, 1.0f));
            }
            out.tracks.push_back(std::move(nt)); // other bones keep the target's own proportions
        }
        FinalizeClip(out);
        if (out.tracks.empty() || clip.duration <= 0.0f) continue;
        out.duration = clip.duration;
        best = std::max(best, static_cast<int>(out.tracks.size()));
        target.clips.push_back(std::move(out));
        ++added;
    }
    if (matched) *matched = best;
    return added;
}

} // namespace mage
