// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "assets/AssetManager.h"
#include "audio/AudioEngine.h"
#include "core/Log.h"
#include "renderer/Renderer.h"

#include <algorithm>
#include <format>

namespace mage {

namespace fs = std::filesystem;

void AssetManager::Init(Renderer* renderer, fs::path directory) {
    m_renderer = renderer;
    m_directory = std::move(directory);
}

fs::path AssetManager::Resolve(const std::string& path) const {
    const fs::path p = Utf8ToPath(path);
    return (p.is_absolute() ? p : m_directory / p).lexically_normal();
}

const Model* AssetManager::GetModel(ModelHandle h) const { return h < m_models.size() ? &m_models[h] : nullptr; }

TextureHandle AssetManager::LoadTexture(const std::string& path, bool srgb) {
    if (!m_renderer || path.empty()) return 0;
    const fs::path file = Resolve(path);
    const std::string key = PathToUtf8(file);
    if (TextureHandle h = m_renderer->Textures().Find(key, srgb)) return h;
    ImageData img;
    std::string error;
    if (!ImportImage(file, img, error)) {
        Log::Error("Texture '{}': {}", path, error);
        return 0;
    }
    return m_renderer->Textures().Add(key, img.width, img.height, std::move(img.rgba), srgb);
}

ModelHandle AssetManager::LoadModel(const std::string& path) {
    if (!m_renderer || path.empty()) return InvalidModel;
    const fs::path file = Resolve(path);
    const std::string key = PathToUtf8(file);
    if (auto it = m_modelByPath.find(key); it != m_modelByPath.end()) return it->second;
    ModelData data;
    std::string error;
    if (!ImportModel(file, data, error)) {
        Log::Error("Model '{}': {}", path, error);
        m_modelByPath[key] = InvalidModel;
        return InvalidModel;
    }
    for (const std::string& w : data.warnings) Log::Warn("Model '{}': {}", path, w);

    TextureLibrary& textures = m_renderer->Textures();
    auto upload = [&](int index, bool srgb) -> TextureHandle {
        if (index < 0) return 0;
        const ModelImage& src = data.images[static_cast<size_t>(index)];
        const std::string name = src.encoded.empty() ? PathToUtf8(src.file) : std::format("{}#{}", key, index);
        if (TextureHandle h = textures.Find(name, srgb)) return h;
        ImageData img;
        std::string e;
        const bool ok = src.encoded.empty() ? ImportImage(src.file, img, e) : DecodeImage(src.encoded.data(), src.encoded.size(), img, e);
        if (!ok) {
            Log::Warn("Model '{}': texture '{}': {}", path, src.name, e);
            return 0;
        }
        return textures.Add(name, img.width, img.height, std::move(img.rgba), srgb);
    };
    std::vector<Material> materials;
    for (const ModelMaterial& m : data.materials) {
        Material p = m.params;
        p.baseColorTexture = upload(m.baseColor, true);
        p.metallicRoughnessTexture = upload(m.metallicRoughness, false);
        p.normalTexture = upload(m.normal, false);
        p.emissiveTexture = upload(m.emissive, true);
        materials.push_back(p);
    }
    Model model;
    model.name = PathToUtf8(file.stem());
    model.bounds = data.bounds;
    model.triangles = data.TriangleCount();
    model.rig = std::move(data.rig);
    std::vector<std::shared_ptr<const std::vector<SkinInfluence>>> influences;
    for (auto& w : data.influences) influences.push_back(std::make_shared<const std::vector<SkinInfluence>>(std::move(w)));
    std::vector<glm::mat4> globals;
    ComputeGlobals(model.rig, model.rig.RestPose(), globals);
    std::vector<Vertex> skinned;
    AABB unused;
    std::vector<MeshHandle> handles;
    for (size_t i = 0; i < data.meshes.size(); ++i) handles.push_back(m_renderer->Meshes().Add(std::format("{}#{}", key, i), data.meshes[i]));
    for (const ModelPart& p : data.parts) {
        Material mat = materials[p.material];
        const bool mirrored = glm::determinant(glm::mat3(p.transform)) < 0.0f;
        if (mirrored) mat.doubleSided = true; // mirrored transforms flip the triangle winding
        model.parts.push_back({handles[p.mesh], mat, p.transform, p.node, p.skin, p.skin >= 0 ? influences[p.mesh] : nullptr});
        if (p.skin >= 0) // collision uses the rest pose
            SkinMesh(model.rig.skins[static_cast<size_t>(p.skin)], globals, data.meshes[p.mesh].vertices, *influences[p.mesh], skinned, unused);
        const auto base = static_cast<uint32_t>(model.collisionVertices.size());
        for (const Vertex& v : p.skin >= 0 ? skinned : data.meshes[p.mesh].vertices)
            model.collisionVertices.push_back(glm::vec3(p.transform * glm::vec4(v.position, 1.0f)));
        const auto& idx = data.meshes[p.mesh].indices;
        for (size_t i = 0; i + 2 < idx.size(); i += 3) { // keep outward winding: Jolt meshes are one-sided
            model.collisionIndices.push_back(base + idx[i]);
            model.collisionIndices.push_back(base + idx[i + (mirrored ? 2 : 1)]);
            model.collisionIndices.push_back(base + idx[i + (mirrored ? 1 : 2)]);
        }
    }
    const auto handle = static_cast<ModelHandle>(m_models.size());
    Log::Info("Model '{}': {} parts, {} triangles, {} materials, {} images", path, model.parts.size(), model.triangles, materials.size(), data.images.size());
    if (!model.rig.Empty()) Log::Info("Model '{}': {} rig nodes, {} skins, {} clips", path, model.rig.nodes.size(), model.rig.skins.size(), model.rig.clips.size());
    m_models.push_back(std::move(model));
    m_modelByPath[key] = handle;
    return handle;
}

int AssetManager::LoadAnimations(ModelHandle handle, const std::string& path) {
    if (handle >= m_models.size() || path.empty()) return -1;
    Model& model = m_models[handle];
    if (model.rig.Empty()) {
        Log::Error("Animations '{}': model '{}' has no skeleton", path, model.name);
        return -1;
    }
    const fs::path file = Resolve(path);
    const std::string key = PathToUtf8(file);
    if (model.animationFiles.contains(key)) return 0;
    Rig source;
    std::string error;
    if (!ImportAnimations(file, source, error)) {
        Log::Error("Animations '{}': {}", path, error);
        return -1;
    }
    int matched = 0;
    const int added = RetargetClips(source, model.rig, PathToUtf8(file.stem()), &matched);
    if (added == 0) {
        Log::Error("Animations '{}': no bone names in common with model '{}'", path, model.name);
        return -1;
    }
    model.animationFiles.insert(key);
    Log::Info("Animations '{}': {} clips added to '{}' ({} bones matched)", path, added, model.name, matched);
    return added;
}

std::string AssetManager::Import(const fs::path& source, std::string& error) {
    error.clear();
    const AssetType type = DetectAssetType(source);
    std::error_code ec;
    if (!fs::is_regular_file(source, ec)) { error = "file not found"; return {}; }
    std::vector<fs::path> deps;
    fs::path folder;
    switch (type) {
        case AssetType::Model: {
            ModelData d;
            Rig clips;
            if (ImportModel(source, d, error)) {
                deps = d.dependencies;
                folder = fs::path("models") / source.stem();
            } else {
                std::string ignored; // a file with clips but no mesh is an animation file
                if (!ImportAnimations(source, clips, ignored, &deps)) return {};
                error.clear();
                folder = fs::path("animations") / source.stem();
            }
            break;
        }
        case AssetType::Image: {
            ImageData img;
            if (!ImportImage(source, img, error)) return {};
            folder = "textures";
            break;
        }
        case AssetType::Font:
            if (!ValidateFont(source, error)) return {};
            folder = "fonts";
            break;
        case AssetType::Sound:
            if (!AudioEngine::Validate(source, error)) return {};
            folder = "sounds";
            break;
        default: error = "unsupported file type"; return {};
    }
    // Pick a destination that does not exist yet.
    const std::string baseFolder = PathToUtf8(folder), stem = PathToUtf8(source.stem()), ext = PathToUtf8(source.extension());
    fs::path name = source.filename();
    for (int n = 2; fs::exists(type == AssetType::Model ? m_directory / folder : m_directory / folder / name, ec); ++n) {
        if (type == AssetType::Model) folder = Utf8ToPath(std::format("{}_{}", baseFolder, n));
        else name = Utf8ToPath(std::format("{}_{}{}", stem, n, ext));
    }
    auto copy = [&](const fs::path& from, const fs::path& to) {
        fs::create_directories(to.parent_path(), ec);
        if (fs::exists(to, ec)) return true; // same dependency listed twice
        std::error_code ce;
        fs::copy_file(from, to, fs::copy_options::none, ce);
        if (ce) error = std::format("copy failed: {}", ce.message());
        return !ce;
    };
    if (!copy(source, m_directory / folder / name)) return {};
    for (const fs::path& d : deps) {
        if (!fs::is_regular_file(source.parent_path() / d, ec)) { Log::Warn("Import: missing referenced file {}", PathToUtf8(d)); continue; }
        if (!copy(source.parent_path() / d, m_directory / folder / d)) return {};
    }
    const auto generic = (folder / name).generic_u8string();
    const std::string rel(reinterpret_cast<const char*>(generic.data()), generic.size());
    Log::Info("Imported {} -> {}", AssetTypeName(type), rel);
    return rel;
}

std::vector<AssetManager::Entry> AssetManager::List() const {
    std::vector<Entry> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(m_directory, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fe;
        if (!it->is_regular_file(fe)) continue;
        const AssetType t = DetectAssetType(it->path());
        if (t == AssetType::Unknown) continue;
        const auto generic = it->path().lexically_relative(m_directory).generic_u8string();
        out.push_back({std::string(reinterpret_cast<const char*>(generic.data()), generic.size()), t});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
    return out;
}

} // namespace mage
