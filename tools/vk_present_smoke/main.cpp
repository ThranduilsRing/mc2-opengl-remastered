// =============================================================================
// VK-P13-SELECTOR-PLAN  T0  --  headless VK swapchain / present + fallback SMOKE
//
// PROVES (headless, in seconds, WITHOUT the game) that the VK present-of-a-
// shared-image machinery works end-to-end:
//
//   1. Stand up a VK instance/device + a HIDDEN SDL_WINDOW_VULKAN + surface.
//   2. Create a swapchain on that hidden window (FIFO present mode).
//   3. Create a source VkImage (the "GL frame" stand-in), filled per frame with
//      a DETERMINISTIC pattern via vkCmdClearColorImage (a rolling color).
//   4. Per frame: blit source image -> acquired swapchain image (vkCmdBlitImage)
//      -> vkQueuePresentKHR. Assert present returns VK_SUCCESS every frame.
//   5. Resize mid-run: recreate the swapchain, keep presenting, validation-clean.
//   6. FALLBACK: force a VK init failure (MC2_VK_FORCE_FAIL) -> clean fail verdict,
//      no crash / no hang.
//   7. Validation (sync preset) enabled: count errors, MUST be 0.
//
// This MIRRORS the proven swapchain/acquire/present/resize logic from
// GameOS/gameos/vulkan_present_object.cpp (the L5 present object) -- including
// the ONE-render-semaphore-PER-SWAPCHAIN-IMAGE fix -- but replaces the object's
// fullscreen-triangle draw with the vkCmdBlitImage(shared-image -> swapchain)
// that P13 actually needs. The present object is volk+vk-bootstrap and engine-
// coupled (inert unless MC2_VULKAN, depends on vulkan_backend_skeleton.h for
// shader loading), so per the plan's guidance the minimal swapchain logic is
// COPIED into this standalone harness rather than linked. Standalone deps: only
// SDL2 + vulkan-1 (raw vulkan.h), exactly like tools/vk_gl_interop_spike.
//
// A pure-VK-present smoke does NOT need the GL side at all: a VK-filled image
// blitted to the swapchain proves present + blit + resize + fallback. The GL
// frame copy (copy-1) is exercised only by the engine wiring (T3), NOT here.
//
// Exit code 0 == PASS (K frames presented, resize clean, fallback clean, 0 val
//   errors), with a "[VERDICT] PASS ..." line.
// Non-zero == FAIL, with the exact failing stage printed + "[VERDICT] FAIL ...".
//
// NEGATIVE-TEST knob (proves the gate has teeth): MC2_VK_NEG_TEST=<mode>
//   skipblit  -> skip the vkCmdBlitImage; swapchain image never transitioned out
//                of UNDEFINED by us -> present a garbage/undefined image + a sync-
//                validation layout error is expected. Harness must then FAIL.
//   badlayout -> present the swapchain image while still in TRANSFER_DST layout
//                (never transitioned to PRESENT_SRC) -> validation error, FAIL.
// With no knob set the harness PASSes; set the knob and it must FAIL. That round
// trip proves the validation/verdict gate is not a rubber stamp.
// =============================================================================

#include <windows.h>

#include <SDL.h>
#include <SDL_vulkan.h>

#include <vulkan/vulkan.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

// ---- config -----------------------------------------------------------------
static const uint32_t kInitW   = 640;
static const uint32_t kInitH   = 360;   // 16:9, arbitrary
static const uint32_t kResizeW = 800;
static const uint32_t kResizeH = 600;
static const uint32_t kFrames  = 32;    // K >= 8 (plan §4.2); 32 exercises reuse well
static const uint32_t kResizeAtFrame = 16;

// ---- logging + verdict ------------------------------------------------------
#define LOG(...)  do { std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while (0)

static int  g_failStage = 0;
static char g_failMsg[512] = {0};
static int  g_valErrors = 0;
// When true, validation ERRORs are EXPECTED (the forced-fail fallback sub-test
// deliberately triggers vkCreateDevice failure) and must NOT taint the real
// run's validation-error count. They are still logged, just not counted.
static bool g_expectValErrors = false;

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
        if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT && !g_expectValErrors) g_valErrors++;
    }
    return VK_FALSE;
}

