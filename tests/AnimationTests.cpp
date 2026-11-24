// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
// Headless tests for rigs, clips, blending, skinning and retargeting.
// With arguments, prints what a file contains instead:  MAGE_AnimationTests <model> [<clips for it>]
#include "animation/AnimationSystem.h"
#include "assets/Importers.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>

using namespace mage;
namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static fs::path g_dir;
static bool Near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) { return glm::distance(a, b) < eps; }
static bool Near(float a, float b, float eps = 1e-4f) { return std::abs(a - b) < eps; }
static glm::quat RotZ(float deg) { return glm::angleAxis(glm::radians(deg), glm::vec3(0, 0, 1)); }

static fs::path WriteText(const char* name, const std::string& s) {
    const fs::path p = g_dir / name;
    std::ofstream(p, std::ios::binary) << s;
    return p;
}

// Two bones along +Y (1 m each); the clip bends the second one 90 degrees and moves the root.
static Rig MakeRig() {
    Rig r;
    r.nodes.resize(3);
    r.nodes[0].name = "Root";
    r.nodes[1] = {"Hips", 0, {}};
    r.nodes[2] = {"mixamorig:Spine", 1, {}};
    r.nodes[2].rest.translation = {0, 1, 0};
    Skin s;
    s.nodes = {1, 2};
    s.inverseBind = {glm::mat4(1.0f), glm::inverse(r.nodes[2].rest.Matrix())};
    r.skins.push_back(s);
    AnimationClip c;
    c.name = "Bend";
    AnimationTrack hips, spine;
    hips.node = 1;
    hips.translation = {{0, 1}, {{0, 0, 0}, {0, 0.5f, 4}}, false};
    spine.node = 2;
    spine.rotation = {{0, 1}, {glm::quat(1, 0, 0, 0), RotZ(90)}, false};
    c.tracks = {spine, hips};
    FinalizeClip(c);
    r.clips.push_back(c);
    return r;
}

static void TestSampling() {
    const Rig rig = MakeRig();
    const AnimationClip& c = rig.clips[0];
    CHECK(Near(c.duration, 1.0f) && c.motionNode == 1);
    Pose p;
    std::vector<glm::mat4> g;
    SamplePose(rig, c, 0.5f, false, p);
    CHECK(Near(p[1].translation, {0, 0.25f, 2}));
    CHECK(Near(glm::degrees(glm::angle(p[2].rotation)), 45.0f, 0.01f));
    CHECK(Near(p[2].translation, {0, 1, 0})); // untouched channel keeps the rest pose
    SamplePose(rig, c, -5.0f, false, p);
    CHECK(Near(p[1].translation, glm::vec3(0.0f)));
    SamplePose(rig, c, 99.0f, true, p); // in place: vertical motion stays
    CHECK(Near(p[1].translation, {0, 0.5f, 0}));
    ComputeGlobals(rig, p, g);
    CHECK(Near(glm::vec3(g[2] * glm::vec4(0, 1, 0, 1)), {-1, 1.5f, 0}));
    // In place below a rotated, scaled parent (FBX style: Z up, centimetres).
    Rig turned = rig;
    turned.nodes[0].rest.rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1, 0, 0));
    turned.nodes[0].rest.scale = glm::vec3(0.01f);
    SamplePose(turned, turned.clips[0], 1.0f, true, p);
    ComputeGlobals(turned, p, g);
    CHECK(Near(glm::vec3(g[1][3]), {0, 0.04f, 0})); // local z is up here

    Pose a = rig.RestPose(), b = a, out;
    b[2].rotation = -RotZ(90); // same rotation, opposite sign: blending must take the short way
    BlendPoses(a, b, 0.5f, out);
    CHECK(Near(std::abs(glm::dot(out[2].rotation, RotZ(45))), 1.0f));
    BlendPoses(a, b, 0.0f, a);
    CHECK(Near(a[2].rotation.w, 1.0f));
    CHECK(rig.FindNode("Spine") == 2 && rig.FindNode("other:spine") == 2 && rig.FindNode("nope") < 0 && rig.FindClip("bend") == 0);
    JointPose jp = JointPose::FromMatrix(JointPose{{1, 2, 3}, RotZ(30), {-2, 1, 0.5f}}.Matrix());
    CHECK(Near(jp.scale, {-2, 1, 0.5f}) && Near(jp.translation, {1, 2, 3}) && Near(std::abs(glm::dot(jp.rotation, RotZ(30))), 1.0f));
}

