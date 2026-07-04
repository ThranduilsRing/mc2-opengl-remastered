// =============================================================================
// VK-P13b-T0  --  SAME-HWND VK-present-to-the-GL-window probe (decisive GO/NO-GO)
//
// THE QUESTION: can Vulkan present a swapchain to the SAME Win32 window that
// already owns an OpenGL context, via raw vkCreateWin32SurfaceKHR on that
// window's HWND -- WITHOUT a separate SDL_WINDOW_VULKAN?  If yes, P13b is a
// small slice (present to the real visible window; no second window; no input
// forwarding).  If no, P13b needs the two-window path.
//
// WHAT THIS DOES (mirrors tools/vk_present_smoke, but with the HWND twist):
//   1. Create a VISIBLE SDL window with SDL_WINDOW_OPENGL (exactly like the game
//      -- gos_render.cpp:306-307) + an SDL GL context on it (SDL_GL_CreateContext,
//      gos_render.cpp:362).  This reproduces the game's window/context creation.
//   2. Pull the native HWND + HINSTANCE via SDL_GetWindowWMInfo
//      (info.info.win.window / .hinstance) -- the same call the game uses at
//      gos_render.cpp:64-74 for the mouse grab.
//   3. Stand up a VK instance+device with VK_KHR_surface + VK_KHR_win32_surface,
//      then RAW vkCreateWin32SurfaceKHR(instance,{hinstance,hwnd}) on that SAME
//      HWND.  Report if it succeeds + vkGetPhysicalDeviceSurfaceSupportKHR true.
//   4. Build a swapchain on that surface.  Per frame: do a trivial GL render into
//      FBO-0 (glClear) but DELIBERATELY DO NOT SDL_GL_SwapWindow (mimics the P13
//      intercept that suppresses GL swap in =vulkan) -> VK acquire -> blit a
//      changing color into the swapchain image -> vkQueuePresentKHR.  ~120 frames
//      so it is visibly on screen for a moment.
//   5. Validation (sync preset).  Count errors -- MUST be 0.
//   6. Exit 0  == HWND trick WORKS (VK presents to the GL window's HWND,
//      validation-clean, no crash, GL context coexisted).
//      Nonzero == does NOT work; the exact failing stage is printed.
//
// Standalone: links ONLY SDL2 + vulkan-1 + opengl32 (raw GL 1.1 for glClear/
// glFinish -- no GLEW needed).  Does NOT touch mclib / gameos / the mc2 build.
// =============================================================================

#include <windows.h>
#include <GL/gl.h>          // core GL 1.1 (glClear/glFinish) via opengl32.lib

#include <SDL.h>
#include <SDL_syswm.h>      // SDL_GetWindowWMInfo -> HWND/HINSTANCE

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

// ---- config -----------------------------------------------------------------
static const uint32_t kInitW  = 800;
static const uint32_t kInitH  = 600;
static const uint32_t kFrames = 120;   // visible for ~2 s at 60 Hz
static const uint32_t kResizeAtFrame = 60;

// ---- logging + verdict ------------------------------------------------------
#define LOG(...)  do { std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while (0)

static int  g_failStage = 0;
static char g_failMsg[512] = {0};
static int  g_valErrors = 0;

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
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        default: { static char b[32]; std::snprintf(b, 32, "VkResult(%d)", (int)r); return b; }
    }
}

#define VKCHECK(expr, stage, what) do { \
    VkResult _r = (expr); \
    if (_r != VK_SUCCESS) return fail((stage), "%s -> %s", (what), vkStr(_r)); \
} while (0)

// ---- validation callback (counts ERRORs; sync preset flags layout hazards) --
static VKAPI_ATTR VkBool32 VKAPI_CALL vkDbg(
    VkDebugUtilsMessageSeverityFlagBitsEXT sev,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* d,
    void*)
{
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        LOG("[VK-VALIDATION %s] %s",
            (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : "WARN",
            d && d->pMessage ? d->pMessage : "(null)");
        if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) g_valErrors++;
    }
    return VK_FALSE;
}

static bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}
static std::string envStr(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// =============================================================================
struct Probe {
    // --- GL side (the game's window) ---
    SDL_Window*   win = nullptr;
    SDL_GLContext glctx = nullptr;
    HWND          hwnd = nullptr;
    HINSTANCE     hinst = nullptr;
    bool          sdlVideoInited = false;

    // --- VK side ---
    VkInstance        inst  = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR      surface = VK_NULL_HANDLE;
    VkPhysicalDevice  phys  = VK_NULL_HANDLE;
    VkDevice          dev   = VK_NULL_HANDLE;
    uint32_t          gfxFamily = UINT32_MAX;
    uint32_t          presentFamily = UINT32_MAX;
    VkQueue           gfxQueue = VK_NULL_HANDLE;
    VkQueue           presentQueue = VK_NULL_HANDLE;

    VkSwapchainKHR    swapchain = VK_NULL_HANDLE;
    VkFormat          scFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D        scExtent = {0, 0};
    std::vector<VkImage>     scImages;
    std::vector<VkSemaphore> renderSems; // one per swapchain image

    VkCommandPool     cpool = VK_NULL_HANDLE;
    VkCommandBuffer   cbuf  = VK_NULL_HANDLE;
    VkSemaphore       acquireSem = VK_NULL_HANDLE;
    VkFence           inFlight = VK_NULL_HANDLE;

    VkImage           srcImage = VK_NULL_HANDLE;
    VkDeviceMemory    srcMem   = VK_NULL_HANDLE;
    uint32_t          srcW = 0, srcH = 0;
    VkFormat          srcFormat = VK_FORMAT_R8G8B8A8_UNORM;

    PFN_vkCreateDebugUtilsMessengerEXT  pCreateDbg  = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT pDestroyDbg = nullptr;
    PFN_vkCreateWin32SurfaceKHR         pCreateWin32Surface = nullptr;

    uint32_t present_frames = 0;
    uint32_t resize_count   = 0;
};

static bool findMemType(VkPhysicalDevice phys, uint32_t typeBits,
                        VkMemoryPropertyFlags want, uint32_t* out) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) { *out = i; return true; }
    return false;
}

// =============================================================================
// STEP 1+2 : create the GAME'S window (SDL_WINDOW_OPENGL, VISIBLE) + GL context,
//            then pull the native HWND/HINSTANCE from it.
// =============================================================================
static bool createGlWindowAndContext(Probe& p) {
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
            return fail(1, "SDL_InitSubSystem(VIDEO): %s", SDL_GetError());
        p.sdlVideoInited = true;
    }

    // EXACTLY like the game (gos_render.cpp:306-307): OPENGL | ALLOW_HIGHDPI,
    // then ShowWindow. NOTE: no SDL_WINDOW_VULKAN flag -- this is a GL window.
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
    p.win = SDL_CreateWindow("vk_hwnd_present_probe (GL window, VK presents to it)",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             (int)kInitW, (int)kInitH, flags);
    if (!p.win) return fail(1, "SDL_CreateWindow(OPENGL): %s", SDL_GetError());
    SDL_ShowWindow(p.win);
    LOG("[probe] created VISIBLE %ux%u SDL_WINDOW_OPENGL window", kInitW, kInitH);

    // GL context on that window (gos_render.cpp:362) -- must succeed on an
    // OPENGL-flagged window; proves the GL side is live and owns the pixel format.
    p.glctx = SDL_GL_CreateContext(p.win);
    if (!p.glctx) return fail(1, "SDL_GL_CreateContext: %s", SDL_GetError());
    if (SDL_GL_MakeCurrent(p.win, p.glctx) < 0)
        return fail(1, "SDL_GL_MakeCurrent: %s", SDL_GetError());
    LOG("[probe] GL context CREATED + current -- GL_VERSION=%s  GL_RENDERER=%s",
        (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));

    // Pull native HWND + HINSTANCE (gos_render.cpp:64-74).
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(p.win, &wm))
        return fail(1, "SDL_GetWindowWMInfo: %s", SDL_GetError());
    if (wm.subsystem != SDL_SYSWM_WINDOWS)
        return fail(1, "window subsystem is not SDL_SYSWM_WINDOWS (%d)", (int)wm.subsystem);
    p.hwnd  = wm.info.win.window;
    p.hinst = wm.info.win.hinstance;
    if (!p.hwnd) return fail(1, "SDL_GetWindowWMInfo gave a NULL HWND");
    // hinstance can legitimately be NULL from SDL; fall back to the process module.
    if (!p.hinst) p.hinst = GetModuleHandle(nullptr);
    LOG("[probe] native HWND=%p HINSTANCE=%p (from the GL window)", (void*)p.hwnd, (void*)p.hinst);
    return true;
}