// ---- env helpers ------------------------------------------------------------
static bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}
static std::string envStr(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// =============================================================================
// The harness state. Mirrors VulkanPresentObject's members but standalone (raw
// vulkan.h, no volk/vkb). One frame in flight; one render-semaphore PER
// swapchain image (the L5 present-semaphore-reuse fix).
// =============================================================================
struct Smoke {
    SDL_Window*       win  = nullptr;
    bool              sdlVideoInited = false;
    bool              sdlVulkanLoaded = false;

    VkInstance        inst  = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR      surface = VK_NULL_HANDLE;
    VkPhysicalDevice  phys  = VK_NULL_HANDLE;
    VkDevice          dev   = VK_NULL_HANDLE;
    uint32_t          gfxFamily = UINT32_MAX;
    uint32_t          presentFamily = UINT32_MAX;
    VkQueue           gfxQueue = VK_NULL_HANDLE;
    VkQueue           presentQueue = VK_NULL_HANDLE;

    // swapchain resources (rebuilt on resize)
    VkSwapchainKHR    swapchain = VK_NULL_HANDLE;
    VkFormat          scFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D        scExtent = {0, 0};
    VkPresentModeKHR  presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage>     scImages;
    std::vector<VkSemaphore> renderSems; // one per swapchain image (present-wait)

    // command + per-frame sync (1 frame in flight)
    VkCommandPool     cpool = VK_NULL_HANDLE;
    VkCommandBuffer   cbuf  = VK_NULL_HANDLE;
    VkSemaphore       acquireSem = VK_NULL_HANDLE;
    VkFence           inFlight = VK_NULL_HANDLE;

    // source image (the "GL frame" stand-in): filled per frame, blitted to swapchain
    VkImage           srcImage = VK_NULL_HANDLE;
    VkDeviceMemory    srcMem   = VK_NULL_HANDLE;
    uint32_t          srcW = 0, srcH = 0;
    VkFormat          srcFormat = VK_FORMAT_R8G8B8A8_UNORM;

    PFN_vkCreateDebugUtilsMessengerEXT  pCreateDbg  = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT pDestroyDbg = nullptr;

    // health / counters
    uint32_t present_frames = 0;
    uint32_t resize_count   = 0;

    // negative-test mode
    enum NegMode { NEG_NONE, NEG_SKIPBLIT, NEG_BADLAYOUT } neg = NEG_NONE;
};

static bool findMemType(VkPhysicalDevice phys, uint32_t typeBits,
                        VkMemoryPropertyFlags want, uint32_t* out) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) { *out = i; return true; }
    }
    return false;
}

