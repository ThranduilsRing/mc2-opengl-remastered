// engine_present_vk.cpp
//
// VK-P13-SELECTOR-PLAN / T3 -- whole-frame GL->VK present intercept implementation.
//
// Borrows the T2 shared VulkanContext device (NO second device / NO second
// volkLoadDevice). Owns only the hidden window + surface + swapchain + per-frame
// sync + the exportable interop image (GL writes it via copy-1, VK blits it into
// the swapchain via copy-2). Swapchain/present logic keeps one
// render-finished semaphore per swapchain image. Fail-soft at every step.
//
// Requires BOTH MC2_VULKAN (context/present) and MC2_VULKAN_ISLAND (interop
// primitives). build64_island defines both; the guards below make the TU inert
// otherwise so the GL shipping build never sees a Vulkan symbol.

#include "engine_present_vk.h"
#include "Gates/gate.h"
#include "gos_visual_capture.h"   // VK-VISUAL-CAPTURE-1
#include "Gates/gate_cold.h"   // gate::get<>() and str<>()
#include "mc2fmt.h"

#if defined(MC2_VULKAN) && defined(MC2_VULKAN_ISLAND)

// Win32 surface entrypoints + names are gated behind this macro (matches
// vulkan_context.cpp / vk_image_resource.h). Define BEFORE volk.h.
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include "vulkan_context.h"
#include "vk_image_resource.h"
// VK-RUNG8-ITEM8: the present tail no longer uses gl_interop_view (GL view /
// copy-1 / GL->VK handoff all deleted).
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#include "vk_shared_gbuffer_gate.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"

#include <GL/glew.h>   // copy-1 GL blit (FBO-0 -> shared GL texture)
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <SDL2/SDL_syswm.h>   // P13b: SDL_GetWindowWMInfo -> HWND / HINSTANCE

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>   // VK-QWI-TO-FENCE-1: composite wait telemetry
#include <tracy/Tracy.hpp>   // VK-ISLAND-TRACY-1: present-path CPU attribution zones
#include <unordered_map>
#include <vector>

namespace mc2vk {

namespace {

void plog(const char* fmt, ...) {
    mc2::fmt::print_err("[VK_PRESENT] ");
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    mc2::fmt::print_err("\n");
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

    mc2vk::OwnedSurface surface;   // released with the instance it was made on (the engine context's), before the swapchain below
    uint32_t         presentFamily = UINT32_MAX;
    VkQueue          presentQ  = VK_NULL_HANDLE;

    // swapchain
    mc2vk::OwnedSwapchain    swapchain;
    VkFormat                 scFormat  = VK_FORMAT_UNDEFINED;
    VkExtent2D               scExtent  = {0, 0};
    VkPresentModeKHR         presentMode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage>     scImages;
    std::vector<mc2vk::OwnedSemaphore> renderSems;   // one per image (present-wait target)

    // per-frame sync (1 frame in flight for the acquire/submit)
    mc2vk::OwnedCommandPool   cpool;
    VkCommandBuffer cbuf      = VK_NULL_HANDLE;   // present tail: overlay + HUD/ImGui + blit
    mc2vk::OwnedSemaphore     acquireSem;
    mc2vk::OwnedFence         inFlight;
    // VK-PRESENT-SINGLESUBMIT-1: the native composite records into its OWN command
    // buffer at composite time (no submit, no drain) and presentGLFrame submits
    // {compositeCbuf, cbuf} in ONE vkQueueSubmit. compositePending marks a recorded-
    // but-not-yet-submitted composite; it is consumed by the single submit, dropped
    // on resize (the recording references the old shared image), and simply
    // re-recorded next frame when present declines mid-frame (DISARM-2b: a decline
    // never leaves the fence unsignaled — inFlight is reset ONLY immediately before
    // a submit that will signal it). This supersedes VK-QWI-TO-FENCE-1's dedicated
    // compositeFence (deleted): the ONLY CPU wait on the present path is now the
    // inFlight wait for the PREVIOUS frame's single submit, taken at composite-record
    // time before either cbuf is reset.
    VkCommandBuffer compositeCbuf   = VK_NULL_HANDLE;
    bool            compositePending = false;
    // VK-PRESENT-SINGLESUBMIT-1: the CSM shadow-debug overlay (opt-in debug) is
    // STASHED here and
    // recorded into the tail cbuf so it stays ordered after the composite (which no
    // longer executes before present records) and joins the single submit.
    VkImageView     pendingShadowDebugView = VK_NULL_HANDLE;
    int             pendingShadowDebugLayer = 0;

    // the shared VK-only present image (native VK composite writes it, VK blits it
    // into the swapchain). VK-RUNG8-ITEM8: no GL interop view / copy FBO any more.
    VkImageResource sharedImg{};
    uint32_t        imgW = 0, imgH = 0;

    // readback (T5 content verification)
    mc2vk::OwnedDeviceMemory  readbackMem; // declared before the buffer bound to it: released after it
    mc2vk::OwnedRawBuffer        readbackBuf;
    VkDeviceSize    readbackSize = 0;
    VkCommandBuffer readbackCbuf = VK_NULL_HANDLE;
    mc2vk::OwnedFence         readbackFence;

    // Rung 8.6 native ImGui renderer backend. GuiRuntime builds ImDrawData;
    // presentGLFrame records it here after GL->VK handoff and before copy-2.
    bool            imguiVkInited = false;
    bool            imguiVkFailed = false;
    mc2vk::OwnedRenderPass    imguiRenderPass;
    mc2vk::OwnedFramebuffer   imguiFramebuffer;
    ImDrawData*     pendingImguiDrawData = nullptr;
    // SINGLE-FRAME-IN-FLIGHT DEPENDENCY (VK-PRESENT-SINGLESUBMIT-1 audit note): the
    // HUD tail is race-free ONLY because exactly one present submission is in flight
    // (the inFlight wait at composite-record time retires frame N-1 before frame N
    // records): hudColorVertexBuf/hudTexVertexBuf are single instances rewritten per
    // frame, texture mirrors are updated in place on serial change, and the 4-frame
    // mirror eviction window assumes retired reuse. Any future 2-frames-in-flight
    // change MUST ship together with per-frame HUD vertex buffers, mirror
    // double-buffering, and a re-derived eviction window.
    std::vector<VkHudDraw> pendingHudDraws;
    // HUD-CANVAS-EXTENT-FIX (2026-07-11): the pixel space the queued HUD vertices
    // were authored in (gosRenderer width_/height_ == Environment.screenWidth/Height,
    // the logical game resolution). This is NOT necessarily the present image size
    // (imgW/imgH == the physical GL drawable): under DPI scaling or a logical-res <
    // window-drawable configuration they differ, and the VK HUD shader must divide by
    // the AUTHORING canvas (NDC is resolution-independent) or the HUD renders at the
    // wrong scale/anchor (the field bug: 1920-authored HUD ÷ 3840 present => half-scale
    // top-left quadrant). Zero => fall back to imgW/imgH (identity when they match).
    uint32_t        hudCanvasW = 0, hudCanvasH = 0;
    bool            hudColorVkFailed = false;
    mc2vk::OwnedRenderPass    hudColorRenderPass;
    mc2vk::OwnedFramebuffer   hudColorFramebuffer;
    mc2vk::OwnedPipelineLayout hudColorLayout;
    mc2vk::OwnedPipeline      hudColorTriPipe;
    mc2vk::OwnedPipeline      hudColorTriOneInvAlphaPipe; // VK-FIELD-VFX-3: additive untextured effect ribbons
    mc2vk::OwnedPipeline      hudColorTriOneOnePipe;      // VK-FIELD-VFX-7: pure-additive (ONE,ONE) effect ribbons
    mc2vk::OwnedPipeline      hudColorLinePipe;
    mc2vk::OwnedPipeline      hudColorTriOpaquePipe;
    mc2vk::OwnedPipeline      hudColorLineOpaquePipe;
    mc2vk::OwnedShaderModule  hudColorVert;
    mc2vk::OwnedShaderModule  hudColorFrag;
    mc2vk::OwnedDeviceMemory  hudColorVertexMem; // declared before the buffer bound to it: released after it
    mc2vk::OwnedRawBuffer        hudColorVertexBuf;
    VkDeviceSize    hudColorVertexBytes = 0;
    mc2vk::OwnedDescriptorSetLayout hudTexDsl;
    mc2vk::OwnedDescriptorPool hudTexPool;
    mc2vk::OwnedPipelineLayout hudTexLayout;
    mc2vk::OwnedPipeline      hudTexPipe;
    mc2vk::OwnedPipeline      hudTexOpaquePipe;
    mc2vk::OwnedPipeline      hudTexOneInvAlphaPipe; // VK-FIELD-VFX-3: GL OneInvAlpha (ONE,1-srcA) effect glow
    mc2vk::OwnedPipeline      hudTexOneOnePipe;      // VK-FIELD-VFX-7: GL OneOne (ONE,ONE) muzzle-flash/tracer sprites
    mc2vk::OwnedShaderModule  hudTexVert;
    mc2vk::OwnedShaderModule  hudTexFrag;
    mc2vk::OwnedDeviceMemory  hudTexVertexMem; // declared before the buffer bound to it: released after it
    mc2vk::OwnedRawBuffer        hudTexVertexBuf;
    VkDeviceSize    hudTexVertexBytes = 0;
    struct HudTextureMirror {
        uint32_t serial = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        mc2vk::OwnedDeviceMemory memory;   // declared first, released last: the buffer bound to it goes first
        mc2vk::OwnedRawBuffer buffer;
        VkDeviceSize bytes = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t lastFrame = 0;      // VK-HUD-GLYPH-CORRUPTION: eviction watermark
    };
    // VK-HUD-GLYPH-CORRUPTION FIX: keyed by (textureHandle<<32 | textureSerial), NOT
    // handle alone. The game re-bakes many distinct runtime strings into ONE reused GL
    // texture handle within a single HUD frame (e.g. handle 275 cycles serials 2..9),
    // bumping the content serial each bake. The old handle-only mirror destroyed +
    // re-uploaded its single buffer in place, so every draw referencing that handle
    // ended up sampling the LAST bake -> earlier strings rendered the wrong glyphs
    // (OBJECTIVES/QUICK SAVE/pilot names garbled while pre-baked serial==1 labels stayed
    // clean). A per-(handle,serial) mirror gives each distinct bake its own persistent
    // buffer+descriptor for the frame. Entries are evicted a few frames after last use.
    std::unordered_map<uint64_t, HudTextureMirror> hudTextureMirrors;
    uint64_t hudMirrorFrameCounter = 0;

    bool            compositeVkFailed = false;
    mc2vk::OwnedRenderPass    compositeRenderPass;
    mc2vk::OwnedFramebuffer   compositeFramebuffer;
    mc2vk::OwnedSampler       compositeSampler;
    mc2vk::OwnedSampler       compositeNearestSampler;   // the R32_UINT object-id image cannot be filtered linearly
    mc2vk::OwnedDescriptorSetLayout compositeDsl;
    mc2vk::OwnedDescriptorPool compositePool;
    VkDescriptorSet  compositeSet = VK_NULL_HANDLE;
    mc2vk::OwnedPipelineLayout compositeLayout;
    mc2vk::OwnedPipeline       compositePipe;
    mc2vk::OwnedShaderModule   compositeVert;
    mc2vk::OwnedShaderModule   compositeFrag;

    bool             shadowDebugVkFailed = false;
    mc2vk::OwnedRenderPass     shadowDebugRenderPass;
    mc2vk::OwnedFramebuffer    shadowDebugFramebuffer;
    mc2vk::OwnedSampler        shadowDebugSampler;
    mc2vk::OwnedDescriptorSetLayout shadowDebugDsl;
    mc2vk::OwnedDescriptorPool shadowDebugPool;
    VkDescriptorSet  shadowDebugSet = VK_NULL_HANDLE;
    mc2vk::OwnedPipelineLayout shadowDebugLayout;
    mc2vk::OwnedPipeline       shadowDebugPipe;
    mc2vk::OwnedShaderModule   shadowDebugVert;
    mc2vk::OwnedShaderModule   shadowDebugFrag;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    // The shared present image holds raw allocations that vkImageResourceDestroy releases (and counts); the framebuffers over its view go first. Every
    // other member is an owner and follows in reverse declaration order.
    ~Impl() {
        imguiFramebuffer.reset();
        hudColorFramebuffer.reset();
        compositeFramebuffer.reset();
        shadowDebugFramebuffer.reset();
        if (dev != VK_NULL_HANDLE) vkImageResourceDestroy(&sharedImg, dev);
    }
};

namespace {

// VK-P14b (MC2_VK_PRESENT_DIRECT_FBO, copy-1 elimination): memoized gate. When ON
// AND MC2_RENDER_BACKEND=vulkan AND the present path is valid, endScene binds the
// shared present FBO instead of FBO-0, so copy-1 is skipped and the Y-flip moves
// into copy-2. Default OFF -> gate-OFF is byte-identical to the copy-1 path.
bool directFboGateEnabled() {
    // VK-FIELD-ENV-COMPOSITE-ARM-1: after VK-RUNG8-ITEM8 deleted the GL copy-1
    // present FBO, the native VK composite is the ONLY producer of the present
    // image for a VK-owned scene. This gate MUST therefore arm from the umbrella
    // (MC2_VK_SHARED_GBUFFER) exactly like the other direct-chain members — else
    // umbrella-ON steals the scene draws into the VK islands but never composites
    // them, giving the catastrophic "terrain invisible / sky over everything"
    // field image.
    const bool on = mc2vkgate::umbrellaOn();   // per-call: VK-FAILSOFT-DISARM-1
    return on;
}

// VK-TAIL-HOOK-1: opt-in proof seam for native final-tail commands. The hook is
// behavior-neutral for now; it records a real command-buffer dependency at the
// exact point future native HUD/ImGui rendering will attach.
bool presentTailHookEnabled() {
    // VK-FIELD-ENV-COMPOSITE-ARM-1: the native HUD/ImGui tail seam is part of the
    // VK-owned present path, so it arms from the umbrella too (keeps the field env
    // == the proof env: one flip name arms the whole present chain). Behaviour-
    // neutral when the native HUD/ImGui draws are absent.
    const bool on = mc2vkgate::umbrellaOn();    // per-call: VK-FAILSOFT-DISARM-1
    return on;
}

bool nativeHudColorProofEnabled() {
    // VK-FIELD-ENV-CONTENT-STACK-ARM-1 (2026-07-11): despite the legacy "COLOR_PROOF"
    // name this is the ACTUAL native VK HUD renderer -- recordPendingHudColorProof()
    // draws NOTHING unless this is on (it clears the queued HUD undrawn otherwise). So
    // the game HUD (colored tris/lines + textured/text quads) only reaches the VK-only
    // present image when this is armed. Post-VK-RUNG8-ITEM8 the GL HUD draws into the
    // DISCARDED backbuffer, so with this OFF the HUD VANISHES (the field bug: MC2_VK_HUD
    // queued the draws but nothing rendered them). It is a member of the VK-owned present
    // tail, so it arms from the umbrella exactly like presentTailHookEnabled /
    // directFboGateEnabled / nativeCompositeGateEnabled. The strict proof armed
    // MC2_VK_HUD_COLOR_PROOF=1 and passed.
    const bool on = mc2vkgate::umbrellaOn();      // per-call: VK-FAILSOFT-DISARM-1
    return on;
}

bool nativeCompositeGateEnabled() {
    // VK-FIELD-ENV-COMPOSITE-ARM-1: arm from the umbrella (see directFboGateEnabled).
    // The native composite is the sole present-image producer post item-8, so the
    // umbrella flip must arm it by default.
    const bool on = mc2vkgate::umbrellaOn();       // per-call: VK-FAILSOFT-DISARM-1
    return on;
}

bool compositeDirectChainGateEnabled() {
    static const bool on = mc2vkgate::umbrellaOn();
    return on;
}

bool objectIdDirectChainGateEnabled() {
    static const bool on = []() {
        return mc2::gate::get<"MC2_VK_OBJECTID_DIRECT_CHAIN">() && compositeDirectChainGateEnabled();
    }();
    return on;
}

// VK-RUNG8-ITEM8: the present interop tail is deleted. A present gate that only
// ever SELECTED a now-deleted branch is retired to a parseable no-op (item-9
// pattern). MC2_VK_PRESENT_SKIP_GL_HANDOFF selected the VK-owned skip of the
// GL->VK handoff (the handoff itself is gone -> the skip is unconditional).
// MC2_VK_PRESENT_TAIL_HOOK is NOT retired -- it still gates the native HUD/ImGui
// tail recording. Emitted once from init().
void emitDeprecatedPresentGateNotes() {
    static bool done = false;
    if (done) return;
    done = true;
    if (mc2::gate::get<"MC2_VK_PRESENT_SKIP_GL_HANDOFF">())
        plog("MC2_VK_PRESENT_SKIP_GL_HANDOFF is deprecated (item-8): the GL->VK "
             "present handoff is deleted; the skip is now unconditional. No-op.");
}

bool presentNativeStrictEnabled() {
    static const bool on = mc2::gate::get<"MC2_VK_PRESENT_NATIVE_STRICT">();
    return on;
}

void presentNativeStrictAbort(const char* reason) {
    if (!presentNativeStrictEnabled())
        return;
    plog("MC2_VK_PRESENT_NATIVE_STRICT abort reason=%s",
         reason ? reason : "unknown");
    std::fflush(stderr);
    std::abort();
}

std::string spv_dir() {
    std::string d = mc2::gate::str<"MC2_VULKAN_SPV_DIR">();   // row default shaders/vulkan, empty reads the default
    if (!d.empty() && d.back() != '/' && d.back() != '\\') d += '/';
    return d;
}

mc2vk::OwnedShaderModule loadSpv(VkDevice device, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { plog("cannot open spv '%s'.", path.c_str()); return {}; }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || (len % 4) != 0) { std::fclose(f); return {}; }
    std::vector<uint32_t> code((size_t)len / 4);
    size_t got = std::fread(code.data(), 1, (size_t)len, f);
    std::fclose(f);
    if (got != (size_t)len) return {};
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = (size_t)len;
    ci.pCode = code.data();
    mc2vk::OwnedShaderModule mod;
    mc2vk::create(mod, device, [&](auto* h) { return vkCreateShaderModule(device, &ci, nullptr, h); });
    return mod;
}

// -------- swapchain (re)build against the SHARED device -------------------
bool chooseQueuesAndPresentMode(EnginePresentVk::Impl& s, std::string& reason) {
    // The shared context picked gfxFamily; confirm it can present to our surface.
    VkBool32 canPresent = VK_FALSE;
    if (vkGetPhysicalDeviceSurfaceSupportKHR(s.phys, s.gfxFamily, s.surface.get(), &canPresent)
            == VK_SUCCESS && canPresent == VK_TRUE) {
        s.presentFamily = s.gfxFamily;
    } else {
        // Find any present-capable family (single-GPU AMD: gfx presents; this is
        // a robustness fallback).
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &qfCount, nullptr);
        for (uint32_t q = 0; q < qfCount; ++q) {
            VkBool32 p = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(s.phys, q, s.surface.get(), &p) == VK_SUCCESS
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
    vkGetPhysicalDeviceSurfacePresentModesKHR(s.phys, s.surface.get(), &pmCount, nullptr);
    std::vector<VkPresentModeKHR> pms(pmCount);
    if (pmCount) vkGetPhysicalDeviceSurfacePresentModesKHR(s.phys, s.surface.get(), &pmCount, pms.data());
    bool haveMailbox = false, haveImmediate = false;
    for (auto m : pms) {
        if (m == VK_PRESENT_MODE_MAILBOX_KHR)   haveMailbox = true;
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) haveImmediate = true;
    }
    const bool vsyncOn = mc2::gate::get<"MC2_VSYNC">();
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
    s.renderSems.clear();   // the per-image semaphores go before the swapchain they were made for
    s.scImages.clear();
    s.swapchain.reset();
}

bool buildSwapchain(EnginePresentVk::Impl& s, uint32_t w, uint32_t h,
                    VkSwapchainKHR oldSwapchain, std::string& reason) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.phys, s.surface.get(), &caps) != VK_SUCCESS) {
        reason = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed"; return false;
    }
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface.get(), &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    if (fmtCount) vkGetPhysicalDeviceSurfaceFormatsKHR(s.phys, s.surface.get(), &fmtCount, fmts.data());
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
    sci.surface          = s.surface.get();
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
    const VkResult createResult = mc2vk::createChecked(s.swapchain, s.dev, [&](auto* h) { return vkCreateSwapchainKHR(s.dev, &sci, nullptr, h); });
    if (createResult != VK_SUCCESS) {
        reason = "vkCreateSwapchainKHR failed (VkResult=" + std::to_string((int)createResult) + ")";
        return false;
    }
    s.scFormat = chosen.format;
    s.scExtent = extent;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(s.dev, s.swapchain.get(), &n, nullptr);
    s.scImages.resize(n);
    vkGetSwapchainImagesKHR(s.dev, s.swapchain.get(), &n, s.scImages.data());
    s.renderSems.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!mc2vk::create(s.renderSems[i], s.dev, [&](auto* h) { return vkCreateSemaphore(s.dev, &semci, nullptr, h); })) {
            reason = "per-image render semaphore create failed"; return false;
        }
    }
    plog("swapchain OK images=%u extent=%ux%u format=%d mode=%s",
         n, extent.width, extent.height, (int)chosen.format, presentModeName(s.presentMode));
    return true;
}

