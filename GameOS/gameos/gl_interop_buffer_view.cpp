// VK-INTEROP-BRIDGE-1 / water-perf STEP 1 -- GlInteropBufferView implementation.
// Transliterated from the validated probe tools/vk_gl_buffer_interop_probe/main.cpp:
// GL import (292-305), GL compute write (307-322), VK GPU-side read of the shared
// buffer (324-385), verify (387-408). GLEW supplies the GL_EXT_* entrypoints.
//
// The self-check uses a TRANSFER read of the shared buffer (vkCmdCopyBuffer shared ->
// host-visible readback) instead of the probe's VK compute read, so gameos needs no
// embedded SPIR-V blob. It proves the same STEP-0 fact: VK reads the GL-written shared
// buffer GPU-side (the SHARED buffer is never CPU-mapped on either API; only the
// separate host-visible readback is).

#include "gl_interop_buffer_view.h"

#ifdef MC2_VULKAN_ISLAND

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

void glog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("[GL_INTEROP_BUFFER_VIEW] %s\n", buf);
    std::fflush(stdout);
}

bool glOk(const char* where) {
    GLenum e; bool bad = false;
    while ((e = glGetError()) != GL_NO_ERROR) { glog("GL error 0x%04X at %s", e, where); bad = true; }
    return !bad;
}

// blocker #2: match the VK physical-device LUID against the GL device LUID (identical
// to gl_interop_view.cpp:35-75). True on match OR when one side can't be queried
// (single-GPU fallback); false only on a proven mismatch.
bool luidMatches(VkPhysicalDevice phys) {
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &idp;
    vkGetPhysicalDeviceProperties2(phys, &p2);

    if (!idp.deviceLUIDValid) {
        glog("VK deviceLUID not valid -> single-GPU assumption (interop still fine).");
        return true;
    }
    if (!GLEW_EXT_memory_object_win32 || glGetUnsignedBytevEXT == nullptr) {
        glog("GL LUID query unavailable -> falling back to single-GPU assert.");
        return true;
    }
    GLubyte glLuid[GL_LUID_SIZE_EXT] = {0};
    glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT, glLuid);
    if (!glOk("glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT)")) {
        glog("GL LUID query errored -> single-GPU assert fallback.");
        return true;
    }
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

bool glInteropBufferViewCreate(GlInteropBufferView* v, const VkGlSharedBuffer* src,
                               VkPhysicalDevice phys) {
    if (!v || !src) return false;
    *v = GlInteropBufferView{};

    assert(src->exportable && "GlInteropBufferView requires an exportable VkGlSharedBuffer");
    if (!src->exportable) { glog("src not exportable -> decline."); return false; }

    if (!GLEW_EXT_memory_object || !GLEW_EXT_memory_object_win32) {
        glog("GL lacks GL_EXT_memory_object[_win32] -> zero-copy import impossible."); return false;
    }
    if (!GLEW_EXT_semaphore || !GLEW_EXT_semaphore_win32) {
        glog("GL lacks GL_EXT_semaphore[_win32] -> cross-API sync impossible."); return false;
    }
    if (!luidMatches(phys)) return false;

    v->src = src;

    // ---- memory object from the imported Win32 handle (probe 293-296) ----
    glCreateMemoryObjectsEXT(1, &v->glMemObj);
    if (!glOk("glCreateMemoryObjectsEXT")) { glInteropBufferViewDestroy(v); return false; }

    GLint dedicated = GL_TRUE;
    glMemoryObjectParameterivEXT(v->glMemObj, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    glOk("glMemoryObjectParameterivEXT");

    // glImportMemoryWin32HandleEXT does NOT take ownership; the driver duplicates the
    // handle. The VkGlSharedBuffer keeps + CloseHandle()s its own.
    glImportMemoryWin32HandleEXT(v->glMemObj, src->size,
                                 GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, src->memWin32);
    if (!glOk("glImportMemoryWin32HandleEXT")) { glInteropBufferViewDestroy(v); return false; }

    // ---- GL buffer backed by the imported memory at offset 0 (probe 297-300) ----
    // This is the BUFFER storage call (glNamedBufferStorageMemEXT), the analog of the
    // image path's glTextureStorageMem2DEXT.
    glCreateBuffers(1, &v->glBuf);
    glNamedBufferStorageMemEXT(v->glBuf, (GLsizeiptr)src->size, v->glMemObj, 0);
    if (!glOk("glNamedBufferStorageMemEXT")) {
        glog("STEP-0 wall: GL cannot back a buffer with imported VK memory.");
        glInteropBufferViewDestroy(v); return false;
    }

    // ---- import both GL->VK glToVkSem[] slots into GL (probe 302-305) ----
    for (int i = 0; i < 2; ++i) {
        glGenSemaphoresEXT(1, &v->glDoneSem[i]);
        glImportSemaphoreWin32HandleEXT(v->glDoneSem[i], GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,
                                        src->glToVkSemWin32[i]);
        if (!glOk("glImportSemaphoreWin32HandleEXT(glToVk)")) { glInteropBufferViewDestroy(v); return false; }
    }

    // ---- P1.5: import both VK->GL vkToGlSem[] slots into GL (fail-soft) ----
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
        glog("imported memory+buffer+2 GL->VK sems +2 VK->GL sems (zero-copy backing established).");
    } else {
        for (int i = 0; i < 2; ++i)
            if (v->glVkToGlSem[i]) { glDeleteSemaphoresEXT(1, &v->glVkToGlSem[i]); v->glVkToGlSem[i] = 0; }
        v->vkToGlImported = false;
        glog("VK->GL semaphore import FAILED -- falling back to CPU fence wait (seam still active).");
    }
    return true;
}

