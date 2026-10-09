// SPIR-V shader-module load/free helpers shared by the Vulkan present object.
// Compiles to nothing unless MC2_VULKAN is defined.

#ifndef MC2_VULKAN_SPV_LOADER_H
#define MC2_VULKAN_SPV_LOADER_H

#ifdef MC2_VULKAN

// volk defines VK_NO_PROTOTYPES and pulls in the Vulkan headers; all Vulkan entry points
// resolve through it so the exe never hard-links vulkan-1.dll.
#include <volk.h>

// Reads a .spv file and creates a VkShaderModule. Returns VK_NULL_HANDLE (and logs) on any
// failure: null args, missing file, size not a positive multiple of 4, short read, create error.
VkShaderModule mc2_vulkan_load_spv(VkDevice device, const char* path);

// vkDestroyShaderModule; no-op when either handle is VK_NULL_HANDLE.
void mc2_vulkan_free_shader(VkDevice device, VkShaderModule module);

#endif // MC2_VULKAN

#endif // MC2_VULKAN_SPV_LOADER_H
