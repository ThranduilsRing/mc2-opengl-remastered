// VULKAN-SWAPCHAIN-PRESENT-1 -- Layer-5 milestone: the engine OWNS a Vulkan
// swapchain and PRESENTS a controlled frame safely, fail-soft to GL.
//
// NARROW SCOPE. This TU is a SEPARATE, self-contained Vulkan present path. It
// does NOT migrate the game renderer, does NOT make Vulkan default, does NOT
// touch terrain/mech/staticprop, and routes NO game render pass through the
// swapchain. It creates its OWN visible SDL Vulkan window (distinct from the GL
// window g_sdl_window / RenderWindow::window_) that never shares the GL context,
// owns its own VkInstance/surface/device/swapchain, presents 16 frames (clearing
// to teal + drawing the fullscreen triangle), handles a mid-loop resize, tears
// everything down in reverse order, and emits a [VK_SWAPCHAIN_PRESENT_HEALTH]
// line. Whole TU compiles to nothing unless MC2_VULKAN is defined (CMake option,
// default OFF) -> the GL build is byte-identical. Fail-soft at EVERY step: any
// error logs a reason + returns false, never crashes (this is also the
// no-Vulkan-runtime path).
//
// Reuse (recon-confirmed): swapchain-probe pattern (SDL_WINDOW_VULKAN +
// SDL_Vulkan_CreateSurface + vkb::SwapchainBuilder), triangle-probe render
// pass/pipeline/shader (fullscreen.{vert,frag}.spv), the validation preset +
// messenger from vulkan_backend_skeleton.cpp (re-implemented locally here to keep
// this TU self-contained), volk init.

#include "vulkan_backend_skeleton.h"

#ifdef MC2_VULKAN

// VULKAN-VOLK-LOADER-1: volk owns Vulkan dispatch (dynamic load; no hard link).
#include <volk.h>

// vk-bootstrap public API (self-dispatching -- safe under VK_NO_PROTOTYPES).
#include "VkBootstrap.h"

// SDL2 Vulkan windowing (visible window + surface).
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void pp_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[VULKAN_SKELETON] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

const char* pp_format_str(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_SRGB:  return "B8G8R8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB:  return "R8G8B8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        default:                       return "other";
    }
}

const char* pp_present_mode_str(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default:                               return "other";
    }
}

// VULKAN-VALIDATION-PRESETS-1 (local copy, kept self-contained). Resolve
// MC2_VULKAN_VALIDATION into an enable/disable + optional feature set + name.
bool pp_resolve_validation_preset(std::vector<VkValidationFeatureEnableEXT>& feats,
                                  const char*& resolvedName) {
    feats.clear();
    resolvedName = "off";
    const char* env = std::getenv("MC2_VULKAN_VALIDATION");
    if (env == nullptr) return false;
    auto eq = [&](const char* s) { return std::strcmp(env, s) == 0; };
    if (eq("0") || eq("off")) { resolvedName = "off"; return false; }
    if (eq("1") || eq("core") || eq("")) { resolvedName = "core"; return true; }
    if (eq("sync")) {
        resolvedName = "sync";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT);
        return true;
    }
    if (eq("gpu-assisted")) {
        resolvedName = "gpu-assisted";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT);
        return true;
    }
    if (eq("best-practices")) {
        resolvedName = "best-practices";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
        return true;
    }
    if (eq("debug-printf")) {
        resolvedName = "debug-printf";
        feats.push_back(VK_VALIDATION_FEATURE_ENABLE_DEBUG_PRINTF_EXT);
        return true;
    }
    pp_log("swapchain-present: validation preset '%s' unrecognized; falling back to 'core'", env);
    resolvedName = "core";
    return true;
}

// Debug-messenger error counter. The probe is one-shot + single-threaded, so a
// plain file-static counter is sufficient (mirrors the descriptor probe).
uint32_t g_pp_validation_errors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL pp_debug_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*types*/,
    const VkDebugUtilsMessengerCallbackDataEXT* pData,
    void* /*user*/) {
    const char* msg = (pData && pData->pMessage) ? pData->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++g_pp_validation_errors;
        pp_log("swapchain-present: VALIDATION: %s", msg);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        pp_log("swapchain-present: validation-warning: %s", msg);
    }
    return VK_FALSE;
}