void glInteropBufferViewDestroy(GlInteropBufferView* v) {
    if (!v) return;
    if (v->glBuf)    glDeleteBuffers(1, &v->glBuf);
    if (v->glMemObj) glDeleteMemoryObjectsEXT(1, &v->glMemObj);
    for (int i = 0; i < 2; ++i) {
        if (v->glDoneSem[i])   glDeleteSemaphoresEXT(1, &v->glDoneSem[i]);
        if (v->glVkToGlSem[i]) glDeleteSemaphoresEXT(1, &v->glVkToGlSem[i]);
    }
    *v = GlInteropBufferView{};
}

// GL signals the slot's semaphore to hand the buffer to VK (probe 316-321). A buffer
// has NO layout; glSignalSemaphoreEXT lists the buffer with a GL_NONE "layout".
void glInteropBufferSignalToVk(const GlInteropBufferView& v, uint32_t parity) {
    GLuint buf = v.glBuf;
    GLenum layout = GL_NONE;  // buffers have no layout (probe 318-319)
    glSignalSemaphoreEXT(v.glDoneSem[parity & 1u], 1, &buf, 0, nullptr, &layout);
    glFlush();  // ensure the signal is submitted to the GL server (probe 320)
    glOk("glInteropBufferSignalToVk");
}

// GL waits the slot's VK->GL semaphore before touching the buffer again. GPU-side wait
// (glWaitSemaphoreEXT); requires vkToGlImported (caller must not invoke otherwise).
void glInteropBufferWaitFromVk(const GlInteropBufferView& v, uint32_t parity) {
    GLuint buf = v.glBuf;
    GLenum layout = GL_NONE;
    glWaitSemaphoreEXT(v.glVkToGlSem[parity & 1u], 1, &buf, 0, nullptr, &layout);
    glOk("glInteropBufferWaitFromVk");
}

