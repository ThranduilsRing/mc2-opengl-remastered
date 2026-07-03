// =============================================================================
// Vulkan -> OpenGL shared DEPTH-image interop SPIKE   (T0, VK-INTEROP-BRIDGE-1)
//
// De-risks a FUTURE depth zero-copy slice. Standalone. Ships nothing into game.
//
// PROVES (or disproves) zero-copy VK depth-image -> GL depth texture on this HW:
//   1. Vulkan device + GL 4.3 core context (SDL2), both validation/debug on.
//   2. Create a VK_FORMAT_D32_SFLOAT image, VK_IMAGE_USAGE_SAMPLED_BIT (+TRANSFER_DST
//      for the clear), exportable dedicated memory as an opaque Win32 handle.
//   3. Import handle into GL via GL_EXT_memory_object(_win32) +
//      glTextureStorageMem2DEXT with GL_DEPTH_COMPONENT32F, import a semaphore.
//   4. ONE direction, zero-copy, synchronized:
//        Vulkan writes a known depth (vkCmdClearDepthStencilImage) -> GL samples
//        (glGetTextureImage GL_DEPTH_COMPONENT / GL_FLOAT) -> verify.
//   5. No validation errors, no GL debug errors.
//
// This is the exact export/import/LUID/semaphore sequence from main.cpp, changed
// ONLY where the depth aspect / depth format demands it. Color spike untouched.
//
// Exit 0 == DEPTH-EXPORT VIABLE (round-trips correctly, validation-clean).
// Non-zero == NOT VIABLE, with the exact failing stage printed.
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

// ---- image / depth constants ------------------------------------------------
static const uint32_t TEX_W = 4;
static const uint32_t TEX_H = 4;
static const VkFormat IMG_FORMAT = VK_FORMAT_D32_SFLOAT;

// Vulkan clears this known depth value. Chosen to be an exact float32 with no
// round-trip ambiguity (0.375 = 0x3EC00000, representable exactly).
static const float DEPTH_VALUE = 0.375f;
static const float DEPTH_EPS   = 1e-6f; // D32_SFLOAT is exact; keep a tiny slack.

// ---- tiny logging + verdict tracking ---------------------------------------
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
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
        default: { static char b[32]; std::snprintf(b, 32, "VkResult(%d)", (int)r); return b; }
    }
}
#define VKCHECK(expr, stage, what) do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) return fail((stage), "%s -> %s", (what), vkStr(_r)); \
} while(0)

// ---- Vulkan validation callback --------------------------------------------
static VKAPI_ATTR VkBool32 VKAPI_CALL vkDbg(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* d,
    void*)
{
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG("[VK-VALIDATION %s] %s",
            (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : "WARN",
            d->pMessage ? d->pMessage : "(null)");
        if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) g_valErrors++;
    }
    return VK_FALSE;
}

// ---- GL debug callback ------------------------------------------------------
static void APIENTRY glDbg(GLenum, GLenum type, GLuint, GLenum sev,
                           GLsizei, const GLchar* msg, const void*)
{
    if (type == GL_DEBUG_TYPE_ERROR || sev == GL_DEBUG_SEVERITY_HIGH) {
        LOG("[GL-DEBUG ERROR] %s", msg ? msg : "(null)");
        g_glDebugSawError = true;
    }
}
static bool glCheck(const char* where) {
    GLenum e; bool bad = false;
    while ((e = glGetError()) != GL_NO_ERROR) {
        LOG("[GL-ERROR 0x%04X] at %s", e, where);
        bad = true;
    }
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

    VkImage          image = VK_NULL_HANDLE;
    VkDeviceMemory   imageMem = VK_NULL_HANDLE;
    VkDeviceSize     memSize = 0;
    HANDLE           memWin32 = nullptr;
    uint8_t          devLuid[VK_LUID_SIZE] = {0};
    bool             haveLuid = false;

    VkSemaphore      semVkToGl = VK_NULL_HANDLE;
    HANDLE           semVkToGlWin32 = nullptr;

    GLuint           glMemObj = 0;
    GLuint           glTex = 0;
    GLuint           glSemVkToGl = 0;

    PFN_vkGetMemoryWin32HandleKHR    pGetMemWin32 = nullptr;
    PFN_vkGetSemaphoreWin32HandleKHR pGetSemWin32 = nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT  pCreateDbg = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT pDestroyDbg = nullptr;
};