// VK-SWAPCHAIN-RESIZE-HARDEN-1: recreate the swapchain with proper oldSwapchain
// chaining, bounded retry + backoff for genuinely-transient window states, and
// resize_recreate_* receipts so future sessions can see recovery behavior.
//
// Root cause of the intermittent vkCreateSwapchainKHR -> VK_ERROR_NATIVE_WINDOW_
// IN_USE (VkResult=-1000000001): the previous retry created a fresh swapchain with
// oldSwapchain=VK_NULL_HANDLE WHILE the old swapchain was still alive on the same
// surface (it was destroyed only after the retry). A surface that still owns a
// live swapchain rejects a second create with NATIVE_WINDOW_IN_USE. The fix:
// attempt 0 uses proper oldSwapchain chaining; on failure the old chain is
// RETIRED before any oldSwapchain-free retry, so the surface is free.
//
// Only the error/retry path changed -- a first-try success is byte-for-byte the
// old happy path (create with oldSwapchain=old, then destroy old) -> no gate.
bool rebuildSwapchain(EnginePresentVk::Impl& s, uint32_t w, uint32_t h,
                      EnginePresentHealth& health, std::string& reason) {
    // Full quiesce: no present/blit submit may still reference the old swapchain
    // when we retire it, and the surface must be settled before recreate.
    s.ctx->waitIdle("present_swapchain_rebuild");

    mc2vk::OwnedSwapchain old = std::move(s.swapchain);
    std::vector<mc2vk::OwnedSemaphore> oldSems = std::move(s.renderSems);
    s.renderSems.clear();
    s.scImages.clear();

    constexpr int kMaxAttempts = 3;
    bool ok = false;
    std::string firstReason;
    for (int attempt = 0; attempt < kMaxAttempts && !ok; ++attempt) {
        // Attempt 0: chain the live old swapchain (retire it inside the create).
        // Retries run only AFTER `old` has been destroyed below, so they MUST pass
        // VK_NULL_HANDLE -- a second swapchain on a surface that still owns a live
        // one is exactly the NATIVE_WINDOW_IN_USE we are fixing.
        VkSwapchainKHR chainArg = (attempt == 0) ? old.get() : VK_NULL_HANDLE;
        ok = buildSwapchain(s, w, h, chainArg, reason);
        if (ok) {
            if (attempt > 0) ++health.resize_recreate_retries;
            break;
        }
        if (attempt == 0) {
            firstReason = reason;
            // Retire the old swapchain + its semaphores BEFORE any retry so the
            // surface is free of a live swapchain for the VK_NULL_HANDLE create.
            oldSems.clear();
            old.reset();
            plog("swapchain rebuild with oldSwapchain failed (%s); retired old chain, "
                 "retrying without oldSwapchain.", firstReason.c_str());
        } else {
            plog("swapchain rebuild retry %d failed (%s).", attempt, reason.c_str());
        }
        // Backoff for transient window states (live-resize / DWM transition can
        // briefly hold the HWND): let the OS release it, then re-quiesce.
        if (!ok && attempt + 1 < kMaxAttempts) {
            SDL_Delay(4);
            s.ctx->waitIdle("present_swapchain_retry");
        }
    }

    // Retire any survivors (the retry path may already have destroyed `old`).
    oldSems.clear();
    old.reset();

    if (ok) {
        ++health.resize_recreate_ok;
    } else {
        ++health.resize_recreate_failsoft;
        if (!firstReason.empty())
            reason = firstReason + "; after " + std::to_string(kMaxAttempts) +
                     " attempts: " + reason;
    }
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

uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && ((mp.memoryTypes[i].propertyFlags & flags) == flags))
            return i;
    }
    return UINT32_MAX;
}

struct HudColorGpuVertex {
    float x, y;
    uint32_t argb;
};

struct HudTexGpuVertex {
    float x, y;
    uint32_t argb;
    float u, v;
};

struct HudTexPush {
    float extent[2];
    uint32_t texW;
    uint32_t texH;
    uint32_t textMode;
    uint32_t foreground;
};

void destroyImguiFramebuffer(EnginePresentVk::Impl& s) { s.imguiFramebuffer.reset(); }
void destroyHudColorFramebuffer(EnginePresentVk::Impl& s) { s.hudColorFramebuffer.reset(); }
void destroyCompositeFramebuffer(EnginePresentVk::Impl& s) { s.compositeFramebuffer.reset(); }
void destroyShadowDebugFramebuffer(EnginePresentVk::Impl& s) { s.shadowDebugFramebuffer.reset(); }

