// VK-INTEROP-BRIDGE-1 / water-perf STEP 1 -- GlInteropBufferView: the GL-boundary
// adapter that imports one exportable VkGlSharedBuffer into GL zero-copy as a GL buffer
// object. Analog of GlInteropView (which imports an image as a GL texture). Extracted
// from the validated probe tools/vk_gl_buffer_interop_probe/main.cpp:292-322: the
// glCreateMemoryObjectsEXT + glImportMemoryWin32HandleEXT + glNamedBufferStorageMemEXT
// import (the BUFFER storage call, analog of the image's glTextureStorageMem2DEXT),
// plus the GL->VK glSignalSemaphoreEXT hand-off with a buffer (no image layout).
//
// glInteropBufferViewCreate ASSERTS src->exportable and runs the LUID match (blocker #2)
// before importing. Mismatch / missing GL ext / GL error -> return false fail-soft;
// nothing crashes. Whole TU is #ifdef MC2_VULKAN_ISLAND.

#ifndef MC2_GL_INTEROP_BUFFER_VIEW_H
#define MC2_GL_INTEROP_BUFFER_VIEW_H

#ifdef MC2_VULKAN_ISLAND

#include "vk_gl_shared_buffer.h"
#include <GL/glew.h>
#include <cstdint>

struct GlInteropBufferView {
    GLuint glBuf = 0, glMemObj = 0;
    // GL->VK direction: GL signals via glInteropBufferSignalToVk; the VK submit waits.
    GLuint glDoneSem[2] = {0, 0};
    // P1.5: VK->GL semaphores imported into GL. The VK submit signals them; GL WAITS
    // them (glInteropBufferWaitFromVk) -- a GPU hand-off. false if the import failed
    // (caller falls back to a CPU fence wait; fail-soft).
    GLuint glVkToGlSem[2] = {0, 0};
    bool   vkToGlImported = false;
    const VkGlSharedBuffer* src = nullptr;
};

// Import an exportable VkGlSharedBuffer's memory + both semaphore slots into GL as a GL
// buffer object (probe 292-305). Requires src->exportable (asserted). Runs the LUID
// match; needs the resource's VkPhysicalDevice to query the VK LUID. Returns false
// fail-soft on any mismatch / missing GL extension / GL error.
bool glInteropBufferViewCreate(GlInteropBufferView* v, const VkGlSharedBuffer* exportableSrc,
                               VkPhysicalDevice phys);

void glInteropBufferViewDestroy(GlInteropBufferView* v);

// GL hands the shared buffer to VK (probe 316-321). A buffer has NO layout, so the
// glSignalSemaphoreEXT call lists the buffer with a GL_NONE "layout". `parity` picks
// the slot.
void glInteropBufferSignalToVk(const GlInteropBufferView& v, uint32_t parity);

// GL waits for the VK producer before touching the buffer again. Waits glVkToGlSem[]
// (the sem the VK submit SIGNALS) -- a GPU-side wait, no CPU fence stall. Requires
// v.vkToGlImported; if the VK->GL semaphore was NOT imported the caller must fall back
// to the CPU fence-wait path and this function does nothing.
void glInteropBufferWaitFromVk(const GlInteropBufferView& v, uint32_t parity);

// -----------------------------------------------------------------------------
// Self-check (STEP-1 proof, analog of vkInteropSelfTest). Creates one exportable
// VkGlSharedBuffer, a GlInteropBufferView over it (LUID match runs), has GL compute
// WRITE a known pattern into the imported buffer, then VK reads the shared buffer
// GPU-side (a vkCmdCopyBuffer shared->host-visible readback -- the SHARED buffer is
// never CPU-read on either API) and asserts the pattern matches, validation-clean,
// then destroys both. Returns true iff the pattern matched AND validation-clean AND no
// GL/VK error. Uses the caller's device+phys (external exts required). Fail-soft.
bool vkGlSharedBufferSelfTest(VkDevice device, VkPhysicalDevice phys);

// Standalone variant that spins up its OWN ext-enabled Vulkan instance+device (analog
// of vkInteropSelfTestStandalone) so the self-check runs on the real shared-device path
// without the island device having to carry the external exts. Needs a current GL
// context on the calling thread. Returns true iff clean. Wired behind
// MC2_VK_SHARED_BUFFER_SELFTEST at island init.
bool vkGlSharedBufferSelfTestStandalone();

#endif // MC2_VULKAN_ISLAND
#endif // MC2_GL_INTEROP_BUFFER_VIEW_H