static bool findMemType(App& a, uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t* out) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(a.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) {
            *out = i; return true;
        }
    }
    return false;
}

// =============================================================================
// Stage 1: SDL window + GL 4.3 core context + GLEW + GL debug
// =============================================================================
static bool initGL(App& a) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        return fail(1, "SDL_Init: %s", SDL_GetError());

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    a.win = SDL_CreateWindow("vk_gl_interop_depth_spike",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!a.win) return fail(1, "SDL_CreateWindow: %s", SDL_GetError());

    a.glctx = SDL_GL_CreateContext(a.win);
    if (!a.glctx) return fail(1, "SDL_GL_CreateContext (need GL4.3 core): %s", SDL_GetError());

    glewExperimental = GL_TRUE;
    GLenum ge = glewInit();
    if (ge != GLEW_OK) return fail(1, "glewInit: %s", (const char*)glewGetErrorString(ge));
    glGetError();

    LOG("[GL] vendor=%s", (const char*)glGetString(GL_VENDOR));
    LOG("[GL] renderer=%s", (const char*)glGetString(GL_RENDERER));
    LOG("[GL] version=%s", (const char*)glGetString(GL_VERSION));

    if (GLEW_KHR_debug || GLEW_ARB_debug_output) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDbg, nullptr);
    }

    LOG("[GL] GL_EXT_memory_object       = %d", GLEW_EXT_memory_object ? 1 : 0);
    LOG("[GL] GL_EXT_memory_object_win32 = %d", GLEW_EXT_memory_object_win32 ? 1 : 0);
    LOG("[GL] GL_EXT_semaphore           = %d", GLEW_EXT_semaphore ? 1 : 0);
    LOG("[GL] GL_EXT_semaphore_win32     = %d", GLEW_EXT_semaphore_win32 ? 1 : 0);

    if (!GLEW_EXT_memory_object || !GLEW_EXT_memory_object_win32)
        return fail(1, "GL driver lacks GL_EXT_memory_object[_win32] -> zero-copy import impossible");
    if (!GLEW_EXT_semaphore || !GLEW_EXT_semaphore_win32)
        return fail(1, "GL driver lacks GL_EXT_semaphore[_win32] -> cross-API sync impossible");

    return glCheck("initGL");
}

// =============================================================================
// Stage 2: Vulkan instance (+validation) and device (+external-memory exts)
// =============================================================================
static bool initVk(App& a) {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool haveValidation = false;
    for (auto& l : layers)
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) haveValidation = true;
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
    ai.pApplicationName = "vk_gl_interop_depth_spike";
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

    VKCHECK(vkCreateInstance(&ici, nullptr, &a.inst), 2, "vkCreateInstance");

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
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pd, &p);
        LOG("[VK] found device: %s (type %d)", p.deviceName, (int)p.deviceType);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) a.phys = pd;
    }
    {
        VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(a.phys, &p);
        LOG("[VK] using device: %s", p.deviceName);

        VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &idp;
        vkGetPhysicalDeviceProperties2(a.phys, &p2);
        if (idp.deviceLUIDValid) {
            std::memcpy(a.devLuid, idp.deviceLUID, VK_LUID_SIZE);
            a.haveLuid = true;
            std::string h;
            for (int i = 0; i < VK_LUID_SIZE; ++i) { char b[4]; std::snprintf(b,4,"%02x", idp.deviceLUID[i]); h += b; }
            LOG("[VK] device LUID = %s (valid)", h.c_str());
        } else {
            LOG("[VK] device LUID not valid (single-GPU box: interop still fine)");
        }
    }

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
        for (auto& e : des) if (std::strcmp(e.extensionName, need) == 0) ok = true;
        LOG("[VK] device ext %-48s = %d", need, ok ? 1 : 0);
        if (!ok) return fail(2, "device missing required extension: %s", need);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = a.qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dvci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dvci.queueCreateInfoCount = 1; dvci.pQueueCreateInfos = &qci;
    dvci.enabledExtensionCount = (uint32_t)devExts.size();
    dvci.ppEnabledExtensionNames = devExts.data();
    VKCHECK(vkCreateDevice(a.phys, &dvci, nullptr, &a.dev), 2, "vkCreateDevice");

    vkGetDeviceQueue(a.dev, a.qfam, 0, &a.queue);

    a.pGetMemWin32 = (PFN_vkGetMemoryWin32HandleKHR)    vkGetDeviceProcAddr(a.dev, "vkGetMemoryWin32HandleKHR");
    a.pGetSemWin32 = (PFN_vkGetSemaphoreWin32HandleKHR) vkGetDeviceProcAddr(a.dev, "vkGetSemaphoreWin32HandleKHR");
    if (!a.pGetMemWin32 || !a.pGetSemWin32)
        return fail(2, "vkGetMemory/SemaphoreWin32HandleKHR entry points null");

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = a.qfam;
    VKCHECK(vkCreateCommandPool(a.dev, &cpi, nullptr, &a.cmdPool), 2, "vkCreateCommandPool");

    return true;
}

