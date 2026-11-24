// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
#include "core/Export.h"
// MAGE v1 - texture storage shared by both renderers. Each texture keeps a full CPU mip chain
// (the path tracer uploads it to Vulkan) next to its OpenGL object. Handle 0 is plain white.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "renderer/Material.h"

namespace mage {

struct TextureMip { int width = 0, height = 0; std::vector<uint8_t> rgba; };

struct Texture {
    std::string name;
    bool srgb = true;
    std::vector<TextureMip> mips;
    uint32_t gl = 0;
};

MAGE_API std::vector<TextureMip> BuildMipChain(int width, int height, std::vector<uint8_t> rgba, bool srgb);

class MAGE_API TextureLibrary {
public:
    static constexpr uint32_t kMaxTextures = 1024;
    void CreateDefaults();
    // Returns 0 when the library is full or the image is empty.
    TextureHandle Add(const std::string& name, int width, int height, std::vector<uint8_t> rgba, bool srgb);
    TextureHandle Find(const std::string& name, bool srgb) const;
    const Texture* Get(TextureHandle handle) const { return handle < m_textures.size() ? &m_textures[handle] : nullptr; }
    size_t Count() const { return m_textures.size(); }
    void Destroy();

private:
    std::vector<Texture> m_textures;
    std::unordered_map<std::string, TextureHandle> m_byName;
};

} // namespace mage
