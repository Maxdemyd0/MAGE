// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (C) 2026 Maxdemyd0 and MAGE contributors
#include "raytracing/VulkanContext.h"
#include "core/Log.h"

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

namespace mage::vk {

namespace {
const char* kDeviceExtensions[] = {
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_QUERY_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
};

bool HasExtensions(VkPhysicalDevice pd) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> props(count);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, props.data());
    for (const char* wanted : kDeviceExtensions) {
        bool found = false;
        for (const auto& p : props)
            if (std::strcmp(p.extensionName, wanted) == 0) { found = true; break; }
        if (!found) return false;
    }
    return true;
}
} // namespace

bool Context::Init(std::string& error) {
    if (volkInitialize() != VK_SUCCESS) {
        error = "Vulkan loader (vulkan-1) not found";
        return false;
    }
    const uint32_t loaderVersion = volkGetInstanceVersion();
    if (loaderVersion < VK_API_VERSION_1_2) {
        error = "Vulkan 1.2+ loader required";
        return false;
    }

    // Optional validation (only if the layer is installed and MAGE_VK_VALIDATION is set).
    std::vector<const char*> layers;
    if (std::getenv("MAGE_VK_VALIDATION")) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> available(n);
        vkEnumerateInstanceLayerProperties(&n, available.data());
        for (const auto& l : available)
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "MAGE";
    app.pEngineName = "MAGE";
    app.engineVersion = VK_MAKE_API_VERSION(0, 1, 0, 0);
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        error = "vkCreateInstance failed";
        return false;
    }
    volkLoadInstance(instance);

    // Pick a GPU with hardware ray queries (prefer discrete).
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    int bestScore = -1;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_2 || !HasExtensions(pd)) continue;

        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq};
        VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &as};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        if (!rq.rayQuery || !as.accelerationStructure || !v12.bufferDeviceAddress || !v12.shaderSampledImageArrayNonUniformIndexing ||
            props.limits.maxPerStageDescriptorSamplers < 1100 || props.limits.maxPerStageDescriptorSampledImages < 1100 ||
            !f2.features.shaderStorageImageArrayDynamicIndexing || !f2.features.shaderSampledImageArrayDynamicIndexing)
            continue;

        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfs.data());
        int family = -1;
        for (uint32_t i = 0; i < qfCount; ++i)
            if ((qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && qfs[i].timestampValidBits > 0) { family = static_cast<int>(i); break; }
        if (family < 0) continue;

        const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > bestScore) {
            bestScore = score;
            physicalDevice = pd;
            queueFamily = static_cast<uint32_t>(family);
            deviceName = props.deviceName;
            timestampPeriodNs = props.limits.timestampPeriod;
        }
    }
    if (!physicalDevice) {
        error = "no GPU with VK_KHR_ray_query + VK_KHR_acceleration_structure found";
        Shutdown();
        return false;
    }

    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &asProps};
    vkGetPhysicalDeviceProperties2(physicalDevice, &props2);
    scratchAlignment = asProps.minAccelerationStructureScratchOffsetAlignment;

    // Device with ray query + acceleration structures + buffer device addresses.
    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rq.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq};
    as.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &as};
    v12.bufferDeviceAddress = VK_TRUE;
    v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12};
    f2.features.shaderStorageImageArrayDynamicIndexing = VK_TRUE;
    f2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    f2.features.shaderInt64 = VK_FALSE;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(std::size(kDeviceExtensions));
    dci.ppEnabledExtensionNames = kDeviceExtensions;
    if (vkCreateDevice(physicalDevice, &dci, nullptr, &device) != VK_SUCCESS) {
        error = "vkCreateDevice failed";
        Shutdown();
        return false;
    }
    volkLoadDevice(device);
    vkGetDeviceQueue(device, queueFamily, 0, &queue);

    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    aci.physicalDevice = physicalDevice;
    aci.device = device;
    aci.instance = instance;
    aci.pVulkanFunctions = &functions;
    aci.vulkanApiVersion = VK_API_VERSION_1_2;
    if (vmaCreateAllocator(&aci, &allocator) != VK_SUCCESS) {
        error = "vmaCreateAllocator failed";
        Shutdown();
        return false;
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    vkCreateCommandPool(device, &pci, nullptr, &commandPool);

    Log::Info("Vulkan ray tracing device: {} (scratch alignment {}){}", deviceName, scratchAlignment,
              layers.empty() ? "" : " [validation]");
    return true;
}

void Context::Shutdown() {
    if (device) {
        vkDeviceWaitIdle(device);
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
        if (allocator) vmaDestroyAllocator(allocator);
        vkDestroyDevice(device, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
    commandPool = VK_NULL_HANDLE;
    allocator = nullptr;
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
    physicalDevice = VK_NULL_HANDLE;
}

Buffer Context::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, MemoryKind kind, VkDeviceSize alignment) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (kind == MemoryKind::Upload)
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    else if (kind == MemoryKind::Readback)
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    const VkResult r = alignment > 0 ? vmaCreateBufferWithAlignment(allocator, &bci, &aci, alignment, &b.buffer, &b.allocation, &info)
                                     : vmaCreateBuffer(allocator, &bci, &aci, &b.buffer, &b.allocation, &info);
    if (r != VK_SUCCESS) {
        Log::Error("Vulkan buffer allocation of {} bytes failed ({})", size, static_cast<int>(r));
        return Buffer{};
    }
    b.mapped = info.pMappedData;
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(device, &ai);
    return b;
}

void Context::DestroyBuffer(Buffer& b) {
    if (b.buffer) vmaDestroyBuffer(allocator, b.buffer, b.allocation);
    b = Buffer{};
}

Image Context::CreateTexture(uint32_t width, uint32_t height, uint32_t mips, VkFormat format) {
    Image img;
    img.format = format;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(allocator, &ici, &aci, &img.image, &img.allocation, nullptr) != VK_SUCCESS) {
        Log::Error("Vulkan texture allocation failed ({}x{})", width, height);
        return Image{};
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    vkCreateImageView(device, &vci, nullptr, &img.view);
    return img;
}

Image Context::CreateImage(uint32_t width, uint32_t height, VkFormat format) {
    Image img;
    img.format = format;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vmaCreateImage(allocator, &ici, &aci, &img.image, &img.allocation, nullptr);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device, &vci, nullptr, &img.view);
    return img;
}

void Context::DestroyImage(Image& img) {
    if (img.view) vkDestroyImageView(device, img.view, nullptr);
    if (img.image) vmaDestroyImage(allocator, img.image, img.allocation);
    img = Image{};
}

void Context::DestroyAccelerationStructure(AccelerationStructure& as) {
    if (as.handle) vkDestroyAccelerationStructureKHR(device, as.handle, nullptr);
    DestroyBuffer(as.storage);
    as = AccelerationStructure{};
}

VkCommandBuffer Context::BeginOneShot() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void Context::EndOneShot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkFreeCommandBuffers(device, commandPool, 1, &cmd);
}

} // namespace mage::vk
