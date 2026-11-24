// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - CPU-side file importers. No GPU, window or audio device is needed, so the test
// suite runs them headless. Every importer validates its input and reports errors by string.
//
//   models   .gltf .glb (cgltf), .fbx .obj (ufbx)
//   images   .png .jpg .jpeg .tga .bmp .psd .gif .hdr (stb_image, converted to RGBA8)
//   fonts    .ttf .otf .ttc        sounds   .wav .mp3 .flac .ogg

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "animation/Animation.h"
#include "renderer/Material.h"
#include "renderer/Mesh.h"

namespace mage {

enum class AssetType : uint8_t { Unknown, Model, Image, Font, Sound };
MAGE_API AssetType DetectAssetType(const std::filesystem::path& file);
MAGE_API const char* AssetTypeName(AssetType type);

struct ImageData {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba; // 4 bytes per pixel, top row first
};

// An image used by a model: either embedded (encoded bytes) or a file next to the model.
struct ModelImage {
    std::string name;
    std::filesystem::path file;
    std::vector<uint8_t> encoded;
};

struct ModelMaterial {
    std::string name;
    Material params;         // texture handles stay 0 until AssetManager uploads the images below
    int baseColor = -1, metallicRoughness = -1, normal = -1, emissive = -1; // into ModelData::images
};

struct ModelPart {
    uint32_t mesh = 0;
    uint32_t material = 0;
    glm::mat4 transform{1.0f}; // rest transform (identity for skinned parts)
    int node = -1;             // rig node that moves a rigid part, -1 = never moves
    int skin = -1;             // rig skin that deforms the part, -1 = rigid
};

struct ModelData {
    std::vector<MeshData> meshes;
    std::vector<std::vector<SkinInfluence>> influences; // parallel to meshes, empty = not skinned
    Rig rig;                                            // empty unless the file has skins or animations
    std::vector<ModelMaterial> materials;
    std::vector<ModelImage> images;
    std::vector<ModelPart> parts;
    std::vector<std::filesystem::path> dependencies; // extra files, relative to the model's folder
    std::vector<std::string> warnings;               // features the importer had to drop
    AABB bounds;
    MAGE_API size_t TriangleCount() const;
};

MAGE_API bool ImportModel(const std::filesystem::path& file, ModelData& out, std::string& error);
// Skeleton and clips only: also accepts files without any mesh. Fails if there are no clips.
MAGE_API bool ImportAnimations(const std::filesystem::path& file, Rig& out, std::string& error,
                               std::vector<std::filesystem::path>* dependencies = nullptr);
MAGE_API bool DecodeImage(const uint8_t* data, size_t size, ImageData& out, std::string& error);
MAGE_API bool ImportImage(const std::filesystem::path& file, ImageData& out, std::string& error);
MAGE_API bool ValidateFont(const std::filesystem::path& file, std::string& error);
MAGE_API bool ReadFileBytes(const std::filesystem::path& file, std::vector<uint8_t>& out);
MAGE_API std::string PathToUtf8(const std::filesystem::path& p);
MAGE_API std::filesystem::path Utf8ToPath(const std::string& s);

} // namespace mage
