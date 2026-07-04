// engine_present_vk.cpp
//
// VK-P13-SELECTOR-PLAN / T3 -- whole-frame GL->VK present intercept implementation.
//
// Borrows the T2 shared VulkanContext device (NO second device / NO second
// volkLoadDevice). Owns only the hidden window + surface + swapchain + per-frame
// sync + the exportable interop image (GL writes it via copy-1, VK blits it into
// the swapchain via copy-2). Swapchain/present logic mirrors the T0 harness
// (tools/vk_present_smoke/main.cpp) and vulkan_present_object.cpp's per-image
// render-semaphore discipline. Fail-soft at every step.
//
// Requires BOTH MC2_VULKAN (context/present) and MC2_VULKAN_ISLAND (interop
// primitives). build64_island defines both; the guards below make the TU inert
// otherwise so the GL shipping build never sees a Vulkan symbol.

#include "engine_present_vk.h"

#if defined(MC2_VULKAN) && defined(MC2_VULKAN_ISLAND)

// Win32 surface entrypoints + names are gated behind this macro (matches
// vulkan_context.cpp / vk_image_resource.h). Define BEFORE volk.h.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "vulkan_context.h"
#include "vk_image_resource.h"
#include "gl_interop_view.h"

#include <GL/glew.h>   // copy-1 GL blit (FBO-0 -> shared GL texture)
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <SDL2/SDL_syswm.h>   // P13b: SDL_GetWindowWMInfo -> HWND / HINSTANCE

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace mc2vk {

namespace {

void plog(const char* fmt, ...) {
    std::fprintf(stderr, "[VK_PRESENT] ");
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
}

const char* presentModeName(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX";
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE";
        default:                               return "UNKNOWN";
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Impl: all VK/GL state (kept out of the header so it stays VK-header-free).
// ---------------------------------------------------------------------------
struct EnginePresentVk::Impl {
    VulkanContext*   ctx   = nullptr;
    VkInstance       inst  = VK_NULL_HANDLE;
    VkPhysicalDevice phys  = VK_NULL_HANDLE;
    VkDevice         dev   = VK_NULL_HANDLE;
    VkQueue          gfxQ  = VK_NULL_HANDLE;
    uint32_t         gfxFamily = UINT32_MAX;

    VkSurfaceKHR     surface   = VK_NULL_HANDLE;
    uint32_t         presentFamily = UINT32_MAX;
    VkQueue          presentQ  = VK_NULL_HANDLE;

    // swapchain
    VkSwapchainKHR           swapchain = VK_NULL_HANDLE;
    VkFormat                 scFormat  = VK_FORMAT_UNDEFINED;
    VkExtent2D               scExtent  = {0, 0};
    VkPresentModeKHR         presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage>     scImages;
    std::vector<VkSemaphore> renderSems;   // one per image (present-wait target)

    // per-frame sync (1 frame in flight for the acquire/submit)
    VkCommandPool   cpool     = VK_NULL_HANDLE;
    VkCommandBuffer cbuf      = VK_NULL_HANDLE;
    VkSemaphore     acquireSem = VK_NULL_HANDLE;
    VkFence         inFlight   = VK_NULL_HANDLE;

    // the shared exportable frame image (GL writes it, VK blits it)
    VkImageResource sharedImg{};
    GlInteropView   glView{};
    uint32_t        imgW = 0, imgH = 0;
    GLuint          copyFbo = 0;   // GL draw FBO wrapping glView.glTex (copy-1 dst)

    // readback (T5 content verification)
    VkBuffer        readbackBuf = VK_NULL_HANDLE;
    VkDeviceMemory  readbackMem = VK_NULL_HANDLE;
    VkDeviceSize    readbackSize = 0;
    VkCommandBuffer readbackCbuf = VK_NULL_HANDLE;
    VkFence         readbackFence = VK_NULL_HANDLE;
};

namespace {

// VK-P14b (MC2_VK_PRESENT_DIRECT_FBO, copy-1 elimination): memoized gate. When ON
// AND MC2_RENDER_BACKEND=vulkan AND the present path is valid, endScene binds the
// shared present FBO instead of FBO-0, so copy-1 is skipped and the Y-flip moves
// into copy-2. Default OFF -> gate-OFF is byte-identical to the copy-1 path.
bool directFboGateEnabled() {
    static const bool on = []() {
        const char* v = getenv("MC2_VK_PRESENT_DIRECT_FBO");
        return v && v[0] == '1';
    }();
    return on;
}

// -------- swapchain (re)build against the SHARED device -------------------
bool chooseQueuesAndPresentMode(EnginePresentVk::Impl& s, std::string& reason) {
    // The shared context picked gfxFamily; confirm it can present to our surface.
    VkBool32 canPresent = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(s.phys, s.gfxFamily, s.surface, &canPresent)
            == VK_SUCCESS && canPresent == VK_TRUE) {
        s.presentFamily = s.gfxFamily;
    } else {
        // Find any present-capable family (single-GPU AMD: gfx presents; this is
        // a robustness fallback).
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &qfCount, nullptr);
        for (uint32_t q = 0; q < qfCount; ++q) {
            VkBool32 p = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(s.phys, q, s.surface, &p) == VK_SUCCESS
                    && p == VK_TRUE) { s.presentFamily = q; break; }
        }
        if (s.presentFamily == UINT32_MAX) {
            reason = "no present-capable queue family for the hidden surface";
            return false;
        }
    }
    // The shared device created ONLY the gfx queue family (one queue). If present
    // needs a DIFFERENT family, that queue was never created on the shared device
    // -> we cannot present from it. On the target hardware gfx==present, so this
    // is a hard fail-soft (revert to GL) rather than silently wrong.
    if (s.presentFamily != s.gfxFamily) {
        reason = "present family != gfx family; shared device has no present queue "
                 "(fail-soft to GL; would need a present queue in T2 device create)";
        return false;
    }
    s.presentQ = s.gfxQ;

    // Present mode mapping (plan §6/R8): FIFO when MC2_VSYNC=1 (vsync-equivalent,
    // matches the GL swap-interval), else MAILBOX if available else IMMEDIATE
    // (preserve today's uncapped GL default). FIFO is always supported.
    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(s.phys, s.surface, &pmCount, nullptr);
    std::vector<VkPresentModeKHR> pms(pmCount);
    if (pmCount) vkGetPhysicalDeviceSurfacePresentModesKHR(s.phys, s.surface, &pmCount, pms.data());
    bool haveMailbox = false, haveImmediate = false;
    for (auto m : pms) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR)   haveMailbox = true;
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) haveImmediate = true;
    }
    const char* vs = std::getenv("MC2_VSYNC");
    const bool vsyncOn = (vs && vs[0] == '1');
    if (vsyncOn) {
        s.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    } else if (haveMailbox) {
        s.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (haveImmediate) {
        s.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else {
        s.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    }
    plog("present mode = %s (MC2_VSYNC=%s; mailbox_avail=%d immediate_avail=%d)",
         presentModeName(s.presentMode), vsyncOn ? "1" : "0/unset",
         haveMailbox ? 1 : 0, haveImmediate ? 1 : 0);
    return true;
}

void destroySwapchainOnly(EnginePresentVk::Impl& s) {
    for (VkSemaphore sem : s.renderSems) if (sem) vkDestroySemaphore(s.dev, sem, nullptr);
    s.renderSems.clear();
    s.scImages.clear();
    if (s.swapchain) { vkDestroySwapchainKHR(s.dev, s.swapchain, nullptr); s.swapchain = VK_NULL_HANDLE; }
}

bool buildSwapchain(EnginePresentVk::Impl& s, uint32_t w, uint32_t h,
                    VkSwapchainKHR oldSwapchain, std::string& reason) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.phys, s.surface, &caps) != VK_SUCCESS) {
        reason = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed"; return false;
    }
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    if (fmtCount) vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface, &fmtCount, fmts.data());
    VkSurfaceFormatKHR chosen = fmts.empty()
        ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
        : fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) { extent.width = w; extent.height = h; }
    if (extent.width  < caps.minImageExtent.width)  extent.width  = caps.minImageExtent.width;
    if (extent.width  > caps.maxImageExtent.width)  extent.width  = caps.maxImageExtent.width;
    if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
    if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    if (extent.width == 0 || extent.height == 0) {
        // Minimized / zero drawable: skip (fail-soft, retry next frame).
        reason = "zero surface extent (window minimized?)"; return false;
    }

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
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;   // gfx==present (asserted above)
    sci.preTransform     = caps.currentTransform;
    sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode      = s.presentMode;
    sci.clipped          = VK_TRUE;
    sci.oldSwapchain     = oldSwapchain;
    if (vkCreateSwapchainKHR(s.dev, &sci, nullptr, &s.swapchain) != VK_SUCCESS) {
        reason = "vkCreateSwapchainKHR failed"; return false;
    }
    s.scFormat = chosen.format;
    s.scExtent = extent;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(s.dev, s.swapchain, &n, nullptr);
    s.scImages.resize(n);
    vkGetSwapchainImagesKHR(s.dev, s.swapchain, &n, s.scImages.data());
    s.renderSems.resize(n, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < n; ++i) {
        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(s.dev, &semci, nullptr, &s.renderSems[i]) != VK_SUCCESS) {
            reason = "per-image render semaphore create failed"; return false;
        }
    }
    plog("swapchain OK images=%u extent=%ux%u format=%d mode=%s",
         n, extent.width, extent.height, (int)chosen.format, presentModeName(s.presentMode));
    return true;
}

