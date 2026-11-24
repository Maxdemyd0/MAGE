// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "renderer/Texture.h"
#include "renderer/GL.h"
#include "core/Log.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace mage {

using namespace gl;

namespace {
float ToLinear(uint8_t v) {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const float c = static_cast<float>(i) / 255.0f;
            t[static_cast<size_t>(i)] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table[v];
}
uint8_t ToSrgb(float c) {
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(s * 255.0f + 0.5f, 0.0f, 255.0f));
}
std::string Key(const std::string& name, bool srgb) { return (srgb ? "s:" : "l:") + name; }
} // namespace

std::vector<TextureMip> BuildMipChain(int width, int height, std::vector<uint8_t> rgba, bool srgb) {
    std::vector<TextureMip> mips;
    mips.push_back({width, height, std::move(rgba)});
    while (mips.back().width > 1 || mips.back().height > 1) {
        const TextureMip& s = mips.back();
        TextureMip d{std::max(s.width / 2, 1), std::max(s.height / 2, 1), {}};
        d.rgba.resize(static_cast<size_t>(d.width) * d.height * 4);
        for (int y = 0; y < d.height; ++y)
            for (int x = 0; x < d.width; ++x)
                for (int c = 0; c < 4; ++c) {
                    float sum = 0.0f;
                    for (int k = 0; k < 4; ++k) { // sRGB colour is averaged in linear light
                        const int sx = std::min(x * 2 + (k & 1), s.width - 1), sy = std::min(y * 2 + (k >> 1), s.height - 1);
                        const uint8_t v = s.rgba[(static_cast<size_t>(sy) * s.width + sx) * 4 + c];
                        sum += (srgb && c < 3) ? ToLinear(v) : static_cast<float>(v);
                    }
                    d.rgba[(static_cast<size_t>(y) * d.width + x) * 4 + c] =
                        (srgb && c < 3) ? ToSrgb(sum * 0.25f) : static_cast<uint8_t>(sum * 0.25f + 0.5f);
                }
        mips.push_back(std::move(d));
    }
    return mips;
}

void TextureLibrary::CreateDefaults() { Add("__white", 1, 1, {255, 255, 255, 255}, false); }

TextureHandle TextureLibrary::Add(const std::string& name, int width, int height, std::vector<uint8_t> rgba, bool srgb) {
    if (width <= 0 || height <= 0 || rgba.size() != static_cast<size_t>(width) * height * 4) return 0;
    if (m_textures.size() >= kMaxTextures) {
        Log::Error("Texture limit ({}) reached, '{}' not loaded", static_cast<uint32_t>(kMaxTextures), name);
        return 0;
    }
    Texture t;
    t.name = name;
    t.srgb = srgb;
    t.mips = BuildMipChain(width, height, std::move(rgba), srgb);
    glGenTextures(1, &t.gl);
    glBindTexture(GL_TEXTURE_2D, t.gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    for (size_t i = 0; i < t.mips.size(); ++i)
        glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(i), static_cast<GLint>(srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8), t.mips[i].width,
                     t.mips[i].height, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.mips[i].rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_REPEAT));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_REPEAT));
    glBindTexture(GL_TEXTURE_2D, 0);
    const auto handle = static_cast<TextureHandle>(m_textures.size());
    m_textures.push_back(std::move(t));
    m_byName[Key(name, srgb)] = handle;
    return handle;
}

TextureHandle TextureLibrary::Find(const std::string& name, bool srgb) const {
    auto it = m_byName.find(Key(name, srgb));
    return it == m_byName.end() ? 0 : it->second;
}

void TextureLibrary::Destroy() {
    for (Texture& t : m_textures) if (t.gl) glDeleteTextures(1, &t.gl);
    m_textures.clear();
    m_byName.clear();
}

} // namespace mage
