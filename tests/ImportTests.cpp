// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
// Headless import tests. Every input file is generated here, so nothing needs downloading.
#include "assets/AssetManager.h"
#include "assets/Importers.h"
#include "audio/AudioEngine.h"
#include "physics/PhysicsWorld.h"
#include "renderer/Texture.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

using namespace mage;
namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { ++g_failed; std::printf("FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static fs::path g_dir;

static fs::path WriteBytes(const fs::path& rel, const void* data, size_t size) {
    const fs::path p = g_dir / rel;
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return p;
}
static fs::path WriteText(const fs::path& rel, const std::string& s) { return WriteBytes(rel, s.data(), s.size()); }

static std::vector<uint8_t> Png(int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4), out;
    for (int i = 0; i < w * h; ++i) { px[i * 4] = 255; px[i * 4 + 1] = static_cast<uint8_t>(i % 256); px[i * 4 + 2] = 10; px[i * 4 + 3] = static_cast<uint8_t>(i % 2 ? 255 : 0); }
    stbi_write_png_to_func([](void* c, void* d, int n) { auto* v = static_cast<std::vector<uint8_t>*>(c); auto* b = static_cast<uint8_t*>(d); v->insert(v->end(), b, b + n); }, &out, w, h, 4, px.data(), w * 4);
    return out;
}

// One quad: positions, uvs, indices. 'json' gets %BIN% replaced by the buffer size.
static std::vector<uint8_t> QuadBuffer() {
    const float pos[] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    const float uv[] = {0, 1, 1, 1, 1, 0, 0, 0};
    const uint16_t idx[] = {0, 1, 2, 0, 2, 3};
    std::vector<uint8_t> b(sizeof(pos) + sizeof(uv) + sizeof(idx));
    std::memcpy(b.data(), pos, sizeof(pos));
    std::memcpy(b.data() + sizeof(pos), uv, sizeof(uv));
    std::memcpy(b.data() + sizeof(pos) + sizeof(uv), idx, sizeof(idx));
    return b;
}
static std::string QuadJson(const std::string& buffer, const std::string& images, const std::string& extra = "", int indexCount = 6) {
    return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
"nodes":[{"translation":[5,0,0],"children":[1,2]},{"mesh":0},{"mesh":0,"scale":[2,2,2],"translation":[0,3,0]}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]}],
"accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},
{"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"},{"bufferView":2,"componentType":5123,"count":)" + std::to_string(indexCount) + R"(,"type":"SCALAR"}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":32},{"buffer":0,"byteOffset":80,"byteLength":12}],
"textures":[{"source":0}],"images":[)" + images + R"(],
"materials":[{"name":"m","alphaMode":"MASK","alphaCutoff":0.25,"doubleSided":true,"emissiveFactor":[1,0.5,0],
"pbrMetallicRoughness":{"baseColorFactor":[0.5,0.25,1,0.75],"metallicFactor":0.3,"roughnessFactor":0.7,"baseColorTexture":{"index":0}},
"extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":4},"KHR_materials_clearcoat":{"clearcoatFactor":0.8,"clearcoatRoughnessFactor":0.1}}}],
"buffers":[)" + buffer + "]" + extra + "}";
}

