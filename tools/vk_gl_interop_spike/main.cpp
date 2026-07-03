// =============================================================================
// Vulkan <-> OpenGL shared-image interop SPIKE   (Truth-First arc P5 gate G1)
//
// PROVES (or disproves) zero-copy VkImage<->GL texture sharing on this machine:
//   1. Vulkan device + GL 4.3 core context (SDL2), both validation/debug on.
//   2. Export a VkImage's dedicated device memory as an opaque Win32 handle
//      (VK_KHR_external_memory_win32) + export two binary semaphores.
//   3. Import handle into GL via GL_EXT_memory_object(_win32) +
//      glTextureStorageMem2DEXT, and import the semaphores via
//      GL_EXT_semaphore(_win32).
//   4. Both directions, zero-copy, correctly synchronized:
//        (A) Vulkan writes (vkCmdClearColorImage COLOR_A) -> GL samples -> verify.
//        (B) GL writes (FBO clear COLOR_B)                -> Vulkan reads -> verify.
//   5. No validation errors, no GL debug errors, no read-a-write hazard.
//
// This is a SPIKE: goal is a yes/no answer with evidence. Not production code.
// Standalone: links only SDL2, GLEW, vulkan-1. Touches nothing in mclib/gameos.
//
// Exit code 0 == PASS (both directions pixel-verified, validation-clean).
// Non-zero == FAIL/PARTIAL, with the exact failing stage printed.
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
#include <vector>
#include <string>

// ---- image / color constants ------------------------------------------------
static const uint32_t TEX_W = 4;
static const uint32_t TEX_H = 4;
static const VkFormat IMG_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;

// Vulkan writes this in direction A. Bytes are R,G,B,A in memory (UNORM).
static const uint8_t COLOR_A[4] = { 0x20, 0x40, 0x80, 0xFF }; // 32,64,128,255
// GL writes this in direction B.
static const uint8_t COLOR_B[4] = { 0xC0, 0x30, 0x90, 0xFF }; // 192,48,144,255

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
    // SDL / GL
    SDL_Window*   win = nullptr;
    SDL_GLContext glctx = nullptr;

    // Vulkan
    VkInstance       inst = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice         dev  = VK_NULL_HANDLE;
    uint32_t         qfam = 0;
    VkQueue          queue = VK_NULL_HANDLE;
    VkCommandPool    cmdPool = VK_NULL_HANDLE;

    // shared image
    VkImage          image = VK_NULL_HANDLE;
    VkDeviceMemory   imageMem = VK_NULL_HANDLE;
    VkDeviceSize     memSize = 0;
    HANDLE           memWin32 = nullptr;
    uint8_t          devLuid[VK_LUID_SIZE] = {0};
    bool             haveLuid = false;

    // sync: two binary semaphores (vk->gl, gl->vk), both exported
    VkSemaphore      semVkToGl = VK_NULL_HANDLE;
    VkSemaphore      semGlToVk = VK_NULL_HANDLE;
    HANDLE           semVkToGlWin32 = nullptr;
    HANDLE           semGlToVkWin32 = nullptr;

    // GL side of the imports
    GLuint           glMemObj = 0;
    GLuint           glTex = 0;
    GLuint           glSemVkToGl = 0;
    GLuint           glSemGlToVk = 0;

    // function pointers (win32 external)
    PFN_vkGetMemoryWin32HandleKHR    pGetMemWin32 = nullptr;
    PFN_vkGetSemaphoreWin32HandleKHR pGetSemWin32 = nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT  pCreateDbg = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT pDestroyDbg = nullptr;
};

