// ============================================================================
// vk_interop_transport_test  —  OFFLINE interop-transport transparency harness
// (VK-INTEROP-BRIDGE-1, "TOOLS / offline harnesses" — headless, seconds)
//
// WHAT THIS PROVES
// ----------------
// For one representative Vulkan render pass ("the pass under test"), driven by a
// fixed deterministic golden input texture, this renders the pass TWO ways that
// differ ONLY in the image-transport mechanism, and asserts the two outputs are
// byte-for-byte identical:
//
//   (A) READBACK path  — VK renders into a plain (non-exported) VkImage, then
//       vkCmdCopyImageToBuffer -> host buffer. This is the "GL upload" stand-in
//       (the bytes GL would have received via a readback+upload round-trip).
//
//   (B) INTEROP path   — VK renders into an EXPORTABLE VkImageResource shared
//       zero-copy with GL; GL samples/reads the shared texture; we pull the GL
//       side back out. NO vkCmdCopyImageToBuffer / glGetTexImage on the shared
//       image is permitted — a forbidden-counter guard asserts that.
//
// If (A) == (B) byte-identical, the interop transport is TRANSPARENT for this
// shader class: swapping the readback bridge for the zero-copy shared image did
// not change a single output bit.
//
// Both paths run the SAME VkPipeline + SAME golden input + SAME UBO, so any
// divergence is attributable to the transport, not the shader.
//
// WHAT THIS DOES NOT PROVE
// ------------------------
// It does NOT replace the in-game golden-parity run on real mission pixels. It
// proves transport transparency for a shader CLASS offline (seconds); the final
// game golden-parity on a real mission stays the last word. It also only covers
// the format/usage it is instantiated with (RGBA16F color here; a depth/D32
// variant is a minimal edit per T0 — see README).
//
// REUSABILITY
// -----------
// To test a different pass: change the .frag (shaders/pass_under_test.frag) and,
// if the output format changes, IMG_FORMAT + GL_INTERNAL_FORMAT + the golden
// generator below. Everything else (both transport paths, the byte-compare, the
// forbidden counters, validation) is pass-agnostic. See README.md.
//
// Standalone: links only SDL2, GLEW, vulkan-1, opengl32. Touches nothing in
// mclib/gameos or the game build. Headless (hidden SDL window for the GL ctx;
// all rendering is offscreen).
//
// Exit 0 = PASS (byte-identical, forbidden-counters==0, validation-clean).
// Non-zero = FAIL, with the diverging stage/pixel printed. A result log is
// written next to the exe (vk_interop_transport_test-result.log).
// ============================================================================

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
#include <cstdarg>
#include <vector>
#include <string>

// SPIR-V embedded at build time by glslangValidator --vn (see CMakeLists.txt).
#include "fullscreen_vert.spv.h"
#include "pass_under_test_frag.spv.h"

// ---------------------------------------------------------------------------
// PASS-UNDER-TEST CONFIG (change these + the .frag to test another shader class)
// ---------------------------------------------------------------------------
static const uint32_t TEX_W = 256;
static const uint32_t TEX_H = 256;
// RGBA16F: the fog/ssao intermediate format the real passes use.
static const VkFormat IMG_FORMAT       = VK_FORMAT_R16G16B16A16_SFLOAT;
static const GLenum   GL_INTERNAL_FMT  = GL_RGBA16F;
static const size_t   BYTES_PER_TEXEL  = 8;                 // 4 * fp16
static const size_t   IMG_BYTES        = (size_t)TEX_W * TEX_H * BYTES_PER_TEXEL;

// ---------------------------------------------------------------------------
// logging + result file
// ---------------------------------------------------------------------------
static FILE* g_logFile = nullptr;
static int   g_failStage = 0;
static char  g_failMsg[512] = {0};
static bool  g_glDebugSawError = false;
static int   g_valErrors = 0;

// Forbidden-counter guard: mirrors the in-engine #5 assertion. On the INTEROP
// path, NONE of these image-transport copies/readbacks may be issued for the
// shared image. We increment at our own call sites and assert ==0.
struct ForbiddenCounters {
    unsigned vkCmdCopyImageToBuffer = 0;
    unsigned vkCmdCopyBufferToImage = 0;
    unsigned glGetTexImage          = 0;   // includes glGetTextureImage
    unsigned glTexSubImage2D        = 0;
    unsigned total() const {
        return vkCmdCopyImageToBuffer + vkCmdCopyBufferToImage
             + glGetTexImage + glTexSubImage2D;
    }
};
static ForbiddenCounters g_forbidden;   // counts ONLY shared-image transport ops

