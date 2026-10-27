// GameOS/gameos/vk_device_profile.cpp -- reads the capabilities of a real physical device into profile::DeviceCaps (see vk_device_profile.h for the table
// and the verdict). Inert unless MC2_VULKAN is defined.
#ifdef MC2_VULKAN

#include "vk_device_profile.h"

#include <volk.h>

namespace mc2vk::profile {

DeviceCaps gather(VkPhysicalDevice pd, uint32_t instanceApi) {
    DeviceCaps c;
    c.instanceApi = instanceApi;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(pd, &props);
    c.deviceApi = props.apiVersion;
    c.maxImageDimension2D = props.limits.maxImageDimension2D;
    c.maxBoundDescriptorSets = props.limits.maxBoundDescriptorSets;
    c.maxPushConstantsSize = props.limits.maxPushConstantsSize;
    c.maxStorageBufferRange = props.limits.maxStorageBufferRange;

    // A core 1.0 feature: read before the version early-out below so a pre-1.2 device is reported with the bit it really has.
    VkPhysicalDeviceFeatures core10{};
    vkGetPhysicalDeviceFeatures(pd, &core10);
    c.independentBlend = core10.independentBlend == VK_TRUE;
    c.samplerAnisotropy = core10.samplerAnisotropy == VK_TRUE;
    c.shaderSampledImageArrayDynamicIndexing = core10.shaderSampledImageArrayDynamicIndexing == VK_TRUE;

    if (c.usableApi() >= makeApi(1, 1)) {             // physical-device properties2 is core from 1.1
        VkPhysicalDeviceIDProperties id{};
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 props2{};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &id;
        vkGetPhysicalDeviceProperties2(pd, &props2);
        c.deviceLuidValid = id.deviceLUIDValid == VK_TRUE;
        for (size_t i = 0; i < sizeof(c.deviceLuid); ++i) c.deviceLuid[i] = id.deviceLUID[i];
    }

    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> have(extCount);
    if (extCount) vkEnumerateDeviceExtensionProperties(pd, nullptr, &extCount, have.data());
    for (const VkExtensionProperties& e : have) c.extensions.emplace_back(e.extensionName);

    // The feature structs are only valid to chain at the versions / with the extensions that define them.
    if (c.usableApi() < kFloorApi) return c;

    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr{};
    dr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR;
    VkPhysicalDeviceSynchronization2FeaturesKHR s2{};
    s2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR;

    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    void** tail = &f2.pNext;
    auto chain = [&](auto& s) { *tail = &s; tail = &s.pNext; };
    chain(f12);
    const bool core13 = c.usableApi() >= kUseApi13;
    if (core13) {
        chain(f13);
    } else {
        if (c.hasExtension(kExtDynamicRendering)) chain(dr);
        if (c.hasExtension(kExtSynchronization2)) chain(s2);
    }
    vkGetPhysicalDeviceFeatures2(pd, &f2);

    c.timelineSemaphore = f12.timelineSemaphore == VK_TRUE;
    c.bufferDeviceAddress = f12.bufferDeviceAddress == VK_TRUE;
    c.descriptorIndexing = f12.descriptorIndexing == VK_TRUE;
    c.runtimeDescriptorArray = f12.runtimeDescriptorArray == VK_TRUE;
    c.shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE;
    c.descriptorBindingPartiallyBound = f12.descriptorBindingPartiallyBound == VK_TRUE;
    c.dynamicRendering = (core13 ? f13.dynamicRendering : dr.dynamicRendering) == VK_TRUE;
    c.synchronization2 = (core13 ? f13.synchronization2 : s2.synchronization2) == VK_TRUE;
    return c;
}

}  // namespace mc2vk::profile

#endif  // MC2_VULKAN