// find a memory type index with the requested property flags
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

    a.win = SDL_CreateWindow("vk_gl_interop_spike",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!a.win) return fail(1, "SDL_CreateWindow: %s", SDL_GetError());

    a.glctx = SDL_GL_CreateContext(a.win);
    if (!a.glctx) return fail(1, "SDL_GL_CreateContext (need GL4.3 core): %s", SDL_GetError());

    glewExperimental = GL_TRUE;
    GLenum ge = glewInit();
    if (ge != GLEW_OK) return fail(1, "glewInit: %s", (const char*)glewGetErrorString(ge));
    glGetError(); // swallow glewExperimental's benign INVALID_ENUM

    LOG("[GL] vendor=%s", (const char*)glGetString(GL_VENDOR));
    LOG("[GL] renderer=%s", (const char*)glGetString(GL_RENDERER));
    LOG("[GL] version=%s", (const char*)glGetString(GL_VERSION));

    if (GLEW_KHR_debug || GLEW_ARB_debug_output) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDbg, nullptr);
    }

    // The critical GL-side extension gate:
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
    // ---- instance ----
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
    ai.pApplicationName = "vk_gl_interop_spike";
    ai.apiVersion = VK_API_VERSION_1_2;

    VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dci.pfnUserCallback = vkDbg;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = &dci; // catch instance-creation-time messages too
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = (uint32_t)instExts.size();
    ici.ppEnabledExtensionNames = instExts.data();
    ici.enabledLayerCount = (uint32_t)instLayers.size();
    ici.ppEnabledLayerNames = instLayers.data();

    VKCHECK(vkCreateInstance(&ici, nullptr, &a.inst), 2, "vkCreateInstance");

    a.pCreateDbg  = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(a.inst, "vkCreateDebugUtilsMessengerEXT");
    a.pDestroyDbg = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(a.inst, "vkDestroyDebugUtilsMessengerEXT");
    if (a.pCreateDbg) a.pCreateDbg(a.inst, &dci, nullptr, &a.messenger);

    // ---- physical device: prefer the one whose LUID matches the GL device ----
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

        // Grab device LUID (used to prove GL and VK are the same physical GPU).
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

    // ---- queue family (graphics) ----
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(a.phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(a.phys, &qfCount, qfs.data());
    bool found = false;
    for (uint32_t i = 0; i < qfCount; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { a.qfam = i; found = true; break; }
    if (!found) return fail(2, "no graphics queue family");

    // ---- device with external-memory/semaphore win32 extensions ----
    std::vector<const char*> devExts = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
    };
    // Verify each device extension is actually present before requesting it.
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
// Stage 3: create exportable VkImage + dedicated exported memory + get handle
// =============================================================================
static bool createSharedImage(App& a) {
    const VkExternalMemoryHandleTypeFlagBits handleType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    // sanity: is this image config actually exportable with this handle type?
    VkPhysicalDeviceExternalImageFormatInfo eifi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    eifi.handleType = handleType;
    VkPhysicalDeviceImageFormatInfo2 ifi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    ifi.pNext = &eifi;
    ifi.format = IMG_FORMAT;
    ifi.type = VK_IMAGE_TYPE_2D;
    ifi.tiling = VK_IMAGE_TILING_OPTIMAL;
    ifi.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
              | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkExternalImageFormatProperties eifp{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ifp2{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    ifp2.pNext = &eifp;
    VkResult fr = vkGetPhysicalDeviceImageFormatProperties2(a.phys, &ifi, &ifp2);
    if (fr != VK_SUCCESS)
        return fail(3, "image format not supported for external export: %s", vkStr(fr));
    VkExternalMemoryFeatureFlags feat = eifp.externalMemoryProperties.externalMemoryFeatures;
    LOG("[VK] external image features: exportable=%d importable=%d dedicated-required=%d",
        (feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) ? 1:0,
        (feat & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ? 1:0,
        (feat & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ? 1:0);
    if (!(feat & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
        return fail(3, "OPAQUE_WIN32 export not supported for this image format/config");

    // ---- create the image with external handle types declared ----
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
    ci.usage = ifi.usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(a.dev, &ci, nullptr, &a.image), 3, "vkCreateImage(external)");

    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(a.dev, a.image, &mr);
    a.memSize = mr.size;

    uint32_t memIdx;
    if (!findMemType(a, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memIdx))
        return fail(3, "no DEVICE_LOCAL memory type for shared image");

    // dedicated allocation (GL_EXT_memory_object import expects a dedicated block)
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.image = a.image;

    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext = &ded;
    exp.handleTypes = handleType;

    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &exp;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memIdx;
    VKCHECK(vkAllocateMemory(a.dev, &mai, nullptr, &a.imageMem), 3, "vkAllocateMemory(exported)");
    VKCHECK(vkBindImageMemory(a.dev, a.image, a.imageMem, 0), 3, "vkBindImageMemory");

    // ---- export the Win32 handle for the memory ----
    VkMemoryGetWin32HandleInfoKHR gh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    gh.memory = a.imageMem;
    gh.handleType = handleType;
    VKCHECK(a.pGetMemWin32(a.dev, &gh, &a.memWin32), 3, "vkGetMemoryWin32HandleKHR");
    LOG("[VK] exported memory Win32 handle=%p size=%llu", a.memWin32, (unsigned long long)a.memSize);

    return true;
}

// =============================================================================
// Stage 3b: two exportable binary semaphores + Win32 handles
// =============================================================================
static bool createSharedSemaphores(App& a) {
    const VkExternalSemaphoreHandleTypeFlagBits ht =
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    auto makeSem = [&](VkSemaphore* s, HANDLE* h, const char* name) -> bool {
        VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        esci.handleTypes = ht;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sci.pNext = &esci;
        VKCHECK(vkCreateSemaphore(a.dev, &sci, nullptr, s), 3, "vkCreateSemaphore(exported)");
        VkSemaphoreGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
        gi.semaphore = *s; gi.handleType = ht;
        VKCHECK(a.pGetSemWin32(a.dev, &gi, h), 3, "vkGetSemaphoreWin32HandleKHR");
        LOG("[VK] exported semaphore '%s' handle=%p", name, *h);
        return true;
    };
    if (!makeSem(&a.semVkToGl, &a.semVkToGlWin32, "vk->gl")) return false;
    if (!makeSem(&a.semGlToVk, &a.semGlToVkWin32, "gl->vk")) return false;
    return true;
}

// =============================================================================
// Stage 4: import memory + semaphores into GL; create the GL texture on top
// =============================================================================
static bool importIntoGL(App& a) {
    // memory object from the imported Win32 handle
    glCreateMemoryObjectsEXT(1, &a.glMemObj);
    if (!glCheck("glCreateMemoryObjectsEXT")) return fail(4, "glCreateMemoryObjectsEXT");

    // DEDICATED_MEMORY_OBJECT must be set before import for dedicated allocations.
    GLint dedicated = GL_TRUE;
    glMemoryObjectParameterivEXT(a.glMemObj, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    glCheck("glMemoryObjectParameterivEXT");

    // NOTE: glImportMemoryWin32HandleEXT does NOT take ownership by default; the
    // driver duplicates the handle. We keep the Vulkan-side handle and close ours.
    glImportMemoryWin32HandleEXT(a.glMemObj, a.memSize,
                                 GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, a.memWin32);
    if (!glCheck("glImportMemoryWin32HandleEXT"))
        return fail(4, "glImportMemoryWin32HandleEXT (handle import rejected by GL driver)");

    // create texture storage backed by the imported memory at offset 0 (zero-copy)
    glCreateTextures(GL_TEXTURE_2D, 1, &a.glTex);
    glTextureParameteri(a.glTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(a.glTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Must match the Vulkan tiling: OPTIMAL -> GL_OPTIMAL_TILING_EXT.
    glTextureParameteri(a.glTex, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    glTextureStorageMem2DEXT(a.glTex, 1, GL_RGBA8, TEX_W, TEX_H, a.glMemObj, 0);
    if (!glCheck("glTextureStorageMem2DEXT"))
        return fail(4, "glTextureStorageMem2DEXT (backing GL texture with imported memory failed)");

    // import the two semaphores
    glGenSemaphoresEXT(1, &a.glSemVkToGl);
    glGenSemaphoresEXT(1, &a.glSemGlToVk);
    glImportSemaphoreWin32HandleEXT(a.glSemVkToGl, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, a.semVkToGlWin32);
    if (!glCheck("glImportSemaphoreWin32HandleEXT vk->gl")) return fail(4, "import sem vk->gl");
    glImportSemaphoreWin32HandleEXT(a.glSemGlToVk, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, a.semGlToVkWin32);
    if (!glCheck("glImportSemaphoreWin32HandleEXT gl->vk")) return fail(4, "import sem gl->vk");

    LOG("[GL] imported memory+texture+2 semaphores (zero-copy backing established)");
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
// Direction A: Vulkan writes COLOR_A -> GL samples/reads -> verify
// =============================================================================
static bool directionA(App& a) {
    LOG("--- Direction A: Vulkan writes, GL reads ---");

    // record: transition UNDEFINED->TRANSFER_DST, clear, transition->present-to-GL layout
    struct Rec { static void fn(VkCommandBuffer cb, App& a) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.image = a.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,0,nullptr,0,nullptr,1,&b);
        VkClearColorValue c{};
        c.float32[0]=COLOR_A[0]/255.f; c.float32[1]=COLOR_A[1]/255.f;
        c.float32[2]=COLOR_A[2]/255.f; c.float32[3]=COLOR_A[3]/255.f;
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(cb, a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        // release to the layout GL will consume it in (SHADER_READ_ONLY)
        VkImageMemoryBarrier b2 = b;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0,0,nullptr,0,nullptr,1,&b2);
    }};

    // Vulkan writes, then SIGNALS semVkToGl (no fence wait -> semaphore is the sync).
    if (!vkSubmitOneShot(a, 5, Rec::fn, VK_NULL_HANDLE, 0, a.semVkToGl)) return false;

    // GL waits on the semaphore before touching the texture (cross-API sync;
    // this is what prevents the read-a-write hazard). The layout array tells GL
    // the producer left it in SHADER_READ_ONLY.
    GLenum layout = GL_LAYOUT_SHADER_READ_ONLY_EXT;
    glWaitSemaphoreEXT(a.glSemVkToGl, 0, nullptr, 1, &a.glTex, &layout);
    if (!glCheck("glWaitSemaphoreEXT (A)")) return fail(5, "glWaitSemaphoreEXT");

    // Read the texels back in GL.
    uint8_t px[TEX_W*TEX_H*4] = {0};
    glGetTextureImage(a.glTex, 0, GL_RGBA, GL_UNSIGNED_BYTE, sizeof(px), px);
    if (!glCheck("glGetTextureImage (A)")) return fail(5, "glGetTextureImage");

    LOG("[A] GL read texel[0] = %d,%d,%d,%d  (expected %d,%d,%d,%d)",
        px[0],px[1],px[2],px[3], COLOR_A[0],COLOR_A[1],COLOR_A[2],COLOR_A[3]);

    // Verify every texel matches (allow +-1 for any UNORM rounding).
    for (uint32_t i = 0; i < TEX_W*TEX_H; ++i) {
        for (int c = 0; c < 4; ++c) {
            int got = px[i*4+c], exp = COLOR_A[c];
            if (std::abs(got-exp) > 1)
                return fail(5, "Direction A pixel mismatch at texel %u chan %d: got %d exp %d",
                            i, c, got, exp);
        }
    }
    LOG("[A] PASS: all %u texels match COLOR_A (Vulkan->GL zero-copy verified)", TEX_W*TEX_H);
    return true;
}

// =============================================================================
// Direction B: GL writes COLOR_B via FBO -> Vulkan reads back -> verify
// =============================================================================
static bool directionB(App& a) {
    LOG("--- Direction B: GL writes, Vulkan reads ---");

    // GL renders into the shared texture (FBO clear). First signal so Vulkan's
    // producer barrier ordering is well-defined, but here GL is the producer.
    GLuint fbo = 0;
    glCreateFramebuffers(1, &fbo);
    glNamedFramebufferTexture(fbo, GL_COLOR_ATTACHMENT0, a.glTex, 0);
    if (glCheckNamedFramebufferStatus(fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail(6, "GL FBO incomplete on shared texture");

    // Before GL writes, it should own the image in COLOR_ATTACHMENT layout.
    // (The image is currently in SHADER_READ_ONLY from direction A; a GL
    //  signal/wait pair with layouts handles the transition on the GL side.)
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0,0,TEX_W,TEX_H);
    glClearColor(COLOR_B[0]/255.f, COLOR_B[1]/255.f, COLOR_B[2]/255.f, COLOR_B[3]/255.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!glCheck("GL FBO clear (B)")) return fail(6, "GL clear into shared texture");

    // GL signals semGlToVk, handing the image to Vulkan in COLOR_ATTACHMENT layout.
    GLenum layout = GL_LAYOUT_COLOR_ATTACHMENT_EXT;
    glSignalSemaphoreEXT(a.glSemGlToVk, 0, nullptr, 1, &a.glTex, &layout);
    glFlush(); // ensure the signal is submitted to the GL server
    if (!glCheck("glSignalSemaphoreEXT (B)")) return fail(6, "glSignalSemaphoreEXT");

    // Vulkan: create a linear host-visible buffer, wait on semGlToVk, copy image->buffer.
    VkBuffer rb; VkDeviceMemory rbMem;
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = TEX_W*TEX_H*4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VKCHECK(vkCreateBuffer(a.dev,&bci,nullptr,&rb),6,"vkCreateBuffer(readback)");
        VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(a.dev, rb, &mr);
        uint32_t idx;
        if (!findMemType(a, mr.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &idx))
            return fail(6, "no host-visible memory for readback");
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size; mai.memoryTypeIndex = idx;
        VKCHECK(vkAllocateMemory(a.dev,&mai,nullptr,&rbMem),6,"vkAllocateMemory(readback)");
        VKCHECK(vkBindBufferMemory(a.dev, rb, rbMem, 0),6,"vkBindBufferMemory(readback)");
    }

    struct Ctx { VkBuffer rb; } ctx{rb};
    static VkBuffer s_rb; s_rb = rb;
    struct Rec { static void fn(VkCommandBuffer cb, App& a) {
        // GL produced the image in COLOR_ATTACHMENT layout; barrier it to
        // TRANSFER_SRC for the copy. srcAccess=0 because the cross-API semaphore
        // (not a pipeline barrier) already made GL's writes visible.
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.image = a.image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        region.imageExtent = {TEX_W,TEX_H,1};
        vkCmdCopyImageToBuffer(cb, a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               s_rb, 1, &region);
    }};

    // Vulkan WAITS on semGlToVk (GL's signal) at the transfer stage -> no
    // read-a-write hazard: the copy cannot start until GL's clear has completed.
    if (!vkSubmitOneShot(a, 6, Rec::fn,
                         a.semGlToVk, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_NULL_HANDLE))
        return false;
    VKCHECK(vkQueueWaitIdle(a.queue), 6, "vkQueueWaitIdle (B)");

    // map + verify
    void* mapped = nullptr;
    VKCHECK(vkMapMemory(a.dev, rbMem, 0, VK_WHOLE_SIZE, 0, &mapped),6,"vkMapMemory(readback)");
    uint8_t* px = (uint8_t*)mapped;
    LOG("[B] Vulkan read texel[0] = %d,%d,%d,%d  (expected %d,%d,%d,%d)",
        px[0],px[1],px[2],px[3], COLOR_B[0],COLOR_B[1],COLOR_B[2],COLOR_B[3]);
    bool ok = true;
    for (uint32_t i = 0; i < TEX_W*TEX_H && ok; ++i)
        for (int c = 0; c < 4; ++c) {
            int got=px[i*4+c], exp=COLOR_B[c];
            if (std::abs(got-exp) > 1) {
                fail(6, "Direction B pixel mismatch at texel %u chan %d: got %d exp %d", i,c,got,exp);
                ok = false; break;
            }
        }
    vkUnmapMemory(a.dev, rbMem);
    vkDestroyBuffer(a.dev, rb, nullptr);
    vkFreeMemory(a.dev, rbMem, nullptr);
    glDeleteFramebuffers(1, &fbo);
    (void)ctx;
    if (!ok) return false;
    LOG("[B] PASS: all %u texels match COLOR_B (GL->Vulkan zero-copy verified)", TEX_W*TEX_H);
    return true;
}

static void cleanup(App& a) {
    if (a.dev) vkDeviceWaitIdle(a.dev);
    if (a.glTex) glDeleteTextures(1,&a.glTex);
    if (a.glMemObj) glDeleteMemoryObjectsEXT(1,&a.glMemObj);
    if (a.glSemVkToGl) glDeleteSemaphoresEXT(1,&a.glSemVkToGl);
    if (a.glSemGlToVk) glDeleteSemaphoresEXT(1,&a.glSemGlToVk);
    if (a.semVkToGl) vkDestroySemaphore(a.dev,a.semVkToGl,nullptr);
    if (a.semGlToVk) vkDestroySemaphore(a.dev,a.semGlToVk,nullptr);
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
    LOG(" Vulkan <-> OpenGL zero-copy interop SPIKE (P5 gate G1)");
    LOG("========================================================");
    App a;
    bool ok =
        initGL(a) &&
        initVk(a) &&
        createSharedImage(a) &&
        createSharedSemaphores(a) &&
        importIntoGL(a) &&
        directionA(a) &&
        directionB(a);

    // final validation/debug tally
    LOG("--------------------------------------------------------");
    LOG("[TALLY] VK validation errors = %d", g_valErrors);
    LOG("[TALLY] GL debug errors seen = %d", g_glDebugSawError ? 1 : 0);

    bool clean = (g_valErrors == 0) && !g_glDebugSawError;
    int rc;
    if (ok && clean) {
        LOG("VERDICT: PASS  (both directions pixel-verified, validation-clean)");
        rc = 0;
    } else if (ok && !clean) {
        LOG("VERDICT: PARTIAL  (pixels verified BUT validation/debug errors present)");
        rc = 2;
    } else {
        LOG("VERDICT: FAIL  (stage %d: %s)", g_failStage, g_failMsg);
        rc = 1;
    }
    LOG("========================================================");
    cleanup(a);
    return rc;
}
