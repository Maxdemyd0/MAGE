// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "raytracing/PathTracer.h"
#include "core/Camera.h"
#include "core/Log.h"
#include "renderer/GL.h"
#include "renderer/Renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <unordered_map>
#include <vector>

#ifdef MAGE_HAS_VULKAN_RT
#include "raytracing/VulkanContext.h"
#endif

namespace mage {

#ifndef MAGE_HAS_VULKAN_RT
// ============================================================================ stub (no Vulkan build)
struct PathTracer::Impl {
    PathTracerStats stats;
};
PathTracer::PathTracer() : m(std::make_unique<Impl>()) { m->stats.error = "engine built with MAGE_ENABLE_VULKAN_RT=OFF"; }
PathTracer::~PathTracer() = default;
bool PathTracer::Init(const std::filesystem::path&, const MeshLibrary&, const TextureLibrary&) { return false; }
void PathTracer::Shutdown() {}
bool PathTracer::Available() const { return false; }
uint32_t PathTracer::Render(const std::vector<DrawCommand>&, const Camera&, const LightingSettings&, int, int) { return 0; }
void PathTracer::ResetAccumulation() {}
const PathTracerStats& PathTracer::Stats() const { return m->stats; }
#else

namespace {

using Clock = std::chrono::steady_clock;

// std140 mirror of 'Frame' in shaders/common.glsl.
struct FrameUBO {
    glm::mat4 invViewProj;
    glm::mat4 prevViewProj;
    glm::vec4 cameraPos;
    glm::vec4 cameraBasis;
    glm::vec4 sunDir;
    glm::vec4 solarIrradiance;
    glm::vec4 rayleigh;
    glm::vec4 mie;
    glm::vec4 ozone;
    glm::vec4 planet;
    glm::vec4 camera;
    glm::vec4 night;
    glm::uvec4 params0;
    glm::vec4 params1;
    glm::uvec4 size;
    glm::uvec4 counts;
};
static_assert(sizeof(FrameUBO) == 2 * 64 + 14 * 16);

// std430 mirror of 'Instance' in shaders/common.glsl.
struct GpuInstance {
    glm::vec4 albedoMetallic;
    glm::vec4 emissionRoughness;
    glm::vec4 layer;
    glm::vec4 misc;
    glm::uvec4 ids;
    glm::vec4 row0, row1, row2;
    glm::uvec4 textures;  // base color, metallic-roughness, normal, emissive (0 = none)
    glm::vec4 surface;    // normal scale, alpha, alpha cutoff, -
};
static_assert(sizeof(GpuInstance) == 160);
static_assert(sizeof(Vertex) == 32);

struct GpuLight {
    uint32_t instance;
    float cdf;
    float pdf;
    float pad;
};

constexpr uint32_t kFlagStatic = 1u, kFlagReset = 2u, kFlagAccumulate = 4u;
constexpr uint32_t kCameraAutoExposure = 1u, kCameraLightSampling = 2u, kCameraAerial = 4u;
constexpr uint32_t kInstanceTransmissive = 1u, kInstanceEmissive = 2u, kInstanceMasked = 4u, kInstanceSkinned = 8u;
constexpr VkBufferUsageFlags kSkinnedVertexUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
constexpr int kFramesInFlight = 2;
constexpr int kBloomLevels = 6;
constexpr VkDeviceSize kExposureBufferSize = 256 * 4 + 16;
constexpr uint32_t kLutSizes[3][2] = {{256, 64}, {32, 32}, {192, 108}}; // transmittance, multi-scatter, sky view

enum Pass {
    PassTrace, PassTemporal, PassAtrous, PassComposite,
    PassAtmoTransmittance, PassAtmoMultiScatter, PassAtmoSkyView,
    PassHistogram, PassExposure, PassBloomDown, PassBloomUp, PassTonemap,
    PassCount
};
const char* kShaderFiles[PassCount] = {
    "pathtrace.comp.spv", "temporal.comp.spv", "atrous.comp.spv", "composite.comp.spv",
    "atmo_transmittance.comp.spv", "atmo_multiscatter.comp.spv", "atmo_skyview.comp.spv",
    "histogram.comp.spv", "exposure.comp.spv", "bloom_down.comp.spv", "bloom_up.comp.spv", "tonemap.comp.spv"};

std::vector<uint32_t> ReadSpirv(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const auto size = static_cast<size_t>(in.tellg());
    std::vector<uint32_t> words((size + 3) / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(size));
    return words;
}

struct Hasher {
    uint64_t h = 1469598103934665603ull;
    void Add(const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 1099511628211ull;
    }
    template <class T>
    void Add(const T& v) { Add(&v, sizeof(T)); }
};

bool NearlyEqual(const glm::mat4& a, const glm::mat4& b, float eps) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (std::abs(a[c][r] - b[c][r]) > eps) return false;
    return true;
}

float Luminance(const glm::vec3& c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst, VkAccessFlags dstAccess) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}

void ComputeToCompute(VkCommandBuffer cmd) {
    Barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

} // namespace

// ============================================================================ Impl
struct FrameSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    vk::AccelerationStructure tlas;
    vk::Buffer tlasScratch, instanceBuffer, instanceData, lights, ubo, readback, statsReadback;
    // Skinned meshes: this frame's vertices and one BLAS per mesh instance.
    struct SkinnedBlas {
        vk::AccelerationStructure as;
        VkDeviceSize scratchSize = 0;
        uint32_t vertexCount = 0, primitives = 0;
        uint64_t version = 0, lastUsed = 0;
    };
    std::unordered_map<uint64_t, SkinnedBlas> skinned;
    vk::Buffer skinnedVertices, skinnedScratch;
    uint32_t instanceCapacity = 0;
    bool inFlight = false;
    uint64_t sequence = 0;          // submission order
    uint32_t width = 0, height = 0; // size of the image in 'readback'
    Clock::time_point submitTime;
};

struct PathTracer::Impl {
    vk::Context ctx;
    PathTracerStats stats;
    bool ready = false;

    // Static geometry (all meshes concatenated) + one BLAS per mesh.
    const MeshLibrary* meshLibrary = nullptr;
    const TextureLibrary* textureLibrary = nullptr;
    std::vector<vk::Image> textures;   // index == TextureHandle
    VkSampler textureSampler = VK_NULL_HANDLE;
    size_t builtMeshes = 0;
    vk::Buffer vertices, indices, meshTable;
    std::vector<vk::AccelerationStructure> blas;
    std::vector<uint32_t> meshTriangles;
    std::vector<glm::uvec4> meshRanges; // firstIndex, vertexOffset, indexCount
    std::vector<glm::vec3> meshExtents;

    // Render targets shared by both frame slots (ordered on the single queue).
    struct Targets {
        vk::Image noisy, albedo, emission, guide, output, hdr;
        vk::Image normal[2], position[2], historyColor[2], historyMoments[2], filter[2];
        vk::Image bloom[kBloomLevels];
    } targets;
    vk::Image luts[3];
    vk::Buffer exposure;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0;

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipelines[PassCount] = {};
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    FrameSlot slots[kFramesInFlight];
    uint64_t submitted = 0;
    uint64_t lastDisplayed = 0;

