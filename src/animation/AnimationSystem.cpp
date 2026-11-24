// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "animation/AnimationSystem.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace mage {

// A few threads that run one indexed loop at a time; the calling thread helps.
struct AnimationSystem::Workers {
    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable wake, done;
    const std::function<void(size_t)>* fn = nullptr;
    std::atomic<size_t> next{0};
    size_t count = 0, busy = 0;
    uint64_t generation = 0;
    bool quit = false;

    Workers() {
        const unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 8u);
        for (unsigned i = 1; i < n; ++i) threads.emplace_back([this] { Run(); });
    }
    ~Workers() {
        {
            std::lock_guard lock(mutex);
            quit = true;
        }
        wake.notify_all();
        for (std::thread& t : threads) t.join();
    }
    void Run() {
        uint64_t seen = 0;
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [&] { return quit || generation != seen; });
            if (quit) return;
            seen = generation;
            const auto* f = fn;
            const size_t n = count;
            lock.unlock();
            for (size_t i; (i = next.fetch_add(1)) < n;) (*f)(i);
            lock.lock();
            if (--busy == 0) done.notify_one();
        }
    }
    void ParallelFor(size_t n, const std::function<void(size_t)>& f) {
        if (n < 2 || threads.empty()) {
            for (size_t i = 0; i < n; ++i) f(i);
            return;
        }
        {
            std::lock_guard lock(mutex);
            fn = &f;
            count = n;
            next = 0;
            busy = threads.size();
            ++generation;
        }
        wake.notify_all();
        for (size_t i; (i = next.fetch_add(1)) < n;) f(i);
        std::unique_lock lock(mutex);
        done.wait(lock, [&] { return busy == 0; });
    }
};

AnimationSystem::AnimationSystem() : m_workers(std::make_unique<Workers>()) {}
AnimationSystem::~AnimationSystem() = default;

const AnimationSystem::Instance* AnimationSystem::Get(Entity e) const {
    const auto it = m_instances.find(e);
    return it == m_instances.end() || !it->second->valid ? nullptr : it->second.get();
}

bool PlayClip(AnimatorComponent& a, const Rig& rig, int clip, float fade, bool loop, float speed) {
    if (clip < 0 || clip >= static_cast<int>(rig.clips.size())) return false;
    if (!std::isfinite(speed)) speed = 1.0f;
    if (a.clip == clip && a.playing && a.loop && loop) {
        a.speed = speed;
        return true;
    }
    if (fade > 0.0f && std::isfinite(fade)) {
        // Interrupting a fade freezes the blended pose and fades from there, so nothing pops.
        a.fromSnapshot = a.captureSnapshot = a.fadeTime < a.fadeDuration;
        a.prevClip = a.clip;
        a.prevTime = a.time;
        a.prevSpeed = a.playing ? a.speed : 0.0f;
        a.prevLoop = a.loop;
        a.fadeTime = 0.0f;
        a.fadeDuration = fade;
    } else {
        a.fadeTime = a.fadeDuration = 0.0f;
        a.prevClip = -1;
        a.fromSnapshot = a.captureSnapshot = false;
    }
    a.clip = clip;
    a.speed = speed;
    a.time = speed < 0.0f ? rig.clips[static_cast<size_t>(clip)].duration : 0.0f;
    a.loop = loop;
    a.playing = true;
    return true;
}

namespace {

// Advances a playback position; returns false when a one-shot clip has ended.
bool Advance(float& time, float duration, float delta, bool loop) {
    time += delta;
    if (!std::isfinite(time) || duration <= 0.0f) {
        time = 0.0f;
        return loop;
    }
    if (loop) {
        time = std::fmod(time, duration);
        if (time < 0.0f) time += duration;
        return true;
    }
    const bool ended = delta > 0.0f ? time >= duration : delta < 0.0f && time <= 0.0f;
    time = std::clamp(time, 0.0f, duration);
    return !ended;
}

bool SameInputs(const AnimatorComponent& a, const AnimatorComponent& b) {
    return a.clip == b.clip && a.time == b.time && a.inPlace == b.inPlace && a.fadeTime == b.fadeTime && a.fadeDuration == b.fadeDuration &&
           a.prevClip == b.prevClip && a.prevTime == b.prevTime && a.fromSnapshot == b.fromSnapshot;
}

} // namespace

