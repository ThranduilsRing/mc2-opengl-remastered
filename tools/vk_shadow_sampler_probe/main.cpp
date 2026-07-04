// =============================================================================
// VK-produced D32 shadow map -> GL HARDWARE sampler2DShadow COMPARE probe
//   P8 shadow-region R2 GO/NO-GO   (VK-INTEROP-BRIDGE-1, standalone tooling)
//
// THE QUESTION (recon R2): T0 proved plain texture()/glGetTexImage readback of an
// imported D32 image works. But MC2's calcShadow() (shadow.hglsl) samples the
// shadow map with a HARDWARE depth-compare sampler:
//     GL_TEXTURE_COMPARE_MODE = GL_COMPARE_REF_TO_TEXTURE
//     GL_TEXTURE_COMPARE_FUNC = GL_LEQUAL
//     sampler2DShadow, texture(sampler, vec3(uv, refDepth)) -> 0/1 (or PCF filtered)
// Does that HARDWARE compare path work over an EXTERNALLY-IMPORTED, OPTIMAL-tiled
// D32 image on this AMD driver? T0 did NOT cover it. Settle it here.
//
// P8's calcShadow convention: shadowMap stores light-space depth; the shader
// compares the fragment's light-space depth (ref) LEQUAL the stored depth.
// GL_LEQUAL semantics: result = (ref <= stored) ? 1.0 : 0.0.
//
// TEST STRUCTURE:
//   VK: create exportable D32_SFLOAT SAMPLED image, clear it to a KNOWN uniform
//       depth STORED=0.5, export Win32 handle + a vk->gl semaphore.
//   GL: import as GL_DEPTH_COMPONENT32F (T0's proven sequence), then set
//       COMPARE_REF_TO_TEXTURE + GL_LEQUAL, bind as sampler2DShadow, and run a
//       fragment shader that samples at several ref depths straddling STORED:
//         ref=0.25 -> 0.25<=0.5 -> expect 1
//         ref=0.50 -> 0.50<=0.5 -> expect 1  (boundary, LEQUAL)
//         ref=0.75 -> 0.75<=0.5 -> expect 0
//       Read the compare results back (R32F FBO) and check against expectation.
//   Repeat with GL_LINEAR filtering (hardware 2x2 PCF) — a uniform-depth map must
//   still yield exactly 0 or 1 (all 4 taps agree), so LINEAR is also unambiguous.
//   NEGATIVE discriminator: ref=0.75 MUST return 0 (not 1) — proves the test can
//   tell right from wrong, i.e. it is not trivially passing.
//
// Exit 0  == HARDWARE-COMPARE VIABLE: hardware sampler2DShadow compare over the
//            imported D32 image returns correct results, validation/GL-error clean.
// Exit 1  == NOT VIABLE: reports the exact failing stage (GL error on compare-mode
//            setup? wrong compare results? garbage?) so P8 plans GLSL-manual PCF.
// Exit 2  == PARTIAL: compare correct but validation/GL debug errors present.
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
#include <cstdarg>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>

// ---- image / depth constants ------------------------------------------------
static const uint32_t TEX_W = 16;
static const uint32_t TEX_H = 16;
static const VkFormat IMG_FORMAT = VK_FORMAT_D32_SFLOAT;

// Vulkan clears the shared shadow map to this KNOWN, exactly-representable depth.
// 0.5 = 0x3F000000, exact in float32.
static const float STORED_DEPTH = 0.5f;

// Reference depths sampled by the shadow compare + expected GL_LEQUAL result.
struct RefCase { float ref; int expect; const char* name; };
static const RefCase kCases[] = {
    { 0.25f, 1, "ref<stored -> lit(1)"   },   // 0.25 <= 0.5 -> 1
    { 0.50f, 1, "ref==stored -> lit(1)"  },   // 0.50 <= 0.5 -> 1 (LEQUAL boundary)
    { 0.75f, 0, "ref>stored -> shadow(0)"},   // 0.75 <= 0.5 -> 0
};
static const int NCASES = (int)(sizeof(kCases)/sizeof(kCases[0]));

