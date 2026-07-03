// VK-INTEROP-BRIDGE-1 / T2 -- GlInteropView: the GL-boundary adapter that imports one
// exportable VkImageResource into GL zero-copy (see plan "Strategic reshape"). Used
// ONLY at a GL boundary; deletable the day the boundary moves fully into VK. Imports
// the resource's memory + doneSem[] into GL and wraps GL's signal/wait onto the shared
// texture (spike tools/vk_gl_interop_spike/main.cpp:472-508 / 572-574 / 622-624).
//
// glInteropViewCreate ASSERTS src->exportable and performs the LUID match (blocker #2)
// before importing: VkPhysicalDeviceIDProperties.deviceLUID (vkGetPhysicalDeviceProperties2)
// vs GL's GL_DEVICE_LUID_EXT (glGetUnsignedBytevEXT). Mismatch -> return false fail-soft.
// If GL LUID query is unavailable, falls back to a single-GPU assert. All fail paths
// return false; nothing crashes. Whole TU is #ifdef MC2_VULKAN_ISLAND.

#ifndef MC2_GL_INTEROP_VIEW_H
#define MC2_GL_INTEROP_VIEW_H

#ifdef MC2_VULKAN_ISLAND

#include "vk_image_resource.h"
#include <GL/glew.h>
#include <cstdint>

struct GlInteropView {
    GLuint glTex = 0, glMemObj = 0;
    // doneSem[] imported into GL: one GL semaphore per VkImageResource slot for the
    // GL->VK direction (GL signals it via glInteropSignalToVk; the VK submit waits it).
    GLuint glDoneSem[2] = {0, 0};
    // P1.5: the VK->GL semaphores (vkToGlSem[]) imported into GL. The VK submit signals
    // them; GL WAITS them (glInteropWaitFromVkGpu) -- a GPU hand-off replacing the CPU
    // fence wait. vkToGlImported=false if the import failed -> caller falls back to the
    // fence-wait path (fail-soft; do NOT crash).
    GLuint glVkToGlSem[2] = {0, 0};
    bool   vkToGlImported = false;
    GLenum glInternalFormat = 0;
    const VkImageResource* src = nullptr;
};

// Import an exportable VkImageResource's memory + doneSem[] into GL (spike 472-508).
// Requires src->exportable (asserted). Runs the LUID match (blocker #2); needs the
// resource's VkPhysicalDevice to query the VK LUID. Returns false fail-soft on any
// mismatch / missing GL extension / GL error.
bool glInteropViewCreate(GlInteropView* v, const VkImageResource* exportableSrc,
                         VkPhysicalDevice phys, GLenum glInternalFormat);

void glInteropViewDestroy(GlInteropView* v);

// GL hands the shared image to VK (spike 622-624). glDstLayout = GL_LAYOUT_*_EXT the
// VK consumer will treat as the barrier oldLayout. `parity` picks the slot.
void glInteropSignalToVk(const GlInteropView& v, GLenum glDstLayout, uint32_t parity);

// GL waits for the VK producer before touching the tex again (spike 572-574).
// glSrcLayout = the GL_LAYOUT_*_EXT the VK producer left the image in.
//
// P1.5: this now waits the VK->GL semaphore (glVkToGlSem[]) that the VK submit SIGNALS
// -- a GPU-side wait (glWaitSemaphoreEXT), NOT a CPU fence wait. Requires
// v.vkToGlImported (import succeeded). If the VK->GL semaphore was NOT imported the
// caller must fall back to the CPU fence-wait path; this function then does nothing.
void glInteropWaitFromVk(const GlInteropView& v, GLenum glSrcLayout, uint32_t parity);

// -----------------------------------------------------------------------------
// Self-check (plan T1/T2 proof). Creates one exportable RGBA16F VkImageResource,
// builds a GlInteropView over it (LUID match runs), does one signal/wait round with
// a dummy layout, then destroys both. Returns true iff validation-clean and no GL/VK
// error. Uses the caller's device+phys, which MUST have the external-memory/-semaphore
// Win32 device extensions enabled (T3 adds them to the island device). Fail-soft:
// missing exts -> returns false, never crashes.
bool vkInteropSelfTest(VkDevice device, VkPhysicalDevice phys);

// Standalone variant that spins up its OWN ext-enabled Vulkan instance+device so the
// self-check can run BEFORE the island device carries the external exts (T3). Wired
// behind MC2_VK_INTEROP_SELFTEST at island init. Needs a current GL context on the
// calling thread (island frame-loop thread). Returns true iff clean.
bool vkInteropSelfTestStandalone();

#endif // MC2_VULKAN_ISLAND
#endif // MC2_GL_INTEROP_VIEW_H
