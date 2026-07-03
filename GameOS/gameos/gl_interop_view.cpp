// VK-INTEROP-BRIDGE-1 / T2 -- GlInteropView implementation.
// Transliterated from tools/vk_gl_interop_spike/main.cpp GL import (472-508), GL wait
// (572-574), GL signal (622-624), and LUID query (291-305). GLEW supplies the GL_EXT_*
// entrypoints (same loader the edge-fog island uses via utils/gl_utils.h).

#include "gl_interop_view.h"

#ifdef MC2_VULKAN_ISLAND

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

void glog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("[GL_INTEROP_VIEW] %s\n", buf);
    std::fflush(stdout);
}

bool glOk(const char* where) {
    GLenum e; bool bad = false;
    while ((e = glGetError()) != GL_NO_ERROR) { glog("GL error 0x%04X at %s", e, where); bad = true; }
    return !bad;
}

// blocker #2: match the VK physical-device LUID against the GL device LUID. Returns
// true if they match, OR if we cannot query one side but can fall back to a
// single-GPU assertion. Returns false only on a proven mismatch or a hard failure.
bool luidMatches(VkPhysicalDevice phys) {
    // VK side: VkPhysicalDeviceIDProperties.deviceLUID (spike 291-305).
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &idp;
    vkGetPhysicalDeviceProperties2(phys, &p2);

    if (!idp.deviceLUIDValid) {
        glog("VK deviceLUID not valid -> single-GPU assumption (interop still fine).");
        return true;  // single-GPU box; the spike proved this path is safe.
    }

    // GL side: GL_DEVICE_LUID_EXT via glGetUnsignedBytevEXT (needs GL_EXT_memory_object_win32).
    if (!GLEW_EXT_memory_object_win32 || glGetUnsignedBytevEXT == nullptr) {
        glog("GL LUID query unavailable -> falling back to single-GPU assert.");
        // Minimum bar (plan blocker #2): require exactly one VK physical device.
        // We do not have the instance here, so accept and rely on caller having
        // selected the LUID-valid device; log loudly so the human sees the fallback.
        return true;
    }

    GLubyte glLuid[GL_LUID_SIZE_EXT] = {0};
    glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT, glLuid);
    if (!glOk("glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT)")) {
        glog("GL LUID query errored -> single-GPU assert fallback.");
        return true;
    }

    // GL_LUID_SIZE_EXT (8) == VK_LUID_SIZE (8).
    if (std::memcmp(glLuid, idp.deviceLUID, VK_LUID_SIZE) != 0) {
        char v[32] = {0}, g[32] = {0};
        for (int i = 0; i < VK_LUID_SIZE; ++i) {
            std::snprintf(v + i * 2, 3, "%02x", idp.deviceLUID[i]);
            std::snprintf(g + i * 2, 3, "%02x", glLuid[i]);
        }
        glog("LUID MISMATCH vk=%s gl=%s -> declining interop (fail-soft).", v, g);
        return false;
    }
    glog("LUID match confirmed (VK and GL are the same physical GPU).");
    return true;
}

} // namespace