// =============================================================================
// Instance + hidden VK window + surface + device + queues.
// =============================================================================
static bool initInstanceWindowDevice(Smoke& s, bool forceFail) {
    // ---- SDL video ----
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
            return fail(1, "SDL_InitSubSystem(VIDEO): %s", SDL_GetError());
        s.sdlVideoInited = true;
    }
    if (SDL_Vulkan_LoadLibrary(nullptr) != 0)
        return fail(1, "SDL_Vulkan_LoadLibrary: %s", SDL_GetError());
    s.sdlVulkanLoaded = true;

    // ---- hidden Vulkan window ----
    Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN;
    s.win = SDL_CreateWindow("vk_present_smoke (headless)",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             (int)kInitW, (int)kInitH, flags);
    if (!s.win) return fail(1, "SDL_CreateWindow(VULKAN|HIDDEN): %s", SDL_GetError());
    LOG("[smoke] created HIDDEN %ux%u Vulkan window", kInitW, kInitH);

    // ---- instance extensions (surface exts from SDL) ----
    unsigned int extCount = 0;
    if (SDL_Vulkan_GetInstanceExtensions(s.win, &extCount, nullptr) != SDL_TRUE)
        return fail(2, "GetInstanceExtensions(count): %s", SDL_GetError());
    std::vector<const char*> instExts(extCount);
    if (extCount && SDL_Vulkan_GetInstanceExtensions(s.win, &extCount, instExts.data()) != SDL_TRUE)
        return fail(2, "GetInstanceExtensions(names): %s", SDL_GetError());
    instExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

    // ---- validation preset (sync) ----
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    if (layerCount) vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    bool haveValidation = false;
    for (auto& l : layers)
        if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) haveValidation = true;

    // Resolve MC2_VULKAN_VALIDATION (default: sync, since T0 requires validation).
    std::string valPreset = envStr("MC2_VULKAN_VALIDATION");
    if (valPreset.empty()) valPreset = "sync";
    bool wantValidation = haveValidation && valPreset != "0" && valPreset != "off";
    LOG("[smoke] validation layer available=%d preset=%s enabled=%d",
        haveValidation ? 1 : 0, valPreset.c_str(), wantValidation ? 1 : 0);

    std::vector<const char*> instLayers;
    std::vector<VkValidationFeatureEnableEXT> valFeats;
    if (wantValidation) {
        instLayers.push_back("VK_LAYER_KHRONOS_validation");
        if (valPreset == "sync")
            valFeats.push_back(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
    }

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "vk_present_smoke";
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
    valFeatInfo.pNext = &dci; // chain messenger for instance-creation-time messages

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pNext = wantValidation ? (const void*)&valFeatInfo : (const void*)&dci;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount   = (uint32_t)instExts.size();
    ici.ppEnabledExtensionNames = instExts.data();
    ici.enabledLayerCount   = (uint32_t)instLayers.size();
    ici.ppEnabledLayerNames = instLayers.empty() ? nullptr : instLayers.data();

    VKCHECK(vkCreateInstance(&ici, nullptr, &s.inst), 2, "vkCreateInstance");

    s.pCreateDbg  = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(s.inst, "vkCreateDebugUtilsMessengerEXT");
    s.pDestroyDbg = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(s.inst, "vkDestroyDebugUtilsMessengerEXT");
    if (wantValidation && s.pCreateDbg) s.pCreateDbg(s.inst, &dci, nullptr, &s.messenger);

    // ---- surface ----
    if (SDL_Vulkan_CreateSurface(s.win, s.inst, &s.surface) != SDL_TRUE)
        return fail(2, "SDL_Vulkan_CreateSurface: %s", SDL_GetError());

    // ---- FALLBACK injection point: simulate a VK-present init failure here. We
    // request an impossible device extension so vkCreateDevice fails cleanly,
    // exactly the shape of a real driver/env that can't do the present path. This
    // exercises the fail-soft branch WITHOUT a crash. ----
    const char* kImpossibleExt = "VK_MC2_this_extension_does_not_exist";

    // ---- physical device: prefer discrete, must support present on our surface -
    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(s.inst, &pdCount, nullptr);
    if (pdCount == 0) return fail(2, "no Vulkan physical devices");
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(s.inst, &pdCount, pds.data());
    s.phys = pds[0];
    for (auto pd : pds) {
        VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(pd, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) s.phys = pd;
    }
    {
        VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(s.phys, &p);
        LOG("[smoke] using physical device: %s", p.deviceName);
    }

    // ---- queue families (graphics + present) ----
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &qfCount, qfs.data());
    for (uint32_t q = 0; q < qfCount; ++q) {
        if ((qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && s.gfxFamily == UINT32_MAX)
            s.gfxFamily = q;
        VkBool32 present = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(s.phys, q, s.surface, &present) == VK_SUCCESS &&
            present == VK_TRUE && s.presentFamily == UINT32_MAX)
            s.presentFamily = q;
    }
    if (s.gfxFamily == UINT32_MAX || s.presentFamily == UINT32_MAX)
        return fail(2, "no graphics/present queue family (gfx=%u present=%u)", s.gfxFamily, s.presentFamily);

    // ---- device ----
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qcis[2]{};
    uint32_t qciCount = 0;
    qcis[qciCount] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qcis[qciCount].queueFamilyIndex = s.gfxFamily;
    qcis[qciCount].queueCount = 1;
    qcis[qciCount].pQueuePriorities = &prio;
    ++qciCount;
    if (s.presentFamily != s.gfxFamily) {
        qcis[qciCount] = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qcis[qciCount].queueFamilyIndex = s.presentFamily;
        qcis[qciCount].queueCount = 1;
        qcis[qciCount].pQueuePriorities = &prio;
        ++qciCount;
    }
    std::vector<const char*> devExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    if (forceFail) {
        devExts.push_back(kImpossibleExt);   // <-- forced clean failure
        LOG("[smoke] MC2_VK_FORCE_FAIL: requesting impossible device extension '%s'", kImpossibleExt);
    }
    VkDeviceCreateInfo dvci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dvci.queueCreateInfoCount    = qciCount;
    dvci.pQueueCreateInfos       = qcis;
    dvci.enabledExtensionCount   = (uint32_t)devExts.size();
    dvci.ppEnabledExtensionNames = devExts.data();
    VkResult dr = vkCreateDevice(s.phys, &dvci, nullptr, &s.dev);
    if (dr != VK_SUCCESS)
        return fail(3, "vkCreateDevice -> %s", vkStr(dr));  // fail-soft: caller treats as fallback
    vkGetDeviceQueue(s.dev, s.gfxFamily, 0, &s.gfxQueue);
    vkGetDeviceQueue(s.dev, s.presentFamily, 0, &s.presentQueue);
    return true;
}