// ---- logging + verdict ------------------------------------------------------
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

    VkSemaphore      semVkToGl = VK_NULL_HANDLE;
    HANDLE           semVkToGlWin32 = nullptr;

    GLuint           glMemObj = 0;
    GLuint           glTex = 0;
    GLuint           glSemVkToGl = 0;

    // GL compare-sampling machinery
    GLuint           fbo = 0, colorTex = 0;
    GLuint           prog = 0, vao = 0;
    GLint            uRef = -1;

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
// Stage 1: SDL window + GL 4.3 core + GLEW + GL debug
// =============================================================================
static bool initGL(App& a) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return fail(1, "SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    a.win = SDL_CreateWindow("vk_shadow_sampler_probe",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
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
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDbg, nullptr);
    }
    if (!GLEW_EXT_memory_object || !GLEW_EXT_memory_object_win32)
        return fail(1, "GL driver lacks GL_EXT_memory_object[_win32]");
    if (!GLEW_EXT_semaphore || !GLEW_EXT_semaphore_win32)
        return fail(1, "GL driver lacks GL_EXT_semaphore[_win32]");
    return glCheck("initGL");
}

// =============================================================================
// Stage 2: Vulkan instance + device (external-memory/-semaphore exts) — from T0
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
    ai.pApplicationName = "vk_shadow_sampler_probe";
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
    VKCHECK(vkCreateDevice(a.phys, &dvci, nullptr, &a.dev), 2, "vkCreateDevice");
    vkGetDeviceQueue(a.dev, a.qfam, 0, &a.queue);

    a.pGetMemWin32 = (PFN_vkGetMemoryWin32HandleKHR)    vkGetDeviceProcAddr(a.dev, "vkGetMemoryWin32HandleKHR");
    a.pGetSemWin32 = (PFN_vkGetSemaphoreWin32HandleKHR) vkGetDeviceProcAddr(a.dev, "vkGetSemaphoreWin32HandleKHR");
    if (!a.pGetMemWin32 || !a.pGetSemWin32) return fail(2, "vkGetMemory/SemaphoreWin32HandleKHR null");

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = a.qfam;
    VKCHECK(vkCreateCommandPool(a.dev, &cpi, nullptr, &a.cmdPool), 2, "vkCreateCommandPool");
    return true;
}