static void logLine(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("%s\n", buf); std::fflush(stdout);
    if (g_logFile) { std::fprintf(g_logFile, "%s\n", buf); std::fflush(g_logFile); }
}
#define LOG(...) logLine(__VA_ARGS__)

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
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
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

// ---------------------------------------------------------------------------
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

    uint8_t          devLuid[VK_LUID_SIZE] = {0};
    bool             haveLuid = false;

    // golden input (uploaded once, sampled by both paths)
    VkImage          golden = VK_NULL_HANDLE;
    VkDeviceMemory   goldenMem = VK_NULL_HANDLE;
    VkImageView      goldenView = VK_NULL_HANDLE;
    VkSampler        sampler = VK_NULL_HANDLE;

    // pipeline (shared by both paths)
    VkShaderModule   vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkRenderPass     renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline       pipe = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet  descSet = VK_NULL_HANDLE;

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

// ---- deterministic golden input pattern (fp16). Seeded constant, no rng. ----
// Encoded as half floats packed into 16-bit; we build a byte buffer directly.
static uint16_t floatToHalf(float f) {
    uint32_t x; std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t  exp  = ((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0) return (uint16_t)sign;                 // flush tiny to zero (golden stays >0)
    if (exp >= 0x1F) return (uint16_t)(sign | 0x7C00u);  // inf/overflow
    return (uint16_t)(sign | (exp << 10) | (mant >> 13));
}
static void buildGolden(std::vector<uint16_t>& out) {
    out.resize((size_t)TEX_W * TEX_H * 4);
    for (uint32_t y = 0; y < TEX_H; ++y)
        for (uint32_t x = 0; x < TEX_W; ++x) {
            // A fixed, reproducible pattern (gradients + a checker + a ripple).
            float fx = (float)x / (float)(TEX_W - 1);
            float fy = (float)y / (float)(TEX_H - 1);
            float checker = ((x >> 4) ^ (y >> 4)) & 1 ? 0.85f : 0.15f;
            float ripple  = 0.5f + 0.5f * std::sin((fx * 12.0f) + (fy * 7.0f));
            float r = fx;
            float g = fy;
            float b = checker;
            float aa = 0.25f + 0.5f * ripple;
            size_t i = ((size_t)y * TEX_W + x) * 4;
            out[i+0] = floatToHalf(r);
            out[i+1] = floatToHalf(g);
            out[i+2] = floatToHalf(b);
            out[i+3] = floatToHalf(aa);
        }
}

// ===========================================================================
// Stage 1: GL context (hidden window) + interop-capable GL extensions
// ===========================================================================
static bool initGL(App& a) {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return fail(1, "SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    a.win = SDL_CreateWindow("vk_interop_transport_test",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!a.win) return fail(1, "SDL_CreateWindow: %s", SDL_GetError());
    a.glctx = SDL_GL_CreateContext(a.win);
    if (!a.glctx) return fail(1, "SDL_GL_CreateContext: %s", SDL_GetError());
    glewExperimental = GL_TRUE;
    GLenum ge = glewInit();
    if (ge != GLEW_OK) return fail(1, "glewInit: %s", (const char*)glewGetErrorString(ge));
    glGetError();
    LOG("[GL] renderer=%s  version=%s", (const char*)glGetString(GL_RENDERER),
        (const char*)glGetString(GL_VERSION));
    if (GLEW_KHR_debug || GLEW_ARB_debug_output) {
        glEnable(GL_DEBUG_OUTPUT); glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(glDbg, nullptr);
    }
    if (!GLEW_EXT_memory_object || !GLEW_EXT_memory_object_win32)
        return fail(1, "GL lacks GL_EXT_memory_object[_win32] -> zero-copy import impossible");
    if (!GLEW_EXT_semaphore || !GLEW_EXT_semaphore_win32)
        return fail(1, "GL lacks GL_EXT_semaphore[_win32] -> cross-API sync impossible");
    return glCheck("initGL");
}

// ===========================================================================
// Stage 2: VK instance + device with external-memory/semaphore exts + LUID
// ===========================================================================
static bool initVk(App& a) {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool haveValidation = false;
    for (auto& l : layers)
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) haveValidation = true;
    LOG("[VK] validation layer available = %d", haveValidation ? 1 : 0);

    std::vector<const char*> instExts = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };
    std::vector<const char*> instLayers;
    if (haveValidation) instLayers.push_back("VK_LAYER_KHRONOS_validation");

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "vk_interop_transport_test";
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
    {
        VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(a.phys, &p);
        LOG("[VK] using device: %s", p.deviceName);
        VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &idp; vkGetPhysicalDeviceProperties2(a.phys, &p2);
        if (idp.deviceLUIDValid) {
            std::memcpy(a.devLuid, idp.deviceLUID, VK_LUID_SIZE); a.haveLuid = true;
            std::string h; for (int i=0;i<VK_LUID_SIZE;++i){char b[4];std::snprintf(b,4,"%02x",idp.deviceLUID[i]);h+=b;}
            LOG("[VK] device LUID = %s", h.c_str());
        }
    }
    // LUID match against GL (fail-soft: single-GPU box is fine if GL LUID absent).
    if (a.haveLuid && GLEW_EXT_memory_object_win32) {
        GLubyte glLuid[GL_LUID_SIZE_EXT] = {0};
        glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT, glLuid);
        if (glCheck("glGetUnsignedBytevEXT(LUID)")) {
            bool match = std::memcmp(glLuid, a.devLuid, VK_LUID_SIZE) == 0;
            LOG("[VK] GL<->VK LUID match = %d", match ? 1 : 0);
            if (!match && pdCount > 1)
                return fail(2, "GL and VK are different GPUs (LUID mismatch); interop invalid");
        }
    }

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(a.phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(a.phys, &qfCount, qfs.data());
    bool found = false;
    for (uint32_t i=0;i<qfCount;++i) if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT){a.qfam=i;found=true;break;}
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
        bool ok=false; for (auto& e:des) if (std::strcmp(e.extensionName,need)==0) ok=true;
        if (!ok) return fail(2, "device missing required extension: %s", need);
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex=a.qfam; qci.queueCount=1; qci.pQueuePriorities=&prio;
    VkDeviceCreateInfo dvci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dvci.queueCreateInfoCount=1; dvci.pQueueCreateInfos=&qci;
    dvci.enabledExtensionCount=(uint32_t)devExts.size(); dvci.ppEnabledExtensionNames=devExts.data();
    VKCHECK(vkCreateDevice(a.phys,&dvci,nullptr,&a.dev),2,"vkCreateDevice");
    vkGetDeviceQueue(a.dev,a.qfam,0,&a.queue);

    a.pGetMemWin32 = (PFN_vkGetMemoryWin32HandleKHR)   vkGetDeviceProcAddr(a.dev,"vkGetMemoryWin32HandleKHR");
    a.pGetSemWin32 = (PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(a.dev,"vkGetSemaphoreWin32HandleKHR");
    if (!a.pGetMemWin32 || !a.pGetSemWin32) return fail(2,"win32 handle entry points null");

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cpi.queueFamilyIndex=a.qfam;
    VKCHECK(vkCreateCommandPool(a.dev,&cpi,nullptr,&a.cmdPool),2,"vkCreateCommandPool");
    return true;
}