bool rebuildSwapchain(EnginePresentVk::Impl& s, uint32_t w, uint32_t h, std::string& reason) {
    vkDeviceWaitIdle(s.dev);
    VkSwapchainKHR old = s.swapchain;
    std::vector<VkSemaphore> oldSems = s.renderSems;
    s.renderSems.clear();
    s.scImages.clear();
    s.swapchain = VK_NULL_HANDLE;
    bool ok = buildSwapchain(s, w, h, old, reason);
    for (VkSemaphore sem : oldSems) if (sem) vkDestroySemaphore(s.dev, sem, nullptr);
    if (old) vkDestroySwapchainKHR(s.dev, old, nullptr);
    return ok;
}

// image-layout transition helper (whole color image, single subresource)
void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout oldL, VkImageLayout newL,
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

// Build (or rebuild) the shared exportable interop image + its GlInteropView +
// the GL copy FBO, sized to (w,h). Destroys any prior instance first.
bool buildSharedImage(EnginePresentVk::Impl& s, uint32_t w, uint32_t h, std::string& reason) {
    // Tear down prior.
    if (s.copyFbo) { glDeleteFramebuffers(1, &s.copyFbo); s.copyFbo = 0; }
    if (s.glView.glTex) glInteropViewDestroy(&s.glView);
    if (s.sharedImg.image) vkImageResourceDestroy(&s.sharedImg, s.dev);

    // RGBA8, exportable. COLOR_ATTACHMENT so GL can render/blit into it via an FBO;
    // TRANSFER_SRC so VK can blit it into the swapchain; TRANSFER_DST for the initial
    // acquire barrier bookkeeping / robustness.
    if (!vkImageResourceCreate(&s.sharedImg, s.dev, s.phys, w, h,
                               VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               /*exportable*/ true)) {
        reason = "vkImageResourceCreate(exportable RGBA8) failed"; return false;
    }
    if (!glInteropViewCreate(&s.glView, &s.sharedImg, s.phys, GL_RGBA8)) {
        reason = "glInteropViewCreate failed (LUID / ext / GL error)"; return false;
    }
    // GL draw FBO wrapping the shared texture (copy-1 destination).
    glGenFramebuffers(1, &s.copyFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.copyFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, s.glView.glTex, 0);
    GLenum fbs = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (fbs != GL_FRAMEBUFFER_COMPLETE) {
        reason = "copy-1 GL FBO incomplete (status=" + std::to_string((int)fbs) + ")";
        return false;
    }
    s.imgW = w; s.imgH = h;
    return true;
}

}  // namespace