// =============================================================================
// Self-check (STEP-1 proof) -- analog of vkInteropSelfTest.
// =============================================================================
namespace {

const uint32_t kSelfCount = 4096;                 // elements (uint)
const VkDeviceSize kSelfBytes = (VkDeviceSize)kSelfCount * 4;
uint32_t selfExpected(uint32_t i) { return i * 7u + 3u; }   // MUST match GL_WRITE_SRC

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

// probe main.cpp:241-246 -- GL compute writes sh.v[i] = i*7+3.
const char* GL_WRITE_SRC =
    "#version 430\n"
    "layout(local_size_x=64) in;\n"
    "layout(std430, binding=0) buffer Shared { uint v[]; } sh;\n"
    "uniform uint uCount;\n"
    "void main(){ uint i=gl_GlobalInvocationID.x; if(i>=uCount) return; sh.v[i]=i*7u+3u; }\n";

GLuint glMakeCompute(const char* src) {
    GLuint sh = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(sh, 1, &src, nullptr); glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(sh, 2048, nullptr, log);
        glog("compute compile fail: %s", log); glDeleteShader(sh); return 0; }
    GLuint pr = glCreateProgram(); glAttachShader(pr, sh); glLinkProgram(pr);
    glGetProgramiv(pr, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(pr, 2048, nullptr, log);
        glog("link fail: %s", log); glDeleteShader(sh); glDeleteProgram(pr); return 0; }
    glDeleteShader(sh); return pr;
}

// The shared body: given a device+phys+queue with external exts, run the create/import/
// GL-write/VK-read/verify/destroy round on the calling thread's GL context. VK reads the
// shared buffer GPU-side via a TRANSFER copy into a host-visible readback (no compute /
// no SPIR-V). Returns true iff pattern matched AND clean.
bool runSelfTestBody(VkDevice device, VkPhysicalDevice phys, VkQueue queue, uint32_t qfam) {
    glGetError();  // clear stale GL error state

    // -- STEP 3: VK allocates the exportable shared buffer (STORAGE for GL SSBO write,
    //    TRANSFER_SRC so VK can copy it out GPU-side). --
    VkGlSharedBuffer shared{};
    if (!vkGlSharedBufferCreate(&shared, device, phys, kSelfBytes,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                /*exportable*/ true)) {
        glog("selftest: vkGlSharedBufferCreate(exportable) FAILED.");
        return false;
    }

    // -- STEP 4: GL imports the shared buffer. --
    GlInteropBufferView view{};
    if (!glInteropBufferViewCreate(&view, &shared, phys)) {
        glog("selftest: glInteropBufferViewCreate FAILED (see prior log for LUID/ext cause).");
        vkGlSharedBufferDestroy(&shared, device);
        return false;
    }

    bool pass = false;
    VkBuffer rbBuf = VK_NULL_HANDLE; VkDeviceMemory rbMem = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    GLuint prog = 0;

    do {
        // -- STEP 5: GL compute WRITES the known pattern into the shared buffer, then
        //    signals the GL->VK semaphore (GPU hand-off). --
        prog = glMakeCompute(GL_WRITE_SRC);
        if (!prog) { glog("selftest: GL compute build failed."); break; }
        glUseProgram(prog);
        glUniform1ui(glGetUniformLocation(prog, "uCount"), kSelfCount);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, view.glBuf);
        glDispatchCompute((kSelfCount + 63) / 64, 1, 1);
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        if (!glOk("selftest GL compute write")) break;
        VkFrameSync fs = vkGlSharedBufferBeginFrame(&shared);
        glInteropBufferSignalToVk(view, fs.parity);
        if (!glOk("selftest GL signal")) break;

        // -- STEP 6: VK host-visible readback buffer + copy shared -> readback GPU-side,
        //    the VK submit WAITS the GL->VK semaphore first. --
        {
            VkBufferCreateInfo r{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            r.size = kSelfBytes; r.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            r.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &r, nullptr, &rbBuf) != VK_SUCCESS) { glog("selftest: readback buffer failed."); break; }
            VkMemoryRequirements rr{}; vkGetBufferMemoryRequirements(device, rbBuf, &rr);
            VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
            uint32_t idx = UINT32_MAX;
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
                if ((rr.memoryTypeBits & (1u << i)) &&
                    (mp.memoryTypes[i].propertyFlags &
                     (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { idx = i; break; }
            if (idx == UINT32_MAX) { glog("selftest: no host-visible memtype."); break; }
            VkMemoryAllocateInfo m{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            m.allocationSize = rr.size; m.memoryTypeIndex = idx;
            if (vkAllocateMemory(device, &m, nullptr, &rbMem) != VK_SUCCESS) { glog("selftest: readback alloc failed."); break; }
            if (vkBindBufferMemory(device, rbBuf, rbMem, 0) != VK_SUCCESS) { glog("selftest: readback bind failed."); break; }
        }

        VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpi.queueFamilyIndex = qfam; cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(device, &cpi, nullptr, &pool) != VK_SUCCESS) { glog("selftest: cmd pool failed."); break; }
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        if (vkAllocateCommandBuffers(device, &cbai, &cb) != VK_SUCCESS) { glog("selftest: alloc cb failed."); break; }
        VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb, &cbbi);
        VkBufferCopy region{0, 0, kSelfBytes};
        vkCmdCopyBuffer(cb, shared.buffer, rbBuf, 1, &region);
        vkEndCommandBuffer(cb);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &fs.signal; si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(device, &fci, nullptr, &fence);
        if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) { glog("selftest: vkQueueSubmit failed."); break; }
        if (vkWaitForFences(device, 1, &fence, VK_TRUE, 5ull * 1000 * 1000 * 1000) != VK_SUCCESS) {
            glog("selftest: vkWaitForFences timed out."); break;
        }

        // -- STEP 7: verify the readback (NOT the shared buffer) matches the GL pattern. --
        void* mp = nullptr;
        if (vkMapMemory(device, rbMem, 0, kSelfBytes, 0, &mp) != VK_SUCCESS) { glog("selftest: map readback failed."); break; }
        const uint32_t* got = (const uint32_t*)mp;
        uint32_t mism = 0, firstBad = 0xffffffffu;
        for (uint32_t i = 0; i < kSelfCount; ++i)
            if (got[i] != selfExpected(i)) { if (mism == 0) firstBad = i; ++mism; }
        glog("selftest verify: mismatches=%u sample[0]=%u(exp %u) sample[N-1]=%u(exp %u) firstBad=%d",
             mism, got[0], selfExpected(0), got[kSelfCount - 1], selfExpected(kSelfCount - 1), (int)firstBad);
        vkUnmapMemory(device, rbMem);
        pass = (mism == 0);
    } while (false);

    // teardown
    if (prog) { glUseProgram(0); glDeleteProgram(prog); }
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (pool)  vkDestroyCommandPool(device, pool, nullptr);
    if (rbBuf) vkDestroyBuffer(device, rbBuf, nullptr);
    if (rbMem) vkFreeMemory(device, rbMem, nullptr);
    glInteropBufferViewDestroy(&view);
    vkGlSharedBufferDestroy(&shared, device);

    bool glClean = glOk("selftest teardown");
    return pass && glClean;
}

} // namespace