    uint32_t glTexture = 0;
    uint32_t glWidth = 0, glHeight = 0;
    bool hasResult = false;
    Clock::time_point fpsWindowStart = Clock::now();
    Clock::time_point lastRender = Clock::now();
    uint64_t fpsWindowFrames = 0;

    uint32_t frameIndex = 0;
    glm::mat4 prevViewProj{1.0f};
    bool resetRequested = true;
    uint64_t atmosphereHash = 0;

    // Static-scene baseline (captured whenever accumulation restarts).
    std::vector<glm::mat4> baselineModels;
    uint64_t baselineMaterialHash = 0;
    uint64_t baselineSettingsHash = 0;
    glm::vec3 baselineCamPos{0.0f}, baselineCamTarget{0.0f};
    float baselineFov = 0.0f, baselineAspect = 0.0f;
    std::vector<glm::mat4> currentModels;
    std::vector<GpuInstance> instanceScratch;
    std::vector<GpuLight> lightScratch;

    bool CreatePipelines(const std::filesystem::path& shaderDir);
    bool BuildGeometry(const MeshLibrary& meshes);
    void DestroyGeometry();
    void UploadTextures();
    void SyncLibraries();
    void CreateLuts();
    void CreateSlot(FrameSlot& slot);
    void DestroySlot(FrameSlot& slot);
    void EnsureInstanceCapacity(FrameSlot& slot, uint32_t count);
    void RecreateTargets(uint32_t w, uint32_t h);
    void DestroyTargets();
    void WriteImageDescriptors(VkDescriptorSet set);
    void WriteSceneDescriptors(FrameSlot& slot);
    void WaitIdle();
    void CollectFinished();
    void Display(FrameSlot& slot);
};

