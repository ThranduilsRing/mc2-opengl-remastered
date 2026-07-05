// VK-BOOTSTRAP-INTEGRATE-1 -- Vulkan swapchain PROBE (create + query + destroy).
// NO present, NO frame loop, NO game render path. Entire TU compiles to nothing
// unless MC2_VULKAN is defined (CMake option, default OFF) -> the GL build is
// byte-identical. Fail-soft at EVERY step: any error logs a reason + returns
// false, never crashes (this is also the OpenGL-user path).
//
// Pipeline:
//   volk init -> vkb::InstanceBuilder(volk's vkGetInstanceProcAddr) with the
//   SDL-queried surface extensions -> volkLoadInstance -> hidden SDL Vulkan
//   window -> SDL_Vulkan_CreateSurface -> bespoke devs[0] physical device ->
//   present-capable queue family -> bespoke VkDevice (VK_KHR_swapchain) ->
//   volkLoadDevice -> vkb::SwapchainBuilder(phys, device, surface, gfx, present)
//   .build() -> query image_count/format/extent -> destroy in reverse.
//
// volk <-> vk-bootstrap reconciliation: vk-bootstrap is fully self-dispatching
// (never calls a bare vkXxx; all calls go through its own internal dispatch
// table). We share ONE loader by handing volk's vkGetInstanceProcAddr to
// vkb::InstanceBuilder(fp); InstanceBuilder::build() runs init_instance_funcs(),
// which primes fp_vkGetDeviceProcAddr -- the one thing SwapchainBuilder::build()
// needs from the internal table. Device creation stays bespoke; vk-bootstrap is
// used ONLY for the SwapchainBuilder. See 3rdparty/vk_bootstrap/VK_BOOTSTRAP_VERSION.txt.

#include "vulkan_backend_skeleton.h"

#ifdef MC2_VULKAN

// VULKAN-VOLK-LOADER-1: volk owns Vulkan dispatch (dynamic load; no hard link).
// volk.h defines VK_NO_PROTOTYPES and includes the Vulkan headers.
#include <volk.h>

// vk-bootstrap public API. Self-dispatching (see reconciliation note above), so
// including it under VK_NO_PROTOTYPES is safe -- it references no bare prototype.
#include "VkBootstrap.h"

// SDL2 Vulkan windowing (hidden window + surface). Matches gos_render.cpp's SDL.
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

void sp_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[VULKAN_SKELETON] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

const char* format_str(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_SRGB:  return "B8G8R8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB:  return "R8G8B8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        default:                       return "other";
    }
}

} // namespace