static void TestSkinning() {
    const Rig rig = MakeRig();
    std::vector<Vertex> bind = {{{0, 0.5f, 0}, {1, 0, 0}, {0.1f, 0.2f}}, {{0, 2, 0}, {1, 0, 0}, {0, 0}}, {{0, 1, 0}, {1, 0, 0}, {0, 0}}};
    std::vector<SkinInfluence> w(3);
    w[1].joints[0] = 1;
    w[2] = {{0, 1, 0, 0}, {0.5f, 0.5f, 0, 0}};
    std::vector<glm::mat4> g;
    std::vector<Vertex> out;
    AABB box;
    ComputeGlobals(rig, rig.RestPose(), g);
    SkinMesh(rig.skins[0], g, bind, w, out, box);
    for (size_t i = 0; i < bind.size(); ++i) CHECK(Near(out[i].position, bind[i].position)); // rest pose == bind pose
    Pose p;
    SamplePose(rig, rig.clips[0], 1.0f, true, p);
    p[1].translation = glm::vec3(0.0f);
    ComputeGlobals(rig, p, g);
    SkinMesh(rig.skins[0], g, bind, w, out, box);
    CHECK(Near(out[0].position, {0, 0.5f, 0}) && Near(out[0].normal, {1, 0, 0}) && Near(out[0].uv.x, 0.1f));
    CHECK(Near(out[1].position, {-1, 1, 0}) && Near(out[1].normal, {0, 1, 0}));
    CHECK(Near(out[2].position, {0, 1, 0}) && Near(glm::length(out[2].normal), 1.0f));
    CHECK(Near(box.min, {-1, 0.5f, 0}) && Near(box.max, {0, 1, 0}));
}

static void TestRetarget() {
    const Rig source = MakeRig();
    // Same skeleton, twice the size, under a rotated centimetre root, other name prefix.
    Rig target = MakeRig();
    target.clips.clear();
    target.nodes[0].rest.rotation = glm::angleAxis(glm::radians(-90.0f), glm::vec3(1, 0, 0));
    target.nodes[0].rest.scale = glm::vec3(0.01f);
    target.nodes[1].rest.translation = {0, 0, 200}; // 2 m up in model space
    target.nodes[1].rest.rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(1, 0, 0));
    target.nodes[2].name = "Spine";
    target.nodes[2].rest.translation = {0, 200, 0};
    Rig src = source;
    src.nodes[1].rest.translation = {0, 1, 0};
    for (glm::vec3& v : src.clips[0].tracks[1].translation.values) v.y += 1.0f;
    src.clips[0].name = "mixamo.com";
    for (AnimationTrack& t : src.clips[0].tracks)
        if (t.node == 1) t.rotation = {{0}, {glm::quat(1, 0, 0, 0)}, false};
    int matched = 0;
    CHECK(RetargetClips(src, target, "Walking", &matched) == 1 && matched == 2);
    CHECK(target.clips.size() == 1 && target.clips[0].name == "Walking");
    Pose p;
    std::vector<glm::mat4> g;
    SamplePose(target, target.clips[0], 1.0f, false, p);
    ComputeGlobals(target, p, g);
    CHECK(Near(glm::vec3(g[1][3]), {0, 3, 8}, 1e-3f)); // motion scaled 2x, still along +Z
    CHECK(Near(glm::vec3(g[2][3]), {0, 5, 8}, 1e-3f)); // spine length of the target, upright
    CHECK(Near(glm::normalize(glm::vec3(g[2][1])), {-1, 0, 0}, 1e-3f)); // bent like the source
    CHECK(RetargetClips(src, target, "Walking") == 1 && target.clips[1].name == "Walking_2");
    Rig stranger;
    stranger.nodes = {{"Tentacle", -1, {}}};
    CHECK(RetargetClips(src, stranger, "x") == 0 && stranger.clips.empty());
}