// =============================================================================
// Stage 3: exportable D32 SAMPLED image + dedicated exported mem + handle — T0
// =============================================================================
static bool createSharedImage(App& a) {
    const VkExternalMemoryHandleTypeFlagBits handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkPhysicalDeviceExternalImageFormatInfo eifi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
    eifi.handleType = handleType;
    VkPhysicalDeviceImageFormatInfo2 ifi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    ifi.pNext = &eifi; ifi.format = IMG_FORMAT; ifi.type = VK_IMAGE_TYPE_2D;
    ifi.tiling = VK_IMAGE_TILING_OPTIMAL; ifi.usage = usage;
    VkExternalImageFormatProperties eifp{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 ifp2{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    ifp2.pNext = &eifp;
    VkResult fr = vkGetPhysicalDeviceImageFormatProperties2(a.phys, &ifi, &ifp2);
    if (fr != VK_SUCCESS) return fail(3, "D32 SAMPLED external export unsupported: %s", vkStr(fr));
    if (!(eifp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
        return fail(3, "OPAQUE_WIN32 export not supported for D32 SAMPLED");

    VkExternalMemoryImageCreateInfo emici{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes = handleType;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.pNext = &emici; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = IMG_FORMAT;
    ci.extent = { TEX_W, TEX_H, 1 }; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(a.dev, &ci, nullptr, &a.image), 3, "vkCreateImage(external depth)");

    VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(a.dev, a.image, &mr);
    a.memSize = mr.size;
    uint32_t memIdx;
    if (!findMemType(a, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memIdx))
        return fail(3, "no DEVICE_LOCAL memory type");

    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; ded.image = a.image;
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO}; exp.pNext = &ded; exp.handleTypes = handleType;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &exp; mai.allocationSize = mr.size; mai.memoryTypeIndex = memIdx;
    VKCHECK(vkAllocateMemory(a.dev, &mai, nullptr, &a.imageMem), 3, "vkAllocateMemory(exported depth)");
    VKCHECK(vkBindImageMemory(a.dev, a.image, a.imageMem, 0), 3, "vkBindImageMemory");

    VkMemoryGetWin32HandleInfoKHR gh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    gh.memory = a.imageMem; gh.handleType = handleType;
    VKCHECK(a.pGetMemWin32(a.dev, &gh, &a.memWin32), 3, "vkGetMemoryWin32HandleKHR");
    LOG("[VK] exported depth memory handle=%p size=%llu", a.memWin32, (unsigned long long)a.memSize);
    return true;
}

static bool createSharedSemaphores(App& a) {
    const VkExternalSemaphoreHandleTypeFlagBits ht = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO}; esci.handleTypes = ht;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sci.pNext = &esci;
    VKCHECK(vkCreateSemaphore(a.dev, &sci, nullptr, &a.semVkToGl), 3, "vkCreateSemaphore(exported)");
    VkSemaphoreGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
    gi.semaphore = a.semVkToGl; gi.handleType = ht;
    VKCHECK(a.pGetSemWin32(a.dev, &gi, &a.semVkToGlWin32), 3, "vkGetSemaphoreWin32HandleKHR");
    LOG("[VK] exported vk->gl semaphore handle=%p", a.semVkToGlWin32);
    return true;
}

// =============================================================================
// Stage 4: import mem + semaphore into GL as GL_DEPTH_COMPONENT32F — T0 sequence
// =============================================================================
static bool importIntoGL(App& a) {
    glCreateMemoryObjectsEXT(1, &a.glMemObj);
    if (!glCheck("glCreateMemoryObjectsEXT")) return fail(4, "glCreateMemoryObjectsEXT");
    GLint dedicated = GL_TRUE;
    glMemoryObjectParameterivEXT(a.glMemObj, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    glCheck("glMemoryObjectParameterivEXT");
    glImportMemoryWin32HandleEXT(a.glMemObj, a.memSize, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, a.memWin32);
    if (!glCheck("glImportMemoryWin32HandleEXT")) return fail(4, "glImportMemoryWin32HandleEXT rejected");

    glCreateTextures(GL_TEXTURE_2D, 1, &a.glTex);
    glTextureParameteri(a.glTex, GL_TEXTURE_TILING_EXT, GL_OPTIMAL_TILING_EXT);
    glTextureStorageMem2DEXT(a.glTex, 1, GL_DEPTH_COMPONENT32F, TEX_W, TEX_H, a.glMemObj, 0);
    if (!glCheck("glTextureStorageMem2DEXT"))
        return fail(4, "glTextureStorageMem2DEXT GL_DEPTH_COMPONENT32F rejected");

    glGenSemaphoresEXT(1, &a.glSemVkToGl);
    glImportSemaphoreWin32HandleEXT(a.glSemVkToGl, GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, a.semVkToGlWin32);
    if (!glCheck("glImportSemaphoreWin32HandleEXT")) return fail(4, "import sem vk->gl");

    LOG("[GL] imported D32 depth texture + semaphore (zero-copy backing established)");
    return true;
}

// ---- vk one-shot submit -----------------------------------------------------
static bool vkSubmitOneShot(App& a, int stage, void (*record)(VkCommandBuffer, App&), VkSemaphore signalSem) {
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
    if (signalSem) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &signalSem; }
    VKCHECK(vkQueueSubmit(a.queue, 1, &si, VK_NULL_HANDLE), stage, "vkQueueSubmit");
    return true;
}

// =============================================================================
// Stage 5: VK clears the shared depth image to STORED_DEPTH, signals vk->gl
// =============================================================================
static bool vkWriteKnownDepth(App& a) {
    struct Rec { static void fn(VkCommandBuffer cb, App& a) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.image = a.image; b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,0,nullptr,0,nullptr,1,&b);
        VkClearDepthStencilValue c{}; c.depth = STORED_DEPTH; c.stencil = 0;
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
        vkCmdClearDepthStencilImage(cb, a.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        VkImageMemoryBarrier b2 = b;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0,0,nullptr,0,nullptr,1,&b2);
    }};
    if (!vkSubmitOneShot(a, 5, Rec::fn, a.semVkToGl)) return false;

    GLenum layout = GL_LAYOUT_SHADER_READ_ONLY_EXT;
    glWaitSemaphoreEXT(a.glSemVkToGl, 0, nullptr, 1, &a.glTex, &layout);
    if (!glCheck("glWaitSemaphoreEXT")) return fail(5, "glWaitSemaphoreEXT");
    LOG("[VK] cleared shared shadow map to STORED_DEPTH=%.4f, signalled GL", STORED_DEPTH);
    return true;
}