// =============================================================================
// Swapchain (re)build. Mirrors vulkan_present_object.cpp build_swapchain_resources
// but raw + BLIT-target usage (TRANSFER_DST) instead of COLOR_ATTACHMENT: we blit
// into the swapchain image, we do not render-pass into it.
// =============================================================================
static void destroySwapchainResources(Smoke& s) {
    for (VkSemaphore sem : s.renderSems) if (sem) vkDestroySemaphore(s.dev, sem, nullptr);
    s.renderSems.clear();
    s.scImages.clear();
    if (s.swapchain) { vkDestroySwapchainKHR(s.dev, s.swapchain, nullptr); s.swapchain = VK_NULL_HANDLE; }
}

static bool buildSwapchain(Smoke& s, uint32_t w, uint32_t h, VkSwapchainKHR oldSwapchain) {
    VkSurfaceCapabilitiesKHR caps{};
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.phys, s.surface, &caps), 4, "surface caps");

    // format
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface, &fmtCount, fmts.data());
    VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }

    // present mode: FIFO always supported (plan §6/R8 chosen mode). Note others.
    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(s.phys, s.surface, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> pms(pmCount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(s.phys, s.surface, &pmCount, pms.data());
    bool haveMailbox = false, haveImmediate = false;
    for (auto m : pms) { if (m == VK_PRESENT_MODE_MAILBOX_KHR) haveMailbox = true;
                         if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) haveImmediate = true; }
    s.presentMode = VK_PRESENT_MODE_FIFO_KHR; // guaranteed present, vsync-equivalent

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) { // undefined -> clamp desired
        extent.width  = w; extent.height = h;
    }
    if (extent.width  < caps.minImageExtent.width)  extent.width  = caps.minImageExtent.width;
    if (extent.width  > caps.maxImageExtent.width)  extent.width  = caps.maxImageExtent.width;
    if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
    if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;

    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface          = s.surface;
    sci.minImageCount    = imgCount;
    sci.imageFormat      = chosen.format;
    sci.imageColorSpace  = chosen.colorSpace;
    sci.imageExtent      = extent;
    sci.imageArrayLayers = 1;
    // TRANSFER_DST because we vkCmdBlitImage INTO the swapchain image.
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
    sci.presentMode    = s.presentMode;
    sci.clipped        = VK_TRUE;
    sci.oldSwapchain   = oldSwapchain;

    VKCHECK(vkCreateSwapchainKHR(s.dev, &sci, nullptr, &s.swapchain), 4, "vkCreateSwapchainKHR");
    s.scFormat = chosen.format;
    s.scExtent = extent;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(s.dev, s.swapchain, &n, nullptr);
    s.scImages.resize(n);
    vkGetSwapchainImagesKHR(s.dev, s.swapchain, &n, s.scImages.data());

    // one render-finished semaphore PER IMAGE (the L5 present-semaphore-reuse fix)
    s.renderSems.resize(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i) {
        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(s.dev, &semci, nullptr, &s.renderSems[i]), 4, "per-image renderSem");
    }

    LOG("[smoke] swapchain OK images=%u extent=%ux%u format=%d present_mode=FIFO"
        " (mailbox_avail=%d immediate_avail=%d)",
        n, extent.width, extent.height, (int)chosen.format, haveMailbox ? 1 : 0, haveImmediate ? 1 : 0);
    return true;
}