// =============================================================================
// STEP 3 : VK instance + RAW vkCreateWin32SurfaceKHR on the GL window's HWND +
//          device.  This is the decisive part.
// =============================================================================
static bool createVkInstanceSurfaceDevice(Probe& p) {
    // Instance extensions: surface + win32 surface (+ debug utils). We do NOT
    // ask SDL for the extension list (it would want a VULKAN window); we name
    // them ourselves -- exactly what the same-HWND trick requires.
    std::vector<const char*> instExts = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
    };

    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    if (layerCount) vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool haveValidation = false;
    for (auto& l : layers)
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) haveValidation = true;

    std::string valPreset = envStr("MC2_VULKAN_VALIDATION");
    if (valPreset.empty()) valPreset = "sync";
    bool wantValidation = haveValidation && valPreset != "0" && valPreset != "off";
    LOG("[probe] validation layer available=%d preset=%s enabled=%d",
        haveValidation ? 1 : 0, valPreset.c_str(), wantValidation ? 1 : 0);

    std::vector<const char*> instLayers;
    std::vector<VkValidationFeatureEnableEXT> valFeats;
    if (wantValidation) {
        instLayers.push_back("VK_LAYER_KHRONOS_validation");
        if (valPreset == "sync")
            valFeats.push_back(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
    }

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "vk_hwnd_present_probe";
    ai.apiVersion = VK_API_VERSION_1_1;

    VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                        | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    dci.pfnUserCallback = vkDbg;

    VkValidationFeaturesEXT valFeatInfo{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    valFeatInfo.enabledValidationFeatureCount = (uint32_t)valFeats.size();
    valFeatInfo.pEnabledValidationFeatures    = valFeats.empty() ? nullptr : valFeats.data();
    valFeatInfo.pNext = &dci;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = wantValidation ? (const void*)&valFeatInfo : (const void*)&dci;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount   = (uint32_t)instExts.size();
    ici.ppEnabledExtensionNames = instExts.data();
    ici.enabledLayerCount   = (uint32_t)instLayers.size();
    ici.ppEnabledLayerNames = instLayers.empty() ? nullptr : instLayers.data();

    VKCHECK(vkCreateInstance(&ici, nullptr, &p.inst), 2, "vkCreateInstance");

    p.pCreateDbg  = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(p.inst, "vkCreateDebugUtilsMessengerEXT");
    p.pDestroyDbg = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(p.inst, "vkDestroyDebugUtilsMessengerEXT");
    if (wantValidation && p.pCreateDbg) p.pCreateDbg(p.inst, &dci, nullptr, &p.messenger);

    // ---- THE DECISIVE CALL: raw vkCreateWin32SurfaceKHR on the GL window's HWND
    p.pCreateWin32Surface =
        (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(p.inst, "vkCreateWin32SurfaceKHR");
    if (!p.pCreateWin32Surface)
        return fail(2, "vkGetInstanceProcAddr(vkCreateWin32SurfaceKHR) returned NULL "
                       "(VK_KHR_win32_surface not enabled?)");

    VkWin32SurfaceCreateInfoKHR wsci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    wsci.hinstance = p.hinst;
    wsci.hwnd      = p.hwnd;
    VkResult sr = p.pCreateWin32Surface(p.inst, &wsci, nullptr, &p.surface);
    if (sr != VK_SUCCESS)
        return fail(2, "*** vkCreateWin32SurfaceKHR ON THE GL WINDOW'S HWND REJECTED -> %s ***",
                    vkStr(sr));
    LOG("[probe] *** vkCreateWin32SurfaceKHR ON THE GL WINDOW'S HWND SUCCEEDED ***");

    // ---- physical device: prefer discrete, must support present on our surface -
    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(p.inst, &pdCount, nullptr);
    if (pdCount == 0) return fail(2, "no Vulkan physical devices");
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(p.inst, &pdCount, pds.data());
    p.phys = pds[0];
    for (auto pd : pds) {
        VkPhysicalDeviceProperties pr{}; vkGetPhysicalDeviceProperties(pd, &pr);
        if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) p.phys = pd;
    }
    {
        VkPhysicalDeviceProperties pr{}; vkGetPhysicalDeviceProperties(p.phys, &pr);
        LOG("[probe] using physical device: %s", pr.deviceName);
    }

    // ---- queue families (graphics + present-on-our-surface) ----
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p.phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(p.phys, &qfCount, qfs.data());
    for (uint32_t q = 0; q < qfCount; ++q) {
        if ((qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && p.gfxFamily == UINT32_MAX)
            p.gfxFamily = q;
        VkBool32 present = VK_FALSE;
        VkResult psr = vkGetPhysicalDeviceSurfaceSupportKHR(p.phys, q, p.surface, &present);
        if (psr == VK_SUCCESS && present == VK_TRUE && p.presentFamily == UINT32_MAX)
            p.presentFamily = q;
    }
    LOG("[probe] vkGetPhysicalDeviceSurfaceSupportKHR: present-capable family = %s (gfx=%u present=%u)",
        p.presentFamily == UINT32_MAX ? "NONE" : "found", p.gfxFamily, p.presentFamily);
    if (p.gfxFamily == UINT32_MAX || p.presentFamily == UINT32_MAX)
        return fail(2, "no graphics/present queue family for the GL-window surface "
                       "(gfx=%u present=%u) -- surface not presentable", p.gfxFamily, p.presentFamily);

    // ---- device ----
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qcis[2]{};
    uint32_t qciCount = 0;
    qcis[qciCount] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qcis[qciCount].queueFamilyIndex = p.gfxFamily;
    qcis[qciCount].queueCount = 1;
    qcis[qciCount].pQueuePriorities = &prio;
    ++qciCount;
    if (p.presentFamily != p.gfxFamily) {
        qcis[qciCount] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qcis[qciCount].queueFamilyIndex = p.presentFamily;
        qcis[qciCount].queueCount = 1;
        qcis[qciCount].pQueuePriorities = &prio;
        ++qciCount;
    }
    std::vector<const char*> devExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dvci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dvci.queueCreateInfoCount    = qciCount;
    dvci.pQueueCreateInfos       = qcis;
    dvci.enabledExtensionCount   = (uint32_t)devExts.size();
    dvci.ppEnabledExtensionNames = devExts.data();
    VKCHECK(vkCreateDevice(p.phys, &dvci, nullptr, &p.dev), 3, "vkCreateDevice");
    vkGetDeviceQueue(p.dev, p.gfxFamily, 0, &p.gfxQueue);
    vkGetDeviceQueue(p.dev, p.presentFamily, 0, &p.presentQueue);
    return true;
}

// =============================================================================
// Swapchain (re)build on the HWND surface. TRANSFER_DST (we blit into it).
// =============================================================================
static bool buildSwapchain(Probe& s, uint32_t w, uint32_t h, VkSwapchainKHR oldSwapchain) {
    VkSurfaceCapabilitiesKHR caps{};
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.phys, s.surface, &caps), 4, "surface caps");

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface, &fmtCount, fmts.data());
    VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) { extent.width = w; extent.height = h; }
    if (extent.width  < caps.minImageExtent.width)  extent.width  = caps.minImageExtent.width;
    if (extent.width  > caps.maxImageExtent.width)  extent.width  = caps.maxImageExtent.width;
    if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
    if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    if (extent.width == 0 || extent.height == 0) { extent.width = w; extent.height = h; }

    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface          = s.surface;
    sci.minImageCount    = imgCount;
    sci.imageFormat      = chosen.format;
    sci.imageColorSpace  = chosen.colorSpace;
    sci.imageExtent      = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    uint32_t families[2] = { s.gfxFamily, s.presentFamily };
    if (s.gfxFamily != s.presentFamily) {
        sci.imageSharingMode      = VK_SHARING_MODE_CONCURRENT;
        sci.queueFamilyIndexCount = 2;
        sci.pQueueFamilyIndices   = families;
    } else {
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    sci.preTransform   = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode    = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped        = VK_TRUE;
    sci.oldSwapchain   = oldSwapchain;

    VkResult scr = vkCreateSwapchainKHR(s.dev, &sci, nullptr, &s.swapchain);
    if (scr != VK_SUCCESS)
        return fail(4, "vkCreateSwapchainKHR ON THE GL-WINDOW HWND SURFACE -> %s", vkStr(scr));
    s.scFormat = chosen.format;
    s.scExtent = extent;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(s.dev, s.swapchain, &n, nullptr);
    s.scImages.resize(n);
    vkGetSwapchainImagesKHR(s.dev, s.swapchain, &n, s.scImages.data());

    s.renderSems.resize(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i) {
        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(s.dev, &semci, nullptr, &s.renderSems[i]), 4, "per-image renderSem");
    }
    LOG("[probe] swapchain OK images=%u extent=%ux%u format=%d present_mode=FIFO",
        n, extent.width, extent.height, (int)chosen.format);
    return true;
}