static void TestPlayback() {
    const Rig rig = [] { Rig r = MakeRig(); r.clips.push_back(r.clips[0]); r.clips[1].name = "B"; return r; }();
    AnimatorComponent a;
    CHECK(!PlayClip(a, rig, 5, 0, true, 1) && !PlayClip(a, rig, -1, 0, true, 1) && a.clip == -1);
    CHECK(PlayClip(a, rig, 0, 0.0f, true, 1.0f) && a.playing && a.fadeDuration == 0.0f);
    a.time = 0.4f;
    CHECK(PlayClip(a, rig, 0, 0.3f, true, 2.0f) && a.time == 0.4f && a.speed == 2.0f); // no restart
    CHECK(PlayClip(a, rig, 1, 0.3f, true, 1.0f) && a.prevClip == 0 && a.prevTime == 0.4f && a.time == 0.0f && !a.fromSnapshot);
    a.fadeTime = 0.1f;
    CHECK(PlayClip(a, rig, 0, 0.3f, false, 1.0f) && a.fromSnapshot && a.captureSnapshot); // interrupted fade
    CHECK(PlayClip(a, rig, 1, 0.0f, false, -1.0f) && a.time == 1.0f);                     // backwards starts at the end
    CHECK(PlayClip(a, rig, 1, NAN, true, NAN) && a.speed == 1.0f);
}

// A skinned triangle with one animated joint. Edits let the tests break individual parts.
static std::string SkinnedGltf(const std::function<void(std::vector<float>&)>& edit = {}, const std::string& replaceFrom = "", const std::string& replaceTo = "") {
    std::vector<float> f = {0, 0, 0, 1, 0, 0, 0, 1, 0,               // 0 positions
                            1, 0, 0, 0, 0.5f, 0.5f, 0, 0, 2, 2, 0, 0, // 9 weights (last one not normalized)
                            0, 2,                                     // 21 times
                            0, 0, 0, 1, 0, 0, 2, 0,                   // 23 rotations (second one not normalized: 180 about z)
                            0, 0, 0, 0, 0, 0, 0, 0, 1,                // 31 cubic key 0: in, value, out
                            0, 0, 1, 4, 0, 0, 0, 0, 0};               // 40 cubic key 1
    if (edit) edit(f);
    const uint16_t joints[12] = {0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0};
    std::string bin(reinterpret_cast<const char*>(f.data()), f.size() * 4);
    bin.append(reinterpret_cast<const char*>(joints), sizeof(joints));
    std::ofstream(g_dir / "skin.bin", std::ios::binary) << bin;
    std::string j = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[3,0]}],
"nodes":[{"name":"A","children":[1],"translation":[0,1,0]},{"name":"B","translation":[0,1,0]},{"name":"Other"},{"name":"M","mesh":0,"skin":0,"translation":[50,0,0]}],
"skins":[{"joints":[0,1]}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"WEIGHTS_0":1,"JOINTS_0":2}}]}],
"animations":[{"name":"Arm|Spin","samplers":[{"input":3,"output":4},{"input":3,"output":5,"interpolation":"CUBICSPLINE"}],
"channels":[{"sampler":0,"target":{"node":0,"path":"rotation"}},{"sampler":1,"target":{"node":1,"path":"translation"}}]}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
{"bufferView":0,"byteOffset":36,"componentType":5126,"count":3,"type":"VEC4"},{"bufferView":1,"componentType":5123,"count":3,"type":"VEC4"},
{"bufferView":0,"byteOffset":84,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[2]},
{"bufferView":0,"byteOffset":92,"componentType":5126,"count":2,"type":"VEC4"},{"bufferView":0,"byteOffset":124,"componentType":5126,"count":6,"type":"VEC3"}],
"bufferViews":[{"buffer":0,"byteLength":196},{"buffer":0,"byteOffset":196,"byteLength":24}],"buffers":[{"uri":"skin.bin","byteLength":220}]})";
    if (!replaceFrom.empty()) {
        const size_t at = j.find(replaceFrom);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) j.replace(at, replaceFrom.size(), replaceTo);
    }
    return j;
}

