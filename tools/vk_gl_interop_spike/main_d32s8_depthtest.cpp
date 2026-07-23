// =============================================================================
// Vulkan DEPTH-TESTS against OpenGL-AUTHORED shared scene depth  — SPIKE
//   VK-TIER1-SHARED-SCENE-DEPTH-RECON.md  section 3 "Smallest de-risk probe".
//
// STOP-GATE for the whole geometry-color endgame (props P11 / terrain P10 /
// mechs P12). The P5-T0 spike (main_d24s8.cpp) proved a VK-built D32S8 image can
// be VK-written and GL-SAMPLED. It NEVER depth-tested, and the write direction
// was VK->GL. This spike proves the true Tier-1 capability, in the REAL engine
// direction (GL produces depth / VK consumes it as a depth ATTACHMENT):
//
//   1. VK builds an exportable D32_SFLOAT_S8_UINT image (proven path).
//   2. GL imports it as GL_DEPTH24_STENCIL8, attaches it to an FBO as the
//      DEPTH-STENCIL attachment, and RENDERS a known reverse-Z depth field into
//      it under glClearDepth(0.0) (far) + glDepthFunc(GL_GEQUAL) — matching the
//      engine's reverse-Z / GEQUAL scene-depth convention. Left half of the
//      image gets a NEAR depth (0.7); right half stays at the FAR clear (0.0).
//   3. GL signals a semaphore; hands the image to VK (depth-attachment layout).
//   4. VK imports the SAME image as a DEPTH-STENCIL ATTACHMENT and does a color
//      draw with depthTestEnable=true, depthWriteEnable=false,
//      depthCompareOp=VK_COMPARE_OP_GREATER_OR_EQUAL, at a fixed VK clip depth
//      0.5. Reverse-Z GEQUAL semantics:
//        - left  half GL depth 0.7 : 0.5 >= 0.7 ? NO  -> fragment KILLED
//        - right half GL depth 0.0 : 0.5 >= 0.0 ? YES -> fragment KEPT (red)
//      A wrong compare-op (LESS) inverts this and the asserts FAIL loudly.
//   5. Readback the VK color result; ASSERT the kept/killed pattern matches the
//      GL-authored depth field. This is the actual new capability.
//   6. Round-trip guards (recon risks):
//        (a) GL re-SAMPLES the DEPTH aspect after the VK depth-test and still
//            reads the exact GL-authored field (42-consumer regression guard).
//        (b) glCopyImageSubData depth->copy on the 64bpp D32S8 works and the
//            copy samples identically (size-vs-format-class hazard).
//        (c) stencil glTextureView over the packed image succeeds.
//        (d) run under Vulkan validation; report zero validation errors.
//
// Exit 0 == VK depth-test against GL-authored depth WORKS + all guards pass +
//           validation clean  -> STOP-gate GREEN.
// Non-zero == which stage/assert failed is printed (STOP-gate RED, a critical
//             finding — report it plainly).
// =============================================================================

#include <windows.h>

#include <GL/glew.h>
#include <GL/wglew.h>

#include <SDL.h>
#include <SDL_syswm.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>

// Embedded SPIR-V (compiled by glslc at build time into braced initializers via
// `glslc -mfmt=c` — see CMakeLists.txt custom commands).
static const uint32_t d32s8_depthtest_vert_spv[] =
    #include "d32s8_depthtest.vert.spv.inc"
;
static const uint32_t d32s8_depthtest_frag_spv[] =
    #include "d32s8_depthtest.frag.spv.inc"
;

// ---- image geometry ---------------------------------------------------------
// Wider than tall so "left half" / "right half" is unambiguous. Small = fast.
static const uint32_t TEX_W = 16;
static const uint32_t TEX_H = 4;

// Reverse-Z convention (matches engine): clear depth 0.0 = FAR, larger = NEARER,
// compare GEQUAL keeps a fragment whose depth >= stored depth.
static const float GL_FAR_CLEAR   = 0.0f;  // GL clears depth to far
static const float GL_NEAR_DEPTH  = 0.7f;  // GL draws left half at this near depth
static const float VK_TEST_DEPTH  = 0.5f;  // VK color draw tested at this depth
// Expected VK outcome under GEQUAL:
//   left half  (stored 0.7): 0.5 >= 0.7 -> KILLED (color stays clear/black)
//   right half (stored 0.0): 0.5 >= 0.0 -> KEPT   (color = red)

// Depth sampling tolerance. D32_SFLOAT stores 0.7 exactly-ish; GL normalized
// read of the DEPTH aspect. 1e-4 is tight but safe against float rounding.
static const float DEPTH_EPS = 1e-4f;

#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL_TEXTURE_MODE
#define GL_DEPTH_STENCIL_TEXTURE_MODE 0x90EA
#endif
#ifndef GL_DEPTH_STENCIL
#define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
#define GL_UNSIGNED_INT_24_8 0x84FA
#endif
#ifndef GL_STENCIL_INDEX
#define GL_STENCIL_INDEX 0x1901
#endif

// ---- logging + verdict tracking --------------------------------------------
static int   g_failStage = 0;
static char  g_failMsg[512] = {0};
static bool  g_glDebugSawError = false;
static int   g_valErrors = 0;

#define LOG(...)  do { std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while(0)

