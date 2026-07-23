// =============================================================================
// GL and VK CO-WRITE the same shared RGBA16F color attachment in one frame,
// under a shared depth-test, and neither clobbers the other  — SPIKE.
//   Color analogue of main_d32s8_depthtest.cpp. The STOP-gate before the XXL
//   in-place-shared-GBuffer engine slice (props / terrain / mechs co-authored
//   by GL and VK into one color target with no readback, no re-clear).
//
// The depth-test spike proved GL AUTHORS depth / VK CONSUMES depth as an
// attachment (VK wrote its own private color). This spike proves the real
// endgame capability: GL and VK write the SAME shared COLOR image in-place.
//
//   1. Shared exportable RGBA16F color image (imported to GL as GL_RGBA16F) +
//      shared D32_SFLOAT_S8_UINT depth image (imported to GL as
//      GL_DEPTH24_STENCIL8). Both reuse the proven interop primitives.
//   2. GL phase (FBO = shared color + shared depth):
//        - reverse-Z: clearDepth(0.0=far), GEQUAL.
//        - clear shared COLOR to RED background (1,0,0,1).
//        - author depth: LEFT half (cols 0-7) at NEAR 0.7; RIGHT half stays FAR 0.0.
//        - write GL "terrain" GREEN (0,1,0,1) into cols 4..11 (straddles the
//          depth boundary), depth-testing so it stamps its own depth too — but
//          we keep GL terrain depth == the authored field so VK's test is clean.
//        - signal GL->VK (color=COLOR_ATTACHMENT, depth=DEPTH_STENCIL_ATTACHMENT).
//   3. VK phase (import SAME color as COLOR attachment LOAD — NOT clear — and
//      SAME depth as DEPTH-STENCIL attachment LOAD): depthTest ON, depthWrite
//      OFF, GEQUAL. Draw four BLUE (0,0,1,1) "prop" quads in-place at VK depth
//      0.5, one per column band:
//        A cols 0-3   left/near 0.7 : 0.5>=0.7 NO  -> LOSES -> GL RED   survives
//        B cols 4-7   left/near 0.7 : 0.5>=0.7 NO  -> LOSES -> GL GREEN survives
//        C cols 8-11  right/far 0.0 : 0.5>=0.0 YES -> WINS  -> overwrites GREEN->BLUE
//        D cols 12-15 right/far 0.0 : 0.5>=0.0 YES -> WINS  -> overwrites RED  ->BLUE
//      VK does NOT clear the color attachment (loadOp=LOAD). Signal VK->GL.
//   4. VK->GL: GL waits, reads back the shared RGBA16F color and ASSERTS the
//      per-band pattern (RED / GREEN / BLUE / BLUE). This proves in-place
//      co-write: VK winners overwrote, VK losers left GL intact, background
//      survived, and VK never re-cleared.
//   5. Run under Vulkan sync validation; assert ZERO validation errors — the
//      GL<->VK color acquire/release layout + semaphore dance across a
//      LOAD-preserve co-write is the load-bearing sync contract.
//   6. Forbidden-counter check: the co-write path performs NO readback in the
//      middle and NO color re-clear; counters asserted == 0.
//   7. Negative control (env MC2_COLORCOWRITE_NEGCTL):
//        "compare" -> flip VK depthCompareOp to LESS (inverts win/lose)  -> asserts FAIL
//        "clear"   -> VK color loadOp=CLEAR (clobbers GL)                 -> asserts FAIL
//      proves the harness actually detects a broken co-write. Default: none.
//
// Exit 0 == in-place GL<->VK color co-write correct + validation clean +
//           forbidden-counters zero -> STOP-gate GREEN.
// Non-zero == which assert/stage failed is printed -> STOP-gate RED (critical).
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
#include <cstdarg>
#include <cmath>
#include <vector>
#include <string>

// Embedded SPIR-V (glslc -mfmt=c at build time; see CMakeLists.txt).
static const uint32_t colorcowrite_vert_spv[] =
    #include "colorcowrite.vert.spv.inc"
;
static const uint32_t colorcowrite_frag_spv[] =
    #include "colorcowrite.frag.spv.inc"
;

// ---- image geometry ---------------------------------------------------------
static const uint32_t TEX_W = 16;
static const uint32_t TEX_H = 4;

// Reverse-Z (engine convention): clearDepth 0.0 = FAR, larger = NEARER, GEQUAL.
static const float GL_FAR_CLEAR  = 0.0f;   // GL clears depth to far
static const float GL_NEAR_DEPTH = 0.7f;   // GL draws LEFT half depth at near
static const float VK_TEST_DEPTH = 0.5f;   // VK prop draws tested at this depth