bool EnginePresentVk::init(VulkanContext& ctx, SDL_Window* glWindow, uint32_t w, uint32_t h) {
    health_ = EnginePresentHealth{};
    health_.attempted = true;
    if (!ctx.valid()) {
        health_.fallback_reason = "engine VulkanContext not valid (present init pre-req)";
        plog("%s. fail-soft.", health_.fallback_reason.c_str());
        return false;
    }
    if (!glWindow) {
        health_.fallback_reason = "P13b: NULL GL game window handed to present init";
        plog("%s. fail-soft.", health_.fallback_reason.c_str());
        return false;
    }
    glWindow_ = glWindow;   // BORROWED (g_sdl_window) -- never destroyed here.
    if (w == 0) w = 1;
    if (h == 0) h = 1;

    impl_ = new Impl();
    Impl& s = *impl_;
    s.ctx       = &ctx;
    s.inst      = ctx.instance();
    s.phys      = ctx.physicalDevice();
    s.dev       = ctx.device();
    s.gfxQ      = ctx.graphicsQueue();
    s.gfxFamily = ctx.graphicsFamily();

    auto fail = [&](const std::string& r) -> bool {
        health_.fallback_reason = r;
        plog("init: %s. fail-soft.", r.c_str());
        destroy();
        health_.fallback_reason = r;
        return false;
    };

    // ---- P13b: raw Win32 surface on the EXISTING visible GL game window -----
    // No hidden VK window. Pull the native HWND+HINSTANCE from the game's GL
    // window (SDL_WINDOW_OPENGL, g_sdl_window) via SDL_GetWindowWMInfo -- exactly
    // as the mouse grab does (gos_render.cpp:64-74) and as the T0 probe proved --
    // then vkCreateWin32SurfaceKHR straight onto that HWND. The GL context owns
    // the pixel format/HDC; VK presents via the WSI/compositor path on the same
    // HWND (no conflict, since GL swap is suppressed in =vulkan mode). One window,
    // already visible, already owning focus + all input.
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(glWindow_, &wm) != SDL_TRUE) {
        return fail(std::string("P13b SDL_GetWindowWMInfo: ") + SDL_GetError());
    }
    if (wm.subsystem != SDL_SYSWM_WINDOWS) {
        return fail("P13b: GL window subsystem is not SDL_SYSWM_WINDOWS");
    }
    HWND      hwnd  = wm.info.win.window;
    HINSTANCE hinst = wm.info.win.hinstance;
    if (!hwnd) return fail("P13b: SDL_GetWindowWMInfo gave a NULL HWND");
    if (!hinst) hinst = GetModuleHandle(nullptr);   // SDL may leave hinstance NULL

    // Resolve the Win32 surface entrypoint at runtime (volk's global symbol is
    // only defined when volk.c is built with VK_USE_PLATFORM_WIN32_KHR, which this
    // build's volk TU is not -- so link against vkGetInstanceProcAddr instead, as
    // the T0 probe does). The shared instance enabled VK_KHR_win32_surface.
    auto pfnCreateWin32Surface = (PFN_vkCreateWin32SurfaceKHR)
        vkGetInstanceProcAddr(s.inst, "vkCreateWin32SurfaceKHR");
    if (pfnCreateWin32Surface == nullptr) {
        return fail("P13b: vkGetInstanceProcAddr(vkCreateWin32SurfaceKHR) NULL "
                    "(VK_KHR_win32_surface not on the shared instance?)");
    }
    VkWin32SurfaceCreateInfoKHR wsci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    wsci.hinstance = hinst;
    wsci.hwnd      = hwnd;
    VkResult sr = pfnCreateWin32Surface(s.inst, &wsci, nullptr, &s.surface);
    if (sr != VK_SUCCESS) {
        return fail("P13b: vkCreateWin32SurfaceKHR on the GL window's HWND failed ("
                    + std::to_string((int)sr) + ")");
    }
    plog("P13b: VK surface created on the VISIBLE GL game window HWND=%p (no hidden "
         "window; GL keeps input+focus).", (void*)hwnd);

    // ---- queue confirm + present mode -------------------------------------
    {
        std::string r;
        if (!chooseQueuesAndPresentMode(s, r)) return fail(r);
        health_.present_mode = (int)s.presentMode;
        health_.present_mode_name = presentModeName(s.presentMode);
    }

    // ---- swapchain --------------------------------------------------------
    {
        std::string r;
        if (!buildSwapchain(s, w, h, VK_NULL_HANDLE, r)) return fail(r);
        health_.extent_w = s.scExtent.width;
        health_.extent_h = s.scExtent.height;
    }

    // ---- command pool/buffer + acquire sem + in-flight fence --------------
    {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = s.gfxFamily;
        if (vkCreateCommandPool(s.dev, &pci, nullptr, &s.cpool) != VK_SUCCESS)
            return fail("vkCreateCommandPool failed");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = s.cpool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(s.dev, &cai, &s.cbuf) != VK_SUCCESS)
            return fail("vkAllocateCommandBuffers failed");
        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(s.dev, &semci, nullptr, &s.acquireSem) != VK_SUCCESS)
            return fail("vkCreateSemaphore(acquire) failed");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (vkCreateFence(s.dev, &fci, nullptr, &s.inFlight) != VK_SUCCESS)
            return fail("vkCreateFence(inFlight) failed");
    }

    // ---- shared exportable interop image + GL view + copy-1 FBO -----------
    {
        std::string r;
        if (!buildSharedImage(s, w, h, r)) return fail(r);
    }

    created_ = true;
    health_.created = true;
    if (s.ctx) health_.validation_errors = s.ctx->health().validationErrors;
    plog("present path UP: extent=%ux%u images=%zu mode=%s.",
         s.scExtent.width, s.scExtent.height, s.scImages.size(),
         presentModeName(s.presentMode));
    return true;
}