static void TestGltf() {
    const auto buf = QuadBuffer();
    const auto png = Png(4, 4);
    WriteBytes("g/quad.bin", buf.data(), buf.size());
    WriteBytes("g/my tex.png", png.data(), png.size());
    const std::string ext = R"({"uri":"quad.bin","byteLength":92})";
    const fs::path file = WriteText("g/quad.gltf", QuadJson(ext, R"({"uri":"my%20tex.png"})"));
    ModelData d;
    std::string err;
    CHECK(ImportModel(file, d, err));
    CHECK(d.meshes.size() == 1 && d.parts.size() == 2); // shared mesh, two instances
    CHECK(d.TriangleCount() == 4);
    CHECK(glm::length(d.bounds.min - glm::vec3(5, 0, 0)) < 1e-4f && glm::length(d.bounds.max - glm::vec3(7, 5, 0)) < 1e-4f);
    if (!d.materials.empty()) {
        const Material& m = d.materials[0].params;
        CHECK(std::abs(m.albedo.g - 0.25f) < 1e-5f && std::abs(m.metallic - 0.3f) < 1e-5f && std::abs(m.roughness - 0.7f) < 1e-5f);
        CHECK(std::abs(m.emissive.r - 4.0f) < 1e-5f && std::abs(m.clearcoat - 0.8f) < 1e-5f);
        CHECK(m.doubleSided && std::abs(m.alphaCutoff - 0.25f) < 1e-5f && std::abs(m.alpha - 0.75f) < 1e-5f);
        CHECK(d.materials[0].baseColor == 0);
    }
    CHECK(d.images.size() == 1 && fs::exists(d.images[0].file)); // %20 decoded
    CHECK(d.dependencies.size() == 2);
    if (!d.meshes.empty()) CHECK(std::abs(glm::dot(d.meshes[0].vertices[0].normal, glm::vec3(0, 0, 1)) - 1.0f) < 1e-4f); // generated

    // GLB with an embedded image.
    std::vector<uint8_t> bin = buf;
    const size_t imgOffset = bin.size();
    bin.insert(bin.end(), png.begin(), png.end());
    while (bin.size() % 4) bin.push_back(0);
    std::string json = QuadJson("{\"byteLength\":" + std::to_string(bin.size()) + "}", R"({"bufferView":3,"mimeType":"image/png"})");
    const std::string view = ",{\"buffer\":0,\"byteOffset\":" + std::to_string(imgOffset) + ",\"byteLength\":" + std::to_string(png.size()) + "}]";
    const std::string last = "\"byteLength\":12}]";
    json.replace(json.find(last), last.size(), "\"byteLength\":12}" + view);
    while (json.size() % 4) json += ' ';
    std::vector<uint8_t> glb;
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) glb.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    u32(0x46546C67); u32(2); u32(static_cast<uint32_t>(28 + json.size() + bin.size()));
    u32(static_cast<uint32_t>(json.size())); u32(0x4E4F534A);
    glb.insert(glb.end(), json.begin(), json.end());
    u32(static_cast<uint32_t>(bin.size())); u32(0x004E4942);
    glb.insert(glb.end(), bin.begin(), bin.end());
    CHECK(ImportModel(WriteBytes("g/quad.glb", glb.data(), glb.size()), d, err));
    ImageData img;
    CHECK(d.images.size() == 1 && DecodeImage(d.images[0].encoded.data(), d.images[0].encoded.size(), img, err) && img.width == 4);
    // Non-ASCII file names.
    CHECK(ImportModel(WriteBytes(Utf8ToPath("g/\xD0\xBC\xD0\xB0\xD1\x88\xD0\xB8\xD0\xBD\xD0\xB0.glb"), glb.data(), glb.size()), d, err));

    // Bad input must fail cleanly.
    for (size_t cut : {size_t(0), size_t(3), size_t(20), glb.size() / 2, glb.size() - 8})
        CHECK(!ImportModel(WriteBytes("g/cut.glb", glb.data(), cut), d, err) && !err.empty());
    CHECK(!ImportModel(WriteText("g/esc.gltf", QuadJson(R"({"uri":"../quad.bin","byteLength":92})", "")), d, err)); // leaves the folder
    CHECK(!ImportModel(WriteText("g/esc2.gltf", QuadJson(R"({"uri":"%2e%2e/quad.bin","byteLength":92})", "")), d, err));
    CHECK(!ImportModel(WriteText("g/miss.gltf", QuadJson(R"({"uri":"nope.bin","byteLength":92})", "")), d, err));
    CHECK(!ImportModel(WriteText("g/short.gltf", QuadJson(ext, "", "", 600)), d, err)); // accessor past the end
    CHECK(!ImportModel(WriteText("g/draco.gltf", QuadJson(ext, "", R"(,"extensionsRequired":["KHR_draco_mesh_compression"])")), d, err));
    CHECK(!ImportModel(g_dir / "g/none.glb", d, err));
    auto badIdx = buf;
    badIdx[80] = 200; // index 200 of 4 vertices
    WriteBytes("b/quad.bin", badIdx.data(), badIdx.size());
    CHECK(!ImportModel(WriteText("b/quad.gltf", QuadJson(ext, "")), d, err));
    auto nan = buf;
    const float q = std::nanf("");
    std::memcpy(nan.data(), &q, 4);
    WriteBytes("n/quad.bin", nan.data(), nan.size());
    CHECK(!ImportModel(WriteText("n/quad.gltf", QuadJson(ext, "")), d, err));
}

