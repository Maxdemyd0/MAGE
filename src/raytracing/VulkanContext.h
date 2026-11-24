// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#pragma once
// MAGE v1 - headless Vulkan 1.2+ compute context with hardware ray tracing
// (VK_KHR_acceleration_structure + VK_KHR_ray_query). Engine-internal: not exported.

#include <volk.h>
#include <vk_mem_alloc.h>

#include <cstdint>
#include <string>

namespace mage::vk {

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

struct AccelerationStructure {
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    Buffer storage;
    VkDeviceAddress address = 0;
};

enum class MemoryKind { GpuOnly, Upload, Readback };

class Context {
public:
    bool Init(std::string& error);
    void Shutdown();
    bool Valid() const { return device != VK_NULL_HANDLE; }

    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryKind kind, VkDeviceSize alignment = 0);
    void DestroyBuffer(Buffer& buffer);
    Image CreateImage(uint32_t width, uint32_t height, VkFormat format);
    Image CreateTexture(uint32_t width, uint32_t height, uint32_t mips, VkFormat format);
    void DestroyImage(Image& image);
    void DestroyAccelerationStructure(AccelerationStructure& as);

    VkCommandBuffer BeginOneShot();
    void EndOneShot(VkCommandBuffer cmd);

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    std::string deviceName;
    float timestampPeriodNs = 1.0f;
    VkDeviceSize scratchAlignment = 256;
};

} // namespace mage::vk