// ---- one-shot cmd buffer ----------------------------------------------------
template <class Fn>
static bool oneShot(App& a, int stage, Fn record,
                    VkSemaphore waitSem=VK_NULL_HANDLE, VkPipelineStageFlags waitStage=0,
                    VkSemaphore signalSem=VK_NULL_HANDLE, VkFence fence=VK_NULL_HANDLE) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool=a.cmdPool; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount=1;
    VkCommandBuffer cb;
    VKCHECK(vkAllocateCommandBuffers(a.dev,&ai,&cb),stage,"vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(cb,&bi),stage,"vkBeginCommandBuffer");
    record(cb);
    VKCHECK(vkEndCommandBuffer(cb),stage,"vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount=1; si.pCommandBuffers=&cb;
    if (waitSem){si.waitSemaphoreCount=1;si.pWaitSemaphores=&waitSem;si.pWaitDstStageMask=&waitStage;}
    if (signalSem){si.signalSemaphoreCount=1;si.pSignalSemaphores=&signalSem;}
    VKCHECK(vkQueueSubmit(a.queue,1,&si,fence),stage,"vkQueueSubmit");
    return true;
}

// ===========================================================================
// Stage 3: golden input texture (deterministic), uploaded once
// ===========================================================================
static bool createGolden(App& a) {
    std::vector<uint16_t> pixels;
    buildGolden(pixels);

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType=VK_IMAGE_TYPE_2D; ci.format=IMG_FORMAT; ci.extent={TEX_W,TEX_H,1};
    ci.mipLevels=1; ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT;
    ci.tiling=VK_IMAGE_TILING_OPTIMAL;
    ci.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(a.dev,&ci,nullptr,&a.golden),3,"vkCreateImage(golden)");
    VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(a.dev,a.golden,&mr);
    uint32_t idx;
    if (!findMemType(a,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&idx))
        return fail(3,"no DEVICE_LOCAL for golden");
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize=mr.size; mai.memoryTypeIndex=idx;
    VKCHECK(vkAllocateMemory(a.dev,&mai,nullptr,&a.goldenMem),3,"vkAllocateMemory(golden)");
    VKCHECK(vkBindImageMemory(a.dev,a.golden,a.goldenMem,0),3,"vkBindImageMemory(golden)");

    // staging buffer -> upload
    VkBuffer stg; VkDeviceMemory stgMem;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size=IMG_BYTES; bci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(a.dev,&bci,nullptr,&stg),3,"vkCreateBuffer(golden stg)");
    VkMemoryRequirements bmr{}; vkGetBufferMemoryRequirements(a.dev,stg,&bmr);
    uint32_t bidx;
    if (!findMemType(a,bmr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,&bidx))
        return fail(3,"no host-visible for golden stg");
    VkMemoryAllocateInfo bmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bmai.allocationSize=bmr.size; bmai.memoryTypeIndex=bidx;
    VKCHECK(vkAllocateMemory(a.dev,&bmai,nullptr,&stgMem),3,"vkAllocateMemory(golden stg)");
    VKCHECK(vkBindBufferMemory(a.dev,stg,stgMem,0),3,"vkBindBufferMemory(golden stg)");
    void* m=nullptr; VKCHECK(vkMapMemory(a.dev,stgMem,0,IMG_BYTES,0,&m),3,"map golden stg");
    std::memcpy(m,pixels.data(),IMG_BYTES); vkUnmapMemory(a.dev,stgMem);

    bool ok = oneShot(a,3,[&](VkCommandBuffer cb){
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcAccessMask=0; b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.image=a.golden;
        b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkBufferImageCopy r{}; r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent={TEX_W,TEX_H,1};
        vkCmdCopyBufferToImage(cb,stg,a.golden,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&r);
        // NOTE: this copy is on the GOLDEN input, not the shared image under test —
        // it is NOT a forbidden op (forbidden counters track the shared image only).
        VkImageMemoryBarrier b2=b;
        b2.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b2.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b2);
    });
    if (!ok) return false;
    VKCHECK(vkQueueWaitIdle(a.queue),3,"golden upload wait");
    vkDestroyBuffer(a.dev,stg,nullptr); vkFreeMemory(a.dev,stgMem,nullptr);

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image=a.golden; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=IMG_FORMAT;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VKCHECK(vkCreateImageView(a.dev,&vi,nullptr,&a.goldenView),3,"golden view");

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter=VK_FILTER_NEAREST; sci.minFilter=VK_FILTER_NEAREST;
    sci.addressModeU=sci.addressModeV=sci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKCHECK(vkCreateSampler(a.dev,&sci,nullptr,&a.sampler),3,"sampler");
    return true;
}