bool mc2_vulkan_probe_swapchain() {
    // ---- volk (Vulkan runtime) --------------------------------------------
    // Fail-soft primary gate: on an OpenGL-only machine with no Vulkan runtime,
    // volkInitialize() fails and the probe returns false cleanly.
    if (volkInitialize() != VK_SUCCESS) {
        sp_log("swapchain-probe: volkInitialize() failed -- no Vulkan runtime. fail-soft.");
        return false;
    }

    // ---- SDL video (needed for the hidden Vulkan window + surface) ---------
    // Init only the VIDEO subsystem; if SDL was already initialised elsewhere
    // this is refcounted and harmless. Quit only what we init (below).
    bool sdlVideoInitedHere = false;
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            sp_log("swapchain-probe: SDL_InitSubSystem(VIDEO) failed: %s. fail-soft.", SDL_GetError());
            return false;
        }
        sdlVideoInitedHere = true;
    }
    // Ensure SDL's Vulkan loader is up so SDL_Vulkan_* resolve.
    if (SDL_Vulkan_LoadLibrary(nullptr) != 0) {
        sp_log("swapchain-probe: SDL_Vulkan_LoadLibrary failed: %s. fail-soft.", SDL_GetError());
        if (sdlVideoInitedHere) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

    // ---- Hidden Vulkan SDL window (256x256) --------------------------------
    SDL_Window* window = SDL_CreateWindow(
        "MC2 Vulkan swapchain probe", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256, 256, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) {
        sp_log("swapchain-probe: SDL_CreateWindow(VULKAN|HIDDEN) failed: %s. fail-soft.", SDL_GetError());
        SDL_Vulkan_UnloadLibrary();
        if (sdlVideoInitedHere) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

    // ---- Surface instance extensions (SDL query -- correct per platform) ---
    unsigned int extCount = 0;
    if (SDL_Vulkan_GetInstanceExtensions(window, &extCount, nullptr) != SDL_TRUE) {
        sp_log("swapchain-probe: SDL_Vulkan_GetInstanceExtensions(count) failed: %s. fail-soft.", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Vulkan_UnloadLibrary();
        if (sdlVideoInitedHere) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    std::vector<const char*> instExts(extCount);
    if (extCount &&
        SDL_Vulkan_GetInstanceExtensions(window, &extCount, instExts.data()) != SDL_TRUE) {
        sp_log("swapchain-probe: SDL_Vulkan_GetInstanceExtensions(names) failed: %s. fail-soft.", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Vulkan_UnloadLibrary();
        if (sdlVideoInitedHere) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    {
        std::string joined;
        for (unsigned i = 0; i < extCount; ++i) { joined += instExts[i]; joined += ' '; }
        sp_log("swapchain-probe: SDL surface extensions (%u): %s", extCount, joined.c_str());
    }

    // ---- VkInstance via vk-bootstrap (shares volk's loader) ----------------
    // Pass volk's vkGetInstanceProcAddr so vk-bootstrap and volk use ONE loader.
    // request_validation_layers(false): this probe's proof is create+query, not
    // validation (the other probes cover validation). Enable the SDL surface
    // extensions so the surface + swapchain are usable.
    vkb::InstanceBuilder ib(vkGetInstanceProcAddr);
    ib.set_app_name("MC2 (Vulkan swapchain probe)")
      .set_engine_name("MC2-GameOS")
      .require_api_version(1, 1, 0)
      .request_validation_layers(false);
    for (unsigned i = 0; i < extCount; ++i) ib.enable_extension(instExts[i]);

    auto instRet = ib.build();
    if (!instRet) {
        sp_log("swapchain-probe: vkb::InstanceBuilder.build() failed: %s (VkResult=%d). fail-soft.",
               instRet.error().message().c_str(), (int)instRet.vk_result());
        SDL_DestroyWindow(window);
        SDL_Vulkan_UnloadLibrary();
        if (sdlVideoInitedHere) SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    vkb::Instance vkbInstance = instRet.value();
    VkInstance instance = vkbInstance.instance;
    // Also resolve volk's instance-level dispatch on this instance (so any volk
    // vkXxx we call directly -- e.g. vkGetPhysicalDeviceSurfaceSupportKHR below
    // -- works). vk-bootstrap primed its OWN table during build(); this primes
    // volk's, keeping the two loaders coherent on the same instance.
    volkLoadInstance(instance);

    // From here, a single epilogue tears everything down in reverse.
    bool ok = false;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    bool haveSwapchain = false;
    vkb::Swapchain swapchain{};

    // ---- Surface (SDL) ------------------------------------------------------
    if (SDL_Vulkan_CreateSurface(window, instance, &surface) != SDL_TRUE) {
        sp_log("swapchain-probe: SDL_Vulkan_CreateSurface failed: %s. fail-soft.", SDL_GetError());
        goto cleanup;
    }

    // ---- Bespoke physical-device pick: devs[0] -----------------------------
    {
        uint32_t devCount = 0;
        VkResult r = vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
        if (r != VK_SUCCESS || devCount == 0) {
            sp_log("swapchain-probe: no physical devices (VkResult=%d, count=%u). fail-soft.", (int)r, devCount);
            goto cleanup;
        }
        std::vector<VkPhysicalDevice> devs(devCount);
        vkEnumeratePhysicalDevices(instance, &devCount, devs.data());
        VkPhysicalDevice phys = devs[0];

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(phys, &props);
        sp_log("swapchain-probe: using physical device [0] %s", props.deviceName);

        // ---- Present-capable queue family ----------------------------------
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        if (qfCount) vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs.data());

        uint32_t gfxFamily = UINT32_MAX;
        uint32_t presentFamily = UINT32_MAX;
        for (uint32_t q = 0; q < qfCount; ++q) {
            if ((qfs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && gfxFamily == UINT32_MAX)
                gfxFamily = q;
            VkBool32 present = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(phys, q, surface, &present) == VK_SUCCESS &&
                present == VK_TRUE && presentFamily == UINT32_MAX)
                presentFamily = q;
        }
        if (gfxFamily == UINT32_MAX || presentFamily == UINT32_MAX) {
            sp_log("swapchain-probe: no graphics/present queue family (gfx=%u present=%u). fail-soft.",
                   gfxFamily, presentFamily);
            goto cleanup;
        }
        sp_log("swapchain-probe: queue families gfx=%u present=%u", gfxFamily, presentFamily);

        // ---- Bespoke logical device (VK_KHR_swapchain) ---------------------
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
            sp_log("swapchain-probe: vkCreateDevice failed (VkResult=%d). fail-soft.", (int)r);
            goto cleanup;
        }
        // volk device-level dispatch (for the bespoke destroy path below).
        volkLoadDevice(device);

        // ---- vk-bootstrap SwapchainBuilder (raw handles) -------------------
        // SwapchainBuilder resolves vkCreateSwapchainKHR etc. through
        // vk-bootstrap's own fp_vkGetDeviceProcAddr, primed by InstanceBuilder.
        vkb::SwapchainBuilder scb(phys, device, surface, gfxFamily, presentFamily);
        scb.set_desired_format(VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_SRGB,
                                                   VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
           .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
           .set_desired_extent(256, 256);
        auto scRet = scb.build();
        if (!scRet) {
            sp_log("swapchain-probe: vkb::SwapchainBuilder.build() failed: %s (VkResult=%d). fail-soft.",
                   scRet.error().message().c_str(), (int)scRet.vk_result());
            goto cleanup;
        }
        swapchain = scRet.value();
        haveSwapchain = true;

        sp_log("swapchain-probe: OK image_count=%u format=%s(%d) extent=%ux%u",
               swapchain.image_count, format_str(swapchain.image_format),
               (int)swapchain.image_format, swapchain.extent.width, swapchain.extent.height);
        ok = (swapchain.image_count > 0) &&
             (swapchain.extent.width > 0) && (swapchain.extent.height > 0);
    }

cleanup:
    // Reverse-order teardown, each guarded (fail-soft partial paths land here).
    if (haveSwapchain) vkb::destroy_swapchain(swapchain);
    if (device)        vkDestroyDevice(device, nullptr);
    if (surface)       vkDestroySurfaceKHR(instance, surface, nullptr);
    vkb::destroy_instance(vkbInstance);
    SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    if (sdlVideoInitedHere) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    sp_log("swapchain-probe: %s -- cleaned up (swapchain/device/surface/window/instance destroyed).",
           ok ? "PASS" : "FAIL");
    return ok;
}

#endif // MC2_VULKAN