// Health snapshot emitted at the end (and optionally to debug_state).
struct VkSwapchainPresentHealth {
    bool         swapchain_available = false;
    bool         surface_created     = false;
    VkFormat     format              = VK_FORMAT_UNDEFINED;
    VkPresentModeKHR present_mode    = VK_PRESENT_MODE_FIFO_KHR;
    uint32_t     image_count         = 0;
    uint32_t     extent_w            = 0;
    uint32_t     extent_h            = 0;
    uint32_t     resize_count        = 0;
    uint32_t     present_frames      = 0;   // frames presented VK_SUCCESS
    std::string  fallback_reason;           // empty == clean
    uint32_t     validation_errors   = 0;
};

// Per-swapchain GPU resources that must be rebuilt on resize.
//
// SYNC NOTE (VULKAN-SWAPCHAIN-PRESENT-1): the render-finished semaphore that the
// PRESENT waits on MUST be PER SWAPCHAIN IMAGE, not per frame-in-flight. Present
// completion (when the semaphore's wait is retired) is tied to the swapchain
// image, not to our host-side frame counter. With one shared render semaphore and
// N>1 images, vkQueueSubmit can re-signal that semaphore while a prior present
// still holds a pending wait on it -> the "semaphore is being signaled ... but it
// may still be in use by VkSwapchainKHR" hazard that sync validation flags. One
// render semaphore per image (indexed by the acquired imageIndex) closes it: a
// given image's semaphore is only re-signaled after that image cycles back, by
// which time its previous present has necessarily retired.
struct SwapchainResources {
    vkb::Swapchain             swapchain{};
    bool                       have = false;
    std::vector<VkImage>       images;
    std::vector<VkImageView>   views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore>   renderSems; // one per image (present-wait target)
};

void destroy_swapchain_resources(VkDevice device, SwapchainResources& r) {
    for (VkSemaphore s : r.renderSems) {
        if (s) vkDestroySemaphore(device, s, nullptr);
    }
    for (VkFramebuffer fb : r.framebuffers) {
        if (fb) vkDestroyFramebuffer(device, fb, nullptr);
    }
    for (VkImageView v : r.views) {
        if (v) vkDestroyImageView(device, v, nullptr);
    }
    r.renderSems.clear();
    r.framebuffers.clear();
    r.views.clear();
    r.images.clear();
    if (r.have) {
        vkb::destroy_swapchain(r.swapchain);
        r.have = false;
    }
}

// Build (or rebuild) swapchain + image views + framebuffers for the current
// window/surface size. On failure returns false with `reason` set; the caller
// treats that as a fail-soft break. `oldSwapchain` (may be VK_NULL_HANDLE) is
// passed to vkb so the driver can reuse resources across a resize.
bool build_swapchain_resources(VkPhysicalDevice phys, VkDevice device,
                               VkSurfaceKHR surface, uint32_t gfxFamily,
                               uint32_t presentFamily, VkRenderPass rpass,
                               uint32_t desiredW, uint32_t desiredH,
                               VkSwapchainKHR oldSwapchain,
                               SwapchainResources& out, std::string& reason) {
    vkb::SwapchainBuilder scb(phys, device, surface, gfxFamily, presentFamily);
    scb.set_desired_format(VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_SRGB,
                                               VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
       .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
       .set_desired_extent(desiredW, desiredH)
       .set_image_usage_flags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (oldSwapchain != VK_NULL_HANDLE) scb.set_old_swapchain(oldSwapchain);

    auto scRet = scb.build();
    if (!scRet) {
        reason = "SwapchainBuilder.build failed: " + scRet.error().message();
        pp_log("swapchain-present: %s (VkResult=%d). fail-soft.",
               reason.c_str(), (int)scRet.vk_result());
        return false;
    }
    out.swapchain = scRet.value();
    out.have = true;

    // vkb owns image + view retrieval; query them so we can build framebuffers.
    auto imgs = out.swapchain.get_images();
    auto imgViews = out.swapchain.get_image_views();
    if (!imgs || !imgViews) {
        reason = "swapchain get_images/get_image_views failed";
        pp_log("swapchain-present: %s. fail-soft.", reason.c_str());
        return false;
    }
    out.images = imgs.value();
    out.views  = imgViews.value();

    out.framebuffers.resize(out.views.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < out.views.size(); ++i) {
        VkFramebufferCreateInfo fci{};
        fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass      = rpass;
        fci.attachmentCount = 1;
        fci.pAttachments    = &out.views[i];
        fci.width           = out.swapchain.extent.width;
        fci.height          = out.swapchain.extent.height;
        fci.layers          = 1;
        VkResult fr = vkCreateFramebuffer(device, &fci, nullptr, &out.framebuffers[i]);
        if (fr != VK_SUCCESS) {
            reason = "vkCreateFramebuffer failed (" + std::to_string((int)fr) + ")";
            pp_log("swapchain-present: %s. fail-soft.", reason.c_str());
            return false;
        }
    }

    // One render-finished semaphore per image (see SYNC NOTE on the struct).
    out.renderSems.resize(out.views.size(), VK_NULL_HANDLE);
    for (size_t i = 0; i < out.renderSems.size(); ++i) {
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkResult sr = vkCreateSemaphore(device, &sci, nullptr, &out.renderSems[i]);
        if (sr != VK_SUCCESS) {
            reason = "vkCreateSemaphore(per-image) failed (" + std::to_string((int)sr) + ")";
            pp_log("swapchain-present: %s. fail-soft.", reason.c_str());
            return false;
        }
    }
    return true;
}

// Optional debug_state dump of the health line (best-effort; never fatal).
void maybe_dump_debug_state(const VkSwapchainPresentHealth& h) {
    if (!std::getenv("MC2_DEBUG_STATE_DUMP")) return;
    std::FILE* f = std::fopen("debug_state/vk_swapchain_present_health.txt", "w");
    if (!f) return; // debug_state dir may not exist; best-effort only.
    std::fprintf(f,
        "swapchain_available=%d surface_created=%d format=%s present_mode=%s "
        "image_count=%u extent=%ux%u resize_count=%u present_frames=%u "
        "fallback_reason=%s validation_errors=%u\n",
        h.swapchain_available ? 1 : 0, h.surface_created ? 1 : 0,
        pp_format_str(h.format), pp_present_mode_str(h.present_mode),
        h.image_count, h.extent_w, h.extent_h, h.resize_count, h.present_frames,
        h.fallback_reason.empty() ? "(none)" : h.fallback_reason.c_str(),
        h.validation_errors);
    std::fclose(f);
}

} // namespace