static void TestGltf() {
    ModelData d;
    std::string err;
    const bool ok = ImportModel(WriteText("skin.gltf", SkinnedGltf()), d, err);
    if (!ok) std::printf("skin.gltf: %s\n", err.c_str());
    CHECK(ok);
    if (!ok) return;
    CHECK(d.rig.nodes.size() == 4 && d.rig.skins.size() == 1 && d.rig.clips.size() == 1 && d.rig.clips[0].name == "Spin");
    for (size_t i = 0; i < d.rig.nodes.size(); ++i) CHECK(d.rig.nodes[i].parent < static_cast<int>(i));
    CHECK(d.parts.size() == 1 && d.parts[0].skin == 0 && d.influences[0].size() == 3);
    const auto& w = d.influences[0];
    CHECK(Near(w[2].weights[0] + w[2].weights[1], 1.0f)); // normalized
    // Joint A is 1 m up without an inverse bind matrix; the mesh node's own transform (50 m) must not count.
    CHECK(Near(d.bounds.min, {0, 1, 0}) && d.bounds.max.x < 2.0f);
    Pose p;
    SamplePose(d.rig, d.rig.clips[0], 1.0f, false, p);
    const int a = d.rig.FindNode("A"), b = d.rig.FindNode("B");
    CHECK(Near(std::abs(glm::dot(p[static_cast<size_t>(a)].rotation, RotZ(90))), 1.0f));
    // Hermite at u = 0.5 over 2 s: p0/2 + p1/2 + dt * (out0 - in1) / 8.
    CHECK(Near(p[static_cast<size_t>(b)].translation, {2, 0, 0}, 1e-2f));
    SamplePose(d.rig, d.rig.clips[0], 2.0f, false, p);
    CHECK(Near(p[static_cast<size_t>(b)].translation, {4, 0, 0}));

    Rig rig;
    CHECK(ImportAnimations(g_dir / "skin.gltf", rig, err) && rig.clips.size() == 1);
    const std::string animOnly = SkinnedGltf({}, "\"mesh\":0,\"skin\":0,", "");
    CHECK(!ImportModel(WriteText("anim.gltf", animOnly), d, err));
    CHECK(ImportAnimations(g_dir / "anim.gltf", rig, err) && rig.clips.size() == 1);
    AssetManager am; // a file with clips but no mesh lands in animations/, with its buffer
    am.Init(nullptr, g_dir / "project");
    CHECK(am.Import(g_dir / "anim.gltf", err) == "animations/anim/anim.gltf" && ImportAnimations(am.Resolve("animations/anim/anim.gltf"), rig, err));
    CHECK(!ImportAnimations(WriteText("none.gltf", SkinnedGltf({}, "\"animations\"", "\"extras\"")), rig, err) && !err.empty());

    // Damaged files.
    auto fails = [&](const char* what, const std::string& json) {
        const bool r = ImportModel(WriteText("bad.gltf", json), d, err);
        if (r) std::printf("accepted: %s\n", what);
        CHECK(!r && !err.empty() && d.parts.empty());
    };
    fails("joint out of range", SkinnedGltf({}, "\"joints\":[0,1]", "\"joints\":[0]"));
    fails("joint node missing", SkinnedGltf({}, "\"joints\":[0,1]", "\"joints\":[0,77]"));
    fails("empty skin", SkinnedGltf({}, "\"joints\":[0,1]", "\"joints\":[]"));
    fails("no weights", SkinnedGltf({}, "\"WEIGHTS_0\":1,", ""));
    fails("descending times", SkinnedGltf([](auto& f) { f[21] = 3; }));
    fails("NaN time", SkinnedGltf([](auto& f) { f[22] = NAN; }));
    fails("NaN rotation", SkinnedGltf([](auto& f) { f[24] = NAN; }));
    fails("key count mismatch", SkinnedGltf({}, "\"interpolation\":\"CUBICSPLINE\"", "\"interpolation\":\"LINEAR\""));
    fails("short inverse bind", SkinnedGltf({}, "{\"joints\":[0,1]}", "{\"joints\":[0,1],\"inverseBindMatrices\":0}"));
    fails("node cycle", SkinnedGltf({}, "{\"name\":\"B\",", "{\"name\":\"B\",\"children\":[0],"));
    // NaN and zero weights fall back to the first joint instead of failing.
    CHECK(ImportModel(WriteText("zero.gltf", SkinnedGltf([](auto& f) { f[9] = 0; f[13] = NAN; f[14] = NAN; })), d, err));
    CHECK(!d.influences.empty() && Near(d.influences[0][0].weights[0], 1.0f) && Near(d.influences[0][1].weights[0], 1.0f));
}