static bool rebuildSwapchain(Probe& s, uint32_t w, uint32_t h) {
    vkDeviceWaitIdle(s.dev);
    VkSwapchainKHR old = s.swapchain;
    std::vector<VkSemaphore> oldSems = s.renderSems;
    s.renderSems.clear(); s.scImages.clear(); s.swapchain = VK_NULL_HANDLE;
    bool ok = buildSwapchain(s, w, h, old);
    for (VkSemaphore sem : oldSems) if (sem) vkDestroySemaphore(s.dev, sem, nullptr);
    if (old) vkDestroySwapchainKHR(s.dev, old, nullptr);
    if (!ok) return false;
    ++s.resize_count;
    return true;
}

static bool createSrcImage(Probe& s, uint32_t w, uint32_t h) {
    s.srcW = w; s.srcH = h;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format    = s.srcFormat;
    ici.extent    = {w, h, 1};
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples   = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling    = VK_IMAGE_TILING_OPTIMAL;
    ici.usage     = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(s.dev, &ici, nullptr, &s.srcImage), 5, "vkCreateImage(src)");
    VkMemoryRequirements mr{};
    vkGetImageMemoryRequirements(s.dev, s.srcImage, &mr);
    uint32_t memType = 0;
    if (!findMemType(s.phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memType))
        return fail(5, "no DEVICE_LOCAL memory type for src image");
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = mr.size;
    mai.memoryTypeIndex = memType;
    VKCHECK(vkAllocateMemory(s.dev, &mai, nullptr, &s.srcMem), 5, "vkAllocateMemory(src)");
    VKCHECK(vkBindImageMemory(s.dev, s.srcImage, s.srcMem, 0), 5, "vkBindImageMemory(src)");
    return true;
}

