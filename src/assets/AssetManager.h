// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - asset manager: loads models and textures into the renderer (each file once) and
// imports files from anywhere on disk into the project's asset folder.
// Paths are UTF-8, absolute or relative to the asset directory. Loading is synchronous.

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/Importers.h"

namespace mage {

class Renderer;

using ModelHandle = uint32_t;
inline constexpr ModelHandle InvalidModel = 0xFFFFFFFFu;

struct Model {
    struct Part { MeshHandle mesh; Material material; glm::mat4 transform; };
    std::string name;
    std::vector<Part> parts;
    AABB bounds;
    std::vector<glm::vec3> collisionVertices; // model space, all parts merged
    std::vector<uint32_t> collisionIndices;
    size_t triangles = 0;
};

class MAGE_API AssetManager {
public:
    void Init(Renderer* renderer, std::filesystem::path assetDirectory); // renderer may be null: Import/List only

    std::filesystem::path Resolve(const std::string& path) const;
    ModelHandle LoadModel(const std::string& path);
    const Model* GetModel(ModelHandle handle) const;
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
