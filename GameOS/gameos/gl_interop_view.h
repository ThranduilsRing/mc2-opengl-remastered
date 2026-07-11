// VK-INTEROP-BRIDGE-1 / T2 -- GlInteropView: the GL-boundary adapter that imports one
// exportable VkImageResource into GL zero-copy (see plan "Strategic reshape"). Used
// ONLY at a GL boundary; deletable the day the boundary moves fully into VK. Imports
// the resource's memory into GL as a zero-copy texture (spike tools/vk_gl_interop_spike/
// main.cpp:472-508).
//
// glInteropViewCreate ASSERTS src->exportable and performs the LUID match (blocker #2)
// before importing: VkPhysicalDeviceIDProperties.deviceLUID (vkGetPhysicalDeviceProperties2)
// vs GL's GL_DEVICE_LUID_EXT (glGetUnsignedBytevEXT). Mismatch -> return false fail-soft.
// If GL LUID query is unavailable, falls back to a single-GPU assert. All fail paths
// return false; nothing crashes. Whole TU is #ifdef MC2_VULKAN_ISLAND.
//
// ITEM-1 (VK-RUNG8-ITEM1-GL-INTEROP-SIGNALWAIT-DELETE): the GL<->VK semaphore handoff
// functions glInteropSignalToVk / glInteropWaitFromVk and the vkInteropSelfTest* proof
// block are DELETED (zero live callers after item-8 removed the present handoff). The
// adapter is now an IMAGE-ONLY zero-copy importer; the sole surviving caller (item-6
// static shadow) renders + samples the imported depth entirely in GL, no cross-API sync.

#ifndef MC2_GL_INTEROP_VIEW_H
#define MC2_GL_INTEROP_VIEW_H

#ifdef MC2_VULKAN_ISLAND

#include "vk_image_resource.h"
#include <GL/glew.h>
#include <cstdint>

struct GlInteropView {
    GLuint glTex = 0, glMemObj = 0;
    GLenum glInternalFormat = 0;
    const VkImageResource* src = nullptr;
};

// Import an exportable VkImageResource's memory into GL as a zero-copy texture
// (spike 472-508). Requires src->exportable (asserted). Runs the LUID match (blocker
// #2); needs the resource's VkPhysicalDevice to query the VK LUID. Returns false
// fail-soft on any mismatch / missing GL extension / GL error.
bool glInteropViewCreate(GlInteropView* v, const VkImageResource* exportableSrc,
                         VkPhysicalDevice phys, GLenum glInternalFormat);

void glInteropViewDestroy(GlInteropView* v);

#endif // MC2_VULKAN_ISLAND
#endif // MC2_GL_INTEROP_VIEW_H