static bool fail(int stage, const char* fmt, ...) {
    g_failStage = stage;
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(g_failMsg, sizeof(g_failMsg), fmt, ap);
    va_end(ap);
    LOG("[FAIL stage %d] %s", stage, g_failMsg);
    return false;
}

static const char* vkStr(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        default: { static char b[32]; std::snprintf(b, 32, "VkResult(%d)", (int)r); return b; }
    }
}

static VKAPI_ATTR VkBool32 VKAPI_CALL vkDbg(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* d, void*)
{
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG("[VK-VALIDATION %s] %s",
            (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : "WARN",
            d->pMessage ? d->pMessage : "(null)");
        if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) g_valErrors++;
    }
    return VK_FALSE;
}

static void APIENTRY glDbg(GLenum, GLenum type, GLuint, GLenum sev,
                           GLsizei, const GLchar* msg, const void*) {
    if (type == GL_DEBUG_TYPE_ERROR || sev == GL_DEBUG_SEVERITY_HIGH) {
        LOG("[GL-DEBUG ERROR] %s", msg ? msg : "(null)");
        g_glDebugSawError = true;
    }
}
static bool glCheck(const char* where) {
    GLenum e; bool bad = false;
    while ((e = glGetError()) != GL_NO_ERROR) { LOG("[GL-ERROR 0x%04X] at %s", e, where); bad = true; }
    return !bad;
}

// -----------------------------------------------------------------------------
struct App {
    SDL_Window*   win = nullptr;
    SDL_GLContext glctx = nullptr;
    VkInstance       inst = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice         dev  = VK_NULL_HANDLE;
    uint32_t         qfam = 0;
    VkQueue          queue = VK_NULL_HANDLE;
    VkCommandPool    cmdPool = VK_NULL_HANDLE;
    PFN_vkGetMemoryWin32HandleKHR    pGetMemWin32 = nullptr;
    PFN_vkGetSemaphoreWin32HandleKHR pGetSemWin32 = nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT  pCreateDbg = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT pDestroyDbg = nullptr;
};

static bool findMemType(App& a, uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t* out) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(a.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) { *out = i; return true; }
    return false;
}

// =============================================================================
// Stage 1: SDL + GL 4.3 core + GLEW + debug.
// =============================================================================
static bool initGL(App& a) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return fail(1, "SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    a.win = SDL_CreateWindow("vk_gl_d32s8_depthtest_spike", SDL_WINDOWPOS_CENTERED,
                             SDL_WINDOWPOS_CENTERED, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!a.win) return fail(1, "SDL_CreateWindow: %s", SDL_GetError());
    a.glctx = SDL_GL_CreateContext(a.win);
    if (!a.glctx) return fail(1, "SDL_GL_CreateContext (need GL4.3 core): %s", SDL_GetError());
    glewExperimental = GL_TRUE;
    GLenum ge = glewInit();
    if (ge != GLEW_OK) return fail(1, "glewInit: %s", (const char*)glewGetErrorString(ge));
    glGetError();
    LOG("[GL] vendor=%s",   (const char*)glGetString(GL_VENDOR));
    LOG("[GL] renderer=%s", (const char*)glGetString(GL_RENDERER));
    LOG("[GL] version=%s",  (const char*)glGetString(GL_VERSION));
    if (GLEW_KHR_debug || GLEW_ARB_debug_output) {
        glEnable(GL_DEBUG_OUTPUT); glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDbg, nullptr);
    }
    if (!GLEW_EXT_memory_object || !GLEW_EXT_memory_object_win32)
        return fail(1, "GL lacks GL_EXT_memory_object[_win32]");
    if (!GLEW_EXT_semaphore || !GLEW_EXT_semaphore_win32)
        return fail(1, "GL lacks GL_EXT_semaphore[_win32]");
    return glCheck("initGL");
}