bool mc2_vulkan_probe_swapchain_present(const char* spvDir) {
    g_pp_validation_errors = 0;
    VkSwapchainPresentHealth health;

    // ---- volk (Vulkan runtime) --------------------------------------------
    if (volkInitialize() != VK_SUCCESS) {
        health.fallback_reason = "volkInitialize failed -- no Vulkan runtime";
        pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
        // No instance yet -> emit the (empty) health line + return.
        goto emit_health;
    }

    {
    // ---- SDL video (visible Vulkan window) --------------------------------
    bool sdlVideoInitedHere = false;
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            health.fallback_reason = std::string("SDL_InitSubSystem(VIDEO): ") + SDL_GetError();
            pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
            goto emit_health;
        }
        sdlVideoInitedHere = true;
    }
    if (SDL_Vulkan_LoadLibrary(nullptr) != 0) {
        health.fallback_reason = std::string("SDL_Vulkan_LoadLibrary: ") + SDL_GetError();
        pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
        if (sdlVideoInitedHere) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        goto emit_health;
    }

    // Visible by default; MC2_VULKAN_SWAPCHAIN_PRESENT_HIDDEN forces hidden for
    // headless CI. Separate window from the GL window -- shares no GL context.
    const bool hidden = std::getenv("MC2_VULKAN_SWAPCHAIN_PRESENT_HIDDEN") != nullptr;
    Uint32 winFlags = SDL_WINDOW_VULKAN | (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    SDL_Window* window = SDL_CreateWindow(
        "MC2 Vulkan swapchain present (Layer 5)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, winFlags);
    if (!window) {
        health.fallback_reason = std::string("SDL_CreateWindow(VULKAN): ") + SDL_GetError();
        pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
        SDL_Vulkan_UnloadLibrary();
        if (sdlVideoInitedHere) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        goto emit_health;
    }
    pp_log("swapchain-present: created %s 800x600 Vulkan window (separate from GL window).",
           hidden ? "HIDDEN" : "SHOWN");

    // ---- Surface instance extensions --------------------------------------
    unsigned int extCount = 0;
    if (SDL_Vulkan_GetInstanceExtensions(window, &extCount, nullptr) != SDL_TRUE) {
        health.fallback_reason = std::string("GetInstanceExtensions(count): ") + SDL_GetError();
        pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
        SDL_DestroyWindow(window); SDL_Vulkan_UnloadLibrary();
        if (sdlVideoInitedHere) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        goto emit_health;
    }
    std::vector<const char*> instExts(extCount);
    if (extCount && SDL_Vulkan_GetInstanceExtensions(window, &extCount, instExts.data()) != SDL_TRUE) {
        health.fallback_reason = std::string("GetInstanceExtensions(names): ") + SDL_GetError();
        pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
        SDL_DestroyWindow(window); SDL_Vulkan_UnloadLibrary();
        if (sdlVideoInitedHere) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        goto emit_health;
    }

    // ---- Validation preset (optional) -------------------------------------
    std::vector<VkValidationFeatureEnableEXT> valFeatures;
    const char* valPreset = "off";
    bool wantValidation = pp_resolve_validation_preset(valFeatures, valPreset);

    // ---- VkInstance via vk-bootstrap (shares volk's loader) ---------------
    vkb::InstanceBuilder ib(vkGetInstanceProcAddr);
    ib.set_app_name("MC2 (Vulkan swapchain present)")
      .set_engine_name("MC2-GameOS")
      .require_api_version(1, 1, 0);
    for (unsigned i = 0; i < extCount; ++i) ib.enable_extension(instExts[i]);
    if (wantValidation) {
        ib.request_validation_layers(true);
        ib.enable_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        // Chain the extra validation features (sync/gpu-assisted/...) if any.
        for (auto feat : valFeatures) ib.add_validation_feature_enable(feat);
        pp_log("swapchain-present: validation ENABLED (preset=%s, +%zu feature(s))",
               valPreset, valFeatures.size());
    }

    auto instRet = ib.build();
    if (!instRet) {
        health.fallback_reason = "InstanceBuilder.build failed: " + instRet.error().message();
        pp_log("swapchain-present: %s (VkResult=%d). fail-soft.",
               health.fallback_reason.c_str(), (int)instRet.vk_result());
        SDL_DestroyWindow(window); SDL_Vulkan_UnloadLibrary();
        if (sdlVideoInitedHere) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        goto emit_health;
    }
    vkb::Instance vkbInstance = instRet.value();
    VkInstance instance = vkbInstance.instance;
    volkLoadInstance(instance);

    // Bespoke debug messenger (vkb built its own only if validation requested;
    // create ours so g_pp_validation_errors counts hard errors here too).
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (wantValidation) {
        auto pfnCreate = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (pfnCreate) {
            VkDebugUtilsMessengerCreateInfoEXT mci{};
            mci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mci.pfnUserCallback = pp_debug_cb;
            pfnCreate(instance, &mci, nullptr, &messenger);
        } else {
            pp_log("swapchain-present: vkCreateDebugUtilsMessengerEXT not found; no error capture.");
        }
    }

    // From here a single epilogue tears everything down in reverse.
    bool ok = false;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass rpass = VK_NULL_HANDLE;
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkCommandBuffer cbuf = VK_NULL_HANDLE;
    VkSemaphore acquireSem = VK_NULL_HANDLE; // render sems are per-image (in scr)
    VkFence inFlight = VK_NULL_HANDLE;
    SwapchainResources scr;
    VkQueue gfxQueue = VK_NULL_HANDLE, presentQueue = VK_NULL_HANDLE;
    uint32_t gfxFamily = UINT32_MAX, presentFamily = UINT32_MAX;
    VkPhysicalDevice phys = VK_NULL_HANDLE;

    // ---- Surface (SDL) ----------------------------------------------------
    if (SDL_Vulkan_CreateSurface(window, instance, &surface) != SDL_TRUE) {
        health.fallback_reason = std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError();
        pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
        goto cleanup;
    }
    health.surface_created = true;

    // ---- Bespoke physical-device pick: devs[0] with present+graphics ------
    {
        uint32_t devCount = 0;
        VkResult r = vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
        if (r != VK_SUCCESS || devCount == 0) {
            health.fallback_reason = "no physical devices";
            pp_log("swapchain-present: %s (VkResult=%d count=%u). fail-soft.",
                   health.fallback_reason.c_str(), (int)r, devCount);
            goto cleanup;
        }
        std::vector<VkPhysicalDevice> devs(devCount);
        vkEnumeratePhysicalDevices(instance, &devCount, devs.data());
        phys = devs[0];
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys, &props);
        pp_log("swapchain-present: using physical device [0] %s", props.deviceName);

        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs.data());
        for (uint32_t q = 0; q < qfCount; ++q) {
            if ((qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && gfxFamily == UINT32_MAX)
                gfxFamily = q;
            VkBool32 present = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(phys, q, surface, &present) == VK_SUCCESS &&
                present == VK_TRUE && presentFamily == UINT32_MAX)
                presentFamily = q;
        }
        if (gfxFamily == UINT32_MAX || presentFamily == UINT32_MAX) {
            health.fallback_reason = "no graphics/present queue family";
            pp_log("swapchain-present: %s (gfx=%u present=%u). fail-soft.",
                   health.fallback_reason.c_str(), gfxFamily, presentFamily);
            goto cleanup;
        }
        pp_log("swapchain-present: queue families gfx=%u present=%u", gfxFamily, presentFamily);

        // ---- Bespoke logical device (VK_KHR_swapchain) --------------------
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qcis[2]{};
        uint32_t qciCount = 0;
        qcis[qciCount].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qcis[qciCount].queueFamilyIndex = gfxFamily;
        qcis[qciCount].queueCount       = 1;
        qcis[qciCount].pQueuePriorities = &prio;
        ++qciCount;
        if (presentFamily != gfxFamily) {
            qcis[qciCount].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            qcis[qciCount].queueFamilyIndex = presentFamily;
            qcis[qciCount].queueCount       = 1;
            qcis[qciCount].pQueuePriorities = &prio;
            ++qciCount;
        }
        const char* devExts[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo dci{};
        dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount    = qciCount;
        dci.pQueueCreateInfos       = qcis;
        dci.enabledExtensionCount   = 1;
        dci.ppEnabledExtensionNames = devExts;
        r = vkCreateDevice(phys, &dci, nullptr, &device);
        if (r != VK_SUCCESS) {
            health.fallback_reason = "vkCreateDevice failed (" + std::to_string((int)r) + ")";
            pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
            goto cleanup;
        }
        volkLoadDevice(device);
        vkGetDeviceQueue(device, gfxFamily, 0, &gfxQueue);
        vkGetDeviceQueue(device, presentFamily, 0, &presentQueue);
    }

    // ---- Shader modules (reuse fullscreen triangle) -----------------------
    {
        std::string dir = spvDir ? spvDir : "shaders/vulkan";
        if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += '/';
        vert = mc2_vulkan_load_spv(device, (dir + "fullscreen.vert.spv").c_str());
        frag = mc2_vulkan_load_spv(device, (dir + "fullscreen.frag.spv").c_str());
        if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE) {
            health.fallback_reason = "fullscreen shader module load failed";
            pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
            goto cleanup;
        }
    }

    // ---- Render pass (1 color, CLEAR->STORE, -> PRESENT_SRC) --------------
    {
        VkAttachmentDescription att{};
        att.format         = VK_FORMAT_B8G8R8A8_SRGB;
        att.samples        = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        att.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference ref{};
        ref.attachment = 0;
        ref.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription sub{};
        sub.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments    = &ref;

        // Subpass dependency: make the color-attachment write wait for the
        // acquire (external -> subpass 0). Together with the acquire-semaphore
        // wait at COLOR_ATTACHMENT_OUTPUT this closes the WSI hazard that sync
        // validation checks.
        VkSubpassDependency dep{};
        dep.srcSubpass      = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass      = 0;
        dep.srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.srcAccessMask   = 0;
        dep.dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpci{};
        rpci.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1;
        rpci.pAttachments    = &att;
        rpci.subpassCount    = 1;
        rpci.pSubpasses      = &sub;
        rpci.dependencyCount = 1;
        rpci.pDependencies   = &dep;
        VkResult r = vkCreateRenderPass(device, &rpci, nullptr, &rpass);
        if (r != VK_SUCCESS) {
            health.fallback_reason = "vkCreateRenderPass failed (" + std::to_string((int)r) + ")";
            pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
            goto cleanup;
        }
    }

    // ---- Graphics pipeline (fullscreen.vert/frag; dynamic viewport/scissor
    //      so it survives resize without a rebuild) --------------------------
    {
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName  = "main";

        VkPipelineVertexInputStateCreateInfo vin{};
        vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        // Dynamic viewport + scissor: 1 of each, set at record time.
        VkPipelineViewportStateCreateInfo vps{};
        vps.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.scissorCount  = 1;

        VkDynamicState dynStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dyn{};
        dyn.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates    = dynStates;

        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms{};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        cba.blendEnable    = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb{};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments    = &cba;

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        VkResult r = vkCreatePipelineLayout(device, &plci, nullptr, &playout);
        if (r != VK_SUCCESS) {
            health.fallback_reason = "vkCreatePipelineLayout failed (" + std::to_string((int)r) + ")";
            pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
            goto cleanup;
        }

        VkGraphicsPipelineCreateInfo gp{};
        gp.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gp.stageCount          = 2;
        gp.pStages             = stages;
        gp.pVertexInputState   = &vin;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState      = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState   = &ms;
        gp.pColorBlendState    = &cb;
        gp.pDynamicState       = &dyn;
        gp.layout              = playout;
        gp.renderPass          = rpass;
        gp.subpass             = 0;
        r = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe);
        if (r != VK_SUCCESS) {
            health.fallback_reason = "vkCreateGraphicsPipelines failed (" + std::to_string((int)r) + ")";
            pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
            goto cleanup;
        }
    }

    // ---- Initial swapchain + framebuffers ---------------------------------
    {
        std::string reason;
        if (!build_swapchain_resources(phys, device, surface, gfxFamily, presentFamily,
                                       rpass, 800, 600, VK_NULL_HANDLE, scr, reason)) {
            health.fallback_reason = reason;
            goto cleanup;
        }
        health.swapchain_available = true;
        health.image_count = scr.swapchain.image_count;
        health.format      = scr.swapchain.image_format;
        health.present_mode = scr.swapchain.present_mode;
        health.extent_w    = scr.swapchain.extent.width;
        health.extent_h    = scr.swapchain.extent.height;
        pp_log("swapchain-present: swapchain OK image_count=%u format=%s extent=%ux%u present_mode=%s",
               scr.swapchain.image_count, pp_format_str(scr.swapchain.image_format),
               scr.swapchain.extent.width, scr.swapchain.extent.height,
               pp_present_mode_str(scr.swapchain.present_mode));
    }

    // ---- Command pool/buffer + sync primitives (1 frame in flight) --------
    {
        VkCommandPoolCreateInfo pci{};
        pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = gfxFamily;
        VkResult r = vkCreateCommandPool(device, &pci, nullptr, &cpool);
        if (r != VK_SUCCESS) {
            health.fallback_reason = "vkCreateCommandPool failed (" + std::to_string((int)r) + ")";
            pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
            goto cleanup;
        }
        VkCommandBufferAllocateInfo cai{};
        cai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool        = cpool;
        cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        r = vkAllocateCommandBuffers(device, &cai, &cbuf);
        if (r != VK_SUCCESS) {
            health.fallback_reason = "vkAllocateCommandBuffers failed (" + std::to_string((int)r) + ")";
            pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
            goto cleanup;
        }
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(device, &sci, nullptr, &acquireSem) != VK_SUCCESS) {
            health.fallback_reason = "vkCreateSemaphore(acquire) failed";
            pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
            goto cleanup;
        }
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT; // so frame 0 doesn't block forever
        if (vkCreateFence(device, &fci, nullptr, &inFlight) != VK_SUCCESS) {
            health.fallback_reason = "vkCreateFence failed";
            pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
            goto cleanup;
        }
    }

    // ---- PRESENT LOOP (16 frames) -----------------------------------------
    {
        const uint32_t kFrames = 16;
        const uint32_t kResizeAtFrame = 8;
        bool deviceLost = false;

        for (uint32_t frame = 0; frame < kFrames && !deviceLost; ++frame) {
            // Pump SDL events so the window stays responsive.
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) { /* drain; do not honor quit -- fixed count */ }

            // MID-LOOP RESIZE: shrink the window; the next acquire/present will
            // return OUT_OF_DATE and drive the recreate path below.
            if (frame == kResizeAtFrame) {
                pp_log("swapchain-present: frame %u -- SDL_SetWindowSize(640,480) to force resize.", frame);
                SDL_SetWindowSize(window, 640, 480);
            }

            // Wait for the previous frame's work, then reset the fence.
            VkResult r = vkWaitForFences(device, 1, &inFlight, VK_TRUE, UINT64_MAX);
            if (r == VK_ERROR_DEVICE_LOST) { deviceLost = true; break; }
            vkResetFences(device, 1, &inFlight);

            // Acquire.
            uint32_t imageIndex = 0;
            r = vkAcquireNextImageKHR(device, scr.swapchain.swapchain, UINT64_MAX,
                                      acquireSem, VK_NULL_HANDLE, &imageIndex);
            if (r == VK_ERROR_OUT_OF_DATE_KHR) {
                pp_log("swapchain-present: acquire OUT_OF_DATE at frame %u -- recreating swapchain.", frame);
                vkDeviceWaitIdle(device);
                VkSwapchainKHR old = scr.swapchain.swapchain;
                SwapchainResources fresh;
                int w = 0, h = 0;
                SDL_Vulkan_GetDrawableSize(window, &w, &h);
                std::string reason;
                bool built = build_swapchain_resources(phys, device, surface, gfxFamily,
                                                       presentFamily, rpass,
                                                       (uint32_t)(w > 0 ? w : 1),
                                                       (uint32_t)(h > 0 ? h : 1),
                                                       old, fresh, reason);
                destroy_swapchain_resources(device, scr); // frees old (incl. old handle)
                scr = std::move(fresh);
                if (!built) { health.fallback_reason = reason; deviceLost = true; break; }
                ++health.resize_count;
                health.image_count = scr.swapchain.image_count;
                health.extent_w = scr.swapchain.extent.width;
                health.extent_h = scr.swapchain.extent.height;
                // The fence is currently unsignaled (we reset it). Re-signal-equivalent:
                // since we did not submit, re-signal by skipping this frame cleanly.
                VkFenceCreateInfo dummy{}; (void)dummy;
                // Re-signal the fence so next iteration's wait does not deadlock.
                {
                    VkSubmitInfo empty{};
                    empty.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                    vkQueueSubmit(gfxQueue, 1, &empty, inFlight);
                }
                continue; // retry acquire next iteration
            } else if (r == VK_ERROR_DEVICE_LOST) {
                deviceLost = true; break;
            } else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
                health.fallback_reason = "vkAcquireNextImageKHR failed (" + std::to_string((int)r) + ")";
                pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
                deviceLost = true; break;
            }

            // Record: begin render pass (clear to teal), draw fullscreen tri.
            vkResetCommandBuffer(cbuf, 0);
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cbuf, &bi);

            VkClearValue clear{};
            clear.color = {{0.0f, 0.5f, 0.5f, 1.0f}}; // distinct teal
            VkRenderPassBeginInfo rbi{};
            rbi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rbi.renderPass      = rpass;
            rbi.framebuffer     = scr.framebuffers[imageIndex];
            rbi.renderArea      = {{0, 0}, scr.swapchain.extent};
            rbi.clearValueCount = 1;
            rbi.pClearValues    = &clear;
            vkCmdBeginRenderPass(cbuf, &rbi, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport vp{0.0f, 0.0f, (float)scr.swapchain.extent.width,
                          (float)scr.swapchain.extent.height, 0.0f, 1.0f};
            VkRect2D   sc{{0, 0}, scr.swapchain.extent};
            vkCmdSetViewport(cbuf, 0, 1, &vp);
            vkCmdSetScissor(cbuf, 0, 1, &sc);
            vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdDraw(cbuf, 3, 1, 0, 0);
            vkCmdEndRenderPass(cbuf);
            vkEndCommandBuffer(cbuf);

            // Submit: wait acquire sem at COLOR_ATTACHMENT_OUTPUT, signal the
            // PER-IMAGE render sem (indexed by the acquired image -- see SYNC NOTE).
            VkSemaphore imgRenderSem = scr.renderSems[imageIndex];
            VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            VkSubmitInfo si{};
            si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.waitSemaphoreCount   = 1;
            si.pWaitSemaphores      = &acquireSem;
            si.pWaitDstStageMask    = &waitStage;
            si.commandBufferCount   = 1;
            si.pCommandBuffers      = &cbuf;
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores    = &imgRenderSem;
            r = vkQueueSubmit(gfxQueue, 1, &si, inFlight);
            if (r == VK_ERROR_DEVICE_LOST) { deviceLost = true; break; }
            if (r != VK_SUCCESS) {
                health.fallback_reason = "vkQueueSubmit failed (" + std::to_string((int)r) + ")";
                pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
                deviceLost = true; break;
            }

            // Present: wait render sem.
            VkPresentInfoKHR pi{};
            pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores    = &imgRenderSem;
            pi.swapchainCount     = 1;
            pi.pSwapchains        = &scr.swapchain.swapchain;
            pi.pImageIndices      = &imageIndex;
            r = vkQueuePresentKHR(presentQueue, &pi);
            if (r == VK_SUCCESS) {
                ++health.present_frames;
            } else if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
                // Present still handed the image over; count it, then recreate.
                ++health.present_frames;
                pp_log("swapchain-present: present %s at frame %u -- recreating swapchain.",
                       r == VK_SUBOPTIMAL_KHR ? "SUBOPTIMAL" : "OUT_OF_DATE", frame);
                vkDeviceWaitIdle(device);
                VkSwapchainKHR old = scr.swapchain.swapchain;
                SwapchainResources fresh;
                int w = 0, h = 0;
                SDL_Vulkan_GetDrawableSize(window, &w, &h);
                std::string reason;
                bool built = build_swapchain_resources(phys, device, surface, gfxFamily,
                                                       presentFamily, rpass,
                                                       (uint32_t)(w > 0 ? w : 1),
                                                       (uint32_t)(h > 0 ? h : 1),
                                                       old, fresh, reason);
                destroy_swapchain_resources(device, scr);
                scr = std::move(fresh);
                if (!built) { health.fallback_reason = reason; deviceLost = true; break; }
                ++health.resize_count;
                health.image_count = scr.swapchain.image_count;
                health.extent_w = scr.swapchain.extent.width;
                health.extent_h = scr.swapchain.extent.height;
            } else if (r == VK_ERROR_DEVICE_LOST) {
                deviceLost = true; break;
            } else {
                health.fallback_reason = "vkQueuePresentKHR failed (" + std::to_string((int)r) + ")";
                pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
                deviceLost = true; break;
            }
        }

        if (deviceLost && health.fallback_reason.empty()) {
            health.fallback_reason = "VK_ERROR_DEVICE_LOST during present loop";
            pp_log("swapchain-present: %s. fail-soft.", health.fallback_reason.c_str());
        }
        // Success == presented all 16 frames with no fallback reason recorded.
        ok = (health.present_frames == kFrames) && health.fallback_reason.empty();
    }

    if (ok) pp_log("swapchain-present: PASS -- presented %u frames, %u resize(s), %u validation error(s).",
                   health.present_frames, health.resize_count, g_pp_validation_errors);
    else    pp_log("swapchain-present: FAIL/partial -- present_frames=%u fallback='%s'.",
                   health.present_frames, health.fallback_reason.c_str());