static void TestObjFbx() {
    const auto png = Png(8, 8);
    WriteBytes("o/wood.png", png.data(), png.size());
    WriteText("o/box.mtl", "newmtl wood\nKd 0.8 0.6 0.4\nmap_Kd wood.png\nnewmtl plain\nKd 0.1 0.2 0.3\n");
    const fs::path obj = WriteText("o/box.obj", "mtllib box.mtl\nv 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                                                "usemtl wood\nf 1/1 2/2 3/3 4/4\nusemtl plain\nf 1/1 3/3 2/2\n");
    ModelData d;
    std::string err;
    CHECK(ImportModel(obj, d, err));
    CHECK(d.parts.size() == 2 && d.TriangleCount() == 3); // quad triangulated
    CHECK(d.images.size() == 1);
    CHECK(d.dependencies.size() == 2); // texture + mtl
    bool flipped = false;
    for (const MeshData& m : d.meshes)
        for (const Vertex& v : m.vertices) if (glm::length(v.position) < 1e-5f) flipped = std::abs(v.uv.y - 1.0f) < 1e-5f; // V flipped
    CHECK(flipped);
    CHECK(d.materials.size() == 2);
    for (const ModelMaterial& m : d.materials) if (m.name == "plain") CHECK(std::abs(m.params.albedo.b - 0.3f) < 1e-4f && m.baseColor < 0);
    const char* tri = "v 0 0 0\nv 1 0 0\nv 1 1 0\nusemtl plain\nf 1 2 3\n";
    CHECK(ImportModel(WriteText("o/other.obj", std::string("mtllib box.mtl\n") + tri), d, err) && d.dependencies.size() == 1);
    WriteText("secret.mtl", "newmtl plain\nKd 0.9 0.9 0.1\n");
    CHECK(ImportModel(WriteText("o/escape.obj", std::string("mtllib ../secret.mtl\n") + tri), d, err) && d.dependencies.empty());
    for (const ModelMaterial& m : d.materials) CHECK(std::abs(m.params.albedo.r - 0.9f) > 1e-3f);
    CHECK(!ImportModel(WriteText("o/empty.obj", "# nothing\n"), d, err));

    // ASCII FBX, centimetres: a 100 cm triangle must arrive as 1 m.
    const fs::path fbx = WriteText("o/tri.fbx",
        "; FBX 7.4.0 project file\nFBXHeaderExtension:  {\n FBXHeaderVersion: 1003\n FBXVersion: 7400\n}\n"
        "GlobalSettings:  {\n Version: 1000\n Properties70:  {\n  P: \"UpAxis\", \"int\", \"Integer\", \"\",1\n  P: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "  P: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n  P: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n  P: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
        "  P: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n  P: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n }\n}\n"
        "Objects:  {\n Geometry: 1000, \"Geometry::\", \"Mesh\" {\n  Vertices: *9 {\n   a: 0,0,0,100,0,0,0,100,0\n  }\n  PolygonVertexIndex: *3 {\n   a: 0,1,-3\n  }\n  GeometryVersion: 124\n }\n"
        " Model: 2000, \"Model::Tri\", \"Mesh\" {\n  Version: 232\n }\n}\nConnections:  {\n C: \"OO\",2000,0\n C: \"OO\",1000,2000\n}\n");
    const bool ok = ImportModel(fbx, d, err);
    if (!ok) std::printf("fbx: %s\n", err.c_str());
    CHECK(ok && d.TriangleCount() == 1 && std::abs(d.bounds.max.x - 1.0f) < 1e-4f);
    CHECK(!ImportModel(WriteText("o/bad.fbx", "not an fbx"), d, err));
}

static void TestImages() {
    const auto png = Png(6, 3);
    ImageData img;
    std::string err;
    CHECK(DecodeImage(png.data(), png.size(), img, err) && img.width == 6 && img.height == 3 && img.rgba.size() == 72 && img.rgba[0] == 255);
    CHECK(!DecodeImage(png.data(), 12, img, err));
    CHECK(!DecodeImage(nullptr, 0, img, err));
    CHECK(!ImportImage(WriteText("i/fake.png", "hello"), img, err));
    const auto mips = BuildMipChain(6, 3, std::vector<uint8_t>(72, 255), true);
    CHECK(mips.size() == 3 && mips.back().width == 1 && mips.back().height == 1 && mips.back().rgba[0] == 255);
    // Black + white averages to 50% linear light, i.e. ~188 in sRGB but 128 in linear data.
    const std::vector<uint8_t> bw = {0, 0, 0, 255, 255, 255, 255, 255};
    const int s = BuildMipChain(2, 1, bw, true)[1].rgba[0], l = BuildMipChain(2, 1, bw, false)[1].rgba[0];
    CHECK(std::abs(s - 188) <= 1 && std::abs(l - 128) <= 1);
    CHECK(DetectAssetType("A.GLB") == AssetType::Model && DetectAssetType("a.exe") == AssetType::Unknown);
}

static std::vector<uint8_t> Wav() {
    const uint32_t samples = 4800;
    std::vector<uint8_t> w;
    auto s4 = [&](const char* c) { w.insert(w.end(), c, c + 4); };
    auto n = [&](uint32_t v, int bytes) { for (int i = 0; i < bytes; ++i) w.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    s4("RIFF"); n(36 + samples * 2, 4); s4("WAVE"); s4("fmt "); n(16, 4); n(1, 2); n(1, 2); n(48000, 4); n(96000, 4); n(2, 2); n(16, 2); s4("data"); n(samples * 2, 4);
    for (uint32_t i = 0; i < samples; ++i) n(static_cast<uint16_t>(static_cast<int16_t>(8000 * std::sin(i * 0.06))), 2);
    return w;
}

static void TestAudioFonts() {
    const auto wav = Wav();
    const fs::path file = WriteBytes("a/beep.wav", wav.data(), wav.size());
    std::string err;
    CHECK(AudioEngine::Validate(file, err));
    CHECK(!AudioEngine::Validate(WriteText("a/bad.wav", "nope"), err));
    AudioEngine audio;
    CHECK(audio.Init(true));
    const glm::vec3 p(1, 2, 3);
    const SoundHandle a = audio.Play(file), b = audio.Play(file, 0.5f, 1.2f, true, &p);
    CHECK(a && b && a != b && audio.ActiveCount() == 2 && audio.IsPlaying(b));
    audio.Stop(a);
    audio.Stop(a); // double stop is harmless
    CHECK(audio.ActiveCount() == 1 && !audio.IsPlaying(a));
    CHECK(audio.Play(g_dir / "a/none.wav") == 0);
    CHECK(!ValidateFont(WriteText("f/bad.ttf", "woff2"), err));
    CHECK(ValidateFont(WriteBytes("f/ok.ttf", "\0\1\0\0", 4), err));
}

static void TestImportCopy() {
    AssetManager am;
    am.Init(nullptr, g_dir / "project");
    std::string err;
    const std::string p1 = am.Import(g_dir / "g/quad.gltf", err);
    CHECK(p1 == "models/quad/quad.gltf");
    CHECK(fs::exists(g_dir / "project/models/quad/quad.bin") && fs::exists(g_dir / "project/models/quad/my tex.png"));
    ModelData d;
    CHECK(ImportModel(am.Resolve(p1), d, err) && d.images.size() == 1); // works from its new home
    CHECK(am.Import(g_dir / "g/quad.gltf", err) == "models/quad_2/quad.gltf"); // never overwrites
    CHECK(am.Import(g_dir / "o/wood.png", err) == "textures/wood.png");
    CHECK(am.Import(g_dir / "o/wood.png", err) == "textures/wood_2.png");
    CHECK(am.Import(g_dir / "a/beep.wav", err) == "sounds/beep.wav");
    CHECK(am.Import(g_dir / "a/bad.wav", err).empty() && !err.empty());
    CHECK(am.Import(WriteText("x.exe", "MZ"), err).empty());
    CHECK(am.List().size() == 7); // 2x gltf, 2x its png, 2x wood, 1x wav
}

static void TestPhysics() {
    PhysicsWorld world;
    CHECK(world.Init(256, 1));
    BodyDesc ground;
    ground.shape = ShapeType::Mesh;
    ground.points = {{-5, 0, -5}, {5, 0, -5}, {5, 0, 5}, {-5, 0, 5}};
    ground.triangles = {0, 2, 1, 0, 3, 2};
    CHECK(world.CreateBody(ground) != InvalidBody);
    BodyDesc rock;
    rock.shape = ShapeType::ConvexHull;
    rock.motion = MotionType::Dynamic;
    rock.layer = ObjectLayer::DynamicProp;
    rock.position = {0, 3, 0};
    rock.points = {{-.5f, -.5f, -.5f}, {.5f, -.5f, -.5f}, {.5f, -.5f, .5f}, {-.5f, -.5f, .5f}, {0, .5f, 0}};
    const BodyHandle h = world.CreateBody(rock);
    CHECK(h != InvalidBody);
    for (int i = 0; i < 240; ++i) world.Step(1.0f / 60.0f);
    glm::vec3 p;
    glm::quat q;
    world.GetBodyTransform(h, p, q);
    CHECK(p.y > 0.3f && p.y < 0.7f); // resting on the imported mesh, not through it
    BodyDesc flat = rock;
    flat.points = {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}};
    CHECK(world.CreateBody(flat) != InvalidBody); // flat model falls back to a thin box
    BodyDesc empty = rock;
    empty.points.clear();
    CHECK(world.CreateBody(empty) == InvalidBody);
}

int main() {
    g_dir = fs::temp_directory_path() / "mage_import_tests";
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    TestImages();
    TestGltf();
    TestObjFbx();
    TestAudioFonts();
    TestImportCopy();
    TestPhysics();
    fs::remove_all(g_dir, ec);
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