static int Inspect(int argc, char** argv) {
    ModelData d;
    std::string err;
    const fs::path file = Utf8ToPath(argv[1]);
    if (!ImportModel(file, d, err)) {
        std::printf("model: %s\n", err.c_str());
        if (!ImportAnimations(file, d.rig, err)) return std::printf("animations: %s\n", err.c_str()), 1;
    }
    if (argc > 2) {
        Rig src;
        int matched = 0;
        if (!ImportAnimations(Utf8ToPath(argv[2]), src, err)) return std::printf("clips: %s\n", err.c_str()), 1;
        const int n = RetargetClips(src, d.rig, "clips", &matched);
        std::printf("retargeted %d clips, %d bones matched\n", n, matched);
    }
    for (const std::string& w : d.warnings) std::printf("warning: %s\n", w.c_str());
    std::printf("%zu parts, %zu nodes, %zu skins, %zu clips, bounds (%.3f %.3f %.3f) - (%.3f %.3f %.3f)\n", d.parts.size(), d.rig.nodes.size(), d.rig.skins.size(),
                d.rig.clips.size(), d.bounds.min.x, d.bounds.min.y, d.bounds.min.z, d.bounds.max.x, d.bounds.max.y, d.bounds.max.z);
    for (const AnimationClip& c : d.rig.clips) {
        std::printf("clip '%s' %.3f s, %zu tracks, motion node '%s'\n", c.name.c_str(), c.duration, c.tracks.size(),
                    c.motionNode >= 0 ? d.rig.nodes[static_cast<size_t>(c.motionNode)].name.c_str() : "-");
        for (float u : {0.0f, 0.25f, 0.5f, 1.0f}) {
            Pose p;
            std::vector<glm::mat4> g;
            SamplePose(d.rig, c, c.duration * u, false, p);
            ComputeGlobals(d.rig, p, g);
            std::printf("  %.0f%%:", u * 100);
            for (const char* name : {"Hips", "Head", "RightHand", "LeftFoot"})
                if (const int n = d.rig.FindNode(name); n >= 0) std::printf("  %s (%.3f %.3f %.3f)", name, g[static_cast<size_t>(n)][3].x, g[static_cast<size_t>(n)][3].y, g[static_cast<size_t>(n)][3].z);
            for (const ModelPart& part : d.parts) {
                if (part.skin < 0) continue;
                std::vector<Vertex> out;
                AABB box;
                SkinMesh(d.rig.skins[static_cast<size_t>(part.skin)], g, d.meshes[part.mesh].vertices, d.influences[part.mesh], out, box);
                std::printf("  skin (%.3f %.3f %.3f)-(%.3f %.3f %.3f)", box.min.x, box.min.y, box.min.z, box.max.x, box.max.y, box.max.z);
            }
            std::printf("\n");
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return Inspect(argc, argv);
    g_dir = fs::temp_directory_path() / "mage_animation_tests";
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir);
    TestSampling();
    TestSkinning();
    TestRetarget();
    TestPlayback();
    TestGltf();
    fs::remove_all(g_dir, ec);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