// =============================================================================
// Stage 2: Vulkan instance + device (external mem/sem + validation).
// =============================================================================
static bool initVk(App& a) {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool haveValidation = false;
    for (auto& l : layers) if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) haveValidation = true;
    LOG("[VK] VK_LAYER_KHRONOS_validation available = %d", haveValidation ? 1 : 0);

    std::vector<const char*> instExts = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };
    std::vector<const char*> instLayers;
    if (haveValidation) instLayers.push_back("VK_LAYER_KHRONOS_validation");

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "vk_gl_d32s8_depthtest_spike";
    ai.apiVersion = VK_API_VERSION_1_2;

    VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dci.pfnUserCallback = vkDbg;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = &dci;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = (uint32_t)instExts.size();
    ici.ppEnabledExtensionNames = instExts.data();
    ici.enabledLayerCount = (uint32_t)instLayers.size();
    ici.ppEnabledLayerNames = instLayers.data();
    if (vkCreateInstance(&ici, nullptr, &a.inst) != VK_SUCCESS) return fail(2, "vkCreateInstance");

    a.pCreateDbg  = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(a.inst, "vkCreateDebugUtilsMessengerEXT");
    a.pDestroyDbg = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(a.inst, "vkDestroyDebugUtilsMessengerEXT");
    if (a.pCreateDbg) a.pCreateDbg(a.inst, &dci, nullptr, &a.messenger);

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(a.inst, &pdCount, nullptr);
    if (pdCount == 0) return fail(2, "no Vulkan physical devices");
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(a.inst, &pdCount, pds.data());
    a.phys = pds[0];
    for (auto pd : pds) {
        VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(pd, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) a.phys = pd;
    }
    { VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(a.phys, &p);
      LOG("[VK] using device: %s", p.deviceName); }

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(a.phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(a.phys, &qfCount, qfs.data());
    bool found = false;
    for (uint32_t i = 0; i < qfCount; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { a.qfam = i; found = true; break; }
    if (!found) return fail(2, "no graphics queue family");

    std::vector<const char*> devExts = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    };
    uint32_t deCount = 0;
    vkEnumerateDeviceExtensionProperties(a.phys, nullptr, &deCount, nullptr);
    std::vector<VkExtensionProperties> des(deCount);
    vkEnumerateDeviceExtensionProperties(a.phys, nullptr, &deCount, des.data());
    for (const char* need : devExts) {
        bool ok = false;
        for (auto& e : des) if (!std::strcmp(e.extensionName, need)) ok = true;
        if (!ok) return fail(2, "device missing required extension: %s", need);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = a.qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dvci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dvci.queueCreateInfoCount = 1; dvci.pQueueCreateInfos = &qci;
    dvci.enabledExtensionCount = (uint32_t)devExts.size();
    dvci.ppEnabledExtensionNames = devExts.data();
    if (vkCreateDevice(a.phys, &dvci, nullptr, &a.dev) != VK_SUCCESS) return fail(2, "vkCreateDevice");
    vkGetDeviceQueue(a.dev, a.qfam, 0, &a.queue);

    a.pGetMemWin32 = (PFN_vkGetMemoryWin32HandleKHR)    vkGetDeviceProcAddr(a.dev, "vkGetMemoryWin32HandleKHR");
    a.pGetSemWin32 = (PFN_vkGetSemaphoreWin32HandleKHR) vkGetDeviceProcAddr(a.dev, "vkGetSemaphoreWin32HandleKHR");
    if (!a.pGetMemWin32 || !a.pGetSemWin32) return fail(2, "Win32 handle entry points null");

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = a.qfam;
    if (vkCreateCommandPool(a.dev, &cpi, nullptr, &a.cmdPool) != VK_SUCCESS) return fail(2, "vkCreateCommandPool");
    return true;
}

// =============================================================================
// Shared D32S8 image resources + both-direction semaphores + a VK color target.
// =============================================================================
struct Shared {
    // GL-produced / VK-consumed shared DEPTH image (D32_SFLOAT_S8_UINT).
    VkImage        depthImg = VK_NULL_HANDLE;
    VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkDeviceSize   depthMemSize = 0;
    HANDLE         depthMemWin32 = nullptr;
    GLuint         glDepthMemObj = 0;
    GLuint         glDepthTex = 0;         // GL_DEPTH24_STENCIL8 view of shared img
    VkImageView    depthView = VK_NULL_HANDLE;

    // GL->VK semaphore (GL signals after authoring depth; VK waits).
    VkSemaphore    semGlToVk = VK_NULL_HANDLE;
    HANDLE         semGlToVkWin32 = nullptr;
    GLuint         glSemGlToVk = 0;
    // VK->GL semaphore (VK signals after depth-test; GL waits before re-sampling).
    VkSemaphore    semVkToGl = VK_NULL_HANDLE;
    HANDLE         semVkToGlWin32 = nullptr;
    GLuint         glSemVkToGl = 0;

    // VK-private color attachment (NOT shared) that the depth-tested draw writes.
    VkImage        colorImg = VK_NULL_HANDLE;
    VkDeviceMemory colorMem = VK_NULL_HANDLE;
    VkImageView    colorView = VK_NULL_HANDLE;
    VkBuffer       readbackBuf = VK_NULL_HANDLE;
    VkDeviceMemory readbackMem = VK_NULL_HANDLE;

    // GL FBO for authoring depth.
    GLuint         glFbo = 0;

    // Copy-path + stencil-view guards.
    GLuint         glDepthCopyTex = 0;     // dst of glCopyImageSubData
    GLuint         glStencilView = 0;      // glTextureView stencil aspect
};

static const VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
static const VkFormat kColorFmt = VK_FORMAT_R8G8B8A8_UNORM;

// Create the exportable shared D32S8 image (usage includes DEPTH_STENCIL_ATTACHMENT
// so VK can bind it as an attachment, SAMPLED so GL/VK can read it).
static bool createSharedDepth(App& a, Shared& s) {
    const VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkExternalMemoryImageCreateInfo emici{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes = ht;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = &emici;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kDepthFmt;
    ci.extent = { TEX_W, TEX_H, 1 };
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT
             | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
             | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(a.dev, &ci, nullptr, &s.depthImg) != VK_SUCCESS)
        return fail(3, "vkCreateImage(shared D32S8)");

    VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(a.dev, s.depthImg, &mr);
    s.depthMemSize = mr.size;
    uint32_t memIdx;
    if (!findMemType(a, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memIdx))
        return fail(3, "no DEVICE_LOCAL mem for shared depth");
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = s.depthImg;
    VkExportMemoryAllocateInfo em{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    em.pNext = &ded; em.handleTypes = ht;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &em; mai.allocationSize = mr.size; mai.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(a.dev, &mai, nullptr, &s.depthMem) != VK_SUCCESS)
        return fail(3, "vkAllocateMemory(shared depth)");
    if (vkBindImageMemory(a.dev, s.depthImg, s.depthMem, 0) != VK_SUCCESS)
        return fail(3, "vkBindImageMemory(shared depth)");
    VkMemoryGetWin32HandleInfoKHR gh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    gh.memory = s.depthMem; gh.handleType = ht;
    if (a.pGetMemWin32(a.dev, &gh, &s.depthMemWin32) != VK_SUCCESS)
        return fail(3, "vkGetMemoryWin32HandleKHR(depth)");
    LOG("[VK] shared D32S8 depth image: memWin32=%p size=%llu",
        s.depthMemWin32, (unsigned long long)s.depthMemSize);

    // Depth-stencil ATTACHMENT view (this is the new capability vs P5-T0).
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = s.depthImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = kDepthFmt;
    vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0,1,0,1 };
    if (vkCreateImageView(a.dev, &vci, nullptr, &s.depthView) != VK_SUCCESS)
        return fail(3, "vkCreateImageView(depth attachment)");
    return true;
}