static bool rebuildSwapchain(Smoke& s, uint32_t w, uint32_t h) {
    vkDeviceWaitIdle(s.dev);
    VkSwapchainKHR old = s.swapchain;
    // Build fresh with `old` so driver reuses; then destroy old resources.
    std::vector<VkSemaphore> oldSems = s.renderSems;
    s.renderSems.clear();
    s.scImages.clear();
    s.swapchain = VK_NULL_HANDLE;
    bool ok = buildSwapchain(s, w, h, old);
    for (VkSemaphore sem : oldSems) if (sem) vkDestroySemaphore(s.dev, sem, nullptr);
    if (old) vkDestroySwapchainKHR(s.dev, old, nullptr);
    if (!ok) return false;
    ++s.resize_count;
    return true;
}

// =============================================================================
// Source image (the "GL frame" stand-in). Plain device-local RGBA8, TRANSFER_SRC
// (blit source) + TRANSFER_DST (we clear it each frame to a rolling color). This
// is the primitive the real path replaces with the exportable interop image; for
// a pure-VK present smoke a plain VK-filled image is sufficient.
// =============================================================================
static bool createSrcImage(Smoke& s, uint32_t w, uint32_t h) {
    s.srcW = w; s.srcH = h;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format    = s.srcFormat;
    ici.extent    = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
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

// image-layout transition helper (single subresource, whole color image)
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

// =============================================================================
// One frame: fill src (deterministic rolling color), acquire, blit src->swapchain,
// transition to PRESENT_SRC, submit, present. Returns false fail-soft on hard err.
// =============================================================================
static bool presentOneFrame(Smoke& s, uint32_t frameIdx) {
    SDL_Event ev; while (SDL_PollEvent(&ev)) { /* drain */ }

    VKCHECK(vkWaitForFences(s.dev, 1, &s.inFlight, VK_TRUE, UINT64_MAX), 6, "waitFences");
    vkResetFences(s.dev, 1, &s.inFlight);

    uint32_t imageIndex = 0;
    VkResult ar = vkAcquireNextImageKHR(s.dev, s.swapchain, UINT64_MAX,
                                        s.acquireSem, VK_NULL_HANDLE, &imageIndex);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) {
        int w = 0, h = 0; SDL_Vulkan_GetDrawableSize(s.win, &w, &h);
        if (!rebuildSwapchain(s, (uint32_t)w, (uint32_t)h)) return false;
        VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        vkQueueSubmit(s.gfxQueue, 1, &empty, s.inFlight); // re-signal fence
        return true; // retry next call
    } else if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
        return fail(6, "vkAcquireNextImageKHR -> %s", vkStr(ar));
    }

    // deterministic rolling color: derived purely from frame index.
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

    // 1) fill src: UNDEFINED -> TRANSFER_DST, clear, -> TRANSFER_SRC
    barrier(s.cbuf, s.srcImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdClearColorImage(s.cbuf, s.srcImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &whole);
    barrier(s.cbuf, s.srcImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // 2) swapchain image: UNDEFINED -> TRANSFER_DST (skipped in NEG_SKIPBLIT to
    //    prove the gate: no transition + no blit -> present an UNDEFINED image and
    //    sync-validation flags it).
    if (s.neg != Smoke::NEG_SKIPBLIT) {
        // srcStageMask MUST include the stage the acquire semaphore is waited at
        // (COLOR_ATTACHMENT_OUTPUT below) so this transition forms an execution
        // dependency with vkAcquireNextImageKHR -- otherwise sync-validation flags
        // a WRITE_AFTER_READ hazard (acquire "reads" the image before our write).
        barrier(s.cbuf, scImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        // 3) BLIT src -> swapchain (scaled to swapchain extent).
        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[0] = {0, 0, 0};
        region.srcOffsets[1] = {(int32_t)s.srcW, (int32_t)s.srcH, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstOffsets[0] = {0, 0, 0};
        region.dstOffsets[1] = {(int32_t)s.scExtent.width, (int32_t)s.scExtent.height, 1};
        vkCmdBlitImage(s.cbuf,
                       s.srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       scImg,      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region, VK_FILTER_LINEAR);

        // 4) swapchain -> PRESENT_SRC (skipped in NEG_BADLAYOUT: present while
        //    still TRANSFER_DST -> validation error, proving the gate).
        if (s.neg != Smoke::NEG_BADLAYOUT) {
            barrier(s.cbuf, scImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        }
    }

    vkEndCommandBuffer(s.cbuf);

    VkSemaphore imgRenderSem = s.renderSems[imageIndex];
    // Wait the acquire semaphore at COLOR_ATTACHMENT_OUTPUT -- must match the
    // srcStageMask of the first swapchain-image barrier above (execution
    // dependency with the acquire; see sync-validation note there).
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
            int w = 0, h = 0; SDL_Vulkan_GetDrawableSize(s.win, &w, &h);
            rebuildSwapchain(s, (uint32_t)w, (uint32_t)h);
        }
    } else if (pr == VK_ERROR_OUT_OF_DATE_KHR) {
        int w = 0, h = 0; SDL_Vulkan_GetDrawableSize(s.win, &w, &h);
        if (!rebuildSwapchain(s, (uint32_t)w, (uint32_t)h)) return false;
    } else {
        return fail(6, "vkQueuePresentKHR -> %s", vkStr(pr));
    }
    return true;
}

static bool createCmdAndSync(Smoke& s) {
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

static void destroySmoke(Smoke& s) {
    if (s.dev) vkDeviceWaitIdle(s.dev);
    if (s.inFlight)   vkDestroyFence(s.dev, s.inFlight, nullptr);
    if (s.acquireSem) vkDestroySemaphore(s.dev, s.acquireSem, nullptr);
    if (s.cpool)      vkDestroyCommandPool(s.dev, s.cpool, nullptr);
    destroySwapchainResources(s);
    if (s.srcImage) vkDestroyImage(s.dev, s.srcImage, nullptr);
    if (s.srcMem)   vkFreeMemory(s.dev, s.srcMem, nullptr);
    if (s.dev)      vkDestroyDevice(s.dev, nullptr);
    if (s.surface)  vkDestroySurfaceKHR(s.inst, s.surface, nullptr);
    if (s.messenger && s.pDestroyDbg) s.pDestroyDbg(s.inst, s.messenger, nullptr);
    if (s.inst)     vkDestroyInstance(s.inst, nullptr);
    if (s.win)      SDL_DestroyWindow(s.win);
    if (s.sdlVulkanLoaded)  SDL_Vulkan_UnloadLibrary();
    if (s.sdlVideoInited)   SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

// =============================================================================
// FALLBACK path: force VK init failure, confirm clean fail verdict, no crash.
// Returns true if the fallback behaved correctly (detected the failure cleanly).
// =============================================================================
static bool runFallbackTest() {
    LOG("[smoke] === FALLBACK TEST (forced VK init failure) ===");
    Smoke s;
    g_failStage = 0; g_failMsg[0] = 0;
    g_expectValErrors = true;   // forced failure deliberately triggers val ERRORs
    bool up = initInstanceWindowDevice(s, /*forceFail=*/true);
    g_expectValErrors = false;
    bool clean = !up && g_failStage != 0 && g_failMsg[0] != 0;
    if (clean)
        LOG("[smoke] FALLBACK OK: init failed cleanly at stage %d ('%s'), no crash -> GL fallback reachable",
            g_failStage, g_failMsg);
    else
        LOG("[smoke] FALLBACK BAD: forced-fail did not produce a clean failure (up=%d stage=%d)",
            up ? 1 : 0, g_failStage);
    destroySmoke(s);
    // reset global fail state so it doesn't taint the real run's verdict
    g_failStage = 0; g_failMsg[0] = 0;
    return clean;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    LOG("=== vk_present_smoke (VK-P13 T0) : headless swapchain/present + fallback ===");

    // negative-test mode selection
    std::string negMode = envStr("MC2_VK_NEG_TEST");
    Smoke::NegMode neg = Smoke::NEG_NONE;
    if (negMode == "skipblit") neg = Smoke::NEG_SKIPBLIT;
    else if (negMode == "badlayout") neg = Smoke::NEG_BADLAYOUT;
    if (neg != Smoke::NEG_NONE)
        LOG("[smoke] NEGATIVE-TEST mode='%s' -- expecting a validation error + FAIL verdict", negMode.c_str());

    // ---- (A) FALLBACK sub-test first (independent VK instance) --------------
    bool fallbackOk = runFallbackTest();

    // ---- (B) the real present run -------------------------------------------
    Smoke s;
    s.neg = neg;
    int exitCode = 0;

    if (!initInstanceWindowDevice(s, /*forceFail=*/false)) {
        LOG("[VERDICT] FAIL: could not stand up VK device/window (stage %d: %s)", g_failStage, g_failMsg);
        destroySmoke(s);
        return 2;
    }
    if (!buildSwapchain(s, kInitW, kInitH, VK_NULL_HANDLE) ||
        !createSrcImage(s, kInitW, kInitH) ||
        !createCmdAndSync(s)) {
        LOG("[VERDICT] FAIL: swapchain/src/cmd setup failed (stage %d: %s)", g_failStage, g_failMsg);
        destroySmoke(s);
        return 2;
    }

    bool runOk = true;
    for (uint32_t f = 0; f < kFrames && runOk; ++f) {
        if (f == kResizeAtFrame) {
            LOG("[smoke] --- resize at frame %u: %ux%u -> %ux%u ---",
                f, kInitW, kInitH, kResizeW, kResizeH);
            SDL_SetWindowSize(s.win, (int)kResizeW, (int)kResizeH);
            if (!rebuildSwapchain(s, kResizeW, kResizeH)) { runOk = false; break; }
        }
        if (!presentOneFrame(s, f)) runOk = false;
    }
    vkDeviceWaitIdle(s.dev);

    LOG("[smoke] present_frames=%u resize_count=%u validation_errors=%d",
        s.present_frames, s.resize_count, g_valErrors);

    // ---- verdict ------------------------------------------------------------
    // Expected: K frames presented, >=1 resize, fallback clean, 0 validation errs.
    bool framesOk   = (s.present_frames >= kFrames);
    bool resizeOk   = (s.resize_count   >= 1);
    bool valClean   = (g_valErrors == 0);
    bool pass = runOk && framesOk && resizeOk && fallbackOk && valClean;

    if (neg != Smoke::NEG_NONE) {
        // In negative-test mode we EXPECT failure (validation error). A "pass"
        // here means the deliberate break was NOT caught -> the gate is toothless.
        if (!valClean || !pass) {
            LOG("[VERDICT] NEGATIVE-TEST PASS: deliberate break ('%s') was DETECTED "
                "(validation_errors=%d) -> the gate has teeth.", negMode.c_str(), g_valErrors);
            exitCode = 0; // the negative test itself succeeded
        } else {
            LOG("[VERDICT] NEGATIVE-TEST FAIL: deliberate break ('%s') was NOT detected "
                "-> the gate is a rubber stamp. Fix the harness.", negMode.c_str());
            exitCode = 3;
        }
    } else {
        if (pass) {
            LOG("[VERDICT] PASS: presented %u frames (>=%u), resize_count=%u, fallback clean, "
                "validation_errors=0. VK swapchain+blit+present+resize+fallback proven headless.",
                s.present_frames, kFrames, s.resize_count);
            exitCode = 0;
        } else {
            LOG("[VERDICT] FAIL: runOk=%d frames=%u(need>=%u) resize=%u(need>=1) fallback=%d val_errors=%d",
                runOk ? 1 : 0, s.present_frames, kFrames, s.resize_count, fallbackOk ? 1 : 0, g_valErrors);
            exitCode = 1;
        }
    }

    destroySmoke(s);
    LOG("=== vk_present_smoke done (exit %d) ===", exitCode);
    return exitCode;
}