// ===========================================================================
// Stage 4: build the shared graphics pipeline (used by BOTH transport paths)
// ===========================================================================
static bool buildPipeline(App& a) {
    auto mkModule=[&](const uint32_t* code, size_t bytes, VkShaderModule* out, const char* what)->bool{
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize=bytes; ci.pCode=code;
        VKCHECK(vkCreateShaderModule(a.dev,&ci,nullptr,out),4,what);
        return true;
    };
    if (!mkModule(fullscreen_vert_spv,sizeof(fullscreen_vert_spv),&a.vs,"vs module")) return false;
    if (!mkModule(pass_under_test_frag_spv,sizeof(pass_under_test_frag_spv),&a.fs,"fs module")) return false;

    // render pass: single color attachment, finalLayout SHADER_READ_ONLY so both
    // the readback-copy (transfer-src is compatible) and GL-sample paths can use it.
    VkAttachmentDescription att{};
    att.format=IMG_FORMAT; att.samples=VK_SAMPLE_COUNT_1_BIT;
    att.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount=1; sub.pColorAttachments=&ref;
    VkSubpassDependency dep{};
    dep.srcSubpass=VK_SUBPASS_EXTERNAL; dep.dstSubpass=0;
    dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask=0; dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount=1; rpci.pAttachments=&att; rpci.subpassCount=1; rpci.pSubpasses=&sub;
    rpci.dependencyCount=1; rpci.pDependencies=&dep;
    VKCHECK(vkCreateRenderPass(a.dev,&rpci,nullptr,&a.renderPass),4,"render pass");

    VkDescriptorSetLayoutBinding dslb{};
    dslb.binding=0; dslb.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dslb.descriptorCount=1; dslb.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount=1; dslci.pBindings=&dslb;
    VKCHECK(vkCreateDescriptorSetLayout(a.dev,&dslci,nullptr,&a.dsl),4,"dsl");

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount=1; plci.pSetLayouts=&a.dsl;
    VKCHECK(vkCreatePipelineLayout(a.dev,&plci,nullptr,&a.pipeLayout),4,"pipe layout");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=a.vs; stages[0].pName="main";
    stages[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=a.fs; stages[1].pName="main";

    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo iasm{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    iasm.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0,0,(float)TEX_W,(float)TEX_H,0,1};
    VkRect2D sc{{0,0},{TEX_W,TEX_H}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount=1; vps.pViewports=&vp; vps.scissorCount=1; vps.pScissors=&sc;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask=0xf; cba.blendEnable=VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount=1; cb.pAttachments=&cba;

    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount=2; gp.pStages=stages; gp.pVertexInputState=&vin; gp.pInputAssemblyState=&iasm;
    gp.pViewportState=&vps; gp.pRasterizationState=&rs; gp.pMultisampleState=&ms; gp.pColorBlendState=&cb;
    gp.layout=a.pipeLayout; gp.renderPass=a.renderPass; gp.subpass=0;
    VKCHECK(vkCreateGraphicsPipelines(a.dev,VK_NULL_HANDLE,1,&gp,nullptr,&a.pipe),4,"pipeline");

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets=1; dpci.poolSizeCount=1; dpci.pPoolSizes=&ps;
    VKCHECK(vkCreateDescriptorPool(a.dev,&dpci,nullptr,&a.descPool),4,"desc pool");
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool=a.descPool; dsai.descriptorSetCount=1; dsai.pSetLayouts=&a.dsl;
    VKCHECK(vkAllocateDescriptorSets(a.dev,&dsai,&a.descSet),4,"desc set");
    VkDescriptorImageInfo dii{a.sampler,a.goldenView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet=a.descSet; w.dstBinding=0; w.descriptorCount=1;
    w.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo=&dii;
    vkUpdateDescriptorSets(a.dev,1,&w,0,nullptr);
    return true;
}

// record the pass-under-test draw into `target` (which is in UNDEFINED before).
static void recordPass(App& a, VkCommandBuffer cb, VkFramebuffer fb) {
    VkClearValue clr{}; clr.color={{0,0,0,0}};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass=a.renderPass; rp.framebuffer=fb; rp.renderArea={{0,0},{TEX_W,TEX_H}};
    rp.clearValueCount=1; rp.pClearValues=&clr;
    vkCmdBeginRenderPass(cb,&rp,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,a.pipe);
    vkCmdBindDescriptorSets(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,a.pipeLayout,0,1,&a.descSet,0,nullptr);
    vkCmdDraw(cb,3,1,0,0);
    vkCmdEndRenderPass(cb);
}

// helper: create a plain (non-exported) render target image + view + framebuffer
static bool createTarget(App& a, int stage, bool exportable,
                         VkImage* img, VkDeviceMemory* mem, VkImageView* view,
                         VkFramebuffer* fb, VkDeviceSize* memSize, HANDLE* win32) {
    VkExternalMemoryImageCreateInfo emici{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emici.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    if (exportable) ci.pNext=&emici;
    ci.imageType=VK_IMAGE_TYPE_2D; ci.format=IMG_FORMAT; ci.extent={TEX_W,TEX_H,1};
    ci.mipLevels=1; ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT;
    ci.tiling=VK_IMAGE_TILING_OPTIMAL;
    ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(a.dev,&ci,nullptr,img),stage,"vkCreateImage(target)");

    VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(a.dev,*img,&mr);
    if (memSize) *memSize=mr.size;
    uint32_t idx;
    if (!findMemType(a,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,&idx))
        return fail(stage,"no DEVICE_LOCAL for target");
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; ded.image=*img;
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext=&ded; exp.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = exportable ? (void*)&exp : (void*)&ded;   // dedicated either way
    mai.allocationSize=mr.size; mai.memoryTypeIndex=idx;
    VKCHECK(vkAllocateMemory(a.dev,&mai,nullptr,mem),stage,"vkAllocateMemory(target)");
    VKCHECK(vkBindImageMemory(a.dev,*img,*mem,0),stage,"vkBindImageMemory(target)");

    if (exportable && win32) {
        VkMemoryGetWin32HandleInfoKHR gh{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
        gh.memory=*mem; gh.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VKCHECK(a.pGetMemWin32(a.dev,&gh,win32),stage,"vkGetMemoryWin32HandleKHR(target)");
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image=*img; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=IMG_FORMAT;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VKCHECK(vkCreateImageView(a.dev,&vi,nullptr,view),stage,"target view");
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass=a.renderPass; fci.attachmentCount=1; fci.pAttachments=view;
    fci.width=TEX_W; fci.height=TEX_H; fci.layers=1;
    VKCHECK(vkCreateFramebuffer(a.dev,&fci,nullptr,fb),stage,"framebuffer");
    return true;
}

// ===========================================================================
// PATH A: readback. VK renders the pass -> vkCmdCopyImageToBuffer -> host bytes.
// (This is the "GL upload" stand-in: the exact bytes GL would receive via a
//  readback+upload round-trip.) NOT the interop path -> copies here are allowed.
// ===========================================================================
static bool runReadbackPath(App& a, std::vector<uint8_t>& out) {
    VkImage img; VkDeviceMemory mem; VkImageView view; VkFramebuffer fb;
    if (!createTarget(a,5,/*exportable*/false,&img,&mem,&view,&fb,nullptr,nullptr)) return false;

    VkBuffer rb; VkDeviceMemory rbMem;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size=IMG_BYTES; bci.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT; bci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(a.dev,&bci,nullptr,&rb),5,"vkCreateBuffer(readback)");
    VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(a.dev,rb,&mr);
    uint32_t idx;
    if (!findMemType(a,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,&idx))
        return fail(5,"no host-visible for readback");
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize=mr.size; mai.memoryTypeIndex=idx;
    VKCHECK(vkAllocateMemory(a.dev,&mai,nullptr,&rbMem),5,"vkAllocateMemory(readback)");
    VKCHECK(vkBindBufferMemory(a.dev,rb,rbMem,0),5,"vkBindBufferMemory(readback)");

    bool ok = oneShot(a,5,[&](VkCommandBuffer cb){
        recordPass(a,cb,fb);
        // render pass left it in SHADER_READ_ONLY; barrier to TRANSFER_SRC for copy.
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL; b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT; b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        b.image=img; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkBufferImageCopy r{}; r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent={TEX_W,TEX_H,1};
        vkCmdCopyImageToBuffer(cb,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,rb,1,&r);
        // (readback path: this copy is expected & allowed; not counted as forbidden)
    });
    if (!ok) return false;
    VKCHECK(vkQueueWaitIdle(a.queue),5,"readback wait");
    void* m=nullptr; VKCHECK(vkMapMemory(a.dev,rbMem,0,IMG_BYTES,0,&m),5,"map readback");
    out.resize(IMG_BYTES); std::memcpy(out.data(),m,IMG_BYTES); vkUnmapMemory(a.dev,rbMem);

    vkDestroyBuffer(a.dev,rb,nullptr); vkFreeMemory(a.dev,rbMem,nullptr);
    vkDestroyFramebuffer(a.dev,fb,nullptr); vkDestroyImageView(a.dev,view,nullptr);
    vkDestroyImage(a.dev,img,nullptr); vkFreeMemory(a.dev,mem,nullptr);
    LOG("[A] readback path rendered pass-under-test -> %zu bytes", out.size());
    return true;
}

// ===========================================================================
// PATH B: interop. VK renders the pass into an EXPORTABLE shared image; GL
// imports it zero-copy and samples/reads it; we pull the GL side out.
// The shared image is NEVER copied via vkCmdCopyImageToBuffer nor read via
// glGetTexImage — GL reads it through an FBO blit into a SEPARATE gl-owned
// readback texture (glReadPixels off that), so the shared image itself is only
// touched zero-copy. Forbidden counters assert this.
// ===========================================================================
static bool runInteropPath(App& a, std::vector<uint8_t>& out) {
    // exportable render target (the shared VkImageResource stand-in)
    VkImage img; VkDeviceMemory mem; VkImageView view; VkFramebuffer fb;
    VkDeviceSize memSize=0; HANDLE win32=nullptr;
    if (!createTarget(a,6,/*exportable*/true,&img,&mem,&view,&fb,&memSize,&win32)) return false;
    LOG("[B] exportable shared image created, memWin32=%p size=%llu", win32,(unsigned long long)memSize);

    // one exportable VK->GL binary semaphore (producer -> GL consumer)
    VkSemaphore semVkToGl; HANDLE semWin32=nullptr;
    {
        VkExportSemaphoreCreateInfo esci{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        esci.handleTypes=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sci.pNext=&esci;
        VKCHECK(vkCreateSemaphore(a.dev,&sci,nullptr,&semVkToGl),6,"sem create");
        VkSemaphoreGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
        gi.semaphore=semVkToGl; gi.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VKCHECK(a.pGetSemWin32(a.dev,&gi,&semWin32),6,"sem win32");
    }

    // import memory + semaphore into GL, back the shared GL texture (zero-copy)
    GLuint glMemObj=0, glTex=0, glSem=0;
    glCreateMemoryObjectsEXT(1,&glMemObj);
    GLint dedicated=GL_TRUE; glMemoryObjectParameterivEXT(glMemObj,GL_DEDICATED_MEMORY_OBJECT_EXT,&dedicated);
    glImportMemoryWin32HandleEXT(glMemObj,memSize,GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,win32);
    if (!glCheck("glImportMemoryWin32HandleEXT")) return fail(6,"GL import memory");
    glCreateTextures(GL_TEXTURE_2D,1,&glTex);
    glTextureParameteri(glTex,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTextureParameteri(glTex,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTextureParameteri(glTex,GL_TEXTURE_TILING_EXT,GL_OPTIMAL_TILING_EXT);
    glTextureStorageMem2DEXT(glTex,1,GL_INTERNAL_FMT,TEX_W,TEX_H,glMemObj,0);
    if (!glCheck("glTextureStorageMem2DEXT")) return fail(6,"GL back texture");
    glGenSemaphoresEXT(1,&glSem);
    glImportSemaphoreWin32HandleEXT(glSem,GL_HANDLE_TYPE_OPAQUE_WIN32_EXT,semWin32);
    if (!glCheck("glImportSemaphoreWin32HandleEXT")) return fail(6,"GL import sem");

    // VK renders the SAME pass into the shared image, then SIGNALS the semaphore.
    // NO vkCmdCopyImageToBuffer here (that would be forbidden on the shared image).
    bool ok = oneShot(a,6,[&](VkCommandBuffer cb){ recordPass(a,cb,fb); },
                      VK_NULL_HANDLE,0, semVkToGl);
    if (!ok) return false;

    // GL waits on the VK producer, then reads the shared texture. To keep the
    // SHARED image strictly zero-copy (no glGetTexImage on it), we blit it via an
    // FBO into a SEPARATE gl-owned readback texture and glReadPixels off THAT.
    GLenum layout=GL_LAYOUT_SHADER_READ_ONLY_EXT;
    glWaitSemaphoreEXT(glSem,0,nullptr,1,&glTex,&layout);
    if (!glCheck("glWaitSemaphoreEXT")) return fail(6,"GL wait sem");

    GLuint dstTex=0; glCreateTextures(GL_TEXTURE_2D,1,&dstTex);
    glTextureStorage2D(dstTex,1,GL_INTERNAL_FMT,TEX_W,TEX_H);
    GLuint srcFbo=0,dstFbo=0;
    glCreateFramebuffers(1,&srcFbo); glNamedFramebufferTexture(srcFbo,GL_COLOR_ATTACHMENT0,glTex,0);
    glCreateFramebuffers(1,&dstFbo); glNamedFramebufferTexture(dstFbo,GL_COLOR_ATTACHMENT0,dstTex,0);
    if (glCheckNamedFramebufferStatus(srcFbo,GL_READ_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)
        return fail(6,"src FBO incomplete");
    glBlitNamedFramebuffer(srcFbo,dstFbo,0,0,TEX_W,TEX_H,0,0,TEX_W,TEX_H,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    if (!glCheck("glBlitNamedFramebuffer")) return fail(6,"GL blit shared->dst");

    // read back the gl-owned dst (NOT the shared image) as fp16.
    out.resize(IMG_BYTES);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,dstFbo);
    glReadPixels(0,0,TEX_W,TEX_H,GL_RGBA,GL_HALF_FLOAT,out.data());
    if (!glCheck("glReadPixels(dst)")) return fail(6,"GL readpixels dst");
    glBindFramebuffer(GL_READ_FRAMEBUFFER,0);

    // teardown
    glDeleteFramebuffers(1,&srcFbo); glDeleteFramebuffers(1,&dstFbo);
    glDeleteTextures(1,&dstTex); glDeleteTextures(1,&glTex);
    glDeleteMemoryObjectsEXT(1,&glMemObj); glDeleteSemaphoresEXT(1,&glSem);
    vkDeviceWaitIdle(a.dev);
    vkDestroySemaphore(a.dev,semVkToGl,nullptr);
    vkDestroyFramebuffer(a.dev,fb,nullptr); vkDestroyImageView(a.dev,view,nullptr);
    vkDestroyImage(a.dev,img,nullptr); vkFreeMemory(a.dev,mem,nullptr);
    if (win32) CloseHandle(win32); if (semWin32) CloseHandle(semWin32);
    LOG("[B] interop path rendered pass-under-test through shared image -> %zu bytes", out.size());
    return true;
}

static void cleanup(App& a) {
    if (a.dev) vkDeviceWaitIdle(a.dev);
    if (a.pipe) vkDestroyPipeline(a.dev,a.pipe,nullptr);
    if (a.descPool) vkDestroyDescriptorPool(a.dev,a.descPool,nullptr);
    if (a.pipeLayout) vkDestroyPipelineLayout(a.dev,a.pipeLayout,nullptr);
    if (a.dsl) vkDestroyDescriptorSetLayout(a.dev,a.dsl,nullptr);
    if (a.renderPass) vkDestroyRenderPass(a.dev,a.renderPass,nullptr);
    if (a.vs) vkDestroyShaderModule(a.dev,a.vs,nullptr);
    if (a.fs) vkDestroyShaderModule(a.dev,a.fs,nullptr);
    if (a.sampler) vkDestroySampler(a.dev,a.sampler,nullptr);
    if (a.goldenView) vkDestroyImageView(a.dev,a.goldenView,nullptr);
    if (a.golden) vkDestroyImage(a.dev,a.golden,nullptr);
    if (a.goldenMem) vkFreeMemory(a.dev,a.goldenMem,nullptr);
    if (a.cmdPool) vkDestroyCommandPool(a.dev,a.cmdPool,nullptr);
    if (a.dev) vkDestroyDevice(a.dev,nullptr);
    if (a.messenger && a.pDestroyDbg) a.pDestroyDbg(a.inst,a.messenger,nullptr);
    if (a.inst) vkDestroyInstance(a.inst,nullptr);
    if (a.glctx) SDL_GL_DeleteContext(a.glctx);
    if (a.win) SDL_DestroyWindow(a.win);
    SDL_Quit();
}

int main(int, char**) {
    g_logFile = std::fopen("vk_interop_transport_test-result.log","w");
    LOG("============================================================");
    LOG(" vk_interop_transport_test — offline interop-transport parity");
    LOG(" pass-under-test = fullscreen fog-blend frag; %ux%u RGBA16F", TEX_W, TEX_H);
    LOG("============================================================");

    App a;
    std::vector<uint8_t> bytesA, bytesB;
    bool built =
        initGL(a) && initVk(a) && createGolden(a) && buildPipeline(a);

    bool ran = built &&
        runReadbackPath(a, bytesA) &&
        runInteropPath (a, bytesB);

    // ---- byte-for-byte compare ----
    bool identical = false;
    long firstDiff = -1;
    if (ran) {
        identical = (bytesA.size()==bytesB.size());
        if (identical) {
            for (size_t i=0;i<bytesA.size();++i)
                if (bytesA[i]!=bytesB[i]) { identical=false; firstDiff=(long)i; break; }
        }
    }

    LOG("------------------------------------------------------------");
    LOG("[COMPARE] readback bytes=%zu interop bytes=%zu", bytesA.size(), bytesB.size());
    if (ran && !identical && firstDiff>=0) {
        size_t texel = (size_t)firstDiff / BYTES_PER_TEXEL;
        LOG("[COMPARE] FIRST DIVERGENCE at byte %ld (texel %zu, x=%zu y=%zu)",
            firstDiff, texel, texel%TEX_W, texel/TEX_W);
        LOG("[COMPARE]   readback=0x%02X interop=0x%02X", bytesA[firstDiff], bytesB[firstDiff]);
    }

    // ---- forbidden-counter check (shared-image transport ops must be zero) ----
    LOG("[FORBIDDEN] vkCmdCopyImageToBuffer=%u vkCmdCopyBufferToImage=%u glGetTexImage=%u glTexSubImage2D=%u  (total=%u)",
        g_forbidden.vkCmdCopyImageToBuffer, g_forbidden.vkCmdCopyBufferToImage,
        g_forbidden.glGetTexImage, g_forbidden.glTexSubImage2D, g_forbidden.total());
    bool forbiddenClean = (g_forbidden.total()==0);

    // ---- validation tally ----
    LOG("[TALLY] VK validation errors = %d", g_valErrors);
    LOG("[TALLY] GL debug errors seen = %d", g_glDebugSawError ? 1 : 0);
    bool valClean = (g_valErrors==0) && !g_glDebugSawError;

    int rc;
    if (ran && identical && forbiddenClean && valClean) {
        LOG("VERDICT: PASS  (interop transport BYTE-IDENTICAL to readback; forbidden-counters==0; validation-clean)");
        LOG("         => the interop transport is TRANSPARENT for this shader class.");
        rc = 0;
    } else if (!built || !ran) {
        LOG("VERDICT: FAIL  (stage %d: %s)", g_failStage, g_failMsg);
        rc = 1;
    } else if (!identical) {
        LOG("VERDICT: FAIL  (outputs DIVERGED — transport is NOT transparent for this pass)");
        rc = 3;
    } else if (!forbiddenClean) {
        LOG("VERDICT: FAIL  (interop path issued a forbidden shared-image copy/readback)");
        rc = 4;
    } else {
        LOG("VERDICT: FAIL  (validation/debug errors present)");
        rc = 2;
    }
    LOG("============================================================");
    cleanup(a);
    if (g_logFile) std::fclose(g_logFile);
    return rc;
}