bool ensureCompositeObjects(EnginePresentVk::Impl& s) {
    if (s.compositePipe.get() && s.compositeFramebuffer.get()) return true;
    if (s.compositeVkFailed || !s.dev || !s.sharedImg.view || s.imgW == 0 || s.imgH == 0)
        return false;

    VkImageResource* scene = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    if (!scene || !scene->view) return false;

    if (!s.compositeSampler) {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = VK_FILTER_LINEAR;
        ci.minFilter = VK_FILTER_LINEAR;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!mc2vk::create(s.compositeSampler, s.dev, [&](auto* h) { return vkCreateSampler(s.dev, &ci, nullptr, h); })) {
            s.compositeVkFailed = true; return false;
        }
    }
    if (!s.compositeNearestSampler) {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = VK_FILTER_NEAREST;
        ci.minFilter = VK_FILTER_NEAREST;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!mc2vk::create(s.compositeNearestSampler, s.dev, [&](auto* h) { return vkCreateSampler(s.dev, &ci, nullptr, h); })) {
            s.compositeVkFailed = true; return false;
        }
    }
    if (!s.compositeRenderPass) {
        VkAttachmentDescription color{};
        color.format = VK_FORMAT_R8G8B8A8_UNORM;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &color;
        rp.subpassCount = 1; rp.pSubpasses = &sub;
        if (!mc2vk::create(s.compositeRenderPass, s.dev, [&](auto* h) { return vkCreateRenderPass(s.dev, &rp, nullptr, h); })) {
            s.compositeVkFailed = true; return false;
        }
    }
    if (!s.compositeFramebuffer) {
        VkImageView att[] = { s.sharedImg.view.get() };
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = s.compositeRenderPass.get();
        fb.attachmentCount = 1; fb.pAttachments = att;
        fb.width = s.imgW; fb.height = s.imgH; fb.layers = 1;
        if (!mc2vk::create(s.compositeFramebuffer, s.dev, [&](auto* h) { return vkCreateFramebuffer(s.dev, &fb, nullptr, h); })) {
            s.compositeVkFailed = true; return false;
        }
    }
    if (!s.compositeDsl) {
        VkDescriptorSetLayoutBinding b[2]{};
        b[0].binding = 0;
        b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[0].descriptorCount = 1;
        b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        b[1] = b[0];
        b[1].binding = 1;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 2; ci.pBindings = b;
        if (!mc2vk::create(s.compositeDsl, s.dev, [&](auto* h) { return vkCreateDescriptorSetLayout(s.dev, &ci, nullptr, h); })) {
            s.compositeVkFailed = true; return false;
        }
    }
    if (!s.compositePool) {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &size;
        if (!mc2vk::create(s.compositePool, s.dev, [&](auto* h) { return vkCreateDescriptorPool(s.dev, &pci, nullptr, h); })) {
            s.compositeVkFailed = true; return false;
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.compositePool.get();
        ai.descriptorSetCount = 1;
        const VkDescriptorSetLayout compositeDslHandle = s.compositeDsl.get();
        ai.pSetLayouts = &compositeDslHandle;
        if (vkAllocateDescriptorSets(s.dev, &ai, &s.compositeSet) != VK_SUCCESS) {
            s.compositeVkFailed = true; return false;
        }
    }
    VkImageResource* objectId = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_OBJECT_ID);
    VkDescriptorImageInfo ii[2]{};
    ii[0].sampler = s.compositeSampler.get();
    ii[0].imageView = scene->view.get();
    ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[1].sampler = s.compositeNearestSampler.get();                 // the shader reads the id with usampler2D: an exact texel, never a blend
    ii[1].imageView = objectId && objectId->view.get() ? objectId->view.get() : scene->view.get();
    ii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[i].dstSet = s.compositeSet;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(s.dev, 2, wr, 0, nullptr);

    if (!s.compositeLayout) {
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.size = sizeof(float) * 16;
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        const VkDescriptorSetLayout compositeDslHandle = s.compositeDsl.get();
        ci.setLayoutCount = 1; ci.pSetLayouts = &compositeDslHandle;
        ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pc;
        if (!mc2vk::create(s.compositeLayout, s.dev, [&](auto* h) { return vkCreatePipelineLayout(s.dev, &ci, nullptr, h); })) {
            s.compositeVkFailed = true; return false;
        }
    }
    if (!s.compositePipe) {
        s.compositeVert = loadSpv(s.dev, spv_dir() + "postprocess.vert.spv");
        s.compositeFrag = loadSpv(s.dev, spv_dir() + "postprocess_visual.frag.spv");
        if (!s.compositeVert.get() || !s.compositeFrag.get()) { s.compositeVkFailed = true; return false; }

        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = s.compositeVert.get(); st[0].pName = "main";
        st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = s.compositeFrag.get(); st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyns;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dyn; gp.layout = s.compositeLayout.get();
        gp.renderPass = s.compositeRenderPass.get();
        if (!mc2vk::create(s.compositePipe, s.dev, [&](auto* h) { return vkCreateGraphicsPipelines(s.dev, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            s.compositeVkFailed = true; return false;
        }
        mc2vk::nameObject(s.dev, s.compositePipe.get(), "present.compositePipe");
    }
    return true;
}

bool ensureShadowDebugOverlayObjects(EnginePresentVk::Impl& s, VkImageView csmArrayView) {
    if (s.shadowDebugPipe.get() && s.shadowDebugFramebuffer.get() && csmArrayView != VK_NULL_HANDLE)
        return true;
    if (s.shadowDebugVkFailed || !s.dev || !s.sharedImg.view || csmArrayView == VK_NULL_HANDLE ||
        s.imgW == 0 || s.imgH == 0)
        return false;

    if (!s.shadowDebugSampler) {
        VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.magFilter = VK_FILTER_LINEAR;
        ci.minFilter = VK_FILTER_LINEAR;
        ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.compareEnable = VK_FALSE;
        if (!mc2vk::create(s.shadowDebugSampler, s.dev, [&](auto* h) { return vkCreateSampler(s.dev, &ci, nullptr, h); })) {
            s.shadowDebugVkFailed = true; return false;
        }
    }
    if (!s.shadowDebugRenderPass) {
        VkAttachmentDescription color{};
        color.format = VK_FORMAT_R8G8B8A8_UNORM;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &color;
        rp.subpassCount = 1; rp.pSubpasses = &sub;
        if (!mc2vk::create(s.shadowDebugRenderPass, s.dev, [&](auto* h) { return vkCreateRenderPass(s.dev, &rp, nullptr, h); })) {
            s.shadowDebugVkFailed = true; return false;
        }
    }
    if (!s.shadowDebugFramebuffer) {
        VkImageView att[] = { s.sharedImg.view.get() };
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = s.shadowDebugRenderPass.get();
        fb.attachmentCount = 1; fb.pAttachments = att;
        fb.width = s.imgW; fb.height = s.imgH; fb.layers = 1;
        if (!mc2vk::create(s.shadowDebugFramebuffer, s.dev, [&](auto* h) { return vkCreateFramebuffer(s.dev, &fb, nullptr, h); })) {
            s.shadowDebugVkFailed = true; return false;
        }
    }
    if (!s.shadowDebugDsl) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1; ci.pBindings = &b;
        if (!mc2vk::create(s.shadowDebugDsl, s.dev, [&](auto* h) { return vkCreateDescriptorSetLayout(s.dev, &ci, nullptr, h); })) {
            s.shadowDebugVkFailed = true; return false;
        }
    }
    if (!s.shadowDebugPool) {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &size;
        if (!mc2vk::create(s.shadowDebugPool, s.dev, [&](auto* h) { return vkCreateDescriptorPool(s.dev, &pci, nullptr, h); })) {
            s.shadowDebugVkFailed = true; return false;
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.shadowDebugPool.get();
        ai.descriptorSetCount = 1;
        const VkDescriptorSetLayout shadowDebugDslHandle = s.shadowDebugDsl.get();
        ai.pSetLayouts = &shadowDebugDslHandle;
        if (vkAllocateDescriptorSets(s.dev, &ai, &s.shadowDebugSet) != VK_SUCCESS) {
            s.shadowDebugVkFailed = true; return false;
        }
    }
    VkDescriptorImageInfo ii{};
    ii.sampler = s.shadowDebugSampler.get();
    ii.imageView = csmArrayView;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = s.shadowDebugSet;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(s.dev, 1, &wr, 0, nullptr);

    if (!s.shadowDebugLayout) {
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.size = sizeof(int) * 4;
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        const VkDescriptorSetLayout shadowDebugDslHandle = s.shadowDebugDsl.get();
        ci.setLayoutCount = 1; ci.pSetLayouts = &shadowDebugDslHandle;
        ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pc;
        if (!mc2vk::create(s.shadowDebugLayout, s.dev, [&](auto* h) { return vkCreatePipelineLayout(s.dev, &ci, nullptr, h); })) {
            s.shadowDebugVkFailed = true; return false;
        }
    }
    if (!s.shadowDebugPipe) {
        s.shadowDebugVert = loadSpv(s.dev, spv_dir() + "shadow_debug_overlay.vert.spv");
        s.shadowDebugFrag = loadSpv(s.dev, spv_dir() + "shadow_debug_overlay.frag.spv");
        if (!s.shadowDebugVert.get() || !s.shadowDebugFrag.get()) { s.shadowDebugVkFailed = true; return false; }

        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = s.shadowDebugVert.get(); st[0].pName = "main";
        st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = s.shadowDebugFrag.get(); st[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyns;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dyn; gp.layout = s.shadowDebugLayout.get();
        gp.renderPass = s.shadowDebugRenderPass.get();
        if (!mc2vk::create(s.shadowDebugPipe, s.dev, [&](auto* h) { return vkCreateGraphicsPipelines(s.dev, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
            s.shadowDebugVkFailed = true; return false;
        }
        mc2vk::nameObject(s.dev, s.shadowDebugPipe.get(), "present.shadowDebugPipe");
    }
    return true;
}

// A HOST_VISIBLE | HOST_COHERENT buffer with its own bound memory. The owners are filled as the steps succeed, so a failure part-way leaves what was made
// to be released with them (the buffer first: callers declare the memory owner before the buffer owner).
bool createHostBuffer(EnginePresentVk::Impl& s, VkDeviceSize bytes, VkBufferUsageFlags usage,
                      mc2vk::OwnedRawBuffer& outBuf, mc2vk::OwnedDeviceMemory& outMem, std::string_view name) {
    outBuf.reset();
    outMem.reset();
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!mc2vk::create(outBuf, s.dev, [&](auto* h) { return vkCreateBuffer(s.dev, &bci, nullptr, h); }))
        return false;
    mc2vk::nameObject(s.dev, outBuf.get(), name);
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(s.dev, outBuf.get(), &req);
    const uint32_t memType = findMemoryType(s.phys, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memType == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memType;
    if (!mc2vk::create(outMem, s.dev, [&](auto* h) { return vkAllocateMemory(s.dev, &ai, nullptr, h); }))
        return false;
    return vkBindBufferMemory(s.dev, outBuf.get(), outMem.get(), 0) == VK_SUCCESS;
}

bool ensureHudColorVertexBuffer(EnginePresentVk::Impl& s, VkDeviceSize bytes) {
    if (bytes == 0) return false;
    if (s.hudColorVertexBuf && s.hudColorVertexBytes >= bytes)
        return true;
    if (!createHostBuffer(s, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          s.hudColorVertexBuf, s.hudColorVertexMem, "present.hudColorVertex"))
        return false;
    s.hudColorVertexBytes = bytes;
    return true;
}

bool ensureHudTexVertexBuffer(EnginePresentVk::Impl& s, VkDeviceSize bytes) {
    if (bytes == 0) return false;
    if (s.hudTexVertexBuf && s.hudTexVertexBytes >= bytes)
        return true;
    if (!createHostBuffer(s, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          s.hudTexVertexBuf, s.hudTexVertexMem, "present.hudTexVertex"))
        return false;
    s.hudTexVertexBytes = bytes;
    return true;
}

bool ensureHudColorObjects(EnginePresentVk::Impl& s) {
    if (s.hudColorTriPipe.get() && s.hudColorLinePipe.get() &&
        s.hudColorTriOpaquePipe.get() && s.hudColorLineOpaquePipe.get() &&
        s.hudColorTriOneInvAlphaPipe.get() && s.hudColorTriOneOnePipe.get() &&
        s.hudColorFramebuffer.get())
        return true;
    if (s.hudColorVkFailed || !s.dev || !s.sharedImg.view || s.imgW == 0 || s.imgH == 0)
        return false;

    if (!s.hudColorRenderPass) {
        VkAttachmentDescription color{};
        color.format = VK_FORMAT_R8G8B8A8_UNORM;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &color;
        rp.subpassCount = 1; rp.pSubpasses = &sub;
        if (!mc2vk::create(s.hudColorRenderPass, s.dev, [&](auto* h) { return vkCreateRenderPass(s.dev, &rp, nullptr, h); })) {
            s.hudColorVkFailed = true; return false;
        }
    }
    if (!s.hudColorFramebuffer) {
        VkImageView att[] = { s.sharedImg.view.get() };
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = s.hudColorRenderPass.get();
        fb.attachmentCount = 1; fb.pAttachments = att;
        fb.width = s.imgW; fb.height = s.imgH; fb.layers = 1;
        if (!mc2vk::create(s.hudColorFramebuffer, s.dev, [&](auto* h) { return vkCreateFramebuffer(s.dev, &fb, nullptr, h); })) {
            s.hudColorVkFailed = true; return false;
        }
    }
    if (!s.hudColorLayout) {
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pc.size = sizeof(float) * 2;
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pc;
        if (!mc2vk::create(s.hudColorLayout, s.dev, [&](auto* h) { return vkCreatePipelineLayout(s.dev, &ci, nullptr, h); })) {
            s.hudColorVkFailed = true; return false;
        }
    }
    if (!s.hudColorVert.get() || !s.hudColorFrag.get()) {
        s.hudColorVert = loadSpv(s.dev, spv_dir() + "hud_color.vert.spv");
        s.hudColorFrag = loadSpv(s.dev, spv_dir() + "hud_color.frag.spv");
        if (!s.hudColorVert.get() || !s.hudColorFrag.get()) { s.hudColorVkFailed = true; return false; }
    }

    auto makePipe = [&](VkPrimitiveTopology topology, bool blend, VkBlendFactor srcColor, mc2vk::OwnedPipeline& out,
                        VkBlendFactor dstColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA) -> bool {
        if (out) return true;
        VkPipelineShaderStageCreateInfo st[2]{};
        st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = s.hudColorVert.get(); st[0].pName = "main";
        st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = s.hudColorFrag.get(); st[1].pName = "main";
        VkVertexInputBindingDescription bind{};
        bind.binding = 0; bind.stride = sizeof(HudColorGpuVertex); bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attr[2]{};
        attr[0].location = 0; attr[0].binding = 0; attr[0].format = VK_FORMAT_R32G32_SFLOAT; attr[0].offset = offsetof(HudColorGpuVertex, x);
        attr[1].location = 1; attr[1].binding = 0; attr[1].format = VK_FORMAT_R32_UINT; attr[1].offset = offsetof(HudColorGpuVertex, argb);
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 2; vin.pVertexAttributeDescriptions = attr;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = topology;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1; vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = blend ? VK_TRUE : VK_FALSE;
        cba.srcColorBlendFactor = srcColor;
        cba.dstColorBlendFactor = dstColor;
        // VFX-7: pure-additive (ONE,ONE) alpha channel adds too
        if (dstColor == VK_BLEND_FACTOR_ONE) { /* srcAlpha ONE below, dstAlpha follows color */ }
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = (dstColor == VK_BLEND_FACTOR_ONE)
            ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyns;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dyn; gp.layout = s.hudColorLayout.get();
        gp.renderPass = s.hudColorRenderPass.get();
        return mc2vk::create(out, s.dev, [&](auto* h) { return vkCreateGraphicsPipelines(s.dev, VK_NULL_HANDLE, 1, &gp, nullptr, h); });
    };

    if (!makePipe(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, VK_BLEND_FACTOR_SRC_ALPHA, s.hudColorTriPipe) ||
        !makePipe(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, true, VK_BLEND_FACTOR_SRC_ALPHA, s.hudColorLinePipe) ||
        !makePipe(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false, VK_BLEND_FACTOR_SRC_ALPHA, s.hudColorTriOpaquePipe) ||
        !makePipe(VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false, VK_BLEND_FACTOR_SRC_ALPHA, s.hudColorLineOpaquePipe) ||
        // VK-FIELD-VFX-3: additive (ONE,1-srcA) color-tri pipe for AlphaMode 4 untextured
        // effect ribbons (weapon beams whose sprite has no HUD mirror) -> GL OneInvAlpha parity.
        !makePipe(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, VK_BLEND_FACTOR_ONE, s.hudColorTriOneInvAlphaPipe) ||
        // VK-FIELD-VFX-7: pure-additive (ONE,ONE) color-tri pipe for AlphaMode 2 untextured
        !makePipe(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true, VK_BLEND_FACTOR_ONE, s.hudColorTriOneOnePipe,
                  VK_BLEND_FACTOR_ONE)) {
        s.hudColorVkFailed = true; return false;
    }
    mc2vk::nameObject(s.dev, s.hudColorTriPipe.get(), "present.hudColorTriPipe");
    mc2vk::nameObject(s.dev, s.hudColorLinePipe.get(), "present.hudColorLinePipe");
    mc2vk::nameObject(s.dev, s.hudColorTriOpaquePipe.get(), "present.hudColorTriOpaquePipe");
    mc2vk::nameObject(s.dev, s.hudColorLineOpaquePipe.get(), "present.hudColorLineOpaquePipe");
    mc2vk::nameObject(s.dev, s.hudColorTriOneInvAlphaPipe.get(), "present.hudColorTriOneInvAlphaPipe");
    mc2vk::nameObject(s.dev, s.hudColorTriOneOnePipe.get(), "present.hudColorTriOneOnePipe");
    return true;
}

bool ensureHudTexObjects(EnginePresentVk::Impl& s) {
    if (!ensureHudColorObjects(s))
        return false;
    if (s.hudTexPipe.get() && s.hudTexOpaquePipe.get() && s.hudTexOneInvAlphaPipe.get() && s.hudTexOneOnePipe.get())
        return true;
    if (!s.hudTexDsl) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1; ci.pBindings = &b;
        if (!mc2vk::create(s.hudTexDsl, s.dev, [&](auto* h) { return vkCreateDescriptorSetLayout(s.dev, &ci, nullptr, h); })) {
            s.hudColorVkFailed = true; return false;
        }
    }
    if (!s.hudTexPool) {
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        // FREE_DESCRIPTOR_SET_BIT: the (handle,serial)-keyed HUD mirror evicts stale
        // entries and frees their descriptor sets back to the pool (VK-HUD-GLYPH-
        // CORRUPTION FIX) -- without this flag vkFreeDescriptorSets is illegal and the
        // pool would exhaust as runtime string serials accumulate.
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = 512; pci.poolSizeCount = 1; pci.pPoolSizes = &size;
        if (!mc2vk::create(s.hudTexPool, s.dev, [&](auto* h) { return vkCreateDescriptorPool(s.dev, &pci, nullptr, h); })) {
            s.hudColorVkFailed = true; return false;
        }
    }
    if (!s.hudTexLayout) {
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.size = sizeof(HudTexPush);
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        const VkDescriptorSetLayout hudTexDslHandle = s.hudTexDsl.get();
        ci.setLayoutCount = 1; ci.pSetLayouts = &hudTexDslHandle;
        ci.pushConstantRangeCount = 1; ci.pPushConstantRanges = &pc;
        if (!mc2vk::create(s.hudTexLayout, s.dev, [&](auto* h) { return vkCreatePipelineLayout(s.dev, &ci, nullptr, h); })) {
            s.hudColorVkFailed = true; return false;
        }
    }
    s.hudTexVert = loadSpv(s.dev, spv_dir() + "hud_tex.vert.spv");
    s.hudTexFrag = loadSpv(s.dev, spv_dir() + "hud_tex.frag.spv");
    if (!s.hudTexVert.get() || !s.hudTexFrag.get()) { s.hudColorVkFailed = true; return false; }

    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = s.hudTexVert.get(); st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = s.hudTexFrag.get(); st[1].pName = "main";
    VkVertexInputBindingDescription bind{};
    bind.binding = 0; bind.stride = sizeof(HudTexGpuVertex); bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attr[3]{};
    attr[0].location = 0; attr[0].binding = 0; attr[0].format = VK_FORMAT_R32G32_SFLOAT; attr[0].offset = offsetof(HudTexGpuVertex, x);
    attr[1].location = 1; attr[1].binding = 0; attr[1].format = VK_FORMAT_R32_UINT; attr[1].offset = offsetof(HudTexGpuVertex, argb);
    attr[2].location = 2; attr[2].binding = 0; attr[2].format = VK_FORMAT_R32G32_SFLOAT; attr[2].offset = offsetof(HudTexGpuVertex, u);
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = attr;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    auto makeTexPipe = [&](bool blend, VkBlendFactor srcColor, mc2vk::OwnedPipeline& out,
                           VkBlendFactor dstColor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA) -> bool {
        if (out) return true;
        VkPipelineColorBlendAttachmentState cba{};
        cba.blendEnable = blend ? VK_TRUE : VK_FALSE;
        cba.srcColorBlendFactor = srcColor;
        cba.dstColorBlendFactor = dstColor;
        cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstAlphaBlendFactor = (dstColor == VK_BLEND_FACTOR_ONE)
            ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2; dyn.pDynamicStates = dyns;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = st;
        gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.pDynamicState = &dyn; gp.layout = s.hudTexLayout.get();
        gp.renderPass = s.hudColorRenderPass.get();
        return mc2vk::create(out, s.dev, [&](auto* h) { return vkCreateGraphicsPipelines(s.dev, VK_NULL_HANDLE, 1, &gp, nullptr, h); });
    };
    // hudTexPipe: standard alpha (SRC_ALPHA,1-srcA) for AlphaMode 3 (AlphaInvAlpha).
    // hudTexOpaquePipe: no blend for AlphaMode 1 (OneZero).
    // hudTexOneInvAlphaPipe: (ONE,1-srcA) matching GL's gos_Alpha_OneInvAlpha (AlphaMode 4),
    //   the effect-sprite additive-glow blend used by weapon beams/tracers/explosions.
    if (!makeTexPipe(true, VK_BLEND_FACTOR_SRC_ALPHA, s.hudTexPipe) ||
        !makeTexPipe(false, VK_BLEND_FACTOR_SRC_ALPHA, s.hudTexOpaquePipe) ||
        !makeTexPipe(true, VK_BLEND_FACTOR_ONE, s.hudTexOneInvAlphaPipe) ||
        // VK-FIELD-VFX-7: pure-additive (ONE,ONE) textured pipe for AlphaMode 2 —
        // GL gos_Alpha_OneOne, the muzzle-flash / tracer sprite blend (black
        // sprite background adds nothing instead of rendering an opaque box).
        !makeTexPipe(true, VK_BLEND_FACTOR_ONE, s.hudTexOneOnePipe, VK_BLEND_FACTOR_ONE)) {
        s.hudColorVkFailed = true; return false;
    }
    mc2vk::nameObject(s.dev, s.hudTexPipe.get(), "present.hudTexPipe");
    mc2vk::nameObject(s.dev, s.hudTexOpaquePipe.get(), "present.hudTexOpaquePipe");
    mc2vk::nameObject(s.dev, s.hudTexOneInvAlphaPipe.get(), "present.hudTexOneInvAlphaPipe");
    mc2vk::nameObject(s.dev, s.hudTexOneOnePipe.get(), "present.hudTexOneOnePipe");
    return true;
}

EnginePresentVk::Impl::HudTextureMirror* ensureHudTextureMirror(EnginePresentVk::Impl& s,
                                                                const VkHudDraw& d,
                                                                uint32_t* uploads) {
    if (d.textureHandle == 0 || d.textureWidth == 0 || d.textureHeight == 0 || d.textureSerial == 0)
        return nullptr;
    // VK-HUD-GLYPH-CORRUPTION FIX: key by (handle, serial) so a handle re-baked to a
    // new content serial within the frame gets a DISTINCT mirror instead of clobbering
    // the shared one that earlier draws already reference.
    const uint64_t key = ((uint64_t)d.textureHandle << 32) | (uint64_t)d.textureSerial;
    auto& m = s.hudTextureMirrors[key];
    m.lastFrame = s.hudMirrorFrameCounter;   // touched this frame -> not evictable yet
    const VkDeviceSize bytes = (VkDeviceSize)d.textureWidth * (VkDeviceSize)d.textureHeight * 4u;
    const bool needsUpload = m.serial != d.textureSerial || m.width != d.textureWidth ||
                             m.height != d.textureHeight || m.bytes != bytes || !m.buffer;
    if (!needsUpload)
        return &m;
    // VK-HUD-MIRRORCOPY-1: the draw carries a shared immutable mirror (refcount
    // bump at queue time); bytes are only dereferenced HERE, on a (handle,serial)
    // change — the >99.9% no-upload case never touches them.
    if (!d.textureRgba || d.textureRgba->size() < (size_t)bytes)
        return nullptr;
    if (!createHostBuffer(s, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m.buffer, m.memory, "present.hudTextureMirror"))
        return nullptr;
    const std::vector<uint8_t>& rgba = *d.textureRgba;
    std::vector<uint32_t> packed((size_t)d.textureWidth * (size_t)d.textureHeight);
    for (size_t i = 0; i < packed.size(); ++i) {
        const size_t b = i * 4u;
        packed[i] = (uint32_t)rgba[b] |
                    ((uint32_t)rgba[b + 1] << 8) |
                    ((uint32_t)rgba[b + 2] << 16) |
                    ((uint32_t)rgba[b + 3] << 24);
    }
    void* mapped = nullptr;
    if (vkMapMemory(s.dev, m.memory.get(), 0, bytes, 0, &mapped) != VK_SUCCESS)
        return nullptr;
    std::memcpy(mapped, packed.data(), (size_t)bytes);
    vkUnmapMemory(s.dev, m.memory.get());
    if (!m.set) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.hudTexPool.get();
        ai.descriptorSetCount = 1;
        const VkDescriptorSetLayout hudTexDslHandle = s.hudTexDsl.get();
        ai.pSetLayouts = &hudTexDslHandle;
        if (vkAllocateDescriptorSets(s.dev, &ai, &m.set) != VK_SUCCESS)
            return nullptr;
    }
    VkDescriptorBufferInfo bi{};
    bi.buffer = m.buffer.get();
    bi.offset = 0;
    bi.range = bytes;
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = m.set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr.pBufferInfo = &bi;
    vkUpdateDescriptorSets(s.dev, 1, &wr, 0, nullptr);
    m.serial = d.textureSerial;
    m.width = d.textureWidth;
    m.height = d.textureHeight;
    m.bytes = bytes;
    if (uploads) ++(*uploads);
    return &m;
}

bool ensureImguiRenderPass(EnginePresentVk::Impl& s) {
    if (s.imguiRenderPass.get()) return true;

    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;

    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    return mc2vk::create(s.imguiRenderPass, s.dev, [&](auto* h) { return vkCreateRenderPass(s.dev, &rp, nullptr, h); });
}

bool ensureImguiFramebuffer(EnginePresentVk::Impl& s) {
    if (s.imguiFramebuffer.get()) return true;
    if (!ensureImguiRenderPass(s) || !s.sharedImg.view || s.imgW == 0 || s.imgH == 0)
        return false;

    VkImageView attachments[] = { s.sharedImg.view.get() };
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = s.imguiRenderPass.get();
    fb.attachmentCount = 1;
    fb.pAttachments = attachments;
    fb.width = s.imgW;
    fb.height = s.imgH;
    fb.layers = 1;
    return mc2vk::create(s.imguiFramebuffer, s.dev, [&](auto* h) { return vkCreateFramebuffer(s.dev, &fb, nullptr, h); });
}

bool ensureImguiVulkan(EnginePresentVk::Impl& s) {
    if (s.imguiVkInited) return ensureImguiFramebuffer(s);
    if (s.imguiVkFailed || !s.dev || !s.phys || !s.gfxQ || !s.imgW || !s.imgH)
        return false;
    if (!ensureImguiFramebuffer(s)) {
        s.imguiVkFailed = true;
        return false;
    }

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = s.inst;
    info.PhysicalDevice = s.phys;
    info.Device = s.dev;
    info.QueueFamily = s.gfxFamily;
    info.Queue = s.gfxQ;
    info.RenderPass = s.imguiRenderPass.get();
    info.MinImageCount = 2;
    info.ImageCount = s.scImages.size() >= 2 ? (uint32_t)s.scImages.size() : 2;
    info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.DescriptorPoolSize = 16;
    if (!ImGui_ImplVulkan_Init(&info)) {
        s.imguiVkFailed = true;
        return false;
    }
    s.imguiVkInited = true;
    return true;
}

bool recordPendingImgui(EnginePresentVk::Impl& s, VkCommandBuffer cb) {
    ImDrawData* drawData = s.pendingImguiDrawData;
    s.pendingImguiDrawData = nullptr;
    if (!drawData || !s.imguiVkInited || !ensureImguiFramebuffer(s))
        return false;

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = s.imguiRenderPass.get();
    rp.framebuffer = s.imguiFramebuffer.get();
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {s.imgW, s.imgH};
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(drawData, cb);
    vkCmdEndRenderPass(cb);
    return true;
}

uint32_t recordPendingHudColorProof(EnginePresentVk::Impl& s, VkCommandBuffer cb,
                                    uint32_t* nativeDraws,
                                    uint32_t* texturedDraws,
                                    uint32_t* textureUploadsOut) {
    if (s.pendingHudDraws.empty())
        return 0;
    const uint32_t draws = (uint32_t)s.pendingHudDraws.size();
    if (nativeDraws) *nativeDraws = 0;
    if (texturedDraws) *texturedDraws = 0;
    if (textureUploadsOut) *textureUploadsOut = 0;
    if (!nativeHudColorProofEnabled() || !ensureHudColorObjects(s)) {
        s.pendingHudDraws.clear();
        return draws;
    }

    // VK-HUD-GLYPH-CORRUPTION FIX: advance the HUD frame counter and evict mirror
    // entries not referenced for a few frames (freeing their descriptor set + buffer
    // back to the pool). The window (>= 4 frames) keeps any entry a still-in-flight
    // present submit might read alive; runtime string serials otherwise accumulate
    // unbounded and would exhaust the 512-set pool.
    ++s.hudMirrorFrameCounter;
    {
        const uint64_t now = s.hudMirrorFrameCounter;
        for (auto it = s.hudTextureMirrors.begin(); it != s.hudTextureMirrors.end(); ) {
            if (it->second.lastFrame + 4 < now) {
                auto& m = it->second;
                if (m.set)    vkFreeDescriptorSets(s.dev, s.hudTexPool.get(), 1, &m.set);
                it = s.hudTextureMirrors.erase(it);   // the erase releases the buffer, then its memory
            } else {
                ++it;
            }
        }
    }

    std::vector<HudColorGpuVertex> triVerts;
    std::vector<HudColorGpuVertex> lineVerts;
    std::vector<HudTexGpuVertex> texVerts;
    struct HudNativeCmd {
        uint32_t stream = 0; // 0=tri color, 1=line color, 2=textured tri
        uint32_t first = 0;
        uint32_t count = 0;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t textMode = 0;
        uint32_t foreground = 0;
        bool opaque = false;
        bool oneInvAlpha = false; // VK-FIELD-VFX-3: AlphaMode 4 -> additive-glow tex pipe
        bool oneOne = false;      // VK-FIELD-VFX-7: AlphaMode 2 -> pure-additive (ONE,ONE) pipe
    };
    std::vector<HudNativeCmd> commands;
    uint32_t triDraws = 0;
    uint32_t lineDraws = 0;
    uint32_t texDraws = 0;
    uint32_t textureUploads = 0;
    for (const VkHudDraw& d : s.pendingHudDraws) {
        const bool hasTexture = d.textureHandle != 0 && d.textureHandle != 0xffffffffu;
        if (hasTexture) {
            if (!ensureHudTexObjects(s))
                continue;
            auto* mirror = ensureHudTextureMirror(s, d, &textureUploads);
            if (!mirror || !mirror->set)
                continue;
            HudNativeCmd td;
            td.stream = 2;
            td.first = (uint32_t)texVerts.size();
            td.set = mirror->set;
            td.width = mirror->width;
            td.height = mirror->height;
            td.textMode = (d.kind == 3) ? 1u : 0u;
            td.foreground = d.foregroundColor;
            td.opaque = (d.alphaMode == 1);
            td.oneInvAlpha = (d.alphaMode == 4); // VK-FIELD-VFX-3: additive-glow effect blend
            td.oneOne = (d.alphaMode == 2);      // VK-FIELD-VFX-7: pure-additive sprite blend
            auto addTex = [&](const VkHudVertex& v) {
                texVerts.push_back({v.x, v.y, v.argb, v.u, v.v});
            };
            // VK-HUD-GLYPH-CORRUPTION ROUND-4 (REAL ROOT CAUSE): the draw KIND, not a
            // vertex-count heuristic, determines the primitive layout. kHudTextQuadBatch
            // (kind==3) is ALREADY an expanded TRIANGLE LIST -- addCharacter() emits 6
            // vertices per glyph (tl,tr,bl, tr,br,bl). Only kHudQuadBatch (kind==0) is a
            // 4-vertex-per-quad list. The old test folded kind==3 into the quad branch
            // whenever (6*glyphs) % 4 == 0 -- i.e. for EVEN glyph counts -- so even-length
            // strings (LONGSHOT, OBJECTIVES, QUICK SAVE, RETURN TO GAME) were re-triangulated
            // as if 4-vertex quads and rendered scrambled, while odd-length strings (FLASH,
            // HACKSAW, OPTIONS, EXIT GAME) fell through to the correct triangle path and
            // stayed clean. Gate the quad expansion on kind==0 ONLY; text is a triangle list.
            if (d.kind == 0 && d.vertices.size() >= 4 && (d.vertices.size() % 4) == 0) {
                for (size_t i = 0; i + 3 < d.vertices.size(); i += 4) {
                    addTex(d.vertices[i + 0]); addTex(d.vertices[i + 1]); addTex(d.vertices[i + 2]);
                    addTex(d.vertices[i + 0]); addTex(d.vertices[i + 2]); addTex(d.vertices[i + 3]);
                }
            } else if (d.kind == 2 || d.kind == 3) {
                for (const VkHudVertex& v : d.vertices)
                    addTex(v);
            }
            td.count = (uint32_t)texVerts.size() - td.first;
            if (td.count != 0) {
                commands.push_back(td);
                ++texDraws;
            }
            continue;
        }
        if (d.kind != 0 && d.kind != 1 && d.kind != 2)
            continue;
        std::vector<HudColorGpuVertex>& dst = (d.kind == 1) ? lineVerts : triVerts;
        const size_t before = dst.size();
        if (d.kind == 0 && d.vertices.size() >= 4) {
            dst.reserve(dst.size() + (d.vertices.size() / 4u) * 6u);
            for (size_t i = 0; i + 3 < d.vertices.size(); i += 4) {
                const VkHudVertex& v0 = d.vertices[i + 0];
                const VkHudVertex& v1 = d.vertices[i + 1];
                const VkHudVertex& v2 = d.vertices[i + 2];
                const VkHudVertex& v3 = d.vertices[i + 3];
                dst.push_back({v0.x, v0.y, v0.argb});
                dst.push_back({v1.x, v1.y, v1.argb});
                dst.push_back({v2.x, v2.y, v2.argb});
                dst.push_back({v0.x, v0.y, v0.argb});
                dst.push_back({v2.x, v2.y, v2.argb});
                dst.push_back({v3.x, v3.y, v3.argb});
            }
        } else {
            dst.reserve(dst.size() + d.vertices.size());
            for (const VkHudVertex& v : d.vertices)
                dst.push_back({v.x, v.y, v.argb});
            // VK-FIELD-VFX-2 tail trace: log untextured tri-batch (kind==2) bbox+argb so we
            // can see the ACTUAL geometry the tail rasterizes (bars vs weapon beams).
            static const bool s_vfxTailTrace = mc2::gate::get<"MC2_VK_VFX_TRACE">();
            // VK-FIELD-VFX-2 debug: log the beam LINES specifically (kind==1 alpha==1 — the
            // beam-axis line fallback; brackets/bars are alpha==3) to confirm they reach the tail.
            if (s_vfxTailTrace && d.kind == 1 && d.alphaMode == 1 && !d.vertices.empty()) {
                static unsigned s_n = 0;
                if (s_n < 5000) {
                    ++s_n;
                    float xmn=1e9f,xmx=-1e9f,ymn=1e9f,ymx=-1e9f;
                    for (const VkHudVertex& v : d.vertices) {
                        xmn=v.x<xmn?v.x:xmn; xmx=v.x>xmx?v.x:xmx;
                        ymn=v.y<ymn?v.y:ymn; ymx=v.y>ymx?v.y:ymx;
                    }
                    mc2::fmt::print_err("[VK_VFX_TAIL] kind={:d} nverts={:d} argb0={:08X} canvas={:d}x{:d} bbox=[{:.1f},{:.1f}]-[{:.1f},{:.1f}] w={:.2f} h={:.2f} alpha={:d}\n", mc2::fmt::as_u32((unsigned)d.kind), mc2::fmt::as_size(d.vertices.size()), mc2::fmt::as_u32((unsigned)d.vertices[0].argb), mc2::fmt::as_u32(s.hudCanvasW), mc2::fmt::as_u32(s.hudCanvasH), xmn, ymn, xmx, ymx, xmx-xmn, ymx-ymn, mc2::fmt::as_u32(d.alphaMode));
                }
            }
        }
        if (dst.size() != before) {
            HudNativeCmd cmd;
            cmd.stream = (d.kind == 1) ? 1u : 0u;
            cmd.first = (uint32_t)before;
            cmd.count = (uint32_t)(dst.size() - before);
            cmd.opaque = (d.alphaMode == 1);
            // VK-FIELD-VFX-3: untextured tri effect ribbon (AlphaMode 4) -> additive pipe.
            // Only the tri stream has an additive variant; lines keep standard alpha.
            cmd.oneInvAlpha = (d.kind != 1) && (d.alphaMode == 4);
            cmd.oneOne = (d.kind != 1) && (d.alphaMode == 2);
            commands.push_back(cmd);
            (d.kind == 1) ? ++lineDraws : ++triDraws;
        }
    }

    const size_t triBytes = triVerts.size() * sizeof(HudColorGpuVertex);
    const size_t lineBytes = lineVerts.size() * sizeof(HudColorGpuVertex);
    const size_t texBytes = texVerts.size() * sizeof(HudTexGpuVertex);
    const VkDeviceSize colorBytes = (VkDeviceSize)(triBytes + lineBytes);
    if (colorBytes != 0 && !ensureHudColorVertexBuffer(s, colorBytes)) {
        s.pendingHudDraws.clear();
        return draws;
    }
    if (texBytes != 0 && !ensureHudTexVertexBuffer(s, (VkDeviceSize)texBytes)) {
        s.pendingHudDraws.clear();
        return draws;
    }
    if (colorBytes != 0) {
        void* mapped = nullptr;
        if (vkMapMemory(s.dev, s.hudColorVertexMem.get(), 0, colorBytes, 0, &mapped) != VK_SUCCESS) {
            s.pendingHudDraws.clear();
            return draws;
        }
        if (triBytes) std::memcpy(mapped, triVerts.data(), triBytes);
        if (lineBytes) std::memcpy((uint8_t*)mapped + triBytes, lineVerts.data(), lineBytes);
        vkUnmapMemory(s.dev, s.hudColorVertexMem.get());
    }
    if (texBytes != 0) {
        void* mapped = nullptr;
        if (vkMapMemory(s.dev, s.hudTexVertexMem.get(), 0, (VkDeviceSize)texBytes, 0, &mapped) != VK_SUCCESS) {
            s.pendingHudDraws.clear();
            return draws;
        }
        std::memcpy(mapped, texVerts.data(), texBytes);
        vkUnmapMemory(s.dev, s.hudTexVertexMem.get());
    }

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = s.hudColorRenderPass.get();
    rp.framebuffer = s.hudColorFramebuffer.get();
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {s.imgW, s.imgH};
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{};
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = (float)s.imgW; vp.height = (float)s.imgH;
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    VkRect2D sc{{0, 0}, {s.imgW, s.imgH}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    // HUD-CANVAS-EXTENT-FIX: divide HUD vertices by the canvas they were authored in
    // (logical game res), not the present image size (physical drawable). See hudCanvasW.
    const float hudExW = s.hudCanvasW ? (float)s.hudCanvasW : (float)s.imgW;
    const float hudExH = s.hudCanvasH ? (float)s.hudCanvasH : (float)s.imgH;
    const float extent[2] = {hudExW, hudExH};
    vkCmdPushConstants(cb, s.hudColorLayout.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(extent), extent);
    VkDeviceSize offset = 0;
    VkPipeline boundPipe = VK_NULL_HANDLE;
    VkBuffer boundBuffer = VK_NULL_HANDLE;
    for (const HudNativeCmd& td : commands) {
        if (td.stream == 0 || td.stream == 1) {
            const VkPipeline pipe = (td.stream == 0)
                ? (td.opaque ? s.hudColorTriOpaquePipe.get()
                             : (td.oneOne ? s.hudColorTriOneOnePipe.get()
                                          : (td.oneInvAlpha ? s.hudColorTriOneInvAlphaPipe.get() : s.hudColorTriPipe.get())))
                : (td.opaque ? s.hudColorLineOpaquePipe.get() : s.hudColorLinePipe.get());
            if (boundPipe != pipe) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                boundPipe = pipe;
            }
            if (boundBuffer != s.hudColorVertexBuf.get()) {
                offset = 0;
                { const VkBuffer vb = s.hudColorVertexBuf.get(); vkCmdBindVertexBuffers(cb, 0, 1, &vb, &offset); }
                boundBuffer = s.hudColorVertexBuf.get();
            }
            const uint32_t first = (td.stream == 1)
                ? (uint32_t)triVerts.size() + td.first
                : td.first;
            vkCmdDraw(cb, td.count, 1, first, 0);
        } else {
            const VkPipeline pipe = td.opaque ? s.hudTexOpaquePipe.get()
                                  : (td.oneOne ? s.hudTexOneOnePipe.get()
                                               : (td.oneInvAlpha ? s.hudTexOneInvAlphaPipe.get() : s.hudTexPipe.get()));
            if (boundPipe != pipe) {
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                boundPipe = pipe;
            }
            if (boundBuffer != s.hudTexVertexBuf.get()) {
                VkDeviceSize texOffset = 0;
                { const VkBuffer vb = s.hudTexVertexBuf.get(); vkCmdBindVertexBuffers(cb, 0, 1, &vb, &texOffset); }
                boundBuffer = s.hudTexVertexBuf.get();
            }
            HudTexPush pc{};
            pc.extent[0] = hudExW;   // HUD-CANVAS-EXTENT-FIX (see hudColor path above)
            pc.extent[1] = hudExH;
            pc.texW = td.width;
            pc.texH = td.height;
            pc.textMode = td.textMode;
            pc.foreground = td.foreground;
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.hudTexLayout.get(),
                                    0, 1, &td.set, 0, nullptr);
            vkCmdPushConstants(cb, s.hudTexLayout.get(),
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(pc), &pc);
            vkCmdDraw(cb, td.count, 1, td.first, 0);
        }
    }
    vkCmdEndRenderPass(cb);
    if (nativeDraws) *nativeDraws = triDraws + lineDraws + texDraws;
    if (texturedDraws) *texturedDraws = texDraws;
    if (textureUploadsOut) *textureUploadsOut = textureUploads;
    s.pendingHudDraws.clear();
    return draws;
}

// Build (or rebuild) the shared present image, sized to (w,h). Destroys any prior
// instance first.
//
// VK-RUNG8-ITEM8 (stage 2): the present image is VK-ONLY (non-exportable). The GL
// interop view + the copy-1 GL draw FBO are deleted -- the only producer is the
// native VK composite, which renders into the image's VK view (sharedImg.view).
// This removes the last exportable Win32 VkImageResource + glInteropViewCreate in
// the present tail (Metal-gate: item 2/1 lose their present-tail caller).
bool buildSharedImage(EnginePresentVk::Impl& s, uint32_t w, uint32_t h, std::string& reason) {
    // Tear down prior.
    destroyImguiFramebuffer(s);
    destroyHudColorFramebuffer(s);
    destroyCompositeFramebuffer(s);
    destroyShadowDebugFramebuffer(s);
    if (s.sharedImg.image) vkImageResourceDestroy(&s.sharedImg, s.dev);

    // RGBA8. COLOR_ATTACHMENT so the native VK composite/HUD/ImGui render into it;
    // TRANSFER_SRC so VK can blit it into the swapchain; TRANSFER_DST for the
    // initial acquire barrier bookkeeping / robustness. Non-exportable: no GL side.
    if (!vkImageResourceCreate(&s.sharedImg, s.dev, s.phys, w, h,
                               VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                               VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        reason = "vkImageResourceCreate(VK-only RGBA8) failed";
        return false;
    }
    mc2vk::nameObject(s.dev, s.sharedImg.image.get(), "present.sharedImage");
    s.imgW = w; s.imgH = h;
    static bool loggedOnce = false;
    if (!loggedOnce) {
        loggedOnce = true;
        plog("present image is a VK-only allocation (no GL interop view, item-8).");
    }
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
    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    const VkResult sr = pfnCreateWin32Surface(s.inst, &wsci, nullptr, &rawSurface);
    if (sr == VK_SUCCESS) s.surface = mc2vk::OwnedSurface(s.inst, rawSurface);
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
        if (!mc2vk::create(s.cpool, s.dev, [&](auto* h) { return vkCreateCommandPool(s.dev, &pci, nullptr, h); }))
            return fail("vkCreateCommandPool failed");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = s.cpool.get();
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(s.dev, &cai, &s.cbuf) != VK_SUCCESS)
            return fail("vkAllocateCommandBuffers failed");
        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!mc2vk::create(s.acquireSem, s.dev, [&](auto* h) { return vkCreateSemaphore(s.dev, &semci, nullptr, h); }))
            return fail("vkCreateSemaphore(acquire) failed");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!mc2vk::create(s.inFlight, s.dev, [&](auto* h) { return vkCreateFence(s.dev, &fci, nullptr, h); }))
            return fail("vkCreateFence(inFlight) failed");
        // VK-PRESENT-SINGLESUBMIT-1: the composite's own command buffer (recorded at
        // composite time, submitted together with the tail cbuf in ONE submit).
        VkCommandBufferAllocateInfo cai2{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai2.commandPool = s.cpool.get();
        cai2.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai2.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(s.dev, &cai2, &s.compositeCbuf) != VK_SUCCESS)
            return fail("vkAllocateCommandBuffers(composite) failed");
    }

    // ---- shared exportable interop image + GL view + copy-1 FBO -----------
    {
        std::string r;
        if (!buildSharedImage(s, w, h, r)) return fail(r);
    }

    created_ = true;
    health_.created = true;
    emitDeprecatedPresentGateNotes();
    if (s.ctx) health_.validation_errors = s.ctx->health().validationErrors;
    plog("present path UP: extent=%ux%u images=%zu mode=%s.",
         s.scExtent.width, s.scExtent.height, s.scImages.size(),
         presentModeName(s.presentMode));
    return true;
}

