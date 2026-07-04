// =============================================================================
// Vulkan -> OpenGL shared PACKED DEPTH-STENCIL interop SPIKE
//   P5-T0'  (VK-INTEROP-BRIDGE-1, GO/NO-GO for piece P5: postprocess depth
//   zero-copy).
//
// De-risks the REAL engine boundary that T0 did NOT: the scene depth texture is
// GL_DEPTH24_STENCIL8 (packed depth-stencil), not D32_SFLOAT. Readback works
// today only because glGetTexImage(GL_DEPTH_COMPONENT, GL_FLOAT) CONVERTS D24S8
// -> float32. Zero-copy aliases raw bytes and cannot convert, so P5 needs a
// PACKED depth-stencil external image: VkFormat VK_FORMAT_D24_UNORM_S8_UINT
// (fallback VK_FORMAT_D32_SFLOAT_S8_UINT) shared with a GL GL_DEPTH24_STENCIL8
// texture. AMD packed depth-stencil export is historically shakier than plain
// depth; this spike settles it on the 7900 XTX. Standalone; ships nothing.
//
// For EACH candidate format (D24S8 first, D32S8 fallback), the spike:
//   1. PROBES exportability via vkGetPhysicalDeviceImageFormatProperties2 with
//      the external-memory handle type + real usage. Reports exportable /
//      importable / dedicated-required BEFORE trying — so a non-shareable
//      format is a clean negative, not a crash.
//   2. Creates an exportable VkImage (aspect DEPTH+STENCIL, DEDICATED alloc,
//      OPAQUE_WIN32 export, usage SAMPLED + DEPTH_STENCIL_ATTACHMENT +
//      TRANSFER_DST), exports the Win32 handle.
//   3. Imports into GL as a GL_DEPTH24_STENCIL8 texture via
//      glCreateMemoryObjectsEXT / glImportMemoryWin32HandleEXT /
//      glTextureStorageMem2DEXT. If that GL storage call fails -> KEY negative
//      result (GL error captured).
//   4. ONE direction: VK clears depth=0.5 stencil=0 into the image; GL samples
//      the DEPTH aspect (GL_DEPTH_STENCIL_TEXTURE_MODE = GL_DEPTH_COMPONENT),
//      reads back, verifies within D24 quantization (~1/2^24). Reports the raw
//      sampled value + whether it matches.
//
// Exit 0 == at least one packed depth-stencil format is DEPTH-STENCIL EXPORT
//           VIABLE (round-trips, validation-clean)  -> P5 greenlit.
// Non-zero == NOT VIABLE for every candidate -> P5 blocked at the GL-produced
//            D24S8 boundary (see the printed per-format failing stage).
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

// Vulkan clears this known depth. 0.5 is exactly representable in D24 UNORM
// (0.5 * (2^24-1) = 8388607.5, quantizes to 8388608/16777215 ~= 0.50000003).
static const float DEPTH_VALUE = 0.5f;
// D24 UNORM quantization step is 1/(2^24-1) ~= 5.96e-8; allow a few steps slack
// plus GL's normalized-read rounding. 1e-5 is comfortably tight yet safe.
static const float DEPTH_EPS   = 1e-5f;

// GL internalformat we import the packed depth-stencil memory as.
#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL_TEXTURE_MODE
#define GL_DEPTH_STENCIL_TEXTURE_MODE 0x90EA
#endif

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

    PFN_vkGetMemoryWin32HandleKHR    pGetMemWin32 = nullptr;
    PFN_vkGetSemaphoreWin32HandleKHR pGetSemWin32 = nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT  pCreateDbg = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT pDestroyDbg = nullptr;
};

// Per-format run state (recreated for each candidate).
struct Res {
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory imageMem = VK_NULL_HANDLE;
    VkDeviceSize   memSize = 0;
    HANDLE         memWin32 = nullptr;
    VkSemaphore    semVkToGl = VK_NULL_HANDLE;
    HANDLE         semVkToGlWin32 = nullptr;
    GLuint         glMemObj = 0;
    GLuint         glTex = 0;
    GLuint         glSemVkToGl = 0;
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