cleanup:
    // vkDeviceWaitIdle before destroying anything the GPU may still touch.
    if (device) vkDeviceWaitIdle(device);
    if (inFlight)   vkDestroyFence(device, inFlight, nullptr);
    // Per-image render semaphores are freed inside destroy_swapchain_resources.
    if (acquireSem) vkDestroySemaphore(device, acquireSem, nullptr);
    if (cpool)      vkDestroyCommandPool(device, cpool, nullptr); // frees cbuf
    destroy_swapchain_resources(device, scr);
    if (pipe)    vkDestroyPipeline(device, pipe, nullptr);
    if (playout) vkDestroyPipelineLayout(device, playout, nullptr);
    if (rpass)   vkDestroyRenderPass(device, rpass, nullptr);
    if (vert)    mc2_vulkan_free_shader(device, vert);
    if (frag)    mc2_vulkan_free_shader(device, frag);
    if (device)  vkDestroyDevice(device, nullptr);
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    if (messenger) {
        auto d = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (d) d(instance, messenger, nullptr);
    }
    vkb::destroy_instance(vkbInstance);
    SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    if (sdlVideoInitedHere) SDL_QuitSubSystem(SDL_INIT_VIDEO);

    health.validation_errors = g_pp_validation_errors;
    } // end SDL/instance scope

emit_health:
    health.validation_errors = g_pp_validation_errors;
    pp_log("[VK_SWAPCHAIN_PRESENT_HEALTH] swapchain_available=%d surface_created=%d "
           "format=%s present_mode=%s image_count=%u extent=%ux%u resize_count=%u "
           "present_frames=%u fallback_reason=%s validation_errors=%u",
           health.swapchain_available ? 1 : 0, health.surface_created ? 1 : 0,
           pp_format_str(health.format), pp_present_mode_str(health.present_mode),
           health.image_count, health.extent_w, health.extent_h, health.resize_count,
           health.present_frames,
           health.fallback_reason.empty() ? "(none)" : health.fallback_reason.c_str(),
           health.validation_errors);
    maybe_dump_debug_state(health);

    return (health.present_frames == 16) && health.fallback_reason.empty() &&
           (health.validation_errors == 0);
}

#endif // MC2_VULKAN