bool vkGlSharedBufferSelfTest(VkDevice device, VkPhysicalDevice phys) {
    glog("selftest (caller device): begin.");
    // Caller-device variant needs a queue + family; find a compute/transfer-capable one.
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs.data());
    uint32_t qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qfCount; ++i)
        if (qfs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) { qfam = i; break; }
    if (qfam == UINT32_MAX) { glog("selftest: no usable queue family."); return false; }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, qfam, 0, &queue);
    if (!queue) { glog("selftest: vkGetDeviceQueue returned null (family not created on this device?)."); return false; }
    bool ok = runSelfTestBody(device, phys, queue, qfam);
    glog("selftest (caller device): %s.", ok ? "PASS" : "FAIL");
    return ok;
}

bool vkGlSharedBufferSelfTestStandalone() {
    glog("selftest (standalone ext-enabled device): begin.");
    g_selftestValErrors = 0;

    if (volkInitialize() != VK_SUCCESS) { glog("volkInitialize failed."); return false; }

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
    ai.pApplicationName = "mc2 vk-gl shared-buffer selftest";
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

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs.data());
    uint32_t qfam = UINT32_MAX;
    for (uint32_t i = 0; i < qfCount; ++i)
        if (qfs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { qfam = i; break; }
    if (qfam == UINT32_MAX) {
        glog("no graphics/compute queue family.");
        if (messenger) vkDestroyDebugUtilsMessengerEXT(inst, messenger, nullptr);
        vkDestroyInstance(inst, nullptr); return false;
    }

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
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, qfam, 0, &queue);
        bool body = runSelfTestBody(device, phys, queue, qfam);
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
