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
bool PathTracer::Init(const std::filesystem::path&, const MeshLibrary&) { return false; }
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
    glm::vec4 sunDir;
    glm::vec4 sunColor;
    glm::vec4 skyZenith;
    glm::vec4 skyHorizon;
    glm::vec4 groundColor;
    glm::uvec4 params0;
    glm::vec4 params1;
    glm::uvec4 size;
};
static_assert(sizeof(FrameUBO) == 2 * 64 + 9 * 16);

// std430 mirror of 'Instance' in shaders/common.glsl.
struct GpuInstance {
    glm::vec4 albedoMetallic;
    glm::vec4 emissionRoughness;
    glm::vec4 misc;
    glm::uvec4 ids;
};
static_assert(sizeof(GpuInstance) == 64);

constexpr uint32_t kFlagStatic = 1u, kFlagReset = 2u, kFlagAccumulate = 4u;
constexpr int kFramesInFlight = 2;

enum Pass { PassTrace, PassTemporal, PassAtrous, PassComposite, PassCount };
const char* kShaderFiles[PassCount] = {"pathtrace.comp.spv", "temporal.comp.spv", "atrous.comp.spv", "composite.comp.spv"};

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

void Barrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst, VkAccessFlags dstAccess) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
}

} // namespace

// ============================================================================ Impl
struct FrameSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    vk::AccelerationStructure tlas;
    vk::Buffer tlasScratch, instanceBuffer, instanceData, ubo, readback;
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
    vk::Buffer vertices, indices, meshTable;
    std::vector<vk::AccelerationStructure> blas;

    // Render targets shared by both frame slots (ordered on the single queue).
    struct Targets {
        vk::Image noisy, albedo, emission, guide, output;
        vk::Image normal[2], position[2], historyColor[2], historyMoments[2], filter[2];
    } targets;
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
    uint64_t fpsWindowFrames = 0;

    uint32_t frameIndex = 0;
    glm::mat4 prevViewProj{1.0f};
    bool resetRequested = true;

    // Static-scene baseline (captured whenever accumulation restarts).
    std::vector<glm::mat4> baselineModels;
    uint64_t baselineMaterialHash = 0;
    uint64_t baselineSettingsHash = 0;
    glm::vec3 baselineCamPos{0.0f}, baselineCamTarget{0.0f};
    float baselineFov = 0.0f, baselineAspect = 0.0f;
    std::vector<glm::mat4> currentModels;

    bool CreatePipelines(const std::filesystem::path& shaderDir);
    bool BuildGeometry(const MeshLibrary& meshes);
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
                                          {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * kFramesInFlight},
                                          {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
                                          {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 15 * kFramesInFlight}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = kFramesInFlight;
    dpci.poolSizeCount = 4;
    dpci.pPoolSizes = sizes;
    vkCreateDescriptorPool(dev, &dpci, nullptr, &descriptorPool);
    return true;
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
    Log::Info("Path tracer: built {} BLAS ({} triangles)", meshCount, indexData.size() / 3);
    return true;
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
    EnsureInstanceCapacity(slot, 1024);
}