// =============================================================================
// Stage 3: create exportable DEPTH VkImage + dedicated exported memory + handle
// =============================================================================
static bool createSharedImage(App& a) {
    const VkExternalMemoryHandleTypeFlagBits handleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    // The depth image is SAMPLED (per T0) + TRANSFER_DST so we can clear it with
    // vkCmdClearDepthStencilImage. (SAMPLED is the portability-risky bit for depth.)
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // sanity: is a D32_SFLOAT SAMPLED image exportable via OPAQUE_WIN32 here?
    VkPhysicalDeviceExternalImageFormatInfo eifi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    eifi.handleType = handleType;
    VkPhysicalDeviceImageFormatInfo2 ifi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    ifi.pNext = &eifi;
    ifi.format = IMG_FORMAT;
    ifi.type = VK_IMAGE_TYPE_2D;
    ifi.tiling = VK_IMAGE_TILING_OPTIMAL;
    ifi.usage = usage;
    VkExternalImageFormatProperties eifp{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ifp2{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    ifp2.pNext = &eifp;
    VkResult fr = vkGetPhysicalDeviceImageFormatProperties2(a.phys, &ifi, &ifp2);
    if (fr != VK_SUCCESS)
        return fail(3, "D32_SFLOAT SAMPLED image not supported for external export: %s", vkStr(fr));
    VkExternalMemoryFeatureFlags feat = eifp.externalMemoryProperties.externalMemoryFeatures;
    LOG("[VK] external DEPTH image features: exportable=%d importable=%d dedicated-required=%d",
        (feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ? 1:0,
        (feat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? 1:0,
        (feat & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ? 1:0);
    if (!(feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
        return fail(3, "OPAQUE_WIN32 export not supported for D32_SFLOAT SAMPLED config");

    VkExternalMemoryImageCreateInfo emici{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes = handleType;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = &emici;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = IMG_FORMAT;
    ci.extent = { TEX_W, TEX_H, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(a.dev, &ci, nullptr, &a.image), 3, "vkCreateImage(external depth)");

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(a.dev, a.image, &mr);
    a.memSize = mr.size;

    uint32_t memIdx;
    if (!findMemType(a, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memIdx))
        return fail(3, "no DEVICE_LOCAL memory type for shared depth image");

    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = a.image;

    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext = &ded;
    exp.handleTypes = handleType;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &exp;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memIdx;
    VKCHECK(vkAllocateMemory(a.dev, &mai, nullptr, &a.imageMem), 3, "vkAllocateMemory(exported depth)");
    VKCHECK(vkBindImageMemory(a.dev, a.image, a.imageMem, 0), 3, "vkBindImageMemory");

    VkMemoryGetWin32HandleInfoKHR gh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    gh.memory = a.imageMem;
    gh.handleType = handleType;
    VKCHECK(a.pGetMemWin32(a.dev, &gh, &a.memWin32), 3, "vkGetMemoryWin32HandleKHR");
    LOG("[VK] exported depth memory Win32 handle=%p size=%llu", a.memWin32, (unsigned long long)a.memSize);

    return true;
}

// =============================================================================
// Stage 3b: one exportable binary semaphore (vk->gl) + Win32 handle
// =============================================================================
static bool createSharedSemaphores(App& a) {
    const VkExternalSemaphoreHandleTypeFlagBits ht =
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    esci.handleTypes = ht;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &esci;
    VKCHECK(vkCreateSemaphore(a.dev, &sci, nullptr, &a.semVkToGl), 3, "vkCreateSemaphore(exported)");
    VkSemaphoreGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
    gi.semaphore = a.semVkToGl; gi.handleType = ht;
    VKCHECK(a.pGetSemWin32(a.dev, &gi, &a.semVkToGlWin32), 3, "vkGetSemaphoreWin32HandleKHR");
    LOG("[VK] exported semaphore 'vk->gl' handle=%p", a.semVkToGlWin32);
    return true;
}

// =============================================================================
// Stage 4: import memory + semaphore into GL; create GL_DEPTH_COMPONENT32F tex
// =============================================================================
static bool importIntoGL(App& a) {
    glCreateMemoryObjectsEXT(1, &a.glMemObj);
    if (!glCheck("glCreateMemoryObjectsEXT")) return fail(4, "glCreateMemoryObjectsEXT");

    GLint dedicated = GL_TRUE;
    glMemoryObjectParameterivEXT(a.glMemObj, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    glCheck("glMemoryObjectParameterivEXT");

    glImportMemoryWin32HandleEXT(a.glMemObj, a.memSize,
                                 GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, a.memWin32);
    if (!glCheck("glImportMemoryWin32HandleEXT"))
        return fail(4, "glImportMemoryWin32HandleEXT (handle import rejected by GL driver)");

    glCreateTextures(GL_TEXTURE_2D, 1, &a.glTex);
    glTextureParameteri(a.glTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(a.glTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(a.glTex, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    // THE portability question: GL_DEPTH_COMPONENT32F over imported OPTIMAL-tiled mem.
    glTextureStorageMem2DEXT(a.glTex, 1, GL_DEPTH_COMPONENT32F, TEX_W, TEX_H, a.glMemObj, 0);
    if (!glCheck("glTextureStorageMem2DEXT"))
        return fail(4, "glTextureStorageMem2DEXT GL_DEPTH_COMPONENT32F (depth external image REJECTED)");

    glGenSemaphoresEXT(1, &a.glSemVkToGl);
    glImportSemaphoreWin32HandleEXT(a.glSemVkToGl, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, a.semVkToGlWin32);
    if (!glCheck("glImportSemaphoreWin32HandleEXT vk->gl")) return fail(4, "import sem vk->gl");

    LOG("[GL] imported depth memory+texture+semaphore (zero-copy backing established)");
    return true;
}

// ---- vk helper: one-shot command buffer submit ------------------------------
static bool vkSubmitOneShot(App& a, int stage,
                            void (*record)(VkCommandBuffer, App&),
                            VkSemaphore waitSem, VkPipelineStageFlags waitStage,
                            VkSemaphore signalSem)
{
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = a.cmdPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VKCHECK(vkAllocateCommandBuffers(a.dev, &ai, &cb), stage, "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(cb, &bi), stage, "vkBeginCommandBuffer");
    record(cb, a);
    VKCHECK(vkEndCommandBuffer(cb), stage, "vkEndCommandBuffer");

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (waitSem) { si.waitSemaphoreCount = 1; si.pWaitSemaphores = &waitSem; si.pWaitDstStageMask = &waitStage; }
    if (signalSem) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &signalSem; }
    VKCHECK(vkQueueSubmit(a.queue, 1, &si, VK_NULL_HANDLE), stage, "vkQueueSubmit");
    return true;
}

// =============================================================================
// Direction A: Vulkan clears depth -> GL samples/reads -> verify
// =============================================================================
static bool directionA(App& a) {
    LOG("--- Direction A: Vulkan writes depth, GL reads ---");

    struct Rec { static void fn(VkCommandBuffer cb, App& a) {
        // UNDEFINED -> TRANSFER_DST for the depth clear. DEPTH aspect throughout.
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.image = a.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,0,nullptr,0,nullptr,1,&b);

        VkClearDepthStencilValue c{};
        c.depth = DEPTH_VALUE; c.stencil = 0;
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
        vkCmdClearDepthStencilImage(cb, a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);

        // release to the layout GL will consume it in (SHADER_READ_ONLY)
        VkImageMemoryBarrier b2 = b;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0,0,nullptr,0,nullptr,1,&b2);
    }};

    if (!vkSubmitOneShot(a, 5, Rec::fn, VK_NULL_HANDLE, 0, a.semVkToGl)) return false;

    GLenum layout = GL_LAYOUT_SHADER_READ_ONLY_EXT;
    glWaitSemaphoreEXT(a.glSemVkToGl, 0, nullptr, 1, &a.glTex, &layout);
    if (!glCheck("glWaitSemaphoreEXT (A)")) return fail(5, "glWaitSemaphoreEXT");

    // Read the depth texels back in GL as float.
    float px[TEX_W*TEX_H] = {0};
    glGetTextureImage(a.glTex, 0, GL_DEPTH_COMPONENT, GL_FLOAT, sizeof(px), px);
    if (!glCheck("glGetTextureImage (A, depth)")) return fail(5, "glGetTextureImage depth");

    LOG("[A] GL read depth[0] = %.8f  (expected %.8f)", px[0], DEPTH_VALUE);

    for (uint32_t i = 0; i < TEX_W*TEX_H; ++i) {
        if (std::fabs(px[i] - DEPTH_VALUE) > DEPTH_EPS)
            return fail(5, "Direction A depth mismatch at texel %u: got %.8f exp %.8f",
                        i, px[i], DEPTH_VALUE);
    }
    LOG("[A] PASS: all %u depth texels match %.8f (Vulkan->GL depth zero-copy verified)",
        TEX_W*TEX_H, DEPTH_VALUE);
    return true;
}

static void cleanup(App& a) {
    if (a.dev) vkDeviceWaitIdle(a.dev);
    if (a.glTex) glDeleteTextures(1,&a.glTex);
    if (a.glMemObj) glDeleteMemoryObjectsEXT(1,&a.glMemObj);
    if (a.glSemVkToGl) glDeleteSemaphoresEXT(1,&a.glSemVkToGl);
    if (a.semVkToGl) vkDestroySemaphore(a.dev,a.semVkToGl,nullptr);
    if (a.image) vkDestroyImage(a.dev,a.image,nullptr);
    if (a.imageMem) vkFreeMemory(a.dev,a.imageMem,nullptr);
    if (a.cmdPool) vkDestroyCommandPool(a.dev,a.cmdPool,nullptr);
    if (a.dev) vkDestroyDevice(a.dev,nullptr);
    if (a.messenger && a.pDestroyDbg) a.pDestroyDbg(a.inst,a.messenger,nullptr);
    if (a.inst) vkDestroyInstance(a.inst,nullptr);
    if (a.glctx) SDL_GL_DeleteContext(a.glctx);
    if (a.win) SDL_DestroyWindow(a.win);
    SDL_Quit();
}

int main(int, char**) {
    LOG("========================================================");
    LOG(" VK -> GL zero-copy DEPTH interop SPIKE (T0, D32_SFLOAT)");
    LOG("========================================================");
    App a;
    bool ok =
        initGL(a) &&
        initVk(a) &&
        createSharedImage(a) &&
        createSharedSemaphores(a) &&
        importIntoGL(a) &&
        directionA(a);

    LOG("--------------------------------------------------------");
    LOG("[TALLY] VK validation errors = %d", g_valErrors);
    LOG("[TALLY] GL debug errors seen = %d", g_glDebugSawError ? 1 : 0);

    bool clean = (g_valErrors == 0) && !g_glDebugSawError;
    int rc;
    if (ok && clean) {
        LOG("VERDICT: DEPTH-EXPORT VIABLE  (VK->GL depth zero-copy round-trips, validation-clean)");
        rc = 0;
    } else if (ok && !clean) {
        LOG("VERDICT: PARTIAL  (depth verified BUT validation/debug errors present)");
        rc = 2;
    } else {
        LOG("VERDICT: NOT VIABLE  (stage %d: %s)", g_failStage, g_failMsg);
        rc = 1;
    }
    LOG("========================================================");
    cleanup(a);
    return rc;
}