bool PathTracer::Impl::CreatePipelines(const std::filesystem::path& shaderDir) {
    VkDevice dev = ctx.device;

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    auto add = [&](uint32_t binding, VkDescriptorType type, uint32_t count = 1) {
        bindings.push_back({binding, type, count, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    };
    add(0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR);
    for (uint32_t b = 1; b <= 4; ++b) add(b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    add(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    for (uint32_t b = 6; b <= 8; ++b) add(b, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    for (uint32_t b = 9; b <= 13; ++b) add(b, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2);
    add(14, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    add(15, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    add(16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    add(17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    add(18, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    add(19, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    add(20, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3);
    add(21, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3);
    add(22, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kBloomLevels);
    add(23, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kBloomLevels);
    add(24, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, TextureLibrary::kMaxTextures);
    add(25, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = static_cast<uint32_t>(bindings.size());
    lci.pBindings = bindings.data();
    vkCreateDescriptorSetLayout(dev, &lci, nullptr, &setLayout);

    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    vkCreatePipelineLayout(dev, &plci, nullptr, &pipelineLayout);

    for (int p = 0; p < PassCount; ++p) {
        const std::vector<uint32_t> code = ReadSpirv(shaderDir / kShaderFiles[p]);
        if (code.empty()) {
            stats.error = "missing shader " + (shaderDir / kShaderFiles[p]).string();
            return false;
        }
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = code.size() * 4;
        smci.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &smci, nullptr, &module);

        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = module;
        cpci.stage.pName = "main";
        cpci.layout = pipelineLayout;
        const VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipelines[p]);
        vkDestroyShaderModule(dev, module, nullptr);
        if (r != VK_SUCCESS) {
            stats.error = std::string("failed to create pipeline ") + kShaderFiles[p];
            return false;
        }
    }

    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFramesInFlight},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7 * kFramesInFlight},
                                          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, (16 + 3 + kBloomLevels) * kFramesInFlight},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (1 + 3 + kBloomLevels + TextureLibrary::kMaxTextures) * kFramesInFlight}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = kFramesInFlight;
    dpci.poolSizeCount = 5;
    dpci.pPoolSizes = sizes;
    vkCreateDescriptorPool(dev, &dpci, nullptr, &descriptorPool);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    vkCreateSampler(dev, &sci, nullptr, &sampler);
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(dev, &sci, nullptr, &textureSampler);
    return true;
}

void PathTracer::Impl::DestroyGeometry() {
    for (auto& as : blas) ctx.DestroyAccelerationStructure(as);
    blas.clear();
    meshTriangles.clear();
    meshRanges.clear();
    meshExtents.clear();
    for (vk::Buffer* b : {&vertices, &indices, &meshTable}) ctx.DestroyBuffer(*b);
}

// Uploads textures added since the last call. The GPU must be idle.
void PathTracer::Impl::UploadTextures() {
    for (size_t i = textures.size(); i < textureLibrary->Count(); ++i) {
        const Texture& t = *textureLibrary->Get(static_cast<TextureHandle>(i));
        vk::Image img = ctx.CreateTexture(static_cast<uint32_t>(t.mips[0].width), static_cast<uint32_t>(t.mips[0].height),
                                          static_cast<uint32_t>(t.mips.size()), t.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
        size_t total = 0;
        for (const TextureMip& m : t.mips) total += m.rgba.size();
        vk::Buffer staging = ctx.CreateBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, vk::MemoryKind::Upload);
        if (!img.image || !staging.mapped) { // out of memory: show white instead
            ctx.DestroyImage(img);
            ctx.DestroyBuffer(staging);
            textures.push_back(vk::Image{});
            continue;
        }
        std::vector<VkBufferImageCopy> copies;
        size_t offset = 0;
        for (size_t l = 0; l < t.mips.size(); ++l) {
            const TextureMip& m = t.mips[l];
            std::memcpy(static_cast<uint8_t*>(staging.mapped) + offset, m.rgba.data(), m.rgba.size());
            VkBufferImageCopy c{};
            c.bufferOffset = offset;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<uint32_t>(l), 0, 1};
            c.imageExtent = {static_cast<uint32_t>(m.width), static_cast<uint32_t>(m.height), 1};
            copies.push_back(c);
            offset += m.rgba.size();
        }
        vmaFlushAllocation(ctx.allocator, staging.allocation, 0, VK_WHOLE_SIZE);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(t.mips.size()), 0, 1};
        VkCommandBuffer c = ctx.BeginOneShot();
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkCmdCopyBufferToImage(c, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copies.size()), copies.data());
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        ctx.EndOneShot(c);
        ctx.DestroyBuffer(staging);
        textures.push_back(img);
    }
    // Unused array slots point at the white texture, so every descriptor stays valid.
    std::vector<VkDescriptorImageInfo> infos(TextureLibrary::kMaxTextures);
    for (size_t i = 0; i < infos.size(); ++i)
        infos[i] = {textureSampler, textures[i < textures.size() && textures[i].view ? i : 0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (FrameSlot& slot : slots) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, slot.descriptorSet, 24, 0,
                               static_cast<uint32_t>(infos.size()), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
        w.pImageInfo = infos.data();
        vkUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);
    }
}

// Picks up meshes and textures that were imported since the last frame.
void PathTracer::Impl::SyncLibraries() {
    const bool newMeshes = meshLibrary->All().size() != builtMeshes;
    const bool newTextures = textureLibrary->Count() != textures.size();
    if (!newMeshes && !newTextures) return;
    WaitIdle();
    if (newTextures) UploadTextures();
    if (newMeshes) {
        DestroyGeometry();
        BuildGeometry(*meshLibrary);
        for (FrameSlot& slot : slots) WriteSceneDescriptors(slot);
    }
    resetRequested = true;
}

bool PathTracer::Impl::BuildGeometry(const MeshLibrary& meshes) {
    const size_t meshCount = meshes.All().size();
    std::vector<float> vertexData;
    std::vector<uint32_t> indexData;
    std::vector<glm::uvec4> table;
    for (MeshHandle h = 0; h < meshCount; ++h) {
        const MeshData* d = meshes.GetData(h);
        table.emplace_back(static_cast<uint32_t>(indexData.size()), static_cast<uint32_t>(vertexData.size() / 8),
                           d ? static_cast<uint32_t>(d->indices.size()) : 0u, 0u);
        meshTriangles.push_back(d ? static_cast<uint32_t>(d->indices.size() / 3) : 0u);
        const Mesh* mesh = meshes.Get(h);
        meshExtents.push_back(mesh ? mesh->bounds.max - mesh->bounds.min : glm::vec3(1.0f));
        if (!d) continue;
        for (const Vertex& v : d->vertices)
            vertexData.insert(vertexData.end(), {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.uv.x, v.uv.y});
        indexData.insert(indexData.end(), d->indices.begin(), d->indices.end());
    }
    if (indexData.empty()) {
        stats.error = "no mesh data for acceleration structures";
        return false;
    }

    const VkBufferUsageFlags geomUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
    vertices = ctx.CreateBuffer(vertexData.size() * 4, geomUsage, vk::MemoryKind::Upload);
    indices = ctx.CreateBuffer(indexData.size() * 4, geomUsage, vk::MemoryKind::Upload);
    meshTable = ctx.CreateBuffer(table.size() * sizeof(glm::uvec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vk::MemoryKind::Upload);
    std::memcpy(vertices.mapped, vertexData.data(), vertexData.size() * 4);
    std::memcpy(indices.mapped, indexData.data(), indexData.size() * 4);
    std::memcpy(meshTable.mapped, table.data(), table.size() * sizeof(glm::uvec4));

    meshRanges = table;
    blas.resize(meshCount);
    for (MeshHandle h = 0; h < meshCount; ++h) {
        const glm::uvec4& t = table[h];
        if (t.z == 0) continue;
        const MeshData* d = meshes.GetData(h);

        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        auto& tri = geometry.geometry.triangles;
        tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
        tri.vertexData.deviceAddress = vertices.address + static_cast<VkDeviceAddress>(t.y) * 32;
        tri.vertexStride = 32;
        tri.maxVertex = static_cast<uint32_t>(d->vertices.size() - 1);
        tri.indexType = VK_INDEX_TYPE_UINT32;
        tri.indexData.deviceAddress = indices.address + static_cast<VkDeviceAddress>(t.x) * 4;

        VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        build.geometryCount = 1;
        build.pGeometries = &geometry;
        const uint32_t primitives = t.z / 3;
        VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &primitives, &sizes);

        vk::AccelerationStructure& as = blas[h];
        as.storage = ctx.CreateBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, vk::MemoryKind::GpuOnly);
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = as.storage.buffer;
        ci.size = sizes.accelerationStructureSize;
        ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        vkCreateAccelerationStructureKHR(ctx.device, &ci, nullptr, &as.handle);

        vk::Buffer scratch = ctx.CreateBuffer(sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vk::MemoryKind::GpuOnly, ctx.scratchAlignment);
        build.dstAccelerationStructure = as.handle;
        build.scratchData.deviceAddress = scratch.address;
        VkAccelerationStructureBuildRangeInfoKHR range{primitives, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        VkCommandBuffer c = ctx.BeginOneShot();
        vkCmdBuildAccelerationStructuresKHR(c, 1, &build, &ranges);
        ctx.EndOneShot(c);
        ctx.DestroyBuffer(scratch);

        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = as.handle;
        as.address = vkGetAccelerationStructureDeviceAddressKHR(ctx.device, &ai);
    }
    builtMeshes = meshCount;
    Log::Info("Path tracer: built {} BLAS ({} triangles)", meshCount, indexData.size() / 3);
    return true;
}

// Atmosphere LUTs (fixed size) + the persistent auto-exposure buffer.
void PathTracer::Impl::CreateLuts() {
    for (int i = 0; i < 3; ++i) luts[i] = ctx.CreateImage(kLutSizes[i][0], kLutSizes[i][1], VK_FORMAT_R16G16B16A16_SFLOAT);
    exposure = ctx.CreateBuffer(kExposureBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                vk::MemoryKind::GpuOnly);
    VkCommandBuffer c = ctx.BeginOneShot();
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    for (const vk::Image& img : luts) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = range;
        vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        const VkClearColorValue zero{};
        vkCmdClearColorImage(c, img.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    vkCmdFillBuffer(c, exposure.buffer, 0, VK_WHOLE_SIZE, 0);
    ctx.EndOneShot(c);
}

void PathTracer::Impl::CreateSlot(FrameSlot& slot) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = ctx.commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx.device, &ai, &slot.cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(ctx.device, &fci, nullptr, &slot.fence);
    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = 2;
    vkCreateQueryPool(ctx.device, &qci, nullptr, &slot.queries);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = descriptorPool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &setLayout;
    vkAllocateDescriptorSets(ctx.device, &dsai, &slot.descriptorSet);
    slot.ubo = ctx.CreateBuffer(sizeof(FrameUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, vk::MemoryKind::Upload);
    slot.statsReadback = ctx.CreateBuffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, vk::MemoryKind::Readback);
    slot.skinnedVertices = ctx.CreateBuffer(1 << 16, kSkinnedVertexUsage, vk::MemoryKind::Upload);
    EnsureInstanceCapacity(slot, 1024);
}

void PathTracer::Impl::DestroySlot(FrameSlot& slot) {
    ctx.DestroyAccelerationStructure(slot.tlas);
    for (auto& [id, b] : slot.skinned) ctx.DestroyAccelerationStructure(b.as);
    for (vk::Buffer* b : {&slot.skinnedVertices, &slot.skinnedScratch, &slot.tlasScratch, &slot.instanceBuffer, &slot.instanceData, &slot.lights, &slot.ubo, &slot.readback, &slot.statsReadback})
        ctx.DestroyBuffer(*b);
    if (slot.fence) vkDestroyFence(ctx.device, slot.fence, nullptr);
    if (slot.queries) vkDestroyQueryPool(ctx.device, slot.queries, nullptr);
    slot = FrameSlot{};
}

void PathTracer::Impl::EnsureInstanceCapacity(FrameSlot& slot, uint32_t count) {
    if (count <= slot.instanceCapacity && slot.tlas.handle) return;
    const uint32_t capacity = std::max(count * 2, 1024u);
    ctx.DestroyAccelerationStructure(slot.tlas);
    ctx.DestroyBuffer(slot.tlasScratch);
    ctx.DestroyBuffer(slot.instanceBuffer);
    ctx.DestroyBuffer(slot.instanceData);
    ctx.DestroyBuffer(slot.lights);

    slot.instanceBuffer = ctx.CreateBuffer(capacity * sizeof(VkAccelerationStructureInstanceKHR),
                                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, vk::MemoryKind::Upload);
    slot.instanceData = ctx.CreateBuffer(capacity * sizeof(GpuInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vk::MemoryKind::Upload);
    slot.lights = ctx.CreateBuffer(capacity * sizeof(GpuLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vk::MemoryKind::Upload);

    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.data.deviceAddress = slot.instanceBuffer.address;
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geometry;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &capacity, &sizes);

    slot.tlas.storage = ctx.CreateBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, vk::MemoryKind::GpuOnly);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = slot.tlas.storage.buffer;
    ci.size = sizes.accelerationStructureSize;
    ci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    vkCreateAccelerationStructureKHR(ctx.device, &ci, nullptr, &slot.tlas.handle);
    slot.tlasScratch = ctx.CreateBuffer(sizes.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vk::MemoryKind::GpuOnly, ctx.scratchAlignment);
    slot.instanceCapacity = capacity;
    WriteSceneDescriptors(slot);
}

void PathTracer::Impl::WriteSceneDescriptors(FrameSlot& slot) {
    VkWriteDescriptorSetAccelerationStructureKHR asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asWrite.accelerationStructureCount = 1;
    asWrite.pAccelerationStructures = &slot.tlas.handle;
    const VkDescriptorBufferInfo bufferInfos[8] = {{vertices.buffer, 0, VK_WHOLE_SIZE},
                                                   {indices.buffer, 0, VK_WHOLE_SIZE},
                                                   {meshTable.buffer, 0, VK_WHOLE_SIZE},
                                                   {slot.instanceData.buffer, 0, VK_WHOLE_SIZE},
                                                   {slot.ubo.buffer, 0, sizeof(FrameUBO)},
                                                   {slot.lights.buffer, 0, VK_WHOLE_SIZE},
                                                   {exposure.buffer, 0, VK_WHOLE_SIZE},
                                                   {slot.skinnedVertices.buffer, 0, VK_WHOLE_SIZE}};
    const uint32_t bufferBindings[8] = {1, 2, 3, 4, 5, 16, 17, 25};
    VkWriteDescriptorSet writes[9] = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asWrite, slot.descriptorSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    for (uint32_t i = 0; i < 8; ++i) {
        writes[i + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, slot.descriptorSet, bufferBindings[i], 0, 1,
                         bufferBindings[i] == 5 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        writes[i + 1].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(ctx.device, 9, writes, 0, nullptr);
}

void PathTracer::Impl::DestroyTargets() {
    for (vk::Image* img : {&targets.noisy, &targets.albedo, &targets.emission, &targets.guide, &targets.output, &targets.hdr})
        ctx.DestroyImage(*img);
    for (int i = 0; i < 2; ++i)
        for (vk::Image* img : {&targets.normal[i], &targets.position[i], &targets.historyColor[i], &targets.historyMoments[i], &targets.filter[i]})
            ctx.DestroyImage(*img);
    for (vk::Image& img : targets.bloom) ctx.DestroyImage(img);
    for (FrameSlot& slot : slots) ctx.DestroyBuffer(slot.readback);
}

void PathTracer::Impl::RecreateTargets(uint32_t w, uint32_t h) {
    WaitIdle();
    DestroyTargets();
    width = w;
    height = h;
    targets.noisy = ctx.CreateImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
    targets.albedo = ctx.CreateImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
    targets.emission = ctx.CreateImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
    targets.guide = ctx.CreateImage(w, h, VK_FORMAT_R32G32B32A32_SFLOAT);
    targets.output = ctx.CreateImage(w, h, VK_FORMAT_R8G8B8A8_UNORM);
    targets.hdr = ctx.CreateImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
    for (int i = 0; i < 2; ++i) {
        targets.normal[i] = ctx.CreateImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
        targets.position[i] = ctx.CreateImage(w, h, VK_FORMAT_R32G32B32A32_SFLOAT);
        targets.historyColor[i] = ctx.CreateImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
        targets.historyMoments[i] = ctx.CreateImage(w, h, VK_FORMAT_R32G32B32A32_SFLOAT);
        targets.filter[i] = ctx.CreateImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
    }
    for (int i = 0; i < kBloomLevels; ++i)
        targets.bloom[i] = ctx.CreateImage(std::max(1u, w >> (i + 1)), std::max(1u, h >> (i + 1)), VK_FORMAT_R16G16B16A16_SFLOAT);
    for (FrameSlot& slot : slots)
        slot.readback = ctx.CreateBuffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, vk::MemoryKind::Readback);

    // UNDEFINED -> GENERAL and clear everything to zero.
    VkCommandBuffer c = ctx.BeginOneShot();
    std::vector<VkImage> all = {targets.noisy.image, targets.albedo.image, targets.emission.image, targets.guide.image,
                                targets.output.image, targets.hdr.image};
    for (int i = 0; i < 2; ++i)
        for (const vk::Image* img : {&targets.normal[i], &targets.position[i], &targets.historyColor[i], &targets.historyMoments[i], &targets.filter[i]})
            all.push_back(img->image);
    for (const vk::Image& img : targets.bloom) all.push_back(img.image);
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    std::vector<VkImageMemoryBarrier> barriers;
    for (VkImage img : all) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = range;
        barriers.push_back(b);
    }
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                         static_cast<uint32_t>(barriers.size()), barriers.data());
    const VkClearColorValue zero{};
    for (VkImage img : all) vkCmdClearColorImage(c, img, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    ctx.EndOneShot(c);

    for (FrameSlot& slot : slots) WriteImageDescriptors(slot.descriptorSet);
    resetRequested = true;
}

void PathTracer::Impl::WriteImageDescriptors(VkDescriptorSet set) {
    auto storage = [](const vk::Image& img) { return VkDescriptorImageInfo{VK_NULL_HANDLE, img.view, VK_IMAGE_LAYOUT_GENERAL}; };
    auto sampled = [this](const vk::Image& img) { return VkDescriptorImageInfo{sampler, img.view, VK_IMAGE_LAYOUT_GENERAL}; };

    const VkDescriptorImageInfo singles[6] = {storage(targets.noisy), storage(targets.albedo), storage(targets.emission),
                                              storage(targets.output), storage(targets.guide), storage(targets.hdr)};
    const uint32_t singleBindings[6] = {6, 7, 8, 14, 15, 18};
    const VkDescriptorImageInfo pairs[5][2] = {{storage(targets.normal[0]), storage(targets.normal[1])},
                                               {storage(targets.position[0]), storage(targets.position[1])},
                                               {storage(targets.historyColor[0]), storage(targets.historyColor[1])},
                                               {storage(targets.historyMoments[0]), storage(targets.historyMoments[1])},
                                               {storage(targets.filter[0]), storage(targets.filter[1])}};
    const VkDescriptorImageInfo hdrSampled = sampled(targets.hdr);
    VkDescriptorImageInfo lutStorage[3], lutSampled[3], bloomStorage[kBloomLevels], bloomSampled[kBloomLevels];
    for (int i = 0; i < 3; ++i) {
        lutStorage[i] = storage(luts[i]);
        lutSampled[i] = sampled(luts[i]);
    }
    for (int i = 0; i < kBloomLevels; ++i) {
        bloomStorage[i] = storage(targets.bloom[i]);
        bloomSampled[i] = sampled(targets.bloom[i]);
    }

    std::vector<VkWriteDescriptorSet> writes;
    auto write = [&](uint32_t binding, uint32_t count, VkDescriptorType type, const VkDescriptorImageInfo* infos) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, binding, 0, count, type};
        w.pImageInfo = infos;
        writes.push_back(w);
    };
    for (int i = 0; i < 6; ++i) write(singleBindings[i], 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &singles[i]);
    for (int i = 0; i < 5; ++i) write(static_cast<uint32_t>(9 + i), 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, pairs[i]);
    write(19, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &hdrSampled);
    write(20, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, lutStorage);
    write(21, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, lutSampled);
    write(22, kBloomLevels, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, bloomStorage);
    write(23, kBloomLevels, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, bloomSampled);
    vkUpdateDescriptorSets(ctx.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

void PathTracer::Impl::WaitIdle() {
    vkDeviceWaitIdle(ctx.device);
    CollectFinished();
}

// Hands every finished frame (oldest first) to OpenGL without ever blocking.
void PathTracer::Impl::CollectFinished() {
    for (;;) {
        FrameSlot* oldest = nullptr;
        for (FrameSlot& slot : slots)
            if (slot.inFlight && (!oldest || slot.sequence < oldest->sequence)) oldest = &slot;
        if (!oldest || vkGetFenceStatus(ctx.device, oldest->fence) != VK_SUCCESS) return;
        oldest->inFlight = false;
        stats.latencyMs = std::chrono::duration<double, std::milli>(Clock::now() - oldest->submitTime).count();
        uint64_t timestamps[2] = {};
        if (vkGetQueryPoolResults(ctx.device, oldest->queries, 0, 2, sizeof(timestamps), timestamps, sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            stats.gpuMs = static_cast<double>(timestamps[1] - timestamps[0]) * ctx.timestampPeriodNs * 1e-6;
        if (oldest->statsReadback.mapped) {
            vmaInvalidateAllocation(ctx.allocator, oldest->statsReadback.allocation, 0, VK_WHOLE_SIZE);
            float values[4];
            std::memcpy(values, oldest->statsReadback.mapped, sizeof(values));
            stats.ev100 = values[1];
            stats.sceneLuminance = values[2];
        }
        if (oldest->sequence > lastDisplayed && oldest->readback.buffer) Display(*oldest);
    }
}

void PathTracer::Impl::Display(FrameSlot& slot) {
    const auto start = Clock::now();
    using namespace gl;
    vmaInvalidateAllocation(ctx.allocator, slot.readback.allocation, 0, VK_WHOLE_SIZE);
    if (!glTexture) glGenTextures(1, &glTexture);
    glBindTexture(GL_TEXTURE_2D, glTexture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (glWidth != slot.width || glHeight != slot.height) {
        glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA8), static_cast<GLsizei>(slot.width), static_cast<GLsizei>(slot.height), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glWidth = slot.width;
        glHeight = slot.height;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(slot.width), static_cast<GLsizei>(slot.height), GL_RGBA,
                    GL_UNSIGNED_BYTE, slot.readback.mapped);
    glBindTexture(GL_TEXTURE_2D, 0);
    hasResult = true;
    lastDisplayed = slot.sequence;
    ++stats.framesCompleted;
    ++fpsWindowFrames;
    const double window = std::chrono::duration<double>(Clock::now() - fpsWindowStart).count();
    if (window >= 0.5) {
        stats.tracedFps = static_cast<double>(fpsWindowFrames) / window;
        fpsWindowFrames = 0;
        fpsWindowStart = Clock::now();
    }
    stats.uploadMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// ============================================================================ PathTracer
PathTracer::PathTracer() : m(std::make_unique<Impl>()) {}
PathTracer::~PathTracer() { Shutdown(); }

bool PathTracer::Init(const std::filesystem::path& shaderDirectory, const MeshLibrary& meshes, const TextureLibrary& textures) {
    m->meshLibrary = &meshes;
    m->textureLibrary = &textures;
    std::string error;
    if (!m->ctx.Init(error)) {
        m->stats.error = error;
        return false;
    }
    m->stats.device = m->ctx.deviceName;
    if (!m->CreatePipelines(shaderDirectory) || !m->BuildGeometry(meshes)) {
        Shutdown();
        return false;
    }
    m->CreateLuts();
    for (FrameSlot& slot : m->slots) m->CreateSlot(slot);
    m->UploadTextures();
    m->ready = true;
    m->stats.available = true;
    Log::Info("Path tracer ready on {} ({} frames in flight, physical sky + camera)", m->ctx.deviceName, kFramesInFlight);
    return true;
}

void PathTracer::Shutdown() {
    if (!m->ctx.Valid()) return;
    vkDeviceWaitIdle(m->ctx.device);
    VkDevice dev = m->ctx.device;
    m->DestroyTargets();
    for (FrameSlot& slot : m->slots) m->DestroySlot(slot);
    m->DestroyGeometry();
    for (vk::Image& img : m->textures) m->ctx.DestroyImage(img);
    m->textures.clear();
    for (vk::Image& img : m->luts) m->ctx.DestroyImage(img);
    m->ctx.DestroyBuffer(m->exposure);
    for (VkPipeline& p : m->pipelines) {
        if (p) vkDestroyPipeline(dev, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (m->sampler) vkDestroySampler(dev, m->sampler, nullptr);
    if (m->textureSampler) vkDestroySampler(dev, m->textureSampler, nullptr);
    m->textureSampler = VK_NULL_HANDLE;
    if (m->pipelineLayout) vkDestroyPipelineLayout(dev, m->pipelineLayout, nullptr);
    if (m->setLayout) vkDestroyDescriptorSetLayout(dev, m->setLayout, nullptr);
    if (m->descriptorPool) vkDestroyDescriptorPool(dev, m->descriptorPool, nullptr);
    m->sampler = VK_NULL_HANDLE;
    m->pipelineLayout = VK_NULL_HANDLE;
    m->setLayout = VK_NULL_HANDLE;
    m->descriptorPool = VK_NULL_HANDLE;
    m->ctx.Shutdown();
    if (m->glTexture && gl::glDeleteTextures) gl::glDeleteTextures(1, &m->glTexture);
    m->glTexture = 0;
    m->ready = false;
    m->stats.available = false;
}

bool PathTracer::Available() const { return m->ready; }
const PathTracerStats& PathTracer::Stats() const { return m->stats; }
void PathTracer::ResetAccumulation() { m->resetRequested = true; }

uint32_t PathTracer::Render(const std::vector<DrawCommand>& commands, const Camera& camera, const LightingSettings& lighting,
                            int outputWidth, int outputHeight) {
    if (!m->ready || outputWidth <= 0 || outputHeight <= 0) return 0;
    Impl& s = *m;
    s.CollectFinished();
    s.SyncLibraries();

    // ---- Resolution ----------------------------------------------------------------------------
    const float scale = std::clamp(settings.renderScale, 0.25f, 1.0f);
    const uint32_t w = std::max(1u, static_cast<uint32_t>(std::lround(outputWidth * scale)));
    const uint32_t h = std::max(1u, static_cast<uint32_t>(std::lround(outputHeight * scale)));
    if (w != s.width || h != s.height) s.RecreateTargets(w, h);

    // ---- Free frame slot? (never block: keep showing the newest finished frame) -----------------
    FrameSlot* slot = nullptr;
    for (FrameSlot& candidate : s.slots)
        if (!candidate.inFlight) { slot = &candidate; break; }
    if (!slot) return s.hasResult ? s.glTexture : 0;

    const auto now = Clock::now();
    const float dt = std::chrono::duration<float>(now - s.lastRender).count();
    s.lastRender = now;

    // ---- Instances (TLAS input + materials) and emissive light list ---------------------------
    s.EnsureInstanceCapacity(*slot, static_cast<uint32_t>(commands.size()));
    // Skinned meshes: copy this frame's vertices and queue a BLAS build for everything that moved.
    size_t skinnedCount = 0;
    for (const DrawCommand& c : commands) if (c.skinned) skinnedCount += c.skinned->vertices->size();
    if (skinnedCount * sizeof(Vertex) > slot->skinnedVertices.size) {
        s.ctx.DestroyBuffer(slot->skinnedVertices);
        slot->skinnedVertices = s.ctx.CreateBuffer(skinnedCount * sizeof(Vertex) * 3 / 2, kSkinnedVertexUsage, vk::MemoryKind::Upload);
        s.WriteSceneDescriptors(*slot);
    }
    struct SkinnedBuild { FrameSlot::SkinnedBlas* blas; VkAccelerationStructureGeometryKHR geometry; };
    std::vector<SkinnedBuild> skinnedBuilds;
    uint32_t skinnedOffset = 0;
    VkDeviceSize skinnedScratch = 0;
    auto alignScratch = [&](VkDeviceSize v) { return (v + s.ctx.scratchAlignment - 1) / s.ctx.scratchAlignment * s.ctx.scratchAlignment; };
    auto* vkInstances = static_cast<VkAccelerationStructureInstanceKHR*>(slot->instanceBuffer.mapped);
    s.currentModels.clear();
    s.instanceScratch.clear();
    s.lightScratch.clear();
    float lightTotal = 0.0f;
    Hasher skinnedHash;
    uint32_t count = 0;
    for (const DrawCommand& c : commands) {
        if (c.mesh >= s.blas.size() || !s.blas[c.mesh].handle) continue;
        VkDeviceAddress blasAddress = s.blas[c.mesh].address;
        uint32_t vertexOffset = s.meshRanges[c.mesh].y;
        if (c.skinned) {
            const std::vector<Vertex>& verts = *c.skinned->vertices;
            vertexOffset = skinnedOffset;
            std::memcpy(static_cast<Vertex*>(slot->skinnedVertices.mapped) + skinnedOffset, verts.data(), verts.size() * sizeof(Vertex));
            skinnedOffset += static_cast<uint32_t>(verts.size());

            FrameSlot::SkinnedBlas& b = slot->skinned[c.skinned->id];
            VkAccelerationStructureGeometryKHR g{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            g.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            g.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            auto& tri = g.geometry.triangles;
            tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
            tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
            tri.vertexData.deviceAddress = slot->skinnedVertices.address + static_cast<VkDeviceAddress>(vertexOffset) * sizeof(Vertex);
            tri.vertexStride = sizeof(Vertex);
            tri.maxVertex = static_cast<uint32_t>(verts.size() - 1);
            tri.indexType = VK_INDEX_TYPE_UINT32;
            tri.indexData.deviceAddress = s.indices.address + static_cast<VkDeviceAddress>(s.meshRanges[c.mesh].x) * 4;
            const uint32_t primitives = s.meshTriangles[c.mesh];
            if (!b.as.handle || b.vertexCount != verts.size() || b.primitives != primitives) {
                s.ctx.DestroyAccelerationStructure(b.as);
                VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
                info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
                info.geometryCount = 1;
                info.pGeometries = &g;
                VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
                vkGetAccelerationStructureBuildSizesKHR(s.ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &primitives, &sizes);
                b.as.storage = s.ctx.CreateBuffer(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, vk::MemoryKind::GpuOnly);
                VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
                ci.buffer = b.as.storage.buffer;
                ci.size = sizes.accelerationStructureSize;
                ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                vkCreateAccelerationStructureKHR(s.ctx.device, &ci, nullptr, &b.as.handle);
                VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
                ai.accelerationStructure = b.as.handle;
                b.as.address = vkGetAccelerationStructureDeviceAddressKHR(s.ctx.device, &ai);
                b.scratchSize = sizes.buildScratchSize;
                b.vertexCount = static_cast<uint32_t>(verts.size());
                b.primitives = primitives;
                b.version = 0;
            }
            if (b.version != c.skinned->version) {
                b.version = c.skinned->version;
                skinnedBuilds.push_back({&b, g}); // unordered_map references stay valid
                skinnedScratch += alignScratch(b.scratchSize);
            }
            b.lastUsed = s.submitted;
            blasAddress = b.as.address;
            skinnedHash.Add(c.skinned->id);
            skinnedHash.Add(c.skinned->version);
        }
        const Material& mat = c.material;
        const bool transmissive = mat.transmission > 0.0f;
        const bool masked = mat.alphaCutoff > 0.0f;
        const float emitted = Luminance(mat.emissive);

        VkAccelerationStructureInstanceKHR inst{};
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 4; ++col) inst.transform.matrix[r][col] = c.model[col][r];
        inst.instanceCustomIndex = count;
        inst.mask = 0xFF;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
                     (transmissive || masked ? VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR : 0u);
        inst.accelerationStructureReference = blasAddress;
        vkInstances[count] = inst; // write-only: mapped memory is write-combined

        GpuInstance gi{};
        gi.albedoMetallic = glm::vec4(mat.albedo, mat.metallic);
        gi.emissionRoughness = glm::vec4(mat.emissive, mat.roughness);
        gi.layer = glm::vec4(mat.checkerScale, mat.clearcoat, mat.clearcoatRoughness, std::clamp(mat.transmission, 0.0f, 1.0f));
        gi.misc = glm::vec4(mat.ior, 0.0f, 0.0f, 0.0f);
        gi.ids = glm::uvec4(c.mesh, s.meshTriangles[c.mesh], (transmissive ? kInstanceTransmissive : 0u) | (emitted > 0.0f ? kInstanceEmissive : 0u) | (masked ? kInstanceMasked : 0u) | (c.skinned ? kInstanceSkinned : 0u), vertexOffset);
        gi.row0 = glm::vec4(c.model[0][0], c.model[1][0], c.model[2][0], c.model[3][0]);
        gi.row1 = glm::vec4(c.model[0][1], c.model[1][1], c.model[2][1], c.model[3][1]);
        gi.row2 = glm::vec4(c.model[0][2], c.model[1][2], c.model[2][2], c.model[3][2]);
        gi.textures = glm::uvec4(mat.baseColorTexture, mat.metallicRoughnessTexture, mat.normalTexture, mat.emissiveTexture);
        gi.surface = glm::vec4(mat.normalScale, mat.alpha, mat.alphaCutoff, 0.0f);
        s.instanceScratch.push_back(gi);

        if (emitted > 0.0f && settings.lightSampling) {
            // Select lights proportionally to emitted power ~ luminance * surface area.
            const glm::vec3 e = s.meshExtents[c.mesh] *
                                glm::vec3(glm::length(glm::vec3(c.model[0])), glm::length(glm::vec3(c.model[1])), glm::length(glm::vec3(c.model[2])));
            const float area = 2.0f * (e.x * e.y + e.y * e.z + e.x * e.z) + 1e-4f;
            const float weight = emitted * area;
            s.lightScratch.push_back(GpuLight{count, weight, 0.0f, 0.0f});
            lightTotal += weight;
        }
        s.currentModels.push_back(c.model);
        ++count;
    }
    s.stats.instances = count;
    if (count == 0) return s.hasResult ? s.glTexture : 0;

    float running = 0.0f;
    for (GpuLight& l : s.lightScratch) {
        l.pdf = l.cdf / lightTotal;
        running += l.pdf;
        l.cdf = running;
        s.instanceScratch[l.instance].misc.y = l.pdf;
    }
    if (!s.lightScratch.empty()) s.lightScratch.back().cdf = 1.0f;
    s.stats.lights = static_cast<uint32_t>(s.lightScratch.size());
    std::memcpy(slot->instanceData.mapped, s.instanceScratch.data(), s.instanceScratch.size() * sizeof(GpuInstance));
    if (!s.lightScratch.empty()) std::memcpy(slot->lights.mapped, s.lightScratch.data(), s.lightScratch.size() * sizeof(GpuLight));

    for (auto it = slot->skinned.begin(); it != slot->skinned.end();) { // this slot is idle: safe to free
        if (it->second.lastUsed + 120 < s.submitted) {
            s.ctx.DestroyAccelerationStructure(it->second.as);
            it = slot->skinned.erase(it);
        } else ++it;
    }
    if (skinnedScratch > slot->skinnedScratch.size) {
        s.ctx.DestroyBuffer(slot->skinnedScratch);
        slot->skinnedScratch = s.ctx.CreateBuffer(skinnedScratch * 3 / 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vk::MemoryKind::GpuOnly, s.ctx.scratchAlignment);
    }

    Hasher materialHash;
    materialHash.Add(skinnedHash.h);
    for (const GpuInstance& gi : s.instanceScratch) {
        materialHash.Add(gi.albedoMetallic);
        materialHash.Add(gi.emissionRoughness);
        materialHash.Add(gi.layer);
        materialHash.Add(gi.misc);
        materialHash.Add(gi.ids);
        materialHash.Add(gi.textures);
        materialHash.Add(gi.surface);
    }

    // ---- Static detection: compare against the snapshot taken when accumulation started ----------
    const float aspect = static_cast<float>(outputWidth) / static_cast<float>(outputHeight);
    const glm::mat4 viewProj = camera.Projection(aspect) * camera.View();
    Hasher settingsHash;
    settingsHash.Add(lighting.sunDirection);
    settingsHash.Add(settings.maxBounces);
    settingsHash.Add(settings.samplesPerPixel);
    settingsHash.Add(settings.lightSampling);
    settingsHash.Add(settings.fireflyClamp);
    settingsHash.Add(settings.sunAngularDiameterDeg);
    settingsHash.Add(settings.sunIntensity);
    settingsHash.Add(settings.rayleighScale);
    settingsHash.Add(settings.hazeDensity);
    settingsHash.Add(settings.mieAnisotropy);
    settingsHash.Add(settings.ozoneScale);
    settingsHash.Add(settings.groundAlbedo);
    settingsHash.Add(settings.groundAltitudeKm);
    settingsHash.Add(settings.nightSkyLuminance);
    settingsHash.Add(settings.depthOfField);
    settingsHash.Add(settings.fStop);
    settingsHash.Add(settings.focusDistance);

    const float tol = std::max(settings.staticTolerance, 0.0f);
    const bool cameraStatic = glm::all(glm::lessThanEqual(glm::abs(camera.position - s.baselineCamPos), glm::vec3(tol))) &&
                              glm::all(glm::lessThanEqual(glm::abs(camera.target - s.baselineCamTarget), glm::vec3(tol))) &&
                              camera.fovDegrees == s.baselineFov && aspect == s.baselineAspect;
    bool sceneStatic = count == s.baselineModels.size() && materialHash.h == s.baselineMaterialHash &&
                       settingsHash.h == s.baselineSettingsHash;
    for (uint32_t i = 0; sceneStatic && i < count; ++i) sceneStatic = NearlyEqual(s.currentModels[i], s.baselineModels[i], tol);

    const bool reset = s.resetRequested || s.frameIndex == 0;
    const bool isStatic = !reset && cameraStatic && sceneStatic;
    if (!isStatic) {
        if (!reset) {
            if (!cameraStatic) ++s.stats.cameraChanges;
            if (!sceneStatic) ++s.stats.sceneChanges;
        }
        s.baselineModels = s.currentModels;
        s.baselineMaterialHash = materialHash.h;
        s.baselineSettingsHash = settingsHash.h;
        s.baselineCamPos = camera.position;
        s.baselineCamTarget = camera.target;
        s.baselineFov = camera.fovDegrees;
        s.baselineAspect = aspect;
    }
    s.stats.staticFrames = isStatic ? s.stats.staticFrames + 1 : 0;

    // ---- Frame constants --------------------------------------------------------------------------
    FrameUBO ubo{};
    ubo.invViewProj = glm::inverse(viewProj);
    ubo.prevViewProj = reset ? viewProj : s.prevViewProj;
    ubo.cameraPos = glm::vec4(camera.position, dt);
    const float focus = settings.focusDistance > 0.0f ? settings.focusDistance : glm::length(camera.target - camera.position);
    ubo.cameraBasis = glm::vec4(camera.Forward(), std::max(focus, 0.1f));
    const float halfAngle = glm::radians(std::max(settings.sunAngularDiameterDeg, 0.01f) * 0.5f);
    ubo.sunDir = glm::vec4(glm::normalize(lighting.sunDirection), std::cos(halfAngle));
    ubo.solarIrradiance = glm::vec4(glm::vec3(1.474f, 1.8504f, 1.91198f) * settings.sunIntensity, halfAngle);
    ubo.rayleigh = glm::vec4(glm::vec3(5.802e-3f, 13.558e-3f, 33.1e-3f) * settings.rayleighScale, 8.0f);
    ubo.mie = glm::vec4(3.996e-3f, 4.440e-3f, std::clamp(settings.mieAnisotropy, 0.0f, 0.95f), 1.2f);
    ubo.ozone = glm::vec4(glm::vec3(0.650e-3f, 1.881e-3f, 0.085e-3f) * settings.ozoneScale, settings.groundAlbedo);
    ubo.planet = glm::vec4(6360.0f, 6460.0f, settings.groundAltitudeKm, std::max(settings.hazeDensity, 0.0f));
    // Physical aperture: focal length from the vertical FOV on a 24 mm (full-frame) sensor.
    const float focalLength = 0.012f / std::tan(glm::radians(camera.fovDegrees) * 0.5f);
    const float apertureRadius = settings.depthOfField ? focalLength / (2.0f * std::max(settings.fStop, 0.5f)) : 0.0f;
    ubo.camera = glm::vec4(settings.exposureCompensation, settings.manualEV100, std::clamp(settings.bloomIntensity, 0.0f, 1.0f), apertureRadius);
    const glm::vec3 nightTint(0.6f, 0.75f, 1.0f);
    ubo.night = glm::vec4(nightTint / Luminance(nightTint) * (settings.nightSkyLuminance / kRadianceToLuminance), settings.fireflyClamp);
    const uint32_t flags = (isStatic ? kFlagStatic : 0u) | (reset ? kFlagReset : 0u) | (settings.accumulate ? kFlagAccumulate : 0u);
    ubo.params0 = glm::uvec4(s.frameIndex, static_cast<uint32_t>(std::clamp(settings.maxBounces, 0, 16)),
                             static_cast<uint32_t>(std::clamp(settings.samplesPerPixel, 1, 64)), flags);
    ubo.params1 = glm::vec4(kRadianceToLuminance, static_cast<float>(std::max(settings.maxHistory, 1)), settings.colorSigma,
                            std::clamp(settings.movingMinAlpha, 0.01f, 1.0f));
    const int iterations = settings.denoise ? std::clamp(settings.atrousIterations, 0, 8) : 0;
    ubo.size = glm::uvec4(w, h, count, static_cast<uint32_t>(iterations));
    const uint32_t cameraFlags = (settings.autoExposure ? kCameraAutoExposure : 0u) | (settings.lightSampling ? kCameraLightSampling : 0u) |
                                 (settings.aerialPerspective ? kCameraAerial : 0u);
    ubo.counts = glm::uvec4(static_cast<uint32_t>(s.lightScratch.size()), settings.bloomIntensity > 0.0f ? kBloomLevels : 0, cameraFlags,
                            glm::floatBitsToUint(2.0f * std::tan(glm::radians(camera.fovDegrees) * 0.5f) / static_cast<float>(h)));
    std::memcpy(slot->ubo.mapped, &ubo, sizeof(ubo));

    // Atmosphere LUTs that only depend on the medium (not the sun): rebuild when it changes.
    Hasher atmoHash;
    atmoHash.Add(ubo.rayleigh);
    atmoHash.Add(ubo.mie);
    atmoHash.Add(ubo.ozone);
    atmoHash.Add(ubo.planet.x);
    atmoHash.Add(ubo.planet.y);
    atmoHash.Add(ubo.planet.w);
    const bool rebuildAtmosphere = atmoHash.h != s.atmosphereHash;
    s.atmosphereHash = atmoHash.h;

    // ---- Record ------------------------------------------------------------------------------------
    VkCommandBuffer cmd = slot->cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    vkCmdResetQueryPool(cmd, slot->queries, 0, 2);
    // Order against the previous frame still on the queue (shared history / output images).
    Barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot->queries, 0);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipelineLayout, 0, 1, &slot->descriptorSet, 0, nullptr);
    auto dispatch = [&](Pass pass, glm::ivec4 push, uint32_t gx, uint32_t gy) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipelines[pass]);
        vkCmdPushConstants(cmd, s.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, gx, gy, 1);
        ComputeToCompute(cmd);
    };
    auto groups = [](uint32_t n, uint32_t local) { return (n + local - 1) / local; };

    // Atmosphere.
    if (rebuildAtmosphere) {
        dispatch(PassAtmoTransmittance, glm::ivec4(0), groups(kLutSizes[0][0], 8), groups(kLutSizes[0][1], 8));
        dispatch(PassAtmoMultiScatter, glm::ivec4(0), kLutSizes[1][0], kLutSizes[1][1]);
    }
    dispatch(PassAtmoSkyView, glm::ivec4(0), groups(kLutSizes[2][0], 8), groups(kLutSizes[2][1], 8));

    // Skinned meshes are rebuilt from scratch: robust against large pose changes, and fast enough.
    if (!skinnedBuilds.empty()) {
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
        std::vector<VkAccelerationStructureBuildRangeInfoKHR> rangeData;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> rangePtrs;
        VkDeviceSize scratchOffset = 0;
        for (const SkinnedBuild& sb : skinnedBuilds) {
            VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
            info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
            info.dstAccelerationStructure = sb.blas->as.handle;
            info.geometryCount = 1;
            info.pGeometries = &sb.geometry;
            info.scratchData.deviceAddress = slot->skinnedScratch.address + scratchOffset;
            scratchOffset += alignScratch(sb.blas->scratchSize);
            infos.push_back(info);
            rangeData.push_back({sb.blas->primitives, 0, 0, 0});
        }
        for (const auto& r : rangeData) rangePtrs.push_back(&r);
        vkCmdBuildAccelerationStructuresKHR(cmd, static_cast<uint32_t>(infos.size()), infos.data(), rangePtrs.data());
        Barrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);
    }

    // Scene acceleration structure.
    VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.data.deviceAddress = slot->instanceBuffer.address;
    VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.dstAccelerationStructure = slot->tlas.handle;
    build.geometryCount = 1;
    build.pGeometries = &geometry;
    build.scratchData.deviceAddress = slot->tlasScratch.address;
    VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
    Barrier(cmd, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);

    // Light transport + denoise.
    const uint32_t gx = groups(w, 8), gy = groups(h, 8);
    dispatch(PassTrace, glm::ivec4(0), gx, gy);
    dispatch(PassTemporal, glm::ivec4(0), gx, gy);
    int src = 0;
    for (int i = 0; i < iterations; ++i) {
        dispatch(PassAtrous, glm::ivec4(1 << i, src, src ^ 1, 0), gx, gy);
        src ^= 1;
    }
    dispatch(PassComposite, glm::ivec4(0, src, 0, 0), gx, gy);

    // Camera: metering, bloom, tonemapping.
    dispatch(PassHistogram, glm::ivec4(0), groups(w, 16), groups(h, 16));
    dispatch(PassExposure, glm::ivec4(0), 1, 1);
    if (ubo.counts.y > 0) {
        for (int level = 0; level < kBloomLevels; ++level)
            dispatch(PassBloomDown, glm::ivec4(level, 0, 0, 0), groups(std::max(1u, w >> (level + 1)), 8), groups(std::max(1u, h >> (level + 1)), 8));
        for (int level = kBloomLevels - 2; level >= 0; --level)
            dispatch(PassBloomUp, glm::ivec4(level, 0, 0, 0), groups(std::max(1u, w >> (level + 1)), 8), groups(std::max(1u, h >> (level + 1)), 8));
    }
    dispatch(PassTonemap, glm::ivec4(0), gx, gy);

    Barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, s.targets.output.image, VK_IMAGE_LAYOUT_GENERAL, slot->readback.buffer, 1, &copy);
    const VkBufferCopy statsCopy{256 * 4, 0, 16};
    vkCmdCopyBuffer(cmd, s.exposure.buffer, slot->statsReadback.buffer, 1, &statsCopy);
    Barrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot->queries, 1);
    vkEndCommandBuffer(cmd);

    vkResetFences(s.ctx.device, 1, &slot->fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(s.ctx.queue, 1, &si, slot->fence) != VK_SUCCESS) {
        Log::Error("Path tracer: vkQueueSubmit failed - disabling");
        s.ready = false;
        return 0;
    }
    slot->inFlight = true;
    slot->sequence = ++s.submitted;
    slot->width = w;
    slot->height = h;
    slot->submitTime = Clock::now();
    s.prevViewProj = viewProj;
    s.resetRequested = false;
    ++s.frameIndex;
    s.stats.width = w;
    s.stats.height = h;
    return s.hasResult ? s.glTexture : 0;
}

#endif // MAGE_HAS_VULKAN_RT

} // namespace mage