void PathTracer::Impl::DestroySlot(FrameSlot& slot) {
    ctx.DestroyAccelerationStructure(slot.tlas);
    for (vk::Buffer* b : {&slot.tlasScratch, &slot.instanceBuffer, &slot.instanceData, &slot.ubo, &slot.readback}) ctx.DestroyBuffer(*b);
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

    slot.instanceBuffer = ctx.CreateBuffer(capacity * sizeof(VkAccelerationStructureInstanceKHR),
                                           VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, vk::MemoryKind::Upload);
    slot.instanceData = ctx.CreateBuffer(capacity * sizeof(GpuInstance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, vk::MemoryKind::Upload);

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
    const VkDescriptorBufferInfo bufferInfos[5] = {{vertices.buffer, 0, VK_WHOLE_SIZE},
                                                   {indices.buffer, 0, VK_WHOLE_SIZE},
                                                   {meshTable.buffer, 0, VK_WHOLE_SIZE},
                                                   {slot.instanceData.buffer, 0, VK_WHOLE_SIZE},
                                                   {slot.ubo.buffer, 0, sizeof(FrameUBO)}};
    VkWriteDescriptorSet writes[6] = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asWrite, slot.descriptorSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR};
    for (uint32_t i = 0; i < 5; ++i) {
        writes[i + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, slot.descriptorSet, i + 1, 0, 1,
                         i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        writes[i + 1].pBufferInfo = &bufferInfos[i];
    }
    vkUpdateDescriptorSets(ctx.device, 6, writes, 0, nullptr);
}

void PathTracer::Impl::DestroyTargets() {
    for (vk::Image* img : {&targets.noisy, &targets.albedo, &targets.emission, &targets.guide, &targets.output}) ctx.DestroyImage(*img);
    for (int i = 0; i < 2; ++i)
        for (vk::Image* img : {&targets.normal[i], &targets.position[i], &targets.historyColor[i], &targets.historyMoments[i], &targets.filter[i]})
            ctx.DestroyImage(*img);
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
    for (int i = 0; i < 2; ++i) {
        targets.normal[i] = ctx.CreateImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
        targets.position[i] = ctx.CreateImage(w, h, VK_FORMAT_R32G32B32A32_SFLOAT);
        targets.historyColor[i] = ctx.CreateImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
        targets.historyMoments[i] = ctx.CreateImage(w, h, VK_FORMAT_R32G32B32A32_SFLOAT);
        targets.filter[i] = ctx.CreateImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT);
    }
    for (FrameSlot& slot : slots)
        slot.readback = ctx.CreateBuffer(static_cast<VkDeviceSize>(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, vk::MemoryKind::Readback);

    // UNDEFINED -> GENERAL and clear everything to zero.
    VkCommandBuffer c = ctx.BeginOneShot();
    std::vector<VkImage> all = {targets.noisy.image, targets.albedo.image, targets.emission.image, targets.guide.image, targets.output.image};
    for (int i = 0; i < 2; ++i)
        for (const vk::Image* img : {&targets.normal[i], &targets.position[i], &targets.historyColor[i], &targets.historyMoments[i], &targets.filter[i]})
            all.push_back(img->image);
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
    auto info = [](const vk::Image& img) { return VkDescriptorImageInfo{VK_NULL_HANDLE, img.view, VK_IMAGE_LAYOUT_GENERAL}; };
    const VkDescriptorImageInfo singles[5] = {info(targets.noisy), info(targets.albedo), info(targets.emission), info(targets.output),
                                              info(targets.guide)};
    const VkDescriptorImageInfo pairs[5][2] = {{info(targets.normal[0]), info(targets.normal[1])},
                                               {info(targets.position[0]), info(targets.position[1])},
                                               {info(targets.historyColor[0]), info(targets.historyColor[1])},
                                               {info(targets.historyMoments[0]), info(targets.historyMoments[1])},
                                               {info(targets.filter[0]), info(targets.filter[1])}};
    VkWriteDescriptorSet writes[10] = {};
    const uint32_t singleBindings[5] = {6, 7, 8, 14, 15};
    for (int i = 0; i < 5; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, singleBindings[i], 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        writes[i].pImageInfo = &singles[i];
    }
    for (int i = 0; i < 5; ++i) {
        writes[5 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, static_cast<uint32_t>(9 + i), 0, 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
        writes[5 + i].pImageInfo = pairs[i];
    }
    vkUpdateDescriptorSets(ctx.device, 10, writes, 0, nullptr);
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

bool PathTracer::Init(const std::filesystem::path& shaderDirectory, const MeshLibrary& meshes) {
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
    for (FrameSlot& slot : m->slots) m->CreateSlot(slot);
    m->ready = true;
    m->stats.available = true;
    Log::Info("Path tracer ready on {} ({} frames in flight)", m->ctx.deviceName, kFramesInFlight);
    return true;
}

void PathTracer::Shutdown() {
    if (!m->ctx.Valid()) return;
    vkDeviceWaitIdle(m->ctx.device);
    VkDevice dev = m->ctx.device;
    m->DestroyTargets();
    for (FrameSlot& slot : m->slots) m->DestroySlot(slot);
    for (auto& as : m->blas) m->ctx.DestroyAccelerationStructure(as);
    m->blas.clear();
    for (vk::Buffer* b : {&m->vertices, &m->indices, &m->meshTable}) m->ctx.DestroyBuffer(*b);
    for (VkPipeline& p : m->pipelines) {
        if (p) vkDestroyPipeline(dev, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    if (m->pipelineLayout) vkDestroyPipelineLayout(dev, m->pipelineLayout, nullptr);
    if (m->setLayout) vkDestroyDescriptorSetLayout(dev, m->setLayout, nullptr);
    if (m->descriptorPool) vkDestroyDescriptorPool(dev, m->descriptorPool, nullptr);
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

    // ---- Instances (TLAS input + materials) -------------------------------------------------------
    s.EnsureInstanceCapacity(*slot, static_cast<uint32_t>(commands.size()));
    auto* vkInstances = static_cast<VkAccelerationStructureInstanceKHR*>(slot->instanceBuffer.mapped);
    auto* gpuInstances = static_cast<GpuInstance*>(slot->instanceData.mapped);
    Hasher materialHash;
    s.currentModels.clear();
    uint32_t count = 0;
    for (const DrawCommand& c : commands) {
        if (c.mesh >= s.blas.size() || !s.blas[c.mesh].handle) continue;
        VkAccelerationStructureInstanceKHR inst{};
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 4; ++col) inst.transform.matrix[r][col] = c.model[col][r];
        inst.instanceCustomIndex = count;
        inst.mask = 0xFF;
        inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        inst.accelerationStructureReference = s.blas[c.mesh].address;
        vkInstances[count] = inst;

        // Build locally and only *write* to the mapped buffer: it is write-combined memory, and
        // reading it back (e.g. for hashing) costs tens of milliseconds per frame.
        const Material& mat = c.material;
        const GpuInstance gi{glm::vec4(mat.albedo, mat.metallic), glm::vec4(mat.emissive, mat.roughness),
                             glm::vec4(mat.checkerScale, 0.0f, 0.0f, 0.0f), glm::uvec4(c.mesh, 0u, 0u, 0u)};
        gpuInstances[count] = gi;
        materialHash.Add(gi);
        s.currentModels.push_back(c.model);
        ++count;
    }
    s.stats.instances = count;
    if (count == 0) return s.hasResult ? s.glTexture : 0;

    // ---- Static detection: compare against the snapshot taken when accumulation started ----------
    const float aspect = static_cast<float>(outputWidth) / static_cast<float>(outputHeight);
    const glm::mat4 viewProj = camera.Projection(aspect) * camera.View();
    Hasher settingsHash;
    settingsHash.Add(lighting.sunDirection);
    settingsHash.Add(lighting.sunColor);
    settingsHash.Add(lighting.sunIntensity);
    settingsHash.Add(lighting.skyZenith);
    settingsHash.Add(lighting.skyHorizon);
    settingsHash.Add(lighting.groundColor);
    settingsHash.Add(settings.maxBounces);
    settingsHash.Add(settings.samplesPerPixel);
    settingsHash.Add(settings.sunAngularDiameterDeg);
    settingsHash.Add(settings.skyIntensity);
    settingsHash.Add(settings.fireflyClamp);

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
    ubo.cameraPos = glm::vec4(camera.position, 1.0f);
    const float halfAngle = glm::radians(std::max(settings.sunAngularDiameterDeg, 0.01f) * 0.5f);
    ubo.sunDir = glm::vec4(glm::normalize(lighting.sunDirection), std::cos(halfAngle));
    ubo.sunColor = glm::vec4(lighting.sunColor * lighting.sunIntensity, 0.0f);
    ubo.skyZenith = glm::vec4(lighting.skyZenith, settings.skyIntensity);
    ubo.skyHorizon = glm::vec4(lighting.skyHorizon, lighting.exposure * settings.exposure);
    ubo.groundColor = glm::vec4(lighting.groundColor, lighting.fogDensity);
    const uint32_t flags = (isStatic ? kFlagStatic : 0u) | (reset ? kFlagReset : 0u) | (settings.accumulate ? kFlagAccumulate : 0u);
    ubo.params0 = glm::uvec4(s.frameIndex, static_cast<uint32_t>(std::clamp(settings.maxBounces, 0, 16)),
                             static_cast<uint32_t>(std::clamp(settings.samplesPerPixel, 1, 64)), flags);
    ubo.params1 = glm::vec4(settings.fireflyClamp, static_cast<float>(std::max(settings.maxHistory, 1)), settings.colorSigma,
                            std::clamp(settings.movingMinAlpha, 0.01f, 1.0f));
    const int iterations = settings.denoise ? std::clamp(settings.atrousIterations, 0, 8) : 0;
    ubo.size = glm::uvec4(w, h, count, static_cast<uint32_t>(iterations));
    std::memcpy(slot->ubo.mapped, &ubo, sizeof(ubo));

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

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipelineLayout, 0, 1, &slot->descriptorSet, 0, nullptr);
    const uint32_t gx = (w + 7) / 8, gy = (h + 7) / 8;
    auto dispatch = [&](Pass pass, glm::ivec4 push) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s.pipelines[pass]);
        vkCmdPushConstants(cmd, s.pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, gx, gy, 1);
        Barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    };
    dispatch(PassTrace, glm::ivec4(0));
    dispatch(PassTemporal, glm::ivec4(0));
    int src = 0;
    for (int i = 0; i < iterations; ++i) {
        dispatch(PassAtrous, glm::ivec4(1 << i, src, src ^ 1, 0));
        src ^= 1;
    }
    dispatch(PassComposite, glm::ivec4(0, src, 0, 0));

    Barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, s.targets.output.image, VK_IMAGE_LAYOUT_GENERAL, slot->readback.buffer, 1, &copy);
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