// VK-RUNG8-ITEM8: the GL copy FBO is deleted, so this always returns 0 (endScene
// never GL-renders into the present image). Kept as a stable no-op so endScene's
// call site and the direct_fbo_requests counter are unchanged; the live arming is
// presentDirectVkOnly() below.
unsigned int EnginePresentVk::presentDirectFbo() {
    ++health_.direct_fbo_requests;
    // VK-RUNG8-ITEM8 (stage 2): the GL copy FBO wrapping the present image is
    // deleted. There is no GL FBO to hand endScene any more -- the present image
    // is written only by the native VK composite (via presentDirectVkOnly). This
    // always returns 0 so endScene never GL-renders into the present image.
    directFbo_ = 0;
    presentImageVkOwned_ = false;
    return 0;
}

bool EnginePresentVk::presentDirectVkOnly() {
    // Arms the VK-only present image for this frame: endScene skips its own GL
    // composite bind and the native VK composite (renderNativeCompositeVisual)
    // renders the present image instead. directFbo_ is a sentinel (1u == "native
    // present armed"), not a GL FBO name.
    if (!directFboGateEnabled() || !nativeCompositeGateEnabled() ||
        !created_ || !impl_ || impl_->sharedImg.image.get() == VK_NULL_HANDLE) {
        directFbo_ = 0;
        presentImageVkOwned_ = false;
        return false;
    }
    directFbo_ = 1u;
    presentImageVkOwned_ = false;
    ++health_.direct_fbo_grants;
    health_.last_direct_fbo = directFbo_;
    return true;
}