// VK-P14b: expose the shared present FBO for endScene's direct-composite bind.
// Returns 0 (endScene falls back to FBO-0 + copy-1) UNLESS the gate is ON, the
// present path is up, and the copyFbo exists. Records the decision in directFbo_
// so the SAME frame's presentGLFrame skips copy-1 iff it handed one out. Called
// once per frame from gosPostProcess::endScene, before the composite tail.
unsigned int EnginePresentVk::presentDirectFbo() {
    if (!directFboGateEnabled() || !created_ || !impl_ || impl_->copyFbo == 0) {
        directFbo_ = 0;
        return 0;
    }
    directFbo_ = (unsigned int)impl_->copyFbo;
    return directFbo_;
}

// One frame: copy-1 (GL FBO-0 -> shared GL tex) + signal, then copy-2 (VK blit
// shared -> swapchain) + present. Rebuild swapchain + shared image on resize.
// VK-P14b: when endScene rendered straight into the shared image (directFbo_!=0),
// copy-1 is SKIPPED here and the Y-flip is done in copy-2 instead.
bool EnginePresentVk::presentGLFrame(uint32_t glDrawableW, uint32_t glDrawableH,
                                     bool verifyReadback) {
    if (!created_ || !impl_) {
        health_.fallback_reason = "present path not initialized";
        return false;
    }
    Impl& s = *impl_;

    // P13b: do NOT drain SDL events here. We present onto the SAME visible GL
    // window the game owns; its events (esp. mouse-motion) must flow to the
    // game's normal SDL event loop (gameosmain process_events). The old hidden-
    // window drain-and-discard would starve the game of input.

    // Minimized / zero-drawable: skip present this frame (fail-soft, no crash).
    // The game loop also throttles on MINIMIZED via g_sdl_window, but guard here
    // too so a zero extent never reaches the swapchain rebuild.
    if (glDrawableW == 0 || glDrawableH == 0) {
        return true;   // nothing to present; try again next frame
    }

    // Resize: if the GL drawable changed, rebuild the shared image + swapchain to
    // match the game window's new size (OS-driven resize / DPI change).
    if (glDrawableW != s.imgW || glDrawableH != s.imgH) {
        std::string r;
        if (!buildSharedImage(s, glDrawableW, glDrawableH, r)) {
            health_.fallback_reason = "resize: " + r; return false;
        }
        if (!rebuildSwapchain(s, glDrawableW, glDrawableH, r)) {
            health_.fallback_reason = "resize: " + r; return false;
        }
        ++health_.resize_count;
        health_.extent_w = s.scExtent.width;
        health_.extent_h = s.scExtent.height;
    }

    // ==== COPY-1 (GL): FBO-0 -> shared exportable GL texture ================
    // FBO-0 is the window default framebuffer holding the final composited HUD
    // frame at the swap point. Blit it into the shared texture, flip Y so the VK
    // blit + present orientation matches the GL backbuffer (GL origin is bottom-
    // left; we present top-left origin swapchain images).
    //
    // VK-P14b (MC2_VK_PRESENT_DIRECT_FBO): when endScene rendered the whole
    // composite straight INTO the shared image (directFbo_ != 0), the shared
    // image already IS the finished frame -> SKIP copy-1 entirely. The Y-flip
    // copy-1 used to do is moved into copy-2 below (dst Y offsets swapped) so the
    // presented orientation is unchanged. directFbo_ is set/cleared by
    // presentDirectFbo() earlier this same frame from endScene -> the two sites
    // always agree on which path ran.
    const bool copy1Skipped = (directFbo_ != 0);
    if (!copy1Skipped) {
        glGetError();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.copyFbo);
        // src (0,0)-(W,H) -> dst (0,H)-(W,0): vertical flip.
        glBlitFramebuffer(0, 0, (GLint)s.imgW, (GLint)s.imgH,
                          0, (GLint)s.imgH, (GLint)s.imgW, 0,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        if (glGetError() != GL_NO_ERROR) {
            health_.fallback_reason = "copy-1 glBlitFramebuffer error";
            plog("%s. fail-soft.", health_.fallback_reason.c_str());
            return false;
        }
    }

    // ==== GL -> VK hand-off ================================================
    // Begin the frame on the interop slot (waits the slot fence so its binary
    // semaphore is safe to re-signal), then GL signals glToVkSem, leaving the
    // image in COLOR_ATTACHMENT layout for VK to pick up.
    VkFrameSync fs = vkImageResourceBeginFrame(&s.sharedImg);
    glInteropSignalToVk(s.glView, GL_LAYOUT_COLOR_ATTACHMENT_EXT, fs.parity);

    // ==== COPY-2 (VK): blit shared image -> swapchain + present =============
    VkResult r = vkWaitForFences(s.dev, 1, &s.inFlight, VK_TRUE, UINT64_MAX);
    if (r == VK_ERROR_DEVICE_LOST) { health_.fallback_reason = "DEVICE_LOST at fence wait"; return false; }
    vkResetFences(s.dev, 1, &s.inFlight);

    uint32_t imageIndex = 0;
    r = vkAcquireNextImageKHR(s.dev, s.swapchain, UINT64_MAX, s.acquireSem, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        // OS-driven resize/alt-tab invalidated the swapchain. Rebuild to the GL
        // game window's current drawable. If it's zero (minimized), skip the
        // rebuild and just re-signal the fence -- retry next frame, no crash.
        int dw = 0, dh = 0; SDL_GL_GetDrawableSize(glWindow_, &dw, &dh);
        VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        vkQueueSubmit(s.gfxQ, 1, &empty, s.inFlight);   // re-signal fence
        if (dw > 0 && dh > 0) {
            std::string rr;
            if (!rebuildSwapchain(s, (uint32_t)dw, (uint32_t)dh, rr)) {
                health_.fallback_reason = rr; return false;
            }
        }
        return true;  // retry next frame
    } else if (r == VK_ERROR_DEVICE_LOST) {
        health_.fallback_reason = "DEVICE_LOST at acquire"; return false;
    } else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        health_.fallback_reason = "vkAcquireNextImageKHR failed (" + std::to_string((int)r) + ")";
        plog("%s. fail-soft.", health_.fallback_reason.c_str());
        return false;
    }

    VkImage scImg = s.scImages[imageIndex];
    vkResetCommandBuffer(s.cbuf, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cbuf, &bi);

    // shared image: GL left it in COLOR_ATTACHMENT layout -> TRANSFER_SRC for blit.
    barrier(s.cbuf, s.sharedImg.image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    // swapchain image: UNDEFINED -> TRANSFER_DST. srcStage includes
    // COLOR_ATTACHMENT_OUTPUT (the acquire wait stage) to form the acquire
    // execution dependency (sync-validation WAR guard, per the T0 harness note).
    barrier(s.cbuf, scImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = {0, 0, 0};
    region.srcOffsets[1] = {(int32_t)s.imgW, (int32_t)s.imgH, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    // VK-P14b Y-flip lockstep: when copy-1 ran it already flipped Y (dst had
    // origin at bottom), so copy-2 blits straight (dst 0->H). When copy-1 was
    // SKIPPED (endScene rendered into the shared image with GL bottom-left
    // origin), the flip that copy-1 used to do must happen HERE instead: swap the
    // dst Y offsets (0,H)->(H,0) so the top-left-origin swapchain image is upright.
    if (copy1Skipped) {
        region.dstOffsets[0] = {0, (int32_t)s.scExtent.height, 0};
        region.dstOffsets[1] = {(int32_t)s.scExtent.width, 0, 1};
    } else {
        region.dstOffsets[0] = {0, 0, 0};
        region.dstOffsets[1] = {(int32_t)s.scExtent.width, (int32_t)s.scExtent.height, 1};
    }
    vkCmdBlitImage(s.cbuf,
                   s.sharedImg.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   scImg,             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_LINEAR);

    // Optional readback of the shared image (T5 content proof), recorded on a
    // separate one-shot command buffer AFTER the blit source barrier settles.
    // (Handled below outside this cbuf to avoid layout juggling; see readback.)

    // swapchain -> PRESENT_SRC.
    barrier(s.cbuf, scImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    // Return the shared image to COLOR_ATTACHMENT for the next GL frame + the
    // VK->GL semaphore signal below.
    barrier(s.cbuf, s.sharedImg.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    vkEndCommandBuffer(s.cbuf);

    // Submit: WAIT the GL->VK semaphore (glToVkSem, GL signalled it) at TRANSFER
    // (so the blit reads the image only after GL finished copy-1), AND the acquire
    // semaphore at COLOR_ATTACHMENT_OUTPUT; SIGNAL only the per-image render sem
    // (present-wait). We do NOT signal fs.vk2gl (the VK->GL back-channel): GL is
    // a pure PRODUCER here (writes the shared tex, never waits on VK to read it),
    // so signaling a binary semaphore GL never waits would be a reuse hazard. The
    // interop slot fence (fs.fence) alone guards safe re-signalling of glToVkSem.
    VkSemaphore imgRenderSem = s.renderSems[imageIndex];
    VkSemaphore waitSems[2]  = { s.acquireSem, fs.signal };
    VkPipelineStageFlags waitStages[2] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
    };
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount   = 2;
    si.pWaitSemaphores      = waitSems;
    si.pWaitDstStageMask    = waitStages;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &s.cbuf;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &imgRenderSem;
    r = vkQueueSubmit(s.gfxQ, 1, &si, s.inFlight);  // drives acquire/present cadence
    if (r != VK_SUCCESS) {
        health_.fallback_reason = "vkQueueSubmit failed (" + std::to_string((int)r) + ")";
        plog("%s. fail-soft.", health_.fallback_reason.c_str());
        return false;
    }
    // The interop slot fence (fs.fence) guards binary-semaphore reuse two frames
    // later (vkImageResourceBeginFrame waits it). Our real submit already retired
    // the slot's work; signal fs.fence with a tiny empty submit chained after it
    // so the slot is correctly marked retired without a second heavy fence.
    {
        VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        vkQueueSubmit(s.gfxQ, 1, &empty, fs.fence);
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &imgRenderSem;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &s.swapchain;
    pi.pImageIndices      = &imageIndex;
    r = vkQueuePresentKHR(s.presentQ, &pi);
    if (r == VK_SUCCESS) {
        ++health_.present_frames;
    } else if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        ++health_.present_frames;
        int dw = 0, dh = 0; SDL_GL_GetDrawableSize(glWindow_, &dw, &dh);
        if (dw > 0 && dh > 0) {
            std::string rr;
            if (!rebuildSwapchain(s, (uint32_t)dw, (uint32_t)dh, rr)) {
                health_.fallback_reason = rr; return false;
            }
        }
    } else if (r == VK_ERROR_DEVICE_LOST) {
        health_.fallback_reason = "DEVICE_LOST at present"; return false;
    } else {
        health_.fallback_reason = "vkQueuePresentKHR failed (" + std::to_string((int)r) + ")";
        plog("%s. fail-soft.", health_.fallback_reason.c_str());
        return false;
    }

    // ==== T5 content verification: read back the shared image ==============
    // Proves copy-1 populated the shared image with REAL, CHANGING frame content.
    // Cheap + throttled: only when requested (T5 window), fully synchronous.
    if (verifyReadback) {
        verifyReadbackContent();
    }

    if (s.ctx) health_.validation_errors = s.ctx->health().validationErrors;
    return true;
}

// Synchronous readback of a small strided sample of the shared image -> checksum.
// Not on the hot path (only called when verifyReadback is set). Records its own
// one-shot copy-to-buffer, waits idle, hashes.
void EnginePresentVk::verifyReadbackContent() {
    Impl& s = *impl_;
    const uint32_t rw = s.imgW, rh = s.imgH;
    const VkDeviceSize needed = (VkDeviceSize)rw * rh * 4u;
    if (needed == 0) return;

    // (Re)allocate readback buffer if size changed.
    if (s.readbackSize != needed) {
        if (s.readbackBuf) { vkDestroyBuffer(s.dev, s.readbackBuf, nullptr); s.readbackBuf = VK_NULL_HANDLE; }
        if (s.readbackMem) { vkFreeMemory(s.dev, s.readbackMem, nullptr); s.readbackMem = VK_NULL_HANDLE; }
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = needed;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(s.dev, &bci, nullptr, &s.readbackBuf) != VK_SUCCESS) return;
        VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(s.dev, s.readbackBuf, &mr);
        VkPhysicalDeviceMemoryProperties mp{}; vkGetPhysicalDeviceMemoryProperties(s.phys, &mp);
        uint32_t idx = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((mr.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                idx = i; break;
            }
        }
        if (idx == UINT32_MAX) return;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize = mr.size; mai.memoryTypeIndex = idx;
        if (vkAllocateMemory(s.dev, &mai, nullptr, &s.readbackMem) != VK_SUCCESS) return;
        vkBindBufferMemory(s.dev, s.readbackBuf, s.readbackMem, 0);
        s.readbackSize = needed;
    }
    if (!s.readbackCbuf) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = s.cpool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(s.dev, &cai, &s.readbackCbuf) != VK_SUCCESS) return;
    }
    if (!s.readbackFence) {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(s.dev, &fci, nullptr, &s.readbackFence) != VK_SUCCESS) return;
    }

    vkResetCommandBuffer(s.readbackCbuf, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.readbackCbuf, &bi);
    // shared image is in COLOR_ATTACHMENT (post-present barrier) -> TRANSFER_SRC.
    barrier(s.readbackCbuf, s.sharedImg.image,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bic{};
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {rw, rh, 1};
    vkCmdCopyImageToBuffer(s.readbackCbuf, s.sharedImg.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.readbackBuf, 1, &bic);
    barrier(s.readbackCbuf, s.sharedImg.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    vkEndCommandBuffer(s.readbackCbuf);

    vkResetFences(s.dev, 1, &s.readbackFence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &s.readbackCbuf;
    if (vkQueueSubmit(s.gfxQ, 1, &si, s.readbackFence) != VK_SUCCESS) return;
    vkWaitForFences(s.dev, 1, &s.readbackFence, VK_TRUE, UINT64_MAX);

    void* mapped = nullptr;
    if (vkMapMemory(s.dev, s.readbackMem, 0, needed, 0, &mapped) != VK_SUCCESS) return;
    const unsigned char* px = (const unsigned char*)mapped;
    // FNV-1a over a strided sample (every 257th byte) -- cheap, sensitive to change.
    unsigned long long h = 1469598103934665603ull;
    for (VkDeviceSize i = 0; i < needed; i += 257) {
        h ^= px[i]; h *= 1099511628211ull;
    }
    vkUnmapMemory(s.dev, s.readbackMem);

    health_.prev_readback_checksum = health_.last_readback_checksum;
    health_.last_readback_checksum = h;
    if (health_.prev_readback_checksum != 0 &&
        health_.prev_readback_checksum != health_.last_readback_checksum) {
        health_.readback_changed = true;
    }
}

void EnginePresentVk::destroy() {
    if (impl_) {
        Impl& s = *impl_;
        if (s.dev) vkDeviceWaitIdle(s.dev);
        if (s.readbackFence) { vkDestroyFence(s.dev, s.readbackFence, nullptr); s.readbackFence = VK_NULL_HANDLE; }
        if (s.readbackBuf)   { vkDestroyBuffer(s.dev, s.readbackBuf, nullptr); s.readbackBuf = VK_NULL_HANDLE; }
        if (s.readbackMem)   { vkFreeMemory(s.dev, s.readbackMem, nullptr); s.readbackMem = VK_NULL_HANDLE; }
        if (s.copyFbo)       { glDeleteFramebuffers(1, &s.copyFbo); s.copyFbo = 0; }
        if (s.glView.glTex)  glInteropViewDestroy(&s.glView);
        if (s.sharedImg.image) vkImageResourceDestroy(&s.sharedImg, s.dev);
        if (s.inFlight)   { vkDestroyFence(s.dev, s.inFlight, nullptr); s.inFlight = VK_NULL_HANDLE; }
        if (s.acquireSem) { vkDestroySemaphore(s.dev, s.acquireSem, nullptr); s.acquireSem = VK_NULL_HANDLE; }
        if (s.cpool)      { vkDestroyCommandPool(s.dev, s.cpool, nullptr); s.cpool = VK_NULL_HANDLE; s.cbuf = VK_NULL_HANDLE; s.readbackCbuf = VK_NULL_HANDLE; }
        destroySwapchainOnly(s);
        if (s.surface && s.inst) { vkDestroySurfaceKHR(s.inst, s.surface, nullptr); s.surface = VK_NULL_HANDLE; }
        // Device/instance are OWNED BY THE T2 CONTEXT -- never destroyed here.
        delete impl_;
        impl_ = nullptr;
    }
    // P13b: glWindow_ is BORROWED (g_sdl_window, owned by gos_render). Never
    // destroy it here -- just drop our reference.
    glWindow_ = nullptr;
    created_ = false;
}

void EnginePresentVk::logHealth() const {
    std::fprintf(stderr,
        "[VK_PRESENT] created=%d attempted=%d present_frames=%u resize_count=%u "
        "validation_errors=%u extent=%ux%u present_mode=%s readback_changed=%d "
        "last_checksum=0x%llx fallback_reason=\"%s\"\n",
        health_.created ? 1 : 0, health_.attempted ? 1 : 0,
        health_.present_frames, health_.resize_count, health_.validation_errors,
        health_.extent_w, health_.extent_h,
        health_.present_mode_name.empty() ? "?" : health_.present_mode_name.c_str(),
        health_.readback_changed ? 1 : 0,
        (unsigned long long)health_.last_readback_checksum,
        health_.fallback_reason.empty() ? "" : health_.fallback_reason.c_str());
    std::fflush(stderr);
}

EnginePresentVk& getOrCreateEnginePresentVk() {
    static EnginePresentVk g_enginePresent;
    return g_enginePresent;
}

}  // namespace mc2vk

#endif  // MC2_VULKAN && MC2_VULKAN_ISLAND