// Two exportable binary semaphores (GL<->VK, both directions).
static bool createSemaphores(App& a, Shared& s) {
    const VkExternalSemaphoreHandleTypeFlagBits sht = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    auto mk = [&](VkSemaphore* sem, HANDLE* h, const char* nm) -> bool {
        VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        esci.handleTypes = sht;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sci.pNext = &esci;
        if (vkCreateSemaphore(a.dev, &sci, nullptr, sem) != VK_SUCCESS) return fail(3, "vkCreateSemaphore %s", nm);
        VkSemaphoreGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
        gi.semaphore = *sem; gi.handleType = sht;
        if (a.pGetSemWin32(a.dev, &gi, h) != VK_SUCCESS) return fail(3, "vkGetSemaphoreWin32HandleKHR %s", nm);
        return true;
    };
    if (!mk(&s.semGlToVk, &s.semGlToVkWin32, "gl->vk")) return false;
    if (!mk(&s.semVkToGl, &s.semVkToGlWin32, "vk->gl")) return false;
    return true;
}

// VK-private color attachment + host-visible readback buffer.
static bool createColorTarget(App& a, Shared& s) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = kColorFmt;
    ci.extent = { TEX_W, TEX_H, 1 }; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(a.dev, &ci, nullptr, &s.colorImg) != VK_SUCCESS) return fail(3, "vkCreateImage(color)");
    VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(a.dev, s.colorImg, &mr);
    uint32_t idx;
    if (!findMemType(a, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &idx)) return fail(3, "color mem");
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = idx;
    if (vkAllocateMemory(a.dev, &mai, nullptr, &s.colorMem) != VK_SUCCESS) return fail(3, "alloc color");
    vkBindImageMemory(a.dev, s.colorImg, s.colorMem, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = s.colorImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = kColorFmt;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 };
    if (vkCreateImageView(a.dev, &vci, nullptr, &s.colorView) != VK_SUCCESS) return fail(3, "color view");

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = TEX_W * TEX_H * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(a.dev, &bci, nullptr, &s.readbackBuf) != VK_SUCCESS) return fail(3, "readback buf");
    VkMemoryRequirements br{}; vkGetBufferMemoryRequirements(a.dev, s.readbackBuf, &br);
    uint32_t bidx;
    if (!findMemType(a, br.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &bidx))
        return fail(3, "readback mem");
    VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bmai.allocationSize = br.size; bmai.memoryTypeIndex = bidx;
    if (vkAllocateMemory(a.dev, &bmai, nullptr, &s.readbackMem) != VK_SUCCESS) return fail(3, "alloc readback");
    vkBindBufferMemory(a.dev, s.readbackBuf, s.readbackMem, 0);
    return true;
}