void AnimationSystem::Update(Registry& registry, const AssetManager& assets, const MeshLibrary& meshes, float dt) {
    ++m_frame;
    m_jobs.clear();
    registry.Each<AnimatorComponent, ModelComponent>([&](Entity e, AnimatorComponent& a, ModelComponent& mc) {
        const Model* model = assets.GetModel(mc.model);
        if (!model || model->rig.Empty()) return;
        const Rig& rig = model->rig;
        std::unique_ptr<Instance>& slot = m_instances[e];
        if (!slot || slot->model != mc.model) {
            slot = std::make_unique<Instance>();
            slot->model = mc.model;
            slot->pose = rig.RestPose();
            slot->geometry.resize(model->parts.size());
            slot->vertices.resize(model->parts.size());
            for (size_t i = 0; i < model->parts.size(); ++i)
                if (model->parts[i].skin >= 0) slot->geometry[i].id = m_nextGeometryId++;
        }
        Instance& inst = *slot;
        inst.touched = m_frame;
        if (a.captureSnapshot) {
            inst.snapshot = inst.pose;
            a.captureSnapshot = false;
        }
        if (a.clip >= static_cast<int>(rig.clips.size())) a.clip = -1;
        if (a.prevClip >= static_cast<int>(rig.clips.size())) a.prevClip = -1;
        if (a.clip >= 0 && a.playing)
            a.playing = Advance(a.time, rig.clips[static_cast<size_t>(a.clip)].duration, dt * a.speed, a.loop);
        if (a.fadeTime < a.fadeDuration) {
            a.fadeTime += dt;
            if (a.prevClip >= 0 && !a.fromSnapshot)
                Advance(a.prevTime, rig.clips[static_cast<size_t>(a.prevClip)].duration, dt * a.prevSpeed, a.prevLoop);
            if (a.fadeTime >= a.fadeDuration) {
                a.fadeTime = a.fadeDuration = 0.0f;
                a.prevClip = -1;
                a.fromSnapshot = false;
            }
        }
        if (!mc.visible || (inst.valid && SameInputs(inst.evaluated, a))) return;
        m_jobs.push_back({&inst, model, a});
    });
    // Entities that were destroyed or lost their animator.
    std::erase_if(m_instances, [&](const auto& entry) { return entry.second->touched != m_frame; });

    m_workers->ParallelFor(m_jobs.size(), [&](size_t i) { Evaluate(m_jobs[i], meshes); });
}

void AnimationSystem::Evaluate(const Job& job, const MeshLibrary& meshes) {
    Instance& inst = *job.instance;
    const Rig& rig = job.model->rig;
    const AnimatorComponent& a = job.animator;
    if (a.clip >= 0) SamplePose(rig, rig.clips[static_cast<size_t>(a.clip)], a.time, a.inPlace, inst.pose);
    else inst.pose = rig.RestPose();
    if (a.fadeTime < a.fadeDuration) {
        const float t = a.fadeTime / a.fadeDuration;
        const Pose* from = &inst.snapshot;
        if (!a.fromSnapshot || inst.snapshot.size() != rig.nodes.size()) {
            if (a.prevClip >= 0) SamplePose(rig, rig.clips[static_cast<size_t>(a.prevClip)], a.prevTime, a.inPlace, inst.from);
            else inst.from = rig.RestPose();
            from = &inst.from;
        }
        BlendPoses(*from, inst.pose, t * t * (3.0f - 2.0f * t), inst.pose);
    }
    ComputeGlobals(rig, inst.pose, inst.globals);
    bool first = true;
    for (size_t i = 0; i < job.model->parts.size(); ++i) {
        const Model::Part& part = job.model->parts[i];
        const MeshData* data = meshes.GetData(part.mesh);
        const Mesh* mesh = meshes.Get(part.mesh);
        if (!data || !mesh) continue;
        AABB box;
        if (part.skin >= 0) {
            SkinMesh(rig.skins[static_cast<size_t>(part.skin)], inst.globals, data->vertices, *part.influences, inst.vertices[i], box);
            SkinnedGeometry& g = inst.geometry[i];
            g.vertices = &inst.vertices[i];
            g.bounds = box;
            ++g.version;
        } else {
            box = mesh->bounds.Transformed(part.node >= 0 ? inst.globals[static_cast<size_t>(part.node)] : part.transform);
        }
        inst.bounds = first ? box : AABB{glm::min(inst.bounds.min, box.min), glm::max(inst.bounds.max, box.max)};
        first = false;
    }
    inst.evaluated = a;
    inst.valid = true;
}

} // namespace mage