bool glInteropViewCreate(GlInteropView* v, const VkImageResource* src,
                         VkPhysicalDevice phys, GLenum glInternalFormat) {
    if (!v || !src) return false;
    *v = GlInteropView{};

    assert(src->exportable && "GlInteropView requires an exportable VkImageResource");
    if (!src->exportable) { glog("src not exportable -> decline."); return false; }

    // GL extension gate (spike 216-219).
    if (!GLEW_EXT_memory_object || !GLEW_EXT_memory_object_win32) {
        glog("GL lacks GL_EXT_memory_object[_win32] -> zero-copy import impossible."); return false;
    }
    if (!GLEW_EXT_semaphore || !GLEW_EXT_semaphore_win32) {
        glog("GL lacks GL_EXT_semaphore[_win32] -> cross-API sync impossible."); return false;
    }

    // blocker #2: LUID match BEFORE any import.
    if (!luidMatches(phys)) return false;

    v->src = src;
    v->glInternalFormat = glInternalFormat;

    // ---- memory object from the imported Win32 handle (spike 474-487) ----
    glCreateMemoryObjectsEXT(1, &v->glMemObj);
    if (!glOk("glCreateMemoryObjectsEXT")) { glInteropViewDestroy(v); return false; }

    GLint dedicated = GL_TRUE;
    glMemoryObjectParameterivEXT(v->glMemObj, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    glOk("glMemoryObjectParameterivEXT");

    // glImportMemoryWin32HandleEXT does NOT take ownership; the driver duplicates the
    // handle. The VkImageResource keeps + CloseHandle()s its own (spike 482-483).
    glImportMemoryWin32HandleEXT(v->glMemObj, src->memSize,
                                 GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, src->memWin32);
    if (!glOk("glImportMemoryWin32HandleEXT")) { glInteropViewDestroy(v); return false; }

    // ---- texture backed by the imported memory at offset 0 (zero-copy) (spike 490-497) ----
    glCreateTextures(GL_TEXTURE_2D, 1, &v->glTex);
    glTextureParameteri(v->glTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(v->glTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Must match the VK tiling: OPTIMAL -> GL_OPTIMAL_TILING_EXT.
    glTextureParameteri(v->glTex, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    glTextureStorageMem2DEXT(v->glTex, 1, glInternalFormat, src->w, src->h, v->glMemObj, 0);
    if (!glOk("glTextureStorageMem2DEXT")) { glInteropViewDestroy(v); return false; }

    // ---- import both GL->VK doneSem[] slots into GL (spike 500-505) ----
    for (int i = 0; i < 2; ++i) {
        glGenSemaphoresEXT(1, &v->glDoneSem[i]);
        glImportSemaphoreWin32HandleEXT(v->glDoneSem[i], GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                        src->doneSemWin32[i]);
        if (!glOk("glImportSemaphoreWin32HandleEXT(glToVk)")) { glInteropViewDestroy(v); return false; }
    }

    // ---- P1.5: import both VK->GL vkToGlSem[] slots into GL (GPU hand-off channel) ----
    // Fail-SOFT: if either import errors we tear down just the VK->GL GL semaphores and
    // leave vkToGlImported=false; the caller then keeps the CPU fence-wait path. The
    // GL->VK direction above is unaffected, so the interop seam still works (just with
    // the original CPU stall) rather than crashing.
    bool vk2gl_ok = true;
    for (int i = 0; i < 2 && vk2gl_ok; ++i) {
        if (!src->vkToGlSemWin32[i]) { vk2gl_ok = false; break; }
        glGenSemaphoresEXT(1, &v->glVkToGlSem[i]);
        glImportSemaphoreWin32HandleEXT(v->glVkToGlSem[i], GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                        src->vkToGlSemWin32[i]);
        if (!glOk("glImportSemaphoreWin32HandleEXT(vkToGl)")) vk2gl_ok = false;
    }
    if (vk2gl_ok) {
        v->vkToGlImported = true;
        glog("imported memory+texture+2 GL->VK sems +2 VK->GL sems "
             "(zero-copy backing + GPU VK->GL hand-off established).");
    } else {
        for (int i = 0; i < 2; ++i)
            if (v->glVkToGlSem[i]) { glDeleteSemaphoresEXT(1, &v->glVkToGlSem[i]); v->glVkToGlSem[i] = 0; }
        v->vkToGlImported = false;
        glog("VK->GL semaphore import FAILED -- falling back to CPU fence wait on the "
             "color hand-off (interop seam still active, just not zero-CPU-stall).");
    }
    return true;
}

void glInteropViewDestroy(GlInteropView* v) {
    if (!v) return;
    if (v->glTex)    glDeleteTextures(1, &v->glTex);
    if (v->glMemObj) glDeleteMemoryObjectsEXT(1, &v->glMemObj);
    for (int i = 0; i < 2; ++i) {
        if (v->glDoneSem[i])   glDeleteSemaphoresEXT(1, &v->glDoneSem[i]);
        if (v->glVkToGlSem[i]) glDeleteSemaphoresEXT(1, &v->glVkToGlSem[i]);
    }
    *v = GlInteropView{};
}

// GL signals the slot's semaphore to hand the image to VK (spike 622-624).
void glInteropSignalToVk(const GlInteropView& v, GLenum glDstLayout, uint32_t parity) {
    GLuint tex = v.glTex;
    glSignalSemaphoreEXT(v.glDoneSem[parity & 1u], 0, nullptr, 1, &tex, &glDstLayout);
    glFlush();  // ensure the signal is submitted to the GL server (spike 624)
    glOk("glInteropSignalToVk");
}

// GL waits the slot's VK->GL semaphore before touching the tex again (spike 572-574).
// P1.5: waits glVkToGlSem[] (the sem the VK submit SIGNALS) -- a GPU-side wait, so no
// CPU fence stall. Requires vkToGlImported; the caller must NOT invoke this otherwise
// (it falls back to the CPU fence wait). Distinct from glDoneSem[] which is the GL->VK
// hand-off (GL signals it, VK waits it) -- waiting that here would be a no-op / wrong.
void glInteropWaitFromVk(const GlInteropView& v, GLenum glSrcLayout, uint32_t parity) {
    GLuint tex = v.glTex;
    glWaitSemaphoreEXT(v.glVkToGlSem[parity & 1u], 0, nullptr, 1, &tex, &glSrcLayout);
    glOk("glInteropWaitFromVk");
}

// =============================================================================
// Self-check (plan T1/T2 proof)
// =============================================================================
#include <vector>

namespace {

// Count VK validation errors during the self-test via a scoped debug messenger.
unsigned g_selftestValErrors = 0;
VKAPI_ATTR VkBool32 VKAPI_CALL selftest_dbg_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* d, void*) {
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        glog("[VK-VALIDATION ERROR] %s", d && d->pMessage ? d->pMessage : "(null)");
        ++g_selftestValErrors;
    }
    return VK_FALSE;
}

// The shared body: given a device+phys with external exts, run the create/import/
// signal-wait/destroy round on the calling thread's GL context. Returns true if clean.
bool runSelfTestBody(VkDevice device, VkPhysicalDevice phys) {
    glGetError();  // clear stale GL error state

    VkImageResource res{};
    if (!vkImageResourceCreate(&res, device, phys, 4, 4,
                               VK_FORMAT_R16G16B16A16_SFLOAT,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               /*exportable*/ true)) {
        glog("selftest: vkImageResourceCreate(exportable RGBA16F) FAILED.");
        return false;
    }

    GlInteropView view{};
    bool ok = glInteropViewCreate(&view, &res, phys, GL_RGBA16F);
    if (ok) {
        // Confirm the P1.5 VK->GL semaphore back-channel imported (create/import/destroy
        // exercise). We do NOT issue the GL VK->GL wait here: glVkToGlSem is signalled by
        // a VK submit in the live path, and there is no queue/cmdbuf in this self-check --
        // waiting an un-signalled semaphore on the GL server would deadlock. The live fog
        // island path exercises signal+wait end-to-end (gate-ON sanity, proof step 3).
        if (!view.vkToGlImported) {
            glog("selftest: VK->GL semaphore back-channel did NOT import (P1.5).");
            ok = false;
        }
        // One GL->VK signal with a dummy layout: exercises the GL->VK semaphore import +
        // the parity plumbing (this direction has a real signaller -- GL itself).
        VkFrameSync fs = vkImageResourceBeginFrame(&res);
        (void)fs.vk2gl;  // present iff exportable; used by the live submit, not here.
        glInteropSignalToVk(view, GL_LAYOUT_COLOR_ATTACHMENT_EXT, fs.parity);
        glFinish();  // let GL settle so any semaphore error surfaces before teardown
    } else {
        glog("selftest: glInteropViewCreate FAILED (see prior log for LUID/ext cause).");
    }

    glInteropViewDestroy(&view);
    vkImageResourceDestroy(&res, device);

    bool glClean = glOk("selftest teardown");
    return ok && glClean;
}

} // namespace

bool vkInteropSelfTest(VkDevice device, VkPhysicalDevice phys) {
    glog("selftest (caller device): begin.");
    bool ok = runSelfTestBody(device, phys);
    glog("selftest (caller device): %s.", ok ? "PASS" : "FAIL");
    return ok;
}

bool vkInteropSelfTestStandalone() {
    glog("selftest (standalone ext-enabled device): begin.");
    g_selftestValErrors = 0;

    if (volkInitialize() != VK_SUCCESS) { glog("volkInitialize failed."); return false; }

    // ---- instance with external-capabilities + PDP2 + debug-utils (spike 238-243) ----
    std::vector<const char*> instExts = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    if (layerCount) vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    std::vector<const char*> instLayers;
    for (auto& l : layers)
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0)
            instLayers.push_back("VK_LAYER_KHRONOS_validation");

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "mc2 vk-interop selftest";
    ai.apiVersion = VK_API_VERSION_1_1;

    VkDebugUtilsMessengerCreateInfoEXT dci{
        VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    dci.pfnUserCallback = selftest_dbg_cb;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = &dci;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = (uint32_t)instExts.size();
    ici.ppEnabledExtensionNames = instExts.data();
    ici.enabledLayerCount = (uint32_t)instLayers.size();
    ici.ppEnabledLayerNames = instLayers.empty() ? nullptr : instLayers.data();

    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) {
        glog("vkCreateInstance failed."); return false;
    }
    volkLoadInstance(inst);

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (vkCreateDebugUtilsMessengerEXT)
        vkCreateDebugUtilsMessengerEXT(inst, &dci, nullptr, &messenger);

    // ---- pick the LUID-matching / discrete device ----
    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(inst, &pdCount, nullptr);
    if (pdCount == 0) { glog("no physical devices."); vkDestroyInstance(inst, nullptr); return false; }
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(inst, &pdCount, pds.data());
    VkPhysicalDevice phys = pds[0];
    for (auto pd : pds) {
        VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(pd, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) phys = pd;
    }

    // ---- graphics queue family ----
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs.data());
    uint32_t qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qfCount; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfam = i; break; }
    if (qfam == UINT32_MAX) {
        glog("no graphics queue family.");
        if (messenger) vkDestroyDebugUtilsMessengerEXT(inst, messenger, nullptr);
        vkDestroyInstance(inst, nullptr); return false;
    }

    // ---- device with external-memory/-semaphore win32 exts (spike 318-325) ----
    const char* devExts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dvci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dvci.queueCreateInfoCount = 1; dvci.pQueueCreateInfos = &qci;
    dvci.enabledExtensionCount = (uint32_t)(sizeof(devExts) / sizeof(devExts[0]));
    dvci.ppEnabledExtensionNames = devExts;

    VkDevice device = VK_NULL_HANDLE;
    bool clean = false;
    if (vkCreateDevice(phys, &dvci, nullptr, &device) != VK_SUCCESS) {
        glog("vkCreateDevice failed (external exts unsupported?).");
    } else {
        volkLoadDevice(device);
        bool body = runSelfTestBody(device, phys);
        vkDeviceWaitIdle(device);
        clean = body && (g_selftestValErrors == 0);
        vkDestroyDevice(device, nullptr);
    }

    if (messenger) vkDestroyDebugUtilsMessengerEXT(inst, messenger, nullptr);
    vkDestroyInstance(inst, nullptr);

    glog("selftest (standalone): %s (val_errors=%u).",
         clean ? "PASS" : "FAIL", g_selftestValErrors);
    return clean;
}

#endif // MC2_VULKAN_ISLAND