// Import the shared depth memory + both semaphores into GL as GL_DEPTH24_STENCIL8.
static bool importDepthIntoGL(App& a, Shared& s) {
    glCreateMemoryObjectsEXT(1, &s.glDepthMemObj);
    if (!glCheck("glCreateMemoryObjectsEXT")) return fail(4, "glCreateMemoryObjectsEXT");
    GLint dedicated = GL_TRUE;
    glMemoryObjectParameterivEXT(s.glDepthMemObj, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    glImportMemoryWin32HandleEXT(s.glDepthMemObj, s.depthMemSize, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, s.depthMemWin32);
    if (!glCheck("glImportMemoryWin32HandleEXT")) return fail(4, "glImportMemoryWin32HandleEXT");

    glCreateTextures(GL_TEXTURE_2D, 1, &s.glDepthTex);
    glTextureParameteri(s.glDepthTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(s.glDepthTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(s.glDepthTex, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    glTextureParameteri(s.glDepthTex, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
    glTextureStorageMem2DEXT(s.glDepthTex, 1, GL_DEPTH24_STENCIL8, TEX_W, TEX_H, s.glDepthMemObj, 0);
    if (!glCheck("glTextureStorageMem2DEXT")) return fail(4, "glTextureStorageMem2DEXT GL_DEPTH24_STENCIL8");

    glGenSemaphoresEXT(1, &s.glSemGlToVk);
    glImportSemaphoreWin32HandleEXT(s.glSemGlToVk, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, s.semGlToVkWin32);
    glGenSemaphoresEXT(1, &s.glSemVkToGl);
    glImportSemaphoreWin32HandleEXT(s.glSemVkToGl, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, s.semVkToGlWin32);
    if (!glCheck("glImportSemaphoreWin32HandleEXT")) return fail(4, "import semaphores");
    LOG("[GL] imported shared depth (GL_DEPTH24_STENCIL8) + both semaphores");
    return true;
}

// -----------------------------------------------------------------------------
// GL authors the depth field into the shared image, under reverse-Z + GEQUAL.
//   clear depth -> 0.0 (far); draw a quad over the LEFT half at depth 0.7 (near).
// GL then signals semGlToVk so VK can consume the image as a depth attachment.
// -----------------------------------------------------------------------------
static GLuint compileGlProgram() {
    const char* vs =
        "#version 430 core\n"
        "uniform float uDepth;\n"      // NDC z we author (GL clip space, will map via glDepthRange default 0..1... we override)
        "const vec2 P[6]=vec2[6](vec2(-1,-1),vec2(0,-1),vec2(0,1),vec2(-1,-1),vec2(0,1),vec2(-1,1));\n"
        "void main(){ gl_Position=vec4(P[gl_VertexID],0.0,1.0); }\n";
    const char* fs =
        "#version 430 core\n"
        "uniform float uDepth;\n"
        "void main(){ gl_FragDepth = uDepth; }\n";  // write authored depth explicitly
    GLuint v = glCreateShader(GL_VERTEX_SHADER); glShaderSource(v,1,&vs,nullptr); glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f,1,&fs,nullptr); glCompileShader(f);
    GLint ok=0; glGetShaderiv(v,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[512]; glGetShaderInfoLog(v,512,nullptr,log); LOG("[GL vs] %s",log); }
    glGetShaderiv(f,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[512]; glGetShaderInfoLog(f,512,nullptr,log); LOG("[GL fs] %s",log); }
    GLuint p = glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f); glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

static bool glAuthorDepth(App& a, Shared& s) {
    LOG("--- GL authors reverse-Z depth: clear=%.2f(far), left-half=%.2f(near), GEQUAL ---",
        GL_FAR_CLEAR, GL_NEAR_DEPTH);
    // Attach the shared depth image to an FBO as DEPTH_STENCIL attachment.
    glCreateFramebuffers(1, &s.glFbo);
    glNamedFramebufferTexture(s.glFbo, GL_DEPTH_STENCIL_ATTACHMENT, s.glDepthTex, 0);
    // No color attachment needed; disable draw/read buffers.
    glNamedFramebufferDrawBuffer(s.glFbo, GL_NONE);
    glNamedFramebufferReadBuffer(s.glFbo, GL_NONE);
    GLenum st = glCheckNamedFramebufferStatus(s.glFbo, GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) return fail(5, "GL FBO incomplete 0x%04X", st);

    glBindFramebuffer(GL_FRAMEBUFFER, s.glFbo);
    glViewport(0, 0, TEX_W, TEX_H);
    // Reverse-Z convention: clear to FAR (0.0), GEQUAL passes nearer fragments.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glClearDepth(GL_FAR_CLEAR);
    glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    GLuint prog = compileGlProgram();
    glUseProgram(prog);
    glUniform1f(glGetUniformLocation(prog, "uDepth"), GL_NEAR_DEPTH);
    GLuint vao; glCreateVertexArrays(1, &vao); glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);   // left-half quad at depth 0.7
    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    glFinish();
    if (!glCheck("glAuthorDepth draw")) return fail(5, "GL depth authoring draw error");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Signal GL->VK: transition to depth-attachment-ish layout for VK consume.
    GLenum layout = GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT;
    glSignalSemaphoreEXT(s.glSemGlToVk, 0, nullptr, 1, &s.glDepthTex, &layout);
    glFlush();
    if (!glCheck("glSignalSemaphoreEXT gl->vk")) return fail(5, "glSignalSemaphoreEXT");
    LOG("[GL] authored depth + signalled gl->vk");
    return true;
}

// Sample the GL depth field back (used both as a self-check and as the
// post-VK regression guard). Fills depth[TEX_W*TEX_H].
static bool glSampleDepth(Shared& s, float* depth, const char* tag) {
    glGetTextureImage(s.glDepthTex, 0, GL_DEPTH_COMPONENT, GL_FLOAT, TEX_W*TEX_H*sizeof(float), depth);
    if (!glCheck("glGetTextureImage depth")) return false;
    LOG("[GL sample %s] row0:", tag);
    std::string line;
    for (uint32_t x = 0; x < TEX_W; ++x) { char b[16]; std::snprintf(b,16,"%.2f ", depth[x]); line += b; }
    LOG("   %s", line.c_str());
    return true;
}

// =============================================================================
// VK: import the GL-authored depth as a DEPTH-STENCIL ATTACHMENT and depth-TEST
// a fullscreen red color draw against it (depthTest=on, depthWrite=off, GEQUAL).
// =============================================================================
struct VkPipe {
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
};

static VkShaderModule mkModule(App& a, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes; ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(a.dev, &ci, nullptr, &m);
    return m;
}

static bool buildVkPipeline(App& a, Shared& s, VkPipe& p) {
    p.vs = mkModule(a, d32s8_depthtest_vert_spv, sizeof(d32s8_depthtest_vert_spv));
    p.fs = mkModule(a, d32s8_depthtest_frag_spv, sizeof(d32s8_depthtest_frag_spv));
    if (!p.vs || !p.fs) return fail(6, "vkCreateShaderModule");

    // Render pass: color (clear->store) + depth-stencil (LOAD existing / read-only).
    VkAttachmentDescription att[2]{};
    att[0].format = kColorFmt; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    // Depth: LOAD the GL-authored contents, do NOT clear, do NOT store (read-only test).
    att[1].format = kDepthFmt; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // GL handed the image over as DEPTH_STENCIL attachment; keep it there.
    att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[1].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 2; rpci.pAttachments = att;
    rpci.subpassCount = 1; rpci.pSubpasses = &sub;
    if (vkCreateRenderPass(a.dev, &rpci, nullptr, &p.rp) != VK_SUCCESS) return fail(6, "vkCreateRenderPass");

    VkImageView fbAtt[2] = { s.colorView, s.depthView };
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = p.rp; fci.attachmentCount = 2; fci.pAttachments = fbAtt;
    fci.width = TEX_W; fci.height = TEX_H; fci.layers = 1;
    if (vkCreateFramebuffer(a.dev, &fci, nullptr, &p.fb) != VK_SUCCESS) return fail(6, "vkCreateFramebuffer");

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(a.dev, &plci, nullptr, &p.layout) != VK_SUCCESS) return fail(6, "pipeline layout");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = p.vs; stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = p.fs; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0,0,(float)TEX_W,(float)TEX_H,0.0f,1.0f};
    VkRect2D sc{{0,0},{TEX_W,TEX_H}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // THE convention under test: depth test ON, write OFF, GREATER_OR_EQUAL.
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    ds.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds; gp.pColorBlendState = &cb;
    gp.layout = p.layout; gp.renderPass = p.rp; gp.subpass = 0;
    if (vkCreateGraphicsPipelines(a.dev, VK_NULL_HANDLE, 1, &gp, nullptr, &p.pipe) != VK_SUCCESS)
        return fail(6, "vkCreateGraphicsPipelines");
    return true;
}

// Record + submit the VK depth-tested draw; wait on GL, signal GL after.
static bool vkDepthTestDraw(App& a, Shared& s, VkPipe& p) {
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = a.cmdPool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb; if (vkAllocateCommandBuffers(a.dev, &cbi, &cb) != VK_SUCCESS) return fail(7, "alloc cb");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};   // color clears to black; red only where kept
    clears[1].depthStencil = {0.0f, 0};              // ignored (depth loadOp=LOAD)
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = p.rp; rbi.framebuffer = p.fb;
    rbi.renderArea = {{0,0},{TEX_W,TEX_H}};
    rbi.clearValueCount = 2; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipe);
    float vkDepth = VK_TEST_DEPTH;
    vkCmdPushConstants(cb, p.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float), &vkDepth);
    vkCmdDraw(cb, 3, 1, 0, 0);   // fullscreen triangle at depth 0.5, GEQUAL-tested
    vkCmdEndRenderPass(cb);

    // Copy color -> readback buffer (color already in TRANSFER_SRC_OPTIMAL via finalLayout).
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { TEX_W, TEX_H, 1 };
    vkCmdCopyImageToBuffer(cb, s.colorImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.readbackBuf, 1, &region);
    vkEndCommandBuffer(cb);

    // Wait on GL's depth authoring; signal GL when the depth-test is done.
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s.semGlToVk; si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s.semVkToGl;
    if (vkQueueSubmit(a.queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return fail(7, "vkQueueSubmit(depth test)");
    vkQueueWaitIdle(a.queue);
    return true;
}

// Read the VK color result and assert the kept/killed pattern.
static bool assertColorPattern(App& a, Shared& s, bool* outPass) {
    void* mapped = nullptr;
    vkMapMemory(a.dev, s.readbackMem, 0, TEX_W*TEX_H*4, 0, &mapped);
    uint8_t* px = (uint8_t*)mapped;
    LOG("--- VK depth-test result (R channel; RED=kept, 0=killed) ---");
    bool pass = true;
    for (uint32_t y = 0; y < TEX_H; ++y) {
        std::string line;
        for (uint32_t x = 0; x < TEX_W; ++x) {
            uint8_t r = px[(y*TEX_W + x)*4 + 0];
            char b[8]; std::snprintf(b,8,"%3d ", r); line += b;
            bool leftHalf = (x < TEX_W/2);
            // left half stored depth 0.7: 0.5>=0.7 false -> KILLED (r==0)
            // right half stored depth 0.0: 0.5>=0.0 true -> KEPT   (r==255)
            uint8_t expect = leftHalf ? 0 : 255;
            if (r != expect) {
                pass = false;
                LOG("[ASSERT-FAIL] texel (%u,%u) r=%d expected=%d (%s half, stored %.2f, vk %.2f, GEQUAL)",
                    x, y, r, expect, leftHalf?"left":"right",
                    leftHalf?GL_NEAR_DEPTH:GL_FAR_CLEAR, VK_TEST_DEPTH);
            }
        }
        LOG("   %s", line.c_str());
    }
    vkUnmapMemory(a.dev, s.readbackMem);
    *outPass = pass;
    if (pass) LOG("[ASSERT-1 PASS] VK GEQUAL depth-test correctly kept/killed against GL-authored depth.");
    else      LOG("[ASSERT-1 FAIL] VK depth-test did NOT match GL-authored depth field.");
    return true;
}

// Guard (b)+(c): glCopyImageSubData on the 64bpp D32S8 + stencil view.
static bool copyAndStencilGuards(Shared& s, const float* refDepth, bool* outCopyOk, bool* outStencilOk) {
    *outCopyOk = false; *outStencilOk = false;
    // (b) Copy the shared depth image into a fresh GL_DEPTH24_STENCIL8 texture via
    //     glCopyImageSubData (size-vs-format-class hazard on 64bpp packed DS).
    glCreateTextures(GL_TEXTURE_2D, 1, &s.glDepthCopyTex);
    glTextureParameteri(s.glDepthCopyTex, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
    glTextureStorage2D(s.glDepthCopyTex, 1, GL_DEPTH24_STENCIL8, TEX_W, TEX_H);
    if (!glCheck("copy dst storage")) return fail(8, "copy dst storage");
    glCopyImageSubData(s.glDepthTex, GL_TEXTURE_2D, 0, 0,0,0,
                       s.glDepthCopyTex, GL_TEXTURE_2D, 0, 0,0,0, TEX_W, TEX_H, 1);
    if (!glCheck("glCopyImageSubData D32S8")) return fail(8, "glCopyImageSubData D32S8 rejected");
    float copyDepth[TEX_W*TEX_H] = {0};
    glGetTextureImage(s.glDepthCopyTex, 0, GL_DEPTH_COMPONENT, GL_FLOAT, sizeof(copyDepth), copyDepth);
    if (!glCheck("read copy depth")) return fail(8, "read copy depth");
    bool copyMatch = true;
    for (uint32_t i = 0; i < TEX_W*TEX_H; ++i)
        if (std::fabs(copyDepth[i] - refDepth[i]) > DEPTH_EPS) {
            copyMatch = false;
            LOG("[COPY MISMATCH] texel %u copy=%.4f ref=%.4f", i, copyDepth[i], refDepth[i]); break;
        }
    *outCopyOk = copyMatch;
    LOG("[GUARD-b %s] glCopyImageSubData depth copy on 64bpp D32S8 %s",
        copyMatch?"PASS":"FAIL", copyMatch?"matches source":"DIVERGED");

    // (c) stencil aspect view over the shared packed image.
    glGenTextures(1, &s.glStencilView);
    glTextureView(s.glStencilView, GL_TEXTURE_2D, s.glDepthTex, GL_DEPTH24_STENCIL8, 0, 1, 0, 1);
    if (!glCheck("glTextureView stencil")) { LOG("[GUARD-c FAIL] glTextureView over D32S8 rejected"); return fail(8, "stencil view"); }
    glTextureParameteri(s.glStencilView, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_STENCIL_INDEX);
    if (!glCheck("stencil mode")) return fail(8, "stencil mode");
    *outStencilOk = true;
    LOG("[GUARD-c PASS] stencil-aspect glTextureView over shared D32S8 created + STENCIL_INDEX mode set");
    return true;
}

static void cleanup(App& a, Shared& s, VkPipe& p) {
    if (a.dev) vkDeviceWaitIdle(a.dev);
    if (s.glFbo) glDeleteFramebuffers(1,&s.glFbo);
    if (s.glDepthTex) glDeleteTextures(1,&s.glDepthTex);
    if (s.glDepthCopyTex) glDeleteTextures(1,&s.glDepthCopyTex);
    if (s.glStencilView) glDeleteTextures(1,&s.glStencilView);
    if (s.glDepthMemObj) glDeleteMemoryObjectsEXT(1,&s.glDepthMemObj);
    if (s.glSemGlToVk) glDeleteSemaphoresEXT(1,&s.glSemGlToVk);
    if (s.glSemVkToGl) glDeleteSemaphoresEXT(1,&s.glSemVkToGl);
    if (p.pipe) vkDestroyPipeline(a.dev,p.pipe,nullptr);
    if (p.layout) vkDestroyPipelineLayout(a.dev,p.layout,nullptr);
    if (p.fb) vkDestroyFramebuffer(a.dev,p.fb,nullptr);
    if (p.rp) vkDestroyRenderPass(a.dev,p.rp,nullptr);
    if (p.vs) vkDestroyShaderModule(a.dev,p.vs,nullptr);
    if (p.fs) vkDestroyShaderModule(a.dev,p.fs,nullptr);
    if (s.depthView) vkDestroyImageView(a.dev,s.depthView,nullptr);
    if (s.colorView) vkDestroyImageView(a.dev,s.colorView,nullptr);
    if (s.readbackBuf) vkDestroyBuffer(a.dev,s.readbackBuf,nullptr);
    if (s.readbackMem) vkFreeMemory(a.dev,s.readbackMem,nullptr);
    if (s.colorImg) vkDestroyImage(a.dev,s.colorImg,nullptr);
    if (s.colorMem) vkFreeMemory(a.dev,s.colorMem,nullptr);
    if (s.semGlToVk) vkDestroySemaphore(a.dev,s.semGlToVk,nullptr);
    if (s.semVkToGl) vkDestroySemaphore(a.dev,s.semVkToGl,nullptr);
    if (s.depthImg) vkDestroyImage(a.dev,s.depthImg,nullptr);
    if (s.depthMem) vkFreeMemory(a.dev,s.depthMem,nullptr);
    if (a.cmdPool) vkDestroyCommandPool(a.dev,a.cmdPool,nullptr);
    if (a.dev) vkDestroyDevice(a.dev,nullptr);
    if (a.messenger && a.pDestroyDbg) a.pDestroyDbg(a.inst,a.messenger,nullptr);
    if (a.inst) vkDestroyInstance(a.inst,nullptr);
    if (a.glctx) SDL_GL_DeleteContext(a.glctx);
    if (a.win) SDL_DestroyWindow(a.win);
    SDL_Quit();
}

int main(int, char**) {
    LOG("==============================================================");
    LOG(" VK depth-TESTS against GL-authored shared D32S8 scene depth");
    LOG("   (Tier-1 STOP-gate: GL produces depth / VK consumes as attachment)");
    LOG("   reverse-Z: clear 0.0=far, GEQUAL; GL near=%.2f VK test=%.2f",
        GL_NEAR_DEPTH, VK_TEST_DEPTH);
    LOG("==============================================================");

    App a; Shared s; VkPipe p;
    bool assert1 = false, guardCopy = false, guardStencil = false, guardResample = false;
    float refDepth[TEX_W*TEX_H] = {0};
    float postDepth[TEX_W*TEX_H] = {0};

    bool built =
        initGL(a) && initVk(a) &&
        createSharedDepth(a, s) && createSemaphores(a, s) && createColorTarget(a, s) &&
        importDepthIntoGL(a, s) &&
        glAuthorDepth(a, s) &&
        glSampleDepth(s, refDepth, "GL-authored (pre-VK)") &&
        buildVkPipeline(a, s, p) &&
        vkDepthTestDraw(a, s, p) &&
        assertColorPattern(a, s, &assert1);

    if (built) {
        // (a) 42-consumer regression guard: GL waits on VK, re-samples DEPTH.
        GLenum layout = GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT;
        glWaitSemaphoreEXT(s.glSemVkToGl, 0, nullptr, 1, &s.glDepthTex, &layout);
        glCheck("glWaitSemaphoreEXT vk->gl");
        if (glSampleDepth(s, postDepth, "GL re-sample (post-VK)")) {
            guardResample = true;
            for (uint32_t i = 0; i < TEX_W*TEX_H; ++i)
                if (std::fabs(postDepth[i] - refDepth[i]) > DEPTH_EPS) {
                    guardResample = false;
                    LOG("[GUARD-a FAIL] post-VK depth texel %u changed: %.4f -> %.4f",
                        i, refDepth[i], postDepth[i]); break;
                }
            LOG("[GUARD-a %s] GL re-samples exact DEPTH aspect after VK depth-test (42-consumer guard)",
                guardResample ? "PASS" : "FAIL");
        }
        // (b)+(c) copy-path + stencil-view guards.
        copyAndStencilGuards(s, refDepth, &guardCopy, &guardStencil);
    }

    LOG("");
    LOG("--------------------------------------------------------------");
    LOG("[TALLY] VK validation errors = %d", g_valErrors);
    LOG("[TALLY] GL debug errors seen = %d", g_glDebugSawError ? 1 : 0);
    LOG("[RESULT] ASSERT-1 VK-depth-test-vs-GL-depth = %s", assert1 ? "PASS" : "FAIL");
    LOG("[RESULT] GUARD-a  GL re-sample DEPTH exact   = %s", guardResample ? "PASS" : "FAIL");
    LOG("[RESULT] GUARD-b  glCopyImageSubData D32S8   = %s", guardCopy ? "PASS" : "FAIL");
    LOG("[RESULT] GUARD-c  stencil glTextureView      = %s", guardStencil ? "PASS" : "FAIL");

    bool clean = (g_valErrors == 0) && !g_glDebugSawError;
    bool allPass = built && assert1 && guardResample && guardCopy && guardStencil && clean;
    int rc;
    if (allPass) {
        LOG("VERDICT: STOP-GATE GREEN — VK depth-tests against GL-authored D32S8 depth,");
        LOG("         no regressions, validation clean. Tier-1 geometry-color path unblocked.");
        rc = 0;
    } else {
        LOG("VERDICT: STOP-GATE RED — see the failing assert/guard above.");
        LOG("         (built=%d assert1=%d guardA=%d guardB=%d guardC=%d valClean=%d)",
            built?1:0, assert1?1:0, guardResample?1:0, guardCopy?1:0, guardStencil?1:0, clean?1:0);
        if (!built) LOG("         setup/draw failed at stage %d: %s", g_failStage, g_failMsg);
        rc = 1;
    }
    LOG("==============================================================");
    cleanup(a, s, p);
    return rc;
}
