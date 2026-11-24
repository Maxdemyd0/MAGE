// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - asset manager: loads models and textures into the renderer (each file once) and
// imports files from anywhere on disk into the project's asset folder.
// Paths are UTF-8, absolute or relative to the asset directory. Loading is synchronous.

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/Importers.h"

namespace mage {

class Renderer;

using ModelHandle = uint32_t;
inline constexpr ModelHandle InvalidModel = 0xFFFFFFFFu;

struct Model {
    struct Part {
        MeshHandle mesh;
        Material material;
        glm::mat4 transform;
        int node = -1, skin = -1; // into rig (see ModelPart)
        std::shared_ptr<const std::vector<SkinInfluence>> influences; // skinned parts only
    };
    std::string name;
    std::vector<Part> parts;
    AABB bounds;
    std::vector<glm::vec3> collisionVertices; // model space, all parts merged
    std::vector<uint32_t> collisionIndices;
    size_t triangles = 0;
    Rig rig;                                  // empty for models that cannot move
    std::set<std::string> animationFiles;     // clip files already added
};

class MAGE_API AssetManager {
public:
    void Init(Renderer* renderer, std::filesystem::path assetDirectory); // renderer may be null: Import/List only

    std::filesystem::path Resolve(const std::string& path) const;
    ModelHandle LoadModel(const std::string& path);
    const Model* GetModel(ModelHandle handle) const;
    // Adds the clips of another file made for the same skeleton. Returns the number of clips added
    // (0 if the file was added before), or -1 on failure.
    int LoadAnimations(ModelHandle model, const std::string& path);
    TextureHandle LoadTexture(const std::string& path, bool srgb = true);

    // Copies a file (and the files it references) into the asset folder. Existing files are never
    // overwritten. Returns the new path relative to the asset directory, or "" with 'error' set.
    std::string Import(const std::filesystem::path& source, std::string& error);

    struct Entry { std::string path; AssetType type; };
    std::vector<Entry> List() const;
    const std::filesystem::path& Directory() const { return m_directory; }

private:
    Renderer* m_renderer = nullptr;
    std::filesystem::path m_directory;
    std::vector<Model> m_models;
    std::unordered_map<std::string, ModelHandle> m_modelByPath; // failed loads are remembered too
};

} // namespace mage