// =============================================================================
// Stage 6a: GL machinery — R32F FBO + a sampler2DShadow compare fragment shader
// =============================================================================
static const char* kVS =
    "#version 430 core\n"
    "const vec2 P[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));\n"
    "void main(){ gl_Position=vec4(P[gl_VertexID],0,1); }\n";

// Samples the imported depth texture through a HARDWARE shadow sampler.
// texture(sampler2DShadow, vec3(uv, ref)) performs the depth compare in HW and
// returns the (filtered) 0/1 result. uRef is the light-space reference depth.
static const char* kFS =
    "#version 430 core\n"
    "uniform sampler2DShadow shadowMap;\n"
    "uniform float uRef;\n"
    "out float oResult;\n"
    "void main(){ oResult = texture(shadowMap, vec3(0.5, 0.5, uRef)); }\n";

static GLuint compile(GLenum type, const char* src, const char* tag) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOG("[GL] %s compile error: %s", tag, log); glDeleteShader(s); return 0; }
    return s;
}

static bool buildGLMachinery(App& a) {
    glCreateFramebuffers(1, &a.fbo);
    glCreateTextures(GL_TEXTURE_2D, 1, &a.colorTex);
    glTextureStorage2D(a.colorTex, 1, GL_R32F, 1, 1);   // 1x1 result target
    glTextureParameteri(a.colorTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(a.colorTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glNamedFramebufferTexture(a.fbo, GL_COLOR_ATTACHMENT0, a.colorTex, 0);
    if (glCheckNamedFramebufferStatus(a.fbo, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return fail(6, "result FBO incomplete");

    GLuint vs = compile(GL_VERTEX_SHADER, kVS, "VS");
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFS, "FS(sampler2DShadow)");
    if (!vs || !fs) return fail(6, "shader compile failed");
    a.prog = glCreateProgram();
    glAttachShader(a.prog, vs); glAttachShader(a.prog, fs);
    glLinkProgram(a.prog);
    GLint ok = 0; glGetProgramiv(a.prog, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(a.prog, sizeof(log), nullptr, log);
        return fail(6, "program link failed: %s", log); }
    glDeleteShader(vs); glDeleteShader(fs);
    a.uRef = glGetUniformLocation(a.prog, "uRef");
    glCreateVertexArrays(1, &a.vao);
    return glCheck("buildGLMachinery");
}

// Configure the imported depth texture for HARDWARE compare sampling.
// THE probe: does GL accept + correctly execute COMPARE_REF_TO_TEXTURE over the
// externally-imported OPTIMAL-tiled D32 image?
static bool setCompareMode(App& a, GLenum filter) {
    glTextureParameteri(a.glTex, GL_TEXTURE_MIN_FILTER, filter);
    glTextureParameteri(a.glTex, GL_TEXTURE_MAG_FILTER, filter);
    glTextureParameteri(a.glTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(a.glTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(a.glTex, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glTextureParameteri(a.glTex, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);   // MC2 calcShadow convention
    if (!glCheck("setCompareMode (COMPARE_REF_TO_TEXTURE setup on imported tex)"))
        return fail(6, "GL rejected compare-mode params on imported depth texture");
    return true;
}

// Run one compare sample at ref, read back the 0/1 result from the R32F target.
static bool sampleCompare(App& a, float ref, float* out) {
    glBindFramebuffer(GL_FRAMEBUFFER, a.fbo);
    glViewport(0, 0, 1, 1);
    GLfloat clr[4] = {-1,-1,-1,-1}; glClearBufferfv(GL_COLOR, 0, clr);
    glUseProgram(a.prog);
    glProgramUniform1f(a.prog, a.uRef, ref);
    glBindTextureUnit(0, a.glTex);
    glProgramUniform1i(a.prog, glGetUniformLocation(a.prog, "shadowMap"), 0);
    glBindVertexArray(a.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (!glCheck("draw compare")) return fail(6, "draw with sampler2DShadow errored");
    glFinish();
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, 1, 1, GL_RED, GL_FLOAT, out);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!glCheck("readback compare result")) return fail(6, "readback errored");
    return true;
}

// =============================================================================
// Stage 6: run the compare battery (NEAREST then LINEAR/2x2 PCF)
// =============================================================================
static bool runCompareBattery(App& a, GLenum filter, const char* filterName, bool* allCorrect) {
    LOG("--- compare battery: %s (STORED=%.4f, GL_LEQUAL) ---", filterName, STORED_DEPTH);
    if (!setCompareMode(a, filter)) return false;
    *allCorrect = true;
    for (int i = 0; i < NCASES; ++i) {
        float res = -999.0f;
        if (!sampleCompare(a, kCases[i].ref, &res)) return false;
        // Uniform-depth map => even 2x2 PCF taps agree => exactly 0 or 1.
        int got = (res > 0.5f) ? 1 : 0;
        bool ok = (got == kCases[i].expect) && (std::fabs(res - (float)got) < 1e-4f);
        LOG("[%s] ref=%.3f -> result=%.4f (got %d, expect %d) [%s] %s",
            filterName, kCases[i].ref, res, got, kCases[i].expect, kCases[i].name,
            ok ? "OK" : "MISMATCH");
        if (!ok) *allCorrect = false;
    }
    return true;
}

static void cleanup(App& a) {
    if (a.dev) vkDeviceWaitIdle(a.dev);
    if (a.vao) glDeleteVertexArrays(1,&a.vao);
    if (a.prog) glDeleteProgram(a.prog);
    if (a.colorTex) glDeleteTextures(1,&a.colorTex);
    if (a.fbo) glDeleteFramebuffers(1,&a.fbo);
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
    LOG("============================================================");
    LOG(" VK-produced D32 shadow -> GL HARDWARE sampler2DShadow COMPARE probe");
    LOG(" (P8 shadow-region R2 GO/NO-GO)");
    LOG("============================================================");
    App a;
    bool nearestOK = false, linearOK = false;
    bool ok =
        initGL(a) && initVk(a) &&
        createSharedImage(a) && createSharedSemaphores(a) &&
        importIntoGL(a) && vkWriteKnownDepth(a) &&
        buildGLMachinery(a) &&
        runCompareBattery(a, GL_NEAREST, "NEAREST", &nearestOK) &&
        runCompareBattery(a, GL_LINEAR,  "LINEAR(2x2 PCF)", &linearOK);

    // NEGATIVE discriminator: the ref>stored case MUST have returned 0 above.
    // If the whole battery were trivially returning a constant, the ref=0.75
    // case (expect 0) and the ref=0.25 case (expect 1) could not BOTH pass.
    bool discriminates = nearestOK; // nearestOK requires both 1s and the 0 to be right

    LOG("------------------------------------------------------------");
    LOG("[TALLY] VK validation errors = %d", g_valErrors);
    LOG("[TALLY] GL debug errors seen = %d", g_glDebugSawError ? 1 : 0);
    LOG("[TALLY] NEAREST compare correct  = %d", nearestOK ? 1 : 0);
    LOG("[TALLY] LINEAR  compare correct  = %d", linearOK ? 1 : 0);
    LOG("[TALLY] test discriminates (neg) = %d", discriminates ? 1 : 0);

    bool clean = (g_valErrors == 0) && !g_glDebugSawError;
    bool compareOK = nearestOK && linearOK && discriminates;
    int rc;
    if (ok && compareOK && clean) {
        LOG("VERDICT: HARDWARE-COMPARE VIABLE");
        LOG("  Hardware sampler2DShadow COMPARE_REF_TO_TEXTURE over imported D32 works.");
        LOG("  P8 can use hardware sampler2DShadow (the clean path matching GL calcShadow).");
        rc = 0;
    } else if (ok && compareOK && !clean) {
        LOG("VERDICT: PARTIAL (compare correct BUT validation/GL-debug errors present)");
        rc = 2;
    } else {
        LOG("VERDICT: NOT VIABLE");
        if (!ok) LOG("  failing stage %d: %s", g_failStage, g_failMsg);
        else {
            if (!nearestOK) LOG("  NEAREST compare returned WRONG results over imported memory");
            if (!linearOK)  LOG("  LINEAR (PCF) compare returned WRONG results over imported memory");
        }
        LOG("  => P8 must VK-produce depth + do GLSL-MANUAL PCF in the consuming shader.");
        rc = 1;
    }
    LOG("============================================================");
    cleanup(a);
    return rc;
}