    a.win = SDL_CreateWindow("vk_gl_interop_d24s8_spike",
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
    ai.pApplicationName = "vk_gl_interop_d24s8_spike";
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

    if (vkCreateInstance(&ici, nullptr, &a.inst) != VK_SUCCESS)
        return fail(2, "vkCreateInstance");

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
    if (vkCreateDevice(a.phys, &dvci, nullptr, &a.dev) != VK_SUCCESS)
        return fail(2, "vkCreateDevice");

    vkGetDeviceQueue(a.dev, a.qfam, 0, &a.queue);

    a.pGetMemWin32 = (PFN_vkGetMemoryWin32HandleKHR)    vkGetDeviceProcAddr(a.dev, "vkGetMemoryWin32HandleKHR");
    a.pGetSemWin32 = (PFN_vkGetSemaphoreWin32HandleKHR) vkGetDeviceProcAddr(a.dev, "vkGetSemaphoreWin32HandleKHR");
    if (!a.pGetMemWin32 || !a.pGetSemWin32)
        return fail(2, "vkGetMemory/SemaphoreWin32HandleKHR entry points null");

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = a.qfam;
    if (vkCreateCommandPool(a.dev, &cpi, nullptr, &a.cmdPool) != VK_SUCCESS)
        return fail(2, "vkCreateCommandPool");

    return true;
}

// =============================================================================
// PROBE: report external-image support bits for `fmt` BEFORE trying to create.
// Returns true if the format is exportable (so creation is worth attempting).
// The usage MUST match the create usage below (props are usage-specific).
// =============================================================================
static const VkImageUsageFlags kDSUsage =
    VK_IMAGE_USAGE_SAMPLED_BIT |
    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT;

static bool probeFormat(App& a, VkFormat fmt, const char* name, bool* exportable) {
    *exportable = false;
    const VkExternalMemoryHandleTypeFlagBits handleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkPhysicalDeviceExternalImageFormatInfo eifi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    eifi.handleType = handleType;
    VkPhysicalDeviceImageFormatInfo2 ifi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    ifi.pNext = &eifi;
    ifi.format = fmt;
    ifi.type = VK_IMAGE_TYPE_2D;
    ifi.tiling = VK_IMAGE_TILING_OPTIMAL;
    ifi.usage = kDSUsage;
    VkExternalImageFormatProperties eifp{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ifp2{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    ifp2.pNext = &eifp;
    VkResult fr = vkGetPhysicalDeviceImageFormatProperties2(a.phys, &ifi, &ifp2);
    if (fr != VK_SUCCESS) {
        LOG("[PROBE %s] vkGetPhysicalDeviceImageFormatProperties2 -> %s "
            "(format+usage not supported for external export)", name, vkStr(fr));
        return true; // probe itself ran; just not exportable
    }
    VkExternalMemoryFeatureFlags feat = eifp.externalMemoryProperties.externalMemoryFeatures;
    bool exp = (feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
    bool imp = (feat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
    bool ded = (feat & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;
    LOG("[PROBE %s] external DS-image features: exportable=%d importable=%d dedicated-required=%d",
        name, exp?1:0, imp?1:0, ded?1:0);
    *exportable = exp;
    return true;
}

// =============================================================================
// Create exportable packed DS VkImage + dedicated exported memory + handle,
// plus one exportable binary semaphore (vk->gl).
// =============================================================================
static bool createSharedImage(App& a, Res& s, VkFormat fmt, const char* name) {
    const VkExternalMemoryHandleTypeFlagBits handleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    VkExternalMemoryImageCreateInfo emici{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes = handleType;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = &emici;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = { TEX_W, TEX_H, 1 };
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = kDSUsage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(a.dev, &ci, nullptr, &s.image) != VK_SUCCESS)
        return fail(3, "%s vkCreateImage(external DS)", name);

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(a.dev, s.image, &mr);
    s.memSize = mr.size;

    uint32_t memIdx;
    if (!findMemType(a, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memIdx))
        return fail(3, "%s no DEVICE_LOCAL memory type for shared DS image", name);

    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = s.image;
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext = &ded;
    exp.handleTypes = handleType;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &exp;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memIdx;
    if (vkAllocateMemory(a.dev, &mai, nullptr, &s.imageMem) != VK_SUCCESS)
        return fail(3, "%s vkAllocateMemory(exported DS)", name);
    if (vkBindImageMemory(a.dev, s.image, s.imageMem, 0) != VK_SUCCESS)
        return fail(3, "%s vkBindImageMemory", name);

    VkMemoryGetWin32HandleInfoKHR gh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    gh.memory = s.imageMem;
    gh.handleType = handleType;
    if (a.pGetMemWin32(a.dev, &gh, &s.memWin32) != VK_SUCCESS)
        return fail(3, "%s vkGetMemoryWin32HandleKHR", name);
    LOG("[VK] %s exported DS memory Win32 handle=%p size=%llu",
        name, s.memWin32, (unsigned long long)s.memSize);

    const VkExternalSemaphoreHandleTypeFlagBits sht =
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    esci.handleTypes = sht;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &esci;
    if (vkCreateSemaphore(a.dev, &sci, nullptr, &s.semVkToGl) != VK_SUCCESS)
        return fail(3, "%s vkCreateSemaphore(exported)", name);
    VkSemaphoreGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
    gi.semaphore = s.semVkToGl; gi.handleType = sht;
    if (a.pGetSemWin32(a.dev, &gi, &s.semVkToGlWin32) != VK_SUCCESS)
        return fail(3, "%s vkGetSemaphoreWin32HandleKHR", name);
    LOG("[VK] %s exported semaphore 'vk->gl' handle=%p", name, s.semVkToGlWin32);
    return true;
}

// =============================================================================
// Import memory + semaphore into GL; create GL_DEPTH24_STENCIL8 texture.
// glTextureStorageMem2DEXT with GL_DEPTH24_STENCIL8 over imported OPTIMAL-tiled
// memory is THE decisive GL-side question for P5.
// =============================================================================
static bool importIntoGL(App& a, Res& s, const char* name) {
    glCreateMemoryObjectsEXT(1, &s.glMemObj);
    if (!glCheck("glCreateMemoryObjectsEXT")) return fail(4, "%s glCreateMemoryObjectsEXT", name);

    GLint dedicated = GL_TRUE;
    glMemoryObjectParameterivEXT(s.glMemObj, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    glCheck("glMemoryObjectParameterivEXT");

    glImportMemoryWin32HandleEXT(s.glMemObj, s.memSize,
                                 GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, s.memWin32);
    if (!glCheck("glImportMemoryWin32HandleEXT"))
        return fail(4, "%s glImportMemoryWin32HandleEXT (handle import rejected by GL driver)", name);

    glCreateTextures(GL_TEXTURE_2D, 1, &s.glTex);
    glTextureParameteri(s.glTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(s.glTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(s.glTex, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    // Sample the DEPTH aspect of the packed depth-stencil texture.
    glTextureParameteri(s.glTex, GL_DEPTH_STENCIL_TEXTURE_MODE, GL_DEPTH_COMPONENT);
    glCheck("set GL_DEPTH_STENCIL_TEXTURE_MODE");

    // THE decisive question: GL_DEPTH24_STENCIL8 over imported OPTIMAL-tiled mem.
    glTextureStorageMem2DEXT(s.glTex, 1, GL_DEPTH24_STENCIL8, TEX_W, TEX_H, s.glMemObj, 0);
    if (!glCheck("glTextureStorageMem2DEXT"))
        return fail(4, "%s glTextureStorageMem2DEXT GL_DEPTH24_STENCIL8 "
                       "(packed depth-stencil external image REJECTED by GL)", name);

    glGenSemaphoresEXT(1, &s.glSemVkToGl);
    glImportSemaphoreWin32HandleEXT(s.glSemVkToGl, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, s.semVkToGlWin32);
    if (!glCheck("glImportSemaphoreWin32HandleEXT vk->gl")) return fail(4, "%s import sem vk->gl", name);

    LOG("[GL] %s imported DS memory+GL_DEPTH24_STENCIL8 texture+semaphore (zero-copy backing established)", name);
    return true;
}

// ---- vk helper: one-shot command buffer submit ------------------------------
static bool vkSubmitOneShot(App& a, int stage,
                            void (*record)(VkCommandBuffer, Res&),
                            Res& s, VkSemaphore signalSem)
{
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = a.cmdPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    if (vkAllocateCommandBuffers(a.dev, &ai, &cb) != VK_SUCCESS) return fail(stage, "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) return fail(stage, "vkBeginCommandBuffer");
    record(cb, s);
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) return fail(stage, "vkEndCommandBuffer");

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    if (signalSem) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &signalSem; }
    if (vkQueueSubmit(a.queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return fail(stage, "vkQueueSubmit");
    return true;
}

// =============================================================================
// Direction A: Vulkan clears depth=0.5/stencil=0 -> GL samples DEPTH -> verify.
// For a packed DS image, barriers/clear use aspect DEPTH+STENCIL.
// =============================================================================
static bool directionA(App& a, Res& s, const char* name, float* outSampled, bool* outMatch) {
    LOG("--- Direction A [%s]: Vulkan writes depth=0.5, GL reads DEPTH aspect ---", name);

    struct Rec { static void fn(VkCommandBuffer cb, Res& s) {
        const VkImageAspectFlags dsAspect =
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.image = s.image;
        b.subresourceRange = {dsAspect,0,1,0,1};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,0,nullptr,0,nullptr,1,&b);

        VkClearDepthStencilValue c{};
        c.depth = DEPTH_VALUE; c.stencil = 0;
        VkImageSubresourceRange r{dsAspect,0,1,0,1};
        vkCmdClearDepthStencilImage(cb, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);

        VkImageMemoryBarrier b2 = b;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0,0,nullptr,0,nullptr,1,&b2);
    }};

    if (!vkSubmitOneShot(a, 5, Rec::fn, s, s.semVkToGl)) return false;

    GLenum layout = GL_LAYOUT_SHADER_READ_ONLY_EXT;
    glWaitSemaphoreEXT(s.glSemVkToGl, 0, nullptr, 1, &s.glTex, &layout);
    if (!glCheck("glWaitSemaphoreEXT (A)")) return fail(5, "%s glWaitSemaphoreEXT", name);

    // Read the DEPTH aspect back in GL as float.
    float px[TEX_W*TEX_H] = {0};
    glGetTextureImage(s.glTex, 0, GL_DEPTH_COMPONENT, GL_FLOAT, sizeof(px), px);
    if (!glCheck("glGetTextureImage (A, depth)")) return fail(5, "%s glGetTextureImage depth", name);

    *outSampled = px[0];
    LOG("[A %s] GL sampled DEPTH[0] = %.8f  (expected %.8f, D24 step ~%.3e)",
        name, px[0], DEPTH_VALUE, 1.0 / 16777215.0);

    bool match = true;
    for (uint32_t i = 0; i < TEX_W*TEX_H; ++i) {
        if (std::fabs(px[i] - DEPTH_VALUE) > DEPTH_EPS) { match = false;
            LOG("[A %s] MISMATCH texel %u: got %.8f exp %.8f (|d|=%.3e > eps %.3e)",
                name, i, px[i], DEPTH_VALUE, std::fabs(px[i]-DEPTH_VALUE), DEPTH_EPS);
            break;
        }
    }
    *outMatch = match;
    if (match)
        LOG("[A %s] PASS: all %u DEPTH texels match %.8f within D24 quantization",
            name, TEX_W*TEX_H, DEPTH_VALUE);
    else
        return fail(5, "%s Direction A depth mismatch (sampled %.8f)", name, px[0]);
    return true;
}

static void freeRes(App& a, Res& s) {
    if (a.dev) vkDeviceWaitIdle(a.dev);
    if (s.glTex) glDeleteTextures(1,&s.glTex);
    if (s.glMemObj) glDeleteMemoryObjectsEXT(1,&s.glMemObj);
    if (s.glSemVkToGl) glDeleteSemaphoresEXT(1,&s.glSemVkToGl);
    if (s.semVkToGl) vkDestroySemaphore(a.dev,s.semVkToGl,nullptr);
    if (s.image) vkDestroyImage(a.dev,s.image,nullptr);
    if (s.imageMem) vkFreeMemory(a.dev,s.imageMem,nullptr);
    s = Res{};
}

// Run the full round-trip for one candidate format. Returns true == VIABLE.
static bool tryFormat(App& a, VkFormat fmt, const char* name, float* sampled, bool* match) {
    *sampled = -1.0f; *match = false;
    LOG("");
    LOG("######## CANDIDATE: %s ########", name);
    bool exportable = false;
    if (!probeFormat(a, fmt, name, &exportable)) return false;
    if (!exportable) {
        LOG("[%s] NOT exportable per probe -> skipping create (clean negative).", name);
        return false;
    }
    Res s;
    bool ok = createSharedImage(a, s, fmt, name)
           && importIntoGL(a, s, name)
           && directionA(a, s, name, sampled, match);
    freeRes(a, s);
    if (ok) LOG("[%s] round-trip VIABLE.", name);
    else    LOG("[%s] round-trip FAILED at stage %d: %s", name, g_failStage, g_failMsg);
    return ok;
}

static void cleanup(App& a) {
    if (a.dev) vkDeviceWaitIdle(a.dev);
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
    LOG(" P5-T0': VK -> GL zero-copy PACKED DEPTH-STENCIL interop SPIKE");
    LOG("   candidates: D24_UNORM_S8_UINT (primary), D32_SFLOAT_S8_UINT");
    LOG("   GL import format: GL_DEPTH24_STENCIL8, sample DEPTH aspect");
    LOG("==============================================================");

    App a;
    if (!(initGL(a) && initVk(a))) {
        LOG("VERDICT: NOT VIABLE (setup failed stage %d: %s)", g_failStage, g_failMsg);
        cleanup(a);
        return 1;
    }

    // Reset per-format fail state so a primary failure doesn't taint fallback.
    float s24 = -1, s32 = -1; bool m24 = false, m32 = false;

    g_failStage = 0; g_failMsg[0] = 0;
    bool ok24 = tryFormat(a, VK_FORMAT_D24_UNORM_S8_UINT, "D24_UNORM_S8_UINT", &s24, &m24);

    bool ok32 = false;
    if (!ok24) {
        g_failStage = 0; g_failMsg[0] = 0;
        LOG("");
        LOG(">> D24_UNORM_S8_UINT not viable; trying D32_SFLOAT_S8_UINT fallback <<");
        ok32 = tryFormat(a, VK_FORMAT_D32_SFLOAT_S8_UINT, "D32_SFLOAT_S8_UINT", &s32, &m32);
    }

    LOG("");
    LOG("--------------------------------------------------------------");
    LOG("[TALLY] VK validation errors = %d", g_valErrors);
    LOG("[TALLY] GL debug errors seen = %d", g_glDebugSawError ? 1 : 0);
    LOG("[SUMMARY] D24_UNORM_S8_UINT: round-trip=%d sampled=%.8f match=%d",
        ok24?1:0, s24, m24?1:0);
    if (!ok24)
        LOG("[SUMMARY] D32_SFLOAT_S8_UINT: round-trip=%d sampled=%.8f match=%d",
            ok32?1:0, s32, m32?1:0);

    bool clean = (g_valErrors == 0) && !g_glDebugSawError;
    int rc;
    if ((ok24 || ok32) && clean) {
        const char* winner = ok24 ? "D24_UNORM_S8_UINT (GL_DEPTH24_STENCIL8)"
                                   : "D32_SFLOAT_S8_UINT (GL_DEPTH24_STENCIL8)";
        LOG("VERDICT: DEPTH-STENCIL EXPORT VIABLE via %s -> P5 GREENLIT", winner);
        rc = 0;
    } else if ((ok24 || ok32) && !clean) {
        LOG("VERDICT: PARTIAL (round-trip OK BUT validation/debug errors present) -> investigate before P5");
        rc = 2;
    } else {
        LOG("VERDICT: NOT VIABLE for packed depth-stencil zero-copy on this HW.");
        LOG("         P5 (postprocess depth zero-copy) is BLOCKED at the GL-produced D24S8 boundary.");
        rc = 1;
    }
    LOG("==============================================================");
    cleanup(a);
    return rc;
}