// Known colors (RGBA), stored as RGBA16F but authored/asserted as exact floats.
static const float COL_RED[4]   = {1.0f, 0.0f, 0.0f, 1.0f}; // GL background
static const float COL_GREEN[4] = {0.0f, 1.0f, 0.0f, 1.0f}; // GL "terrain"
static const float COL_BLUE[4]  = {0.0f, 0.0f, 1.0f, 1.0f}; // VK "prop"

// RGBA16F round-trips 0.0/1.0 exactly; tiny tolerance for safety.
static const float COL_EPS = 1e-3f;

#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL_TEXTURE_MODE
#define GL_DEPTH_STENCIL_TEXTURE_MODE 0x90EA
#endif

// ---- logging + verdict tracking --------------------------------------------
static int   g_failStage = 0;
static char  g_failMsg[512] = {0};
static bool  g_glDebugSawError = false;
static int   g_valErrors = 0;

// Forbidden-counter instrumentation for the co-write path.
static int   g_readbacksMidPath = 0;   // any readback between GL-author and final assert
static int   g_colorReclears    = 0;   // any color clear on the VK co-write path

#define LOG(...)  do { std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while(0)

static bool fail(int stage, const char* fmt, ...) {
    g_failStage = stage;
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(g_failMsg, sizeof(g_failMsg), fmt, ap);
    va_end(ap);
    LOG("[FAIL stage %d] %s", stage, g_failMsg);
    return false;
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

// ---- negative-control mode --------------------------------------------------
enum NegCtl { NEG_NONE, NEG_COMPARE, NEG_CLEAR };
static NegCtl g_neg = NEG_NONE;
static const char* negName(NegCtl n) {
    switch (n) { case NEG_COMPARE: return "compare(LESS)"; case NEG_CLEAR: return "clear"; default: return "none"; }
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
    a.win = SDL_CreateWindow("vk_gl_colorcowrite_spike", SDL_WINDOWPOS_CENTERED,
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
    ai.pApplicationName = "vk_gl_colorcowrite_spike";
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
// Shared resources: RGBA16F color image + D32S8 depth image + both semaphores.
// =============================================================================
struct Shared {
    // Shared exportable COLOR image (RGBA16F) — co-written by GL and VK.
    VkImage        colorImg = VK_NULL_HANDLE;
    VkDeviceMemory colorMem = VK_NULL_HANDLE;
    VkDeviceSize   colorMemSize = 0;
    HANDLE         colorMemWin32 = nullptr;
    GLuint         glColorMemObj = 0;
    GLuint         glColorTex = 0;         // GL_RGBA16F view of shared img
    VkImageView    colorView = VK_NULL_HANDLE;

    // Shared exportable DEPTH image (D32_SFLOAT_S8_UINT).
    VkImage        depthImg = VK_NULL_HANDLE;
    VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkDeviceSize   depthMemSize = 0;
    HANDLE         depthMemWin32 = nullptr;
    GLuint         glDepthMemObj = 0;
    GLuint         glDepthTex = 0;         // GL_DEPTH24_STENCIL8 view of shared img
    VkImageView    depthView = VK_NULL_HANDLE;

    // GL->VK + VK->GL semaphores.
    VkSemaphore    semGlToVk = VK_NULL_HANDLE; HANDLE semGlToVkWin32 = nullptr; GLuint glSemGlToVk = 0;
    VkSemaphore    semVkToGl = VK_NULL_HANDLE; HANDLE semVkToGlWin32 = nullptr; GLuint glSemVkToGl = 0;

    GLuint         glFbo = 0;              // GL FBO: shared color + shared depth
};

static const VkFormat kDepthFmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
static const VkFormat kColorFmt = VK_FORMAT_R16G16B16A16_SFLOAT;

// Create the shared exportable COLOR image (RGBA16F).
static bool createSharedColor(App& a, Shared& s) {
    const VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkExternalMemoryImageCreateInfo emici{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes = ht;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = &emici; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = kColorFmt;
    ci.extent = { TEX_W, TEX_H, 1 }; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(a.dev, &ci, nullptr, &s.colorImg) != VK_SUCCESS) return fail(3, "vkCreateImage(shared RGBA16F)");
    VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(a.dev, s.colorImg, &mr);
    s.colorMemSize = mr.size;
    uint32_t memIdx;
    if (!findMemType(a, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memIdx))
        return fail(3, "no DEVICE_LOCAL mem for shared color");
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; ded.image = s.colorImg;
    VkExportMemoryAllocateInfo em{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO}; em.pNext = &ded; em.handleTypes = ht;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &em; mai.allocationSize = mr.size; mai.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(a.dev, &mai, nullptr, &s.colorMem) != VK_SUCCESS) return fail(3, "vkAllocateMemory(shared color)");
    if (vkBindImageMemory(a.dev, s.colorImg, s.colorMem, 0) != VK_SUCCESS) return fail(3, "vkBindImageMemory(shared color)");
    VkMemoryGetWin32HandleInfoKHR gh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    gh.memory = s.colorMem; gh.handleType = ht;
    if (a.pGetMemWin32(a.dev, &gh, &s.colorMemWin32) != VK_SUCCESS) return fail(3, "vkGetMemoryWin32HandleKHR(color)");
    LOG("[VK] shared RGBA16F color image: memWin32=%p size=%llu", s.colorMemWin32, (unsigned long long)s.colorMemSize);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = s.colorImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = kColorFmt;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0,1,0,1 };
    if (vkCreateImageView(a.dev, &vci, nullptr, &s.colorView) != VK_SUCCESS) return fail(3, "vkCreateImageView(color attachment)");
    return true;
}

// Create the shared exportable DEPTH image (D32S8).
static bool createSharedDepth(App& a, Shared& s) {
    const VkExternalMemoryHandleTypeFlagBits ht = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkExternalMemoryImageCreateInfo emici{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes = ht;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = &emici; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = kDepthFmt;
    ci.extent = { TEX_W, TEX_H, 1 }; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(a.dev, &ci, nullptr, &s.depthImg) != VK_SUCCESS) return fail(3, "vkCreateImage(shared D32S8)");
    VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(a.dev, s.depthImg, &mr);
    s.depthMemSize = mr.size;
    uint32_t memIdx;
    if (!findMemType(a, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memIdx))
        return fail(3, "no DEVICE_LOCAL mem for shared depth");
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; ded.image = s.depthImg;
    VkExportMemoryAllocateInfo em{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO}; em.pNext = &ded; em.handleTypes = ht;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &em; mai.allocationSize = mr.size; mai.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(a.dev, &mai, nullptr, &s.depthMem) != VK_SUCCESS) return fail(3, "vkAllocateMemory(shared depth)");
    if (vkBindImageMemory(a.dev, s.depthImg, s.depthMem, 0) != VK_SUCCESS) return fail(3, "vkBindImageMemory(shared depth)");
    VkMemoryGetWin32HandleInfoKHR gh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    gh.memory = s.depthMem; gh.handleType = ht;
    if (a.pGetMemWin32(a.dev, &gh, &s.depthMemWin32) != VK_SUCCESS) return fail(3, "vkGetMemoryWin32HandleKHR(depth)");
    LOG("[VK] shared D32S8 depth image: memWin32=%p size=%llu", s.depthMemWin32, (unsigned long long)s.depthMemSize);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = s.depthImg; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = kDepthFmt;
    vci.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0,1,0,1 };
    if (vkCreateImageView(a.dev, &vci, nullptr, &s.depthView) != VK_SUCCESS) return fail(3, "vkCreateImageView(depth attachment)");
    return true;
}