bool EnginePresentVk::renderNativeCompositeVisual(float exposure,
                                                  bool fxaaEnabled,
                                                  float fxaaSubpix,
                                                  float fxaaEdgeThreshold,
                                                  float fxaaEdgeThresholdMin,
                                                  int viewMode,
                                                  int engineIdxBase,
                                                  float lowLightGain,
                                                  const float lowLightTint[3],
                                                  bool casEnabled, float casSharpness, int tonemapMode,
                                                  int viewportX, int viewportY,
                                                  int viewportW, int viewportH) {
    if (!nativeCompositeGateEnabled() || !created_ || !impl_ || directFbo_ == 0)
        return false;
    Impl& s = *impl_;
    if (viewportW <= 0 || viewportH <= 0 ||
        viewportX < 0 || viewportY < 0 ||
        viewportX + viewportW > (int)s.imgW ||
        viewportY + viewportH > (int)s.imgH)
        return false;
    const bool sceneDirectChain = compositeDirectChainGateEnabled();
    const bool objectIdDirectChainCandidate = objectIdDirectChainGateEnabled();
    // VK-RUNG8-ITEM5: the shared scene is VK-only (no GL interop view). The native
    // composite therefore requires the shared scene already be Vulkan-owned by the
    // upstream islands (direct-chain). If the composite/objectId direct-chain gate
    // is explicitly disabled there is no GL-view rescue, so decline to GL composite.
    if (!sceneDirectChain)
        return false;
    VkImageResource* scene = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    const bool needsObjectId = (viewMode == 1 || viewMode == 3);
    VkImageResource* objectId = needsObjectId
        ? mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_OBJECT_ID)
        : nullptr;
    const bool objectIdDirectChain = needsObjectId && objectIdDirectChainCandidate;
    if (needsObjectId && (!objectId || !objectIdDirectChain))
        return false;
    if (!scene)
        return false;
    if (!ensureCompositeObjects(s))
        return false;
    // The composite's descriptor set always binds the object-id view when that image exists, so the image has to be in the sampled layout for every draw
    // that uses the set, whether or not this view mode reads it.
    VkImageResource* boundObjectId = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_OBJECT_ID);
    if (boundObjectId && !boundObjectId->view) boundObjectId = nullptr;

    // Direct-chain: scene (+objectId) already Vulkan-owned by upstream islands, so
    // no per-image frame-begin and no GL<->VK handoff. The fences remain unused.
    VkFrameSync fsScene{};
    VkFrameSync fsObjectId{};

    // VK-PRESENT-SINGLESUBMIT-1: the ONLY CPU wait on the present path. Retire the
    // PREVIOUS frame's single submit (composite+tail+blit) before either of its two
    // command buffers is reset for re-recording. The fence is NOT reset here — it is
    // reset immediately before the submit that will signal it (presentGLFrame), so a
    // mid-frame present decline (DISARM-2b) can never leave it unsignaled and hang
    // this wait. Timer kept from VK-QWI-TO-FENCE-1 (same [QWI-WAIT] receipt tag):
    // it now measures the previous-frame retire wait instead of an inline drain.
    {
        ZoneScopedN("VKIsl.Present.CompositeWait");   // VK-ISLAND-TRACY-1: retire prev frame's single submit (the ONLY present-path CPU wait)
        using clk = std::chrono::steady_clock;
        const clk::time_point tWait0 = clk::now();
        const VkFence inFlightFence = s.inFlight.get();
        if (vkWaitForFences(s.dev, 1, &inFlightFence, VK_TRUE,
                            5ull * 1000 * 1000 * 1000) != VK_SUCCESS)
            return false;
        health_.composite_wait_us_last =
            std::chrono::duration<double, std::micro>(clk::now() - tWait0).count();
        health_.composite_wait_us_accum += health_.composite_wait_us_last;
        ++health_.composite_wait_samples;
        if ((health_.composite_wait_samples % 600) == 1)
            plog("[QWI-WAIT] composite wait_us_last=%.1f wait_us_avg=%.1f samples=%u",
                 health_.composite_wait_us_last,
                 health_.composite_wait_us_accum / (double)health_.composite_wait_samples,
                 health_.composite_wait_samples);
    }

    // Record into the composite's OWN cbuf; the submit happens in presentGLFrame as
    // half of the single {compositeCbuf, cbuf} submission.
    ZoneNamedN(zCompRec, "VKIsl.Present.CompositeRecord", true);   // VK-ISLAND-TRACY-1 (to fn return)
    VkCommandBuffer cc = s.compositeCbuf;
    s.compositePending = false;
    vkResetCommandBuffer(cc, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cc, &bi) != VK_SUCCESS)
        return false;

    barrier(cc, scene->image.get(),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (boundObjectId) {
        barrier(cc, boundObjectId->image.get(),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = s.compositeRenderPass.get();
    rp.framebuffer = s.compositeFramebuffer.get();
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {s.imgW, s.imgH};
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cc, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{(float)viewportX, (float)viewportY,
                  (float)viewportW, (float)viewportH, 0.0f, 1.0f};
    VkRect2D sc{{viewportX, viewportY}, {(uint32_t)viewportW, (uint32_t)viewportH}};
    vkCmdSetViewport(cc, 0, 1, &vp);
    vkCmdSetScissor(cc, 0, 1, &sc);
    vkCmdBindPipeline(cc, VK_PIPELINE_BIND_POINT_GRAPHICS, s.compositePipe.get());
    vkCmdBindDescriptorSets(cc, VK_PIPELINE_BIND_POINT_GRAPHICS, s.compositeLayout.get(),
                            0, 1, &s.compositeSet, 0, nullptr);
    float pc[16] = {
        exposure,
        1.0f / static_cast<float>(s.imgW),
        1.0f / static_cast<float>(s.imgH),
        fxaaEnabled ? 1.0f : 0.0f,
        fxaaSubpix,
        fxaaEdgeThreshold,
        fxaaEdgeThresholdMin,
        casEnabled ? 1.0f : 0.0f,
        static_cast<float>(viewMode),
        lowLightGain,
        lowLightTint ? lowLightTint[0] : 0.7f,
        lowLightTint ? lowLightTint[1] : 1.0f,
        lowLightTint ? lowLightTint[2] : 0.6f,
        static_cast<float>(engineIdxBase),
        casSharpness,
        static_cast<float>(tonemapMode)
    };
    vkCmdPushConstants(cc, s.compositeLayout.get(), VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), pc);
    vkCmdDraw(cc, 3, 1, 0, 0);
    vkCmdEndRenderPass(cc);

    barrier(cc, scene->image.get(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    if (boundObjectId) {
        barrier(cc, boundObjectId->image.get(),
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

    if (vkEndCommandBuffer(cc) != VK_SUCCESS)
        return false;
    (void)fsScene; (void)fsObjectId;

    // VK-PRESENT-SINGLESUBMIT-1: NO submit and NO drain here. The recorded cbuf is
    // submitted by presentGLFrame together with the tail (HUD/ImGui + swapchain
    // blit) in ONE vkQueueSubmit; GPU-side composite-before-tail ordering comes from
    // command-buffer order within the submit + the tail's entry barriers.
    s.compositePending = true;
    ++health_.composite_vk_frames;
    presentImageVkOwned_ = true;
    return true;
}

bool EnginePresentVk::renderNativeCsmShadowDebugOverlay(void* csmArrayView, int layer) {
    if (!created_ || !impl_ || directFbo_ == 0 || !nativeCompositeGateEnabled())
        return false;
    Impl& s = *impl_;
    VkImageView arrayView = reinterpret_cast<VkImageView>(csmArrayView);
    if (!ensureShadowDebugOverlayObjects(s, arrayView))
        return false;

    // VK-PRESENT-SINGLESUBMIT-1: the overlay used to be its OWN submit here (and it
    // reset s.cbuf mid-frame and left s.inFlight.get() unsignaled after its wait — a latent
    // hang against the present tail's fence wait). It is now only STASHED; the
    // present tail records it into the single {compositeCbuf, cbuf} submission,
    // after the composite (correct paint order — with the composite no longer
    // executed at composite time, an own-submit overlay would run BEFORE the
    // composite on the GPU and be overwritten by it).
    s.pendingShadowDebugView = arrayView;
    s.pendingShadowDebugLayer = layer < 0 ? 0 : layer;
    return true;
}

bool EnginePresentVk::prepareImguiVulkanFrame() {
    if (!created_ || !impl_ || !presentTailHookEnabled())
        return false;
    Impl& s = *impl_;
    if (!ensureImguiVulkan(s))
        return false;
    health_.imgui_vk_ready = true;
    ImGui_ImplVulkan_NewFrame();
    return true;
}

bool EnginePresentVk::renderImguiVulkanDrawData(ImDrawData* drawData) {
    if (!created_ || !impl_ || !presentTailHookEnabled() || !drawData)
        return false;
    Impl& s = *impl_;
    if (!ensureImguiVulkan(s))
        return false;
    health_.imgui_vk_ready = true;
    s.pendingImguiDrawData = drawData;
    return true;
}

bool EnginePresentVk::queueHudVulkanFrame(std::vector<VkHudDraw>&& draws,
                                          uint32_t canvasW, uint32_t canvasH) {
    if (!created_ || !impl_ || !presentTailHookEnabled() || draws.empty())
        return false;
    Impl& s = *impl_;
    s.pendingHudDraws = std::move(draws);
    s.hudCanvasW = canvasW;   // HUD-CANVAS-EXTENT-FIX: authoring canvas (logical res)
    s.hudCanvasH = canvasH;
    health_.hud_vk_draws_last = (uint32_t)s.pendingHudDraws.size();
    health_.hud_vk_textured_last = 0;
    health_.hud_vk_serialized_last = 0;
    for (const VkHudDraw& d : s.pendingHudDraws) {
        if (d.textureHandle != 0 && d.glTextureId != 0 && d.textureWidth != 0 && d.textureHeight != 0) {
            ++health_.hud_vk_textured_last;
            if (d.textureSerial != 0)
                ++health_.hud_vk_serialized_last;
        }
    }
    ++health_.hud_vk_queued_frames;
    return true;
}

bool EnginePresentVk::imguiVulkanReady() const {
    return created_ && impl_ && impl_->imguiVkInited && !impl_->imguiVkFailed;
}

void EnginePresentVk::shutdownImguiVulkanRenderer() {
    if (!impl_) return;
    Impl& s = *impl_;
    if (s.dev) s.ctx->waitIdle("present_imgui_shutdown");
    if (s.imguiVkInited) {
        // VK-IMGUI-SHUTDOWN-GUARD-1: this can be reached twice at teardown —
        // once from GuiRuntime::Shutdown() (which then calls ImGui::DestroyContext())
        // and once from EnginePresentVk::destroy(). It can also be reached with
        // imguiVkInited still true after a present-init failure that left the ImGui
        // renderer backend torn down. ImGui_ImplVulkan_Shutdown()/DestroyDeviceObjects
        // dereference the backend data unconditionally, so once the context or its
        // BackendRendererUserData is gone the call faults with a null read at
        // offset 0x88 (VulkanInitInfo). Mirror ImGui_ImplVulkan_GetBackendData()'s
        // own null contract here so the call is idempotent regardless of ordering.
        if (ImGui::GetCurrentContext() != nullptr &&
            ImGui::GetIO().BackendRendererUserData != nullptr) {
            ImGui_ImplVulkan_Shutdown();
        }
        s.imguiVkInited = false;
    }
    s.pendingImguiDrawData = nullptr;
    s.pendingHudDraws.clear();
    health_.imgui_vk_ready = false;
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
    ZoneScopedN("VKIsl.Present.Frame");   // VK-ISLAND-TRACY-1: presentGLFrame total (CPU)
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
        const bool hadDirectFbo = (directFbo_ != 0);
        // VK-PRESENT-SINGLESUBMIT-1: (a) retire any in-flight prior submit before
        // buildSharedImage destroys the image it may still reference (the old
        // composite-time queue drain used to provide this incidentally; the wait is
        // free when nothing is pending — fence stays SIGNALED, no reset). (b) DROP
        // the pending composite recording: it references the about-to-be-destroyed
        // shared image / framebuffers and must never be submitted.
        { const VkFence inFlightFence = s.inFlight.get(); vkWaitForFences(s.dev, 1, &inFlightFence, VK_TRUE, 5ull * 1000 * 1000 * 1000); }
        s.compositePending = false;
        std::string r;
        if (!buildSharedImage(s, glDrawableW, glDrawableH, r)) {
            health_.fallback_reason = "resize: " + r; return false;
        }
        if (!rebuildSwapchain(s, glDrawableW, glDrawableH, health_, r)) {
            health_.fallback_reason = "resize: " + r; return false;
        }
        ++health_.resize_count;
        health_.extent_w = s.scExtent.width;
        health_.extent_h = s.scExtent.height;
        if (hadDirectFbo) {
            directFbo_ = 0;
            presentImageVkOwned_ = false;
            return true;
        }
    }

    // ==== VK-RUNG8-ITEM8: present interop tail deleted ======================
    // The present image is VK-only. Its sole producer is the native VK composite
    // (renderNativeCompositeVisual, which RECORDS this frame's composite into
    // s.compositeCbuf — VK-PRESENT-SINGLESUBMIT-1: submitted below together with
    // the tail) plus the native HUD/ImGui recorded into the tail hook below. There
    // is NO GL copy-1 blit and NO GL->VK semaphore handoff any more: a GL-scene /
    // VK-present hybrid can no longer exist (the mirror of item-5's VK-scene /
    // GL-present deletion). If VK did not produce the present image this frame
    // there is no GL rescue inside VK present -- copy-1 is gone.
    if (!presentImageVkOwned_ || !s.compositePending) {
        // The sole surviving present strict-canary trigger.
        presentNativeStrictAbort("present_image_not_vk_owned");
        // Native present is armed but the VK composite did not produce the image
        // this frame. Tolerate a bounded warmup/hiccup streak (skip + retry, no
        // stale present, no GL drop); once the streak exceeds the budget this
        // config plainly cannot produce a VK present image (native composite
        // permanently declined, or the gates only look armed) -> fail-soft the
        // whole session to GL. copy-1 no longer rescues a non-VK-owned frame.
        static const unsigned int kNotOwnedBudget = 240u;   // ~a few seconds @60fps
        const bool armed = directFboGateEnabled() && nativeCompositeGateEnabled();
        if (armed && notOwnedStreak_ < kNotOwnedBudget) {
            ++notOwnedStreak_;
            return true;   // transient: retry next frame
        }
        // On Windows GL exists; on Metal (no GL) this decline becomes product
        // behaviour per the METAL-GATE-AUDIT-2 fail-soft redefinition.
        health_.fallback_reason =
            "present image not VK-owned (native VK composite required post item-8)";
        return false;
    }
    notOwnedStreak_ = 0;   // VK produced the present image this frame
    // copy-1 and the GL->VK handoff are structurally gone; the counters remain as
    // checker trip-wires (copy1_blit stays 0; copy1_skip / gl_handoff_skip count
    // every presented frame -- the H-check invariant present==skip==gl_handoff_skip
    // and copy1_blit==0 is preserved without a live GL path).
    ++health_.copy1_skip_frames;
    ++health_.gl_handoff_skip_frames;
    if (health_.gl_handoff_skip_frames == 1) {
        plog("present image is VK-only; copy-1 + GL->VK handoff deleted (item-8).");
    }

    // ==== COPY-2 (VK): blit shared image -> swapchain + present =============
    // VK-PRESENT-SINGLESUBMIT-1: the previous frame's single submit was already
    // fence-waited at composite-record time (before either cbuf was reset), so
    // there is NO CPU wait here. s.inFlight.get() stays SIGNALED until immediately
    // before the submit below — any early return on this path (acquire
    // OUT_OF_DATE, decline) leaves it signaled, so the next composite's wait can
    // never hang and the pending composite is simply re-recorded next frame.
    uint32_t imageIndex = 0;
    VkResult r;
    { ZoneScopedN("VKIsl.Present.Acquire");   // VK-ISLAND-TRACY-1: vkAcquireNextImageKHR (can block on WSI)
      r = vkAcquireNextImageKHR(s.dev, s.swapchain.get(), UINT64_MAX, s.acquireSem.get(), VK_NULL_HANDLE, &imageIndex); }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        // OS-driven resize/alt-tab invalidated the swapchain. Rebuild to the GL
        // game window's current drawable. If it's zero (minimized), skip the
        // rebuild -- retry next frame, no crash. (No fence re-signal needed any
        // more: the fence was not reset on this path.)
        int dw = 0, dh = 0; SDL_GL_GetDrawableSize(glWindow_, &dw, &dh);
        if (dw > 0 && dh > 0) {
            std::string rr;
            if (!rebuildSwapchain(s, (uint32_t)dw, (uint32_t)dh, health_, rr)) {
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

    const bool tailHook = presentTailHookEnabled();
    VkImage scImg = s.scImages[imageIndex];
    ZoneNamedN(zTailRec, "VKIsl.Present.TailRecord", true);   // VK-ISLAND-TRACY-1: overlay/HUD/ImGui + blit record (to fn return)
    vkResetCommandBuffer(s.cbuf, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.cbuf, &bi);

    // VK-PRESENT-SINGLESUBMIT-1: order the composite's color writes (executed as
    // the FIRST command buffer of this same submit) before any tail attachment
    // write. Needed by the overlay/HUD/ImGui render passes; the blit path is
    // covered by its own COLOR_ATTACHMENT->TRANSFER_SRC barrier below.
    const bool tailWrites = tailHook || (s.pendingShadowDebugView != VK_NULL_HANDLE);
    if (tailWrites) {
        barrier(s.cbuf, s.sharedImg.image.get(),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

    // VK-PRESENT-SINGLESUBMIT-1: stashed CSM shadow-debug overlay (opt-in debug) —
    // recorded here so it lands AFTER the composite and BEFORE HUD/ImGui, matching
    // its pre-single-submit paint order.
    if (s.pendingShadowDebugView != VK_NULL_HANDLE && s.shadowDebugRenderPass.get() &&
        s.shadowDebugFramebuffer.get() && s.shadowDebugPipe.get()) {
        VkRenderPassBeginInfo srp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        srp.renderPass = s.shadowDebugRenderPass.get();
        srp.framebuffer = s.shadowDebugFramebuffer.get();
        srp.renderArea.offset = {0, 0};
        srp.renderArea.extent = {s.imgW, s.imgH};
        vkCmdBeginRenderPass(s.cbuf, &srp, VK_SUBPASS_CONTENTS_INLINE);
        const float quadSize = 256.0f;
        const float margin = 16.0f;
        VkViewport svp{margin, (float)s.imgH - quadSize - margin, quadSize, quadSize, 0.0f, 1.0f};
        VkRect2D ssc{{(int32_t)margin, (int32_t)((float)s.imgH - quadSize - margin)},
                     {(uint32_t)quadSize, (uint32_t)quadSize}};
        vkCmdSetViewport(s.cbuf, 0, 1, &svp);
        vkCmdSetScissor(s.cbuf, 0, 1, &ssc);
        vkCmdBindPipeline(s.cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s.shadowDebugPipe.get());
        vkCmdBindDescriptorSets(s.cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, s.shadowDebugLayout.get(),
                                0, 1, &s.shadowDebugSet, 0, nullptr);
        int spc[4] = { s.pendingShadowDebugLayer, 0, 0, 0 };
        vkCmdPushConstants(s.cbuf, s.shadowDebugLayout.get(), VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(spc), spc);
        vkCmdDraw(s.cbuf, 3, 1, 0, 0);
        vkCmdEndRenderPass(s.cbuf);
        ++health_.shadow_debug_vk_frames;
    }
    s.pendingShadowDebugView = VK_NULL_HANDLE;

    // VK-TAIL-HOOK-1: final native-tail seam. The composite left the shared image
    // in COLOR_ATTACHMENT layout; native HUD/ImGui commands attach here before the
    // present blit consumes the final image.
    if (tailHook) {
        ++health_.tail_hook_frames;
        uint32_t hudColorDraws = 0;
        uint32_t hudTexturedDraws = 0;
        uint32_t hudTextureUploads = 0;
        if (uint32_t hudDraws = recordPendingHudColorProof(s, s.cbuf, &hudColorDraws,
                                                           &hudTexturedDraws,
                                                           &hudTextureUploads)) {
            health_.hud_vk_draws_last = hudDraws;
            ++health_.hud_vk_consumed_frames;
            health_.hud_vk_color_draws_last = hudColorDraws;
            if (hudColorDraws != 0)
                ++health_.hud_vk_color_frames;
            health_.hud_vk_textured_draws_last = hudTexturedDraws;
            if (hudTexturedDraws != 0)
                ++health_.hud_vk_textured_frames;
            health_.hud_vk_texture_uploads += hudTextureUploads;
        }
        if (recordPendingImgui(s, s.cbuf))
            ++health_.imgui_vk_frames;
    }

    // shared image: GL left it in COLOR_ATTACHMENT layout -> TRANSFER_SRC for blit.
    barrier(s.cbuf, s.sharedImg.image.get(),
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
    // VK-RUNG8-ITEM8 Y-flip: the present image is always VK-produced with GL
    // bottom-left origin (the composite renders it un-flipped), so copy-2 always
    // does the flip that copy-1 used to do: swap the dst Y offsets (0,H)->(H,0)
    // so the top-left-origin swapchain image is upright.
    region.dstOffsets[0] = {0, (int32_t)s.scExtent.height, 0};
    region.dstOffsets[1] = {(int32_t)s.scExtent.width, 0, 1};
    vkCmdBlitImage(s.cbuf,
                   s.sharedImg.image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
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
    barrier(s.cbuf, s.sharedImg.image.get(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    vkEndCommandBuffer(s.cbuf);

    // VK-PRESENT-SINGLESUBMIT-1: THE single present-path submit — composite cbuf
    // first, tail cbuf (overlay + HUD/ImGui + swapchain blit) second, executed in
    // command-buffer order; the tail's entry barriers order the composite's writes
    // for the tail passes and the blit. Waits ONLY the swapchain acquire semaphore
    // (dst stage COLOR_ATTACHMENT_OUTPUT — GPU-side this also gates the composite's
    // color writes, which is correct and merely schedules them after the acquire;
    // no CPU wait is involved). Signals the per-image render semaphore for present
    // and the inFlight fence, which the NEXT frame's composite-record waits. The
    // fence is reset HERE, immediately before the submit that will signal it —
    // never on a decline path (DISARM-2b hang guard).
    VkSemaphore imgRenderSem = s.renderSems[imageIndex].get();
    VkSemaphore waitSem      = s.acquireSem.get();
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkCommandBuffer submitCbufs[2] = { s.compositeCbuf, s.cbuf };
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &waitSem;
    si.pWaitDstStageMask    = &waitStage;
    si.commandBufferCount   = 2;
    si.pCommandBuffers      = submitCbufs;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &imgRenderSem;
    { const VkFence inFlightFence = s.inFlight.get(); vkResetFences(s.dev, 1, &inFlightFence); }
    s.compositePending = false;   // consumed by this submit (success or fail-soft)
    { ZoneScopedN("VKIsl.Present.Submit");   // VK-ISLAND-TRACY-1: THE single present-path vkQueueSubmit
      r = vkQueueSubmit(s.gfxQ, 1, &si, s.inFlight.get()); }  // drives acquire/present cadence
    if (r != VK_SUCCESS) {
        // The fence is now unsignaled with no submit to signal it; returning false
        // triggers the DISARM-2 present-failure path (vkpres.destroy() + deferred
        // retry), which destroys and re-creates the fence SIGNALED — no hang.
        health_.fallback_reason = "vkQueueSubmit failed (" + std::to_string((int)r) + ")";
        plog("%s. fail-soft.", health_.fallback_reason.c_str());
        return false;
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &imgRenderSem;
    pi.swapchainCount     = 1;
    const VkSwapchainKHR swapchainHandle = s.swapchain.get();
    pi.pSwapchains        = &swapchainHandle;
    pi.pImageIndices      = &imageIndex;
    { ZoneScopedN("VKIsl.Present.QueuePresent");   // VK-ISLAND-TRACY-1: vkQueuePresentKHR (vsync/WSI block lands here)
      r = vkQueuePresentKHR(s.presentQ, &pi); }
    if (r == VK_SUCCESS) {
        ++health_.present_frames;
    } else if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        ++health_.present_frames;
        int dw = 0, dh = 0; SDL_GL_GetDrawableSize(glWindow_, &dw, &dh);
        if (dw > 0 && dh > 0) {
            std::string rr;
            if (!rebuildSwapchain(s, (uint32_t)dw, (uint32_t)dh, health_, rr)) {
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
    // Proves the VK composite populated the shared image with REAL, CHANGING content.
    // Cheap + throttled: only when requested (T5 window), fully synchronous.
    if (verifyReadback) {
        verifyReadbackContent();
    }

    if (s.ctx) health_.validation_errors = s.ctx->health().validationErrors;
    presentImageVkOwned_ = false;
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
        s.readbackBuf.reset();   // the buffer goes before the memory bound to it
        s.readbackMem.reset();
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = needed;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!mc2vk::create(s.readbackBuf, s.dev, [&](auto* h) { return vkCreateBuffer(s.dev, &bci, nullptr, h); })) return;
        mc2vk::nameObject(s.dev, s.readbackBuf.get(), "present.readback");
        VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(s.dev, s.readbackBuf.get(), &mr);
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
        if (!mc2vk::create(s.readbackMem, s.dev, [&](auto* h) { return vkAllocateMemory(s.dev, &mai, nullptr, h); })) return;
        vkBindBufferMemory(s.dev, s.readbackBuf.get(), s.readbackMem.get(), 0);
        s.readbackSize = needed;
    }
    if (!s.readbackCbuf) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = s.cpool.get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(s.dev, &cai, &s.readbackCbuf) != VK_SUCCESS) return;
    }
    if (!s.readbackFence) {
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!mc2vk::create(s.readbackFence, s.dev, [&](auto* h) { return vkCreateFence(s.dev, &fci, nullptr, h); })) return;
    }

    vkResetCommandBuffer(s.readbackCbuf, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.readbackCbuf, &bi);
    // shared image is in COLOR_ATTACHMENT (post-present barrier) -> TRANSFER_SRC.
    barrier(s.readbackCbuf, s.sharedImg.image.get(),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bic{};
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {rw, rh, 1};
    vkCmdCopyImageToBuffer(s.readbackCbuf, s.sharedImg.image.get(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.readbackBuf.get(), 1, &bic);
    barrier(s.readbackCbuf, s.sharedImg.image.get(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    vkEndCommandBuffer(s.readbackCbuf);

    const VkFence readbackFenceHandle = s.readbackFence.get();
    vkResetFences(s.dev, 1, &readbackFenceHandle);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &s.readbackCbuf;
    if (vkQueueSubmit(s.gfxQ, 1, &si, s.readbackFence.get()) != VK_SUCCESS) return;
    vkWaitForFences(s.dev, 1, &readbackFenceHandle, VK_TRUE, UINT64_MAX);

    void* mapped = nullptr;
    if (vkMapMemory(s.dev, s.readbackMem.get(), 0, needed, 0, &mapped) != VK_SUCCESS) return;
    const unsigned char* px = (const unsigned char*)mapped;
    // FNV-1a over a strided sample (every 257th byte) -- cheap, sensitive to change.
    unsigned long long h = 1469598103934665603ull;
    for (VkDeviceSize i = 0; i < needed; i += 257) {
        h ^= px[i]; h *= 1099511628211ull;
    }
    // VK-FIELD-ORACLE-1: optional one-shot dump of the FULL composited present image
    // (s.sharedImg == the RGBA8 backbuffer VK blits to the swapchain) to a PPM P6 file.
    // A headless, non-disruptive VK backbuffer oracle -- the OS/PrintWindow grabs of a
    // Vulkan swapchain return only the clear colour, and the GL scene-FBO capture is
    // empty under shared-GBuffer (VK owns the scene). Gated by MC2_VK_PRESENT_DUMP_PPM
    // (path); writes once. sharedImg is R8G8B8A8_UNORM; we emit RGB (drop A).
    {
        // Overwrite each verify tick so the saved PPM is the LATEST captured frame
        // (the HUD/force-bar is only fully up a few hundred frames in; a one-shot at
        // the first verify catches a bare early-boot HUD).
        static bool s_ppmDone = false; (void)s_ppmDone;
        const std::string ppmPathText = mc2::gate::str<"MC2_VK_PRESENT_DUMP_PPM">();   // "" when unset or empty
        const char* ppmPath = ppmPathText.empty() ? nullptr : ppmPathText.c_str();
        // VK-FIELD-VFX-3: optional frame pin. MC2_VK_PRESENT_DUMP_FRAME=N dumps at the
        // FIRST verify tick with g_mc2FrameCounter >= N, then stops overwriting -- so a
        // capture can be pinned to an early fire-active frame (continuous force-fire
        // overheats/depletes the mechs, so the default overwrite-latest catches a
        // post-fire frame). Unset -> legacy overwrite-each-verify-tick behaviour.
        {
            extern uint32_t g_mc2FrameCounter;
            extern uint32_t g_mc2FxFireCaptureFrame;
            if (mc2::gate::isNonEmpty<"MC2_VK_PRESENT_DUMP_FRAME">()) {
                const unsigned long pin = static_cast<unsigned long>(mc2::gate::get<"MC2_VK_PRESENT_DUMP_FRAME">());
                if (s_ppmDone || (unsigned long)g_mc2FrameCounter < pin)
                    ppmPath = nullptr; // not yet at the pinned frame, or already captured
            } else if (mc2::gate::get<"MC2_VK_PRESENT_DUMP_ON_FIRE">()) {
                // VK-FIELD-VFX-5: dynamic fire-event pin. The force-fire fixture
                // (code/warrior.cpp) arms g_mc2FxFireCaptureFrame ~0.5s after its
                // first volley, so the capture always lands mid-volley -- no
                // pin-frame roulette against the mission's intro/scenario clock.
                // Unarmed (0) = suppress; once armed, capture at the first verify
                // tick >= the armed frame.
                if (s_ppmDone || g_mc2FxFireCaptureFrame == 0 ||
                    g_mc2FrameCounter < g_mc2FxFireCaptureFrame)
                    ppmPath = nullptr;
            }
        }
        if (ppmPath) {
            FILE* pf = fopen(ppmPath, "wb");
            if (pf) {
                mc2::fmt::print(pf, "P6\n{:d} {:d}\n255\n", mc2::fmt::as_u32(rw), mc2::fmt::as_u32(rh));
                std::vector<unsigned char> rgb((size_t)rw * rh * 3u);
                for (size_t i = 0, o = 0; i < (size_t)rw * rh; ++i) {
                    rgb[o++] = px[i * 4 + 0];
                    rgb[o++] = px[i * 4 + 1];
                    rgb[o++] = px[i * 4 + 2];
                }
                fwrite(rgb.data(), 1, rgb.size(), pf);
                fclose(pf);
                s_ppmDone = true;
                mc2::fmt::print_err("[VK_PRESENT_DUMP_PPM] wrote {} ({:d}x{:d})\n", mc2::fmt::safe(ppmPath), mc2::fmt::as_u32(rw), mc2::fmt::as_u32(rh));
                fflush(stderr);
            }
        }
    }
    // VK-VISUAL-CAPTURE-1: hand the presented image to a pending visual capture.
    if (gos::visual_capture::vkCapturePending())
        gos::visual_capture::vkCaptureDeliver(rw, rh, px);
    vkUnmapMemory(s.dev, s.readbackMem.get());

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
        if (s.dev) s.ctx->waitIdle("present_destroy");
        shutdownImguiVulkanRenderer();
        // VK-PRESENT-SINGLESUBMIT-1: compositeCbuf is pool-allocated (freed with the pool); just drop the pending state.
        s.compositeCbuf = VK_NULL_HANDLE;
        s.compositePending = false;
        s.pendingShadowDebugView = VK_NULL_HANDLE;
        // Every owned object (pipelines, layouts, buffers and their memory, the swapchain, the surface, the command pool) is released by ~Impl, in
        // reverse declaration order. The device and instance belong to the engine context and are never destroyed here.
        delete impl_;
        impl_ = nullptr;
    }
    // P13b: glWindow_ is BORROWED (g_sdl_window, owned by gos_render). Never
    // destroy it here -- just drop our reference.
    glWindow_ = nullptr;
    created_ = false;
}

void EnginePresentVk::logHealth() const {
    mc2::fmt::print_err("[VK_PRESENT] created={:d} attempted={:d} present_frames={:d} resize_count={:d} resize_recreate_ok={:d} resize_recreate_retries={:d} resize_recreate_failsoft={:d} validation_errors={:d} extent={:d}x{:d} present_mode={} readback_changed={:d} last_checksum=0x{:x} direct_fbo_requests={:d} direct_fbo_grants={:d} copy1_blit_frames={:d} copy1_skip_frames={:d} gl_handoff_skip_frames={:d} tail_hook_frames={:d} composite_vk_frames={:d} shadow_debug_vk_frames={:d} hud_vk_queued_frames={:d} hud_vk_consumed_frames={:d} hud_vk_draws_last={:d} hud_vk_textured_last={:d} hud_vk_serialized_last={:d} hud_vk_color_frames={:d} hud_vk_color_draws_last={:d} hud_vk_textured_frames={:d} hud_vk_textured_draws_last={:d} hud_vk_texture_uploads={:d} imgui_vk_ready={:d} imgui_vk_frames={:d} last_direct_fbo={:d} composite_wait_us_last={:.1f} composite_wait_us_avg={:.1f} composite_wait_samples={:d} fallback_reason=\"{}\"\n",
        mc2::fmt::as_i32(health_.created ? 1 : 0), mc2::fmt::as_i32(health_.attempted ? 1 : 0),
        mc2::fmt::as_u32(health_.present_frames), mc2::fmt::as_u32(health_.resize_count),
        mc2::fmt::as_u32(health_.resize_recreate_ok), mc2::fmt::as_u32(health_.resize_recreate_retries),
        mc2::fmt::as_u32(health_.resize_recreate_failsoft), mc2::fmt::as_u32(health_.validation_errors),
        mc2::fmt::as_u32(health_.extent_w), mc2::fmt::as_u32(health_.extent_h),
        health_.present_mode_name.empty() ? "?" : health_.present_mode_name.c_str(),
        mc2::fmt::as_i32(health_.readback_changed ? 1 : 0),
        mc2::fmt::as_u64((unsigned long long)health_.last_readback_checksum),
        mc2::fmt::as_u32(health_.direct_fbo_requests), mc2::fmt::as_u32(health_.direct_fbo_grants),
        mc2::fmt::as_u32(health_.copy1_blit_frames), mc2::fmt::as_u32(health_.copy1_skip_frames),
        mc2::fmt::as_u32(health_.gl_handoff_skip_frames),
        mc2::fmt::as_u32(health_.tail_hook_frames),
        mc2::fmt::as_u32(health_.composite_vk_frames), mc2::fmt::as_u32(health_.shadow_debug_vk_frames),
        mc2::fmt::as_u32(health_.hud_vk_queued_frames), mc2::fmt::as_u32(health_.hud_vk_consumed_frames),
        mc2::fmt::as_u32(health_.hud_vk_draws_last), mc2::fmt::as_u32(health_.hud_vk_textured_last),
        mc2::fmt::as_u32(health_.hud_vk_serialized_last),
        mc2::fmt::as_u32(health_.hud_vk_color_frames), mc2::fmt::as_u32(health_.hud_vk_color_draws_last),
        mc2::fmt::as_u32(health_.hud_vk_textured_frames), mc2::fmt::as_u32(health_.hud_vk_textured_draws_last),
        mc2::fmt::as_u32(health_.hud_vk_texture_uploads),
        mc2::fmt::as_i32(health_.imgui_vk_ready ? 1 : 0), mc2::fmt::as_u32(health_.imgui_vk_frames),
        mc2::fmt::as_u32(health_.last_direct_fbo),
        health_.composite_wait_us_last,
        health_.composite_wait_samples
            ? health_.composite_wait_us_accum / (double)health_.composite_wait_samples : 0.0,
        mc2::fmt::as_u32(health_.composite_wait_samples),
        health_.fallback_reason.empty() ? "" : health_.fallback_reason.c_str());
    std::fflush(stderr);
}

EnginePresentVk& getOrCreateEnginePresentVk() {
    static EnginePresentVk g_enginePresent;
    return g_enginePresent;
}

}  // namespace mc2vk

#endif  // MC2_VULKAN && MC2_VULKAN_ISLAND