static void barrier(VkCommandBuffer cb, VkImage img,
                    VkImageLayout oldL, VkImageLayout newL,
                    VkAccessFlags srcA, VkAccessFlags dstA,
                    VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL; b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = srcA; b.dstAccessMask = dstA;
    vkCmdPipelineBarrier(cb, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static bool createCmdAndSync(Probe& s) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = s.gfxFamily;
    VKCHECK(vkCreateCommandPool(s.dev, &pci, nullptr, &s.cpool), 5, "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = s.cpool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VKCHECK(vkAllocateCommandBuffers(s.dev, &cai, &s.cbuf), 5, "vkAllocateCommandBuffers");
    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHECK(vkCreateSemaphore(s.dev, &semci, nullptr, &s.acquireSem), 5, "acquireSem");
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VKCHECK(vkCreateFence(s.dev, &fci, nullptr, &s.inFlight), 5, "inFlight fence");
    return true;
}

// =============================================================================
// One frame: (COEXISTENCE STRESS) do a trivial GL render into FBO-0 but DO NOT
// SwapWindow -> VK acquire -> blit rolling color -> present.
// =============================================================================
static bool presentOneFrame(Probe& s, uint32_t frameIdx) {
    SDL_Event ev; while (SDL_PollEvent(&ev)) { /* drain */ }

    // --- GL side: render into FBO-0 (default framebuffer) but NEVER swap. This
    // mimics the P13 intercept: GL keeps rendering, GL swap is suppressed in
    // =vulkan mode. Exercises the live GL context concurrently with VK present. ---
    if (s.glctx) {
        SDL_GL_MakeCurrent(s.win, s.glctx);
        float g = (float)((frameIdx * 5u) % 256u) / 255.0f;
        glViewport(0, 0, (GLsizei)s.scExtent.width, (GLsizei)s.scExtent.height);
        glClearColor(0.0f, g, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();                     // force the GL work through
        // DELIBERATELY NO SDL_GL_SwapWindow(s.win);  <-- the whole point
    }

    VKCHECK(vkWaitForFences(s.dev, 1, &s.inFlight, VK_TRUE, UINT64_MAX), 6, "waitFences");
    vkResetFences(s.dev, 1, &s.inFlight);

    uint32_t imageIndex = 0;
    VkResult ar = vkAcquireNextImageKHR(s.dev, s.swapchain, UINT64_MAX,
                                        s.acquireSem, VK_NULL_HANDLE, &imageIndex);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        int w=0,h=0; SDL_GL_GetDrawableSize(s.win,&w,&h);
        if (!rebuildSwapchain(s,(uint32_t)w,(uint32_t)h)) return false;
        VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        vkQueueSubmit(s.gfxQueue, 1, &empty, s.inFlight);
        return true;
    } else if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        return fail(6, "vkAcquireNextImageKHR -> %s", vkStr(ar));
    }

    // rolling color so the window visibly changes on screen
    VkClearColorValue col{};
    col.float32[0] = (float)((frameIdx * 37u) % 256u) / 255.0f;
    col.float32[1] = (float)((frameIdx * 73u) % 256u) / 255.0f;
    col.float32[2] = (float)((frameIdx * 113u) % 256u) / 255.0f;
    col.float32[3] = 1.0f;

    vkResetCommandBuffer(s.cbuf, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cbuf, &bi);

    VkImage scImg = s.scImages[imageIndex];
    VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    barrier(s.cbuf, s.srcImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdClearColorImage(s.cbuf, s.srcImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &whole);
    barrier(s.cbuf, s.srcImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    barrier(s.cbuf, scImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = {0,0,0};
    region.srcOffsets[1] = {(int32_t)s.srcW, (int32_t)s.srcH, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstOffsets[0] = {0,0,0};
    region.dstOffsets[1] = {(int32_t)s.scExtent.width, (int32_t)s.scExtent.height, 1};
    vkCmdBlitImage(s.cbuf,
                   s.srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   scImg,      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_LINEAR);

    barrier(s.cbuf, scImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    vkEndCommandBuffer(s.cbuf);

    VkSemaphore imgRenderSem = s.renderSems[imageIndex];
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &s.acquireSem;
    si.pWaitDstStageMask    = &waitStage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &s.cbuf;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &imgRenderSem;
    VKCHECK(vkQueueSubmit(s.gfxQueue, 1, &si, s.inFlight), 6, "vkQueueSubmit");

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &imgRenderSem;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &s.swapchain;
    pi.pImageIndices      = &imageIndex;
    VkResult pr = vkQueuePresentKHR(s.presentQueue, &pi);
    if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) {
        ++s.present_frames;
        if (pr == VK_SUBOPTIMAL_KHR) {
            int w=0,h=0; SDL_GL_GetDrawableSize(s.win,&w,&h);
            rebuildSwapchain(s,(uint32_t)w,(uint32_t)h);
        }
    } else if (pr == VK_ERROR_OUT_OF_DATE_KHR) {
        int w=0,h=0; SDL_GL_GetDrawableSize(s.win,&w,&h);
        if (!rebuildSwapchain(s,(uint32_t)w,(uint32_t)h)) return false;
    } else {
        return fail(6, "vkQueuePresentKHR -> %s", vkStr(pr));
    }
    SDL_Delay(12); // ~ keep it visible on screen a beat
    return true;
}

static void destroyProbe(Probe& s) {
    if (s.dev) vkDeviceWaitIdle(s.dev);
    if (s.inFlight)   vkDestroyFence(s.dev, s.inFlight, nullptr);
    if (s.acquireSem) vkDestroySemaphore(s.dev, s.acquireSem, nullptr);
    if (s.cpool)      vkDestroyCommandPool(s.dev, s.cpool, nullptr);
    for (VkSemaphore sem : s.renderSems) if (sem) vkDestroySemaphore(s.dev, sem, nullptr);
    s.renderSems.clear();
    if (s.swapchain) vkDestroySwapchainKHR(s.dev, s.swapchain, nullptr);
    if (s.srcImage) vkDestroyImage(s.dev, s.srcImage, nullptr);
    if (s.srcMem)   vkFreeMemory(s.dev, s.srcMem, nullptr);
    if (s.dev)      vkDestroyDevice(s.dev, nullptr);
    if (s.surface)  vkDestroySurfaceKHR(s.inst, s.surface, nullptr);
    if (s.messenger && s.pDestroyDbg) s.pDestroyDbg(s.inst, s.messenger, nullptr);
    if (s.inst)     vkDestroyInstance(s.inst, nullptr);
    if (s.glctx)    SDL_GL_DeleteContext(s.glctx);
    if (s.win)      SDL_DestroyWindow(s.win);
    if (s.sdlVideoInited) SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    LOG("=== vk_hwnd_present_probe (VK-P13b T0) : VK-present to the GL window's HWND ===");

    Probe s;
    int exitCode = 0;

    if (!createGlWindowAndContext(s)) {
        LOG("[VERDICT] FAIL: could not stand up the GL window/context (stage %d: %s)", g_failStage, g_failMsg);
        destroyProbe(s);
        return 2;
    }
    if (!createVkInstanceSurfaceDevice(s)) {
        LOG("[VERDICT] NOT VIABLE: same-HWND VK surface/device setup failed (stage %d: %s)", g_failStage, g_failMsg);
        LOG("[VERDICT]   -> P13b needs the two-window input-forward path.");
        destroyProbe(s);
        return 3;
    }
    if (!buildSwapchain(s, kInitW, kInitH, VK_NULL_HANDLE) ||
        !createSrcImage(s, kInitW, kInitH) ||
        !createCmdAndSync(s)) {
        LOG("[VERDICT] NOT VIABLE: swapchain/src/cmd on the HWND surface failed (stage %d: %s)", g_failStage, g_failMsg);
        destroyProbe(s);
        return 3;
    }

    bool runOk = true;
    for (uint32_t f = 0; f < kFrames && runOk; ++f) {
        if (f == kResizeAtFrame) {
            LOG("[probe] --- forcing swapchain rebuild at frame %u (resize handling) ---", f);
            int w=0,h=0; SDL_GL_GetDrawableSize(s.win,&w,&h);
            if (!rebuildSwapchain(s, (uint32_t)w, (uint32_t)h)) { runOk = false; break; }
        }
        if (!presentOneFrame(s, f)) runOk = false;
    }
    vkDeviceWaitIdle(s.dev);

    LOG("[probe] present_frames=%u resize_count=%u validation_errors=%d",
        s.present_frames, s.resize_count, g_valErrors);

    bool framesOk = (s.present_frames >= kFrames - 4); // allow a couple OUT_OF_DATE retries
    bool valClean = (g_valErrors == 0);
    bool pass = runOk && framesOk && valClean;

    if (pass) {
        LOG("[VERDICT] VIABLE: vkCreateWin32SurfaceKHR on the GL window's HWND WORKS -- "
            "presented %u frames to the SAME window the GL context owns, validation_errors=0, "
            "GL context coexisted (rendered into FBO-0 every frame, no GL swap), no crash.",
            s.present_frames);
        LOG("[VERDICT]   -> P13b is a SMALL slice: swap the surface-create to the GL HWND, "
            "drop the hidden window + event drain, don't hide the GL window.");
        exitCode = 0;
    } else {
        LOG("[VERDICT] NOT VIABLE (or degraded): runOk=%d frames=%u(need>=%u) val_errors=%d",
            runOk ? 1 : 0, s.present_frames, kFrames - 4, g_valErrors);
        LOG("[VERDICT]   -> P13b needs the two-window input-forward path (or investigate the errors above).");
        exitCode = 1;
    }

    destroyProbe(s);
    LOG("=== vk_hwnd_present_probe done (exit %d) ===", exitCode);
    return exitCode;
}