// Two exportable binary semaphores (both directions).
static bool createSemaphores(App& a, Shared& s) {
    const VkExternalSemaphoreHandleTypeFlagBits sht = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    auto mk = [&](VkSemaphore* sem, HANDLE* h, const char* nm) -> bool {
        VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO}; esci.handleTypes = sht;
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

// Import shared color (GL_RGBA16F) + shared depth (GL_DEPTH24_STENCIL8) + sems into GL.
static bool importIntoGL(App& a, Shared& s) {
    // Color.
    glCreateMemoryObjectsEXT(1, &s.glColorMemObj);
    if (!glCheck("glCreateMemoryObjectsEXT(color)")) return fail(4, "glCreateMemoryObjectsEXT color");
    GLint dedicated = GL_TRUE;
    glMemoryObjectParameterivEXT(s.glColorMemObj, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    glImportMemoryWin32HandleEXT(s.glColorMemObj, s.colorMemSize, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, s.colorMemWin32);
    if (!glCheck("glImportMemoryWin32HandleEXT(color)")) return fail(4, "glImportMemoryWin32HandleEXT color");
    glCreateTextures(GL_TEXTURE_2D, 1, &s.glColorTex);
    glTextureParameteri(s.glColorTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(s.glColorTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(s.glColorTex, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    glTextureStorageMem2DEXT(s.glColorTex, 1, GL_RGBA16F, TEX_W, TEX_H, s.glColorMemObj, 0);
    if (!glCheck("glTextureStorageMem2DEXT(RGBA16F)")) return fail(4, "glTextureStorageMem2DEXT GL_RGBA16F");

    // Depth.
    glCreateMemoryObjectsEXT(1, &s.glDepthMemObj);
    if (!glCheck("glCreateMemoryObjectsEXT(depth)")) return fail(4, "glCreateMemoryObjectsEXT depth");
    glMemoryObjectParameterivEXT(s.glDepthMemObj, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    glImportMemoryWin32HandleEXT(s.glDepthMemObj, s.depthMemSize, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, s.depthMemWin32);
    if (!glCheck("glImportMemoryWin32HandleEXT(depth)")) return fail(4, "glImportMemoryWin32HandleEXT depth");
    glCreateTextures(GL_TEXTURE_2D, 1, &s.glDepthTex);
    glTextureParameteri(s.glDepthTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(s.glDepthTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(s.glDepthTex, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    glTextureParameteri(s.glDepthTex, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
    glTextureStorageMem2DEXT(s.glDepthTex, 1, GL_DEPTH24_STENCIL8, TEX_W, TEX_H, s.glDepthMemObj, 0);
    if (!glCheck("glTextureStorageMem2DEXT(D24S8)")) return fail(4, "glTextureStorageMem2DEXT GL_DEPTH24_STENCIL8");

    // Semaphores.
    glGenSemaphoresEXT(1, &s.glSemGlToVk);
    glImportSemaphoreWin32HandleEXT(s.glSemGlToVk, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, s.semGlToVkWin32);
    glGenSemaphoresEXT(1, &s.glSemVkToGl);
    glImportSemaphoreWin32HandleEXT(s.glSemVkToGl, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, s.semVkToGlWin32);
    if (!glCheck("glImportSemaphoreWin32HandleEXT")) return fail(4, "import semaphores");
    LOG("[GL] imported shared color (RGBA16F) + depth (D24S8) + both semaphores");
    return true;
}

// -----------------------------------------------------------------------------
// GL authors the frame into the SHARED color+depth: clear color RED, author the
// reverse-Z depth field (left half near 0.7), draw GREEN "terrain" band into
// cols 4..11 stamping the same depth field, then signal GL->VK.
// -----------------------------------------------------------------------------
static GLuint compileGlProgram() {
    // Draws a colored quad over an NDC x-band [uX0,uX1] at a fixed authored depth.
    const char* vs =
        "#version 430 core\n"
        "uniform float uX0; uniform float uX1; uniform float uDepth;\n"
        "const vec2 C[6]=vec2[6](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));\n"
        "void main(){ vec2 q=C[gl_VertexID];\n"
        "  float x=mix(uX0,uX1,q.x); float y=q.y*2.0-1.0;\n"
        "  gl_Position=vec4(x,y,uDepth,1.0); }\n";
    const char* fs =
        "#version 430 core\n"
        "uniform vec4 uColor;\n"
        "layout(location=0) out vec4 o;\n"
        "void main(){ o=uColor; }\n";
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

static bool glAuthorFrame(App& a, Shared& s) {
    LOG("--- GL authors shared frame: color=RED bg, depth left-half=%.2f(near)/right=%.2f(far), GREEN terrain cols4..11 ---",
        GL_NEAR_DEPTH, GL_FAR_CLEAR);
    glCreateFramebuffers(1, &s.glFbo);
    glNamedFramebufferTexture(s.glFbo, GL_COLOR_ATTACHMENT0, s.glColorTex, 0);
    glNamedFramebufferTexture(s.glFbo, GL_DEPTH_STENCIL_ATTACHMENT, s.glDepthTex, 0);
    GLenum drawBuf = GL_COLOR_ATTACHMENT0;
    glNamedFramebufferDrawBuffers(s.glFbo, 1, &drawBuf);
    GLenum st = glCheckNamedFramebufferStatus(s.glFbo, GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) return fail(5, "GL FBO incomplete 0x%04X", st);

    glBindFramebuffer(GL_FRAMEBUFFER, s.glFbo);
    glViewport(0, 0, TEX_W, TEX_H);
    glDisable(GL_BLEND);

    // Reverse-Z: clear depth to FAR (0.0), GEQUAL passes nearer.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);
    glDepthMask(GL_TRUE);
    glClearDepth(GL_FAR_CLEAR);
    // Background RED.
    glClearColor(COL_RED[0], COL_RED[1], COL_RED[2], COL_RED[3]);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    GLuint prog = compileGlProgram();
    glUseProgram(prog);
    GLint lX0 = glGetUniformLocation(prog, "uX0");
    GLint lX1 = glGetUniformLocation(prog, "uX1");
    GLint lD  = glGetUniformLocation(prog, "uDepth");
    GLint lC  = glGetUniformLocation(prog, "uColor");
    GLuint vao; glCreateVertexArrays(1, &vao); glBindVertexArray(vao);

    // (1) Author depth: draw a NEAR (0.7) quad over the LEFT half (cols 0..7,
    //     NDC x [-1,0]). Color-write it RED too (matches bg; keeps left half red
    //     where no terrain). GEQUAL vs cleared-0.0 => passes, stamps depth 0.7.
    glUniform1f(lX0, -1.0f); glUniform1f(lX1, 0.0f);
    glUniform1f(lD, GL_NEAR_DEPTH);
    glUniform4f(lC, COL_RED[0], COL_RED[1], COL_RED[2], COL_RED[3]);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // (2) GL "terrain": GREEN band over cols 4..11 (NDC x [-0.5,0.5]).
    //     Left part (cols4..7) sits on the near-0.7 depth region; to co-exist
    //     with the stored 0.7 under GEQUAL we must draw at depth >= 0.7. Draw the
    //     green band at exactly the region's stored depth so it passes and does
    //     NOT alter the depth field VK will test against:
    //       cols 4..7  (x[-0.5,0]) at depth 0.7 (== stored) : GEQUAL passes, depth unchanged.
    //       cols 8..11 (x[0,0.5])  at depth 0.0 (== stored far) : GEQUAL passes, depth unchanged.
    glUniform4f(lC, COL_GREEN[0], COL_GREEN[1], COL_GREEN[2], COL_GREEN[3]);
    glUniform1f(lX0, -0.5f); glUniform1f(lX1, 0.0f);  glUniform1f(lD, GL_NEAR_DEPTH); glDrawArrays(GL_TRIANGLES, 0, 6);
    glUniform1f(lX0,  0.0f); glUniform1f(lX1, 0.5f);  glUniform1f(lD, GL_FAR_CLEAR);  glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(prog);
    glFinish();
    if (!glCheck("glAuthorFrame draw")) return fail(5, "GL authoring draw error");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // Signal GL->VK. Color -> COLOR_ATTACHMENT layout, depth -> DS_ATTACHMENT.
    GLuint texs[2]   = { s.glColorTex, s.glDepthTex };
    GLenum layouts[2] = { GL_LAYOUT_COLOR_ATTACHMENT_EXT, GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT };
    glSignalSemaphoreEXT(s.glSemGlToVk, 0, nullptr, 2, texs, layouts);
    glFlush();
    if (!glCheck("glSignalSemaphoreEXT gl->vk")) return fail(5, "glSignalSemaphoreEXT");
    LOG("[GL] authored shared color+depth + signalled gl->vk");
    return true;
}

// Sample the shared color image (RGBA16F -> float4 per texel).
static bool glSampleColor(Shared& s, float* rgba /* TEX_W*TEX_H*4 */, const char* tag) {
    glGetTextureImage(s.glColorTex, 0, GL_RGBA, GL_FLOAT, TEX_W*TEX_H*4*sizeof(float), rgba);
    if (!glCheck("glGetTextureImage color")) return false;
    LOG("[GL color sample %s] row0 (R,G,B per texel):", tag);
    std::string line;
    for (uint32_t x = 0; x < TEX_W; ++x) {
        const float* p = &rgba[x*4];
        char b[24]; std::snprintf(b,24,"%.0f%.0f%.0f ", p[0], p[1], p[2]); line += b;
    }
    LOG("   %s", line.c_str());
    return true;
}

// =============================================================================
// VK: import SAME shared color as COLOR attachment (LOAD) + SAME shared depth as
// DS attachment (LOAD); depthTest ON / write OFF / GEQUAL; draw BLUE prop quads
// in-place. No color clear on this path.
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
    VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(a.dev, &ci, nullptr, &m); return m;
}

static bool buildVkPipeline(App& a, Shared& s, VkPipe& p) {
    p.vs = mkModule(a, colorcowrite_vert_spv, sizeof(colorcowrite_vert_spv));
    p.fs = mkModule(a, colorcowrite_frag_spv, sizeof(colorcowrite_frag_spv));
    if (!p.vs || !p.fs) return fail(6, "vkCreateShaderModule");

    // NEG_CLEAR flips the color loadOp to CLEAR to prove the assert detects a clobber.
    VkAttachmentLoadOp colorLoad = (g_neg == NEG_CLEAR)
        ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    if (g_neg == NEG_CLEAR) g_colorReclears++;   // instrument: this IS a forbidden re-clear

    VkAttachmentDescription att[2]{};
    // Color: LOAD the GL-authored contents (preserve), STORE result.
    att[0].format = kColorFmt; att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = colorLoad; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // GL handed color over in COLOR_ATTACHMENT layout; keep it there for GL to resume.
    att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[0].finalLayout   = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // Depth: LOAD GL-authored, read-only test (write disabled), no store.
    att[1].format = kDepthFmt; att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[1].finalLayout   = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 2; rpci.pAttachments = att; rpci.subpassCount = 1; rpci.pSubpasses = &sub;
    if (vkCreateRenderPass(a.dev, &rpci, nullptr, &p.rp) != VK_SUCCESS) return fail(6, "vkCreateRenderPass");

    VkImageView fbAtt[2] = { s.colorView, s.depthView };
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = p.rp; fci.attachmentCount = 2; fci.pAttachments = fbAtt;
    fci.width = TEX_W; fci.height = TEX_H; fci.layers = 1;
    if (vkCreateFramebuffer(a.dev, &fci, nullptr, &p.fb) != VK_SUCCESS) return fail(6, "vkCreateFramebuffer");

    // Push constant: x0,x1,depth,pad (4 floats).
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float)*4};
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
    VkRect2D scr{{0,0},{TEX_W,TEX_H}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scr;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Convention under test: depthTest ON, write OFF, GEQUAL. NEG_COMPARE flips to LESS.
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = (g_neg == NEG_COMPARE) ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_GREATER_OR_EQUAL;
    ds.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF; cba.blendEnable = VK_FALSE;
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

// Record + submit the VK in-place co-write; wait on GL, signal GL after.
static bool vkCoWriteDraw(App& a, Shared& s, VkPipe& p) {
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = a.cmdPool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cb; if (vkAllocateCommandBuffers(a.dev, &cbi, &cb) != VK_SUCCESS) return fail(7, "alloc cb");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);

    // clearValue for color is ignored when loadOp=LOAD; used only in NEG_CLEAR.
    VkClearValue clears[2]{};
    clears[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};  // black (only if NEG_CLEAR)
    clears[1].depthStencil = {0.0f, 0};             // ignored (depth loadOp=LOAD)
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = p.rp; rbi.framebuffer = p.fb;
    rbi.renderArea = {{0,0},{TEX_W,TEX_H}};
    rbi.clearValueCount = 2; rbi.pClearValues = clears;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipe);

    // Four BLUE prop quads, one per column band, all at VK_TEST_DEPTH.
    struct Prop { float x0, x1; };
    const Prop props[4] = {
        { -1.0f, -0.5f },   // A cols 0-3   (LOSES, GL red   survives)
        { -0.5f,  0.0f },   // B cols 4-7   (LOSES, GL green  survives)
        {  0.0f,  0.5f },   // C cols 8-11  (WINS,  overwrites green -> blue)
        {  0.5f,  1.0f },   // D cols 12-15 (WINS,  overwrites red   -> blue)
    };
    for (const Prop& pr : props) {
        float pc[4] = { pr.x0, pr.x1, VK_TEST_DEPTH, 0.0f };
        vkCmdPushConstants(cb, p.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
        vkCmdDraw(cb, 6, 1, 0, 0);
    }
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                                   | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &s.semGlToVk; si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &s.semVkToGl;
    if (vkQueueSubmit(a.queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return fail(7, "vkQueueSubmit(cowrite)");
    vkQueueWaitIdle(a.queue);
    return true;
}

// Compare a texel's rgb to an expected known color.
static bool colorIs(const float* p, const float* want) {
    return std::fabs(p[0]-want[0]) <= COL_EPS
        && std::fabs(p[1]-want[1]) <= COL_EPS
        && std::fabs(p[2]-want[2]) <= COL_EPS;
}
static const char* colName(const float* p) {
    if (colorIs(p, COL_RED))   return "RED";
    if (colorIs(p, COL_GREEN)) return "GREEN";
    if (colorIs(p, COL_BLUE))  return "BLUE";
    return "????";
}

// Assert the co-write pattern per column band from the shared color readback.
static bool assertCoWrite(const float* rgba, bool* outPass) {
    // Expected per column band after the in-place co-write.
    struct Band { uint32_t x0, x1; const float* want; const char* why; };
    const Band bands[4] = {
        { 0,  3,  COL_RED,   "VK lost (depth), GL background survived" },
        { 4,  7,  COL_GREEN, "VK lost (depth), GL terrain survived" },
        { 8, 11,  COL_BLUE,  "VK won (depth), overwrote GL terrain in-place" },
        {12, 15,  COL_BLUE,  "VK won (depth), overwrote GL background in-place" },
    };
    bool pass = true;
    for (const Band& b : bands) {
        for (uint32_t y = 0; y < TEX_H; ++y) {
            for (uint32_t x = b.x0; x <= b.x1; ++x) {
                const float* p = &rgba[(y*TEX_W + x)*4];
                if (!colorIs(p, b.want)) {
                    pass = false;
                    LOG("[ASSERT-FAIL] texel (%u,%u) = %s (%.2f,%.2f,%.2f) expected %s  [%s]",
                        x, y, colName(p), p[0], p[1], p[2], colName(b.want), b.why);
                }
            }
        }
    }
    *outPass = pass;
    if (pass) LOG("[ASSERT-1 PASS] in-place GL<->VK color co-write correct: RED|GREEN|BLUE|BLUE bands, no clobber, no re-clear.");
    else      LOG("[ASSERT-1 FAIL] co-write pattern wrong (clobber / lost-frag-overwrote / bg-lost).");
    return true;
}

static void cleanup(App& a, Shared& s, VkPipe& p) {
    if (a.dev) vkDeviceWaitIdle(a.dev);
    if (s.glFbo) glDeleteFramebuffers(1,&s.glFbo);
    if (s.glColorTex) glDeleteTextures(1,&s.glColorTex);
    if (s.glDepthTex) glDeleteTextures(1,&s.glDepthTex);
    if (s.glColorMemObj) glDeleteMemoryObjectsEXT(1,&s.glColorMemObj);
    if (s.glDepthMemObj) glDeleteMemoryObjectsEXT(1,&s.glDepthMemObj);
    if (s.glSemGlToVk) glDeleteSemaphoresEXT(1,&s.glSemGlToVk);
    if (s.glSemVkToGl) glDeleteSemaphoresEXT(1,&s.glSemVkToGl);
    if (p.pipe) vkDestroyPipeline(a.dev,p.pipe,nullptr);
    if (p.layout) vkDestroyPipelineLayout(a.dev,p.layout,nullptr);
    if (p.fb) vkDestroyFramebuffer(a.dev,p.fb,nullptr);
    if (p.rp) vkDestroyRenderPass(a.dev,p.rp,nullptr);
    if (p.vs) vkDestroyShaderModule(a.dev,p.vs,nullptr);
    if (p.fs) vkDestroyShaderModule(a.dev,p.fs,nullptr);
    if (s.colorView) vkDestroyImageView(a.dev,s.colorView,nullptr);
    if (s.depthView) vkDestroyImageView(a.dev,s.depthView,nullptr);
    if (s.colorImg) vkDestroyImage(a.dev,s.colorImg,nullptr);
    if (s.colorMem) vkFreeMemory(a.dev,s.colorMem,nullptr);
    if (s.depthImg) vkDestroyImage(a.dev,s.depthImg,nullptr);
    if (s.depthMem) vkFreeMemory(a.dev,s.depthMem,nullptr);
    if (s.semGlToVk) vkDestroySemaphore(a.dev,s.semGlToVk,nullptr);
    if (s.semVkToGl) vkDestroySemaphore(a.dev,s.semVkToGl,nullptr);
    if (a.cmdPool) vkDestroyCommandPool(a.dev,a.cmdPool,nullptr);
    if (a.dev) vkDestroyDevice(a.dev,nullptr);
    if (a.messenger && a.pDestroyDbg) a.pDestroyDbg(a.inst,a.messenger,nullptr);
    if (a.inst) vkDestroyInstance(a.inst,nullptr);
    if (a.glctx) SDL_GL_DeleteContext(a.glctx);
    if (a.win) SDL_DestroyWindow(a.win);
    SDL_Quit();
}

int main(int, char**) {
    const char* neg = std::getenv("MC2_COLORCOWRITE_NEGCTL");
    if (neg && !std::strcmp(neg, "compare")) g_neg = NEG_COMPARE;
    else if (neg && !std::strcmp(neg, "clear")) g_neg = NEG_CLEAR;

    LOG("==============================================================");
    LOG(" GL<->VK IN-PLACE shared RGBA16F COLOR co-write w/ depth-test");
    LOG("   STOP-gate for the shared-GBuffer endgame (props/terrain/mechs).");
    LOG("   reverse-Z: clear 0.0=far, GEQUAL; GL near=%.2f VK test=%.2f",
        GL_NEAR_DEPTH, VK_TEST_DEPTH);
    LOG("   negative-control mode = %s%s", negName(g_neg),
        g_neg==NEG_NONE ? " (real run; asserts MUST pass)" : " (asserts MUST fail)");
    LOG("==============================================================");

    App a; Shared s; VkPipe p;
    bool assert1 = false;
    float preColor[TEX_W*TEX_H*4]  = {0};
    float postColor[TEX_W*TEX_H*4] = {0};

    bool built =
        initGL(a) && initVk(a) &&
        createSharedColor(a, s) && createSharedDepth(a, s) &&
        createSemaphores(a, s) &&
        importIntoGL(a, s) &&
        glAuthorFrame(a, s);

    // Self-check GL authoring BEFORE handing to VK. NOTE: this readback is a
    // SETUP diagnostic taken while GL still owns the image, BEFORE the co-write
    // handoff — it is not on the co-write path and does not count as a mid-path
    // readback. (The forbidden-counter guards the GL-author -> VK -> assert span.)
    if (built) glSampleColor(s, preColor, "GL-authored (pre-VK)");

    built = built &&
        buildVkPipeline(a, s, p) &&
        vkCoWriteDraw(a, s, p);

    if (built) {
        // VK->GL: GL waits, then reads back the shared color for the assert.
        // This is the FINAL readback (the frame result), not a mid-path one.
        GLuint texs[2]    = { s.glColorTex, s.glDepthTex };
        GLenum layouts[2] = { GL_LAYOUT_COLOR_ATTACHMENT_EXT, GL_LAYOUT_DEPTH_STENCIL_ATTACHMENT_EXT };
        glWaitSemaphoreEXT(s.glSemVkToGl, 0, nullptr, 2, texs, layouts);
        glCheck("glWaitSemaphoreEXT vk->gl");
        if (glSampleColor(s, postColor, "post-VK co-write (final)")) {
            assertCoWrite(postColor, &assert1);
        }
    }

    LOG("");
    LOG("--------------------------------------------------------------");
    LOG("[TALLY] VK validation errors      = %d", g_valErrors);
    LOG("[TALLY] GL debug errors seen      = %d", g_glDebugSawError ? 1 : 0);
    LOG("[TALLY] forbidden mid-path readbacks = %d (must be 0)", g_readbacksMidPath);
    LOG("[TALLY] forbidden color re-clears    = %d (must be 0 on real path)", g_colorReclears);
    LOG("[RESULT] ASSERT-1 in-place color co-write = %s", assert1 ? "PASS" : "FAIL");

    bool clean = (g_valErrors == 0) && !g_glDebugSawError;
    bool forbiddenClean = (g_readbacksMidPath == 0) && (g_colorReclears == 0);

    int rc;
    if (g_neg != NEG_NONE) {
        // Negative control: the co-write MUST be detected as broken.
        //  - NEG_COMPARE: asserts must FAIL (assert1==false).
        //  - NEG_CLEAR:   asserts must FAIL AND a forbidden re-clear was counted.
        bool detected = built && !assert1;
        if (g_neg == NEG_CLEAR) detected = detected && (g_colorReclears > 0);
        if (detected) {
            LOG("VERDICT: NEG-CONTROL OK — broken co-write (%s) correctly DETECTED (assert failed as intended).",
                negName(g_neg));
            rc = 0;
        } else {
            LOG("VERDICT: NEG-CONTROL BROKEN — harness did NOT detect the injected fault (%s).",
                negName(g_neg));
            LOG("         (built=%d assert1=%d reclears=%d) The harness cannot be trusted.",
                built?1:0, assert1?1:0, g_colorReclears);
            rc = 2;
        }
    } else {
        bool allPass = built && assert1 && clean && forbiddenClean;
        if (allPass) {
            LOG("VERDICT: STOP-GATE GREEN — GL and VK co-write the SAME shared RGBA16F color");
            LOG("         in-place under one depth-test: VK winners overwrote, losers left GL");
            LOG("         intact, background survived, no re-clear, validation clean.");
            LOG("         Shared-GBuffer geometry-color endgame UNBLOCKED.");
            rc = 0;
        } else {
            LOG("VERDICT: STOP-GATE RED — see the failing assert/guard above (CRITICAL FINDING).");
            LOG("         (built=%d assert1=%d valClean=%d forbiddenClean=%d)",
                built?1:0, assert1?1:0, clean?1:0, forbiddenClean?1:0);
            if (!built) LOG("         setup/draw failed at stage %d: %s", g_failStage, g_failMsg);
            rc = 1;
        }
    }
    LOG("==============================================================");
    cleanup(a, s, p);
    return rc;
}
