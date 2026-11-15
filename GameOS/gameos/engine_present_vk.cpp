#include "../../RenderCore/TextureService.h"
#include <algorithm>
#include "engine_present_vk.h"
#include "hud_command_merge.h"
#include "hud_texture_mips.h"
#include "present_thread.h"
#include "../../mclib/battle_bench_wait.h"
#include "../../mclib/sim_overlap.h"
#include "visual_tuning_profile.h"
#include "Gates/gate.h"
#include "gos_visual_capture.h"   // VK-VISUAL-CAPTURE-1
#include "gos_screenshot.h"
#include "Gates/gate_cold.h"   // gate::get<>() and str<>()
#include "mc2fmt.h"
#include "mc2_hitch_trace.h"   // frame-fence wait attribution for the hitch trace
#include "hitch_stats.h"
#include "vk_island_util.h"
#include "vk_texture_residency.h"
#include "vk_one_shot.h"
#include "texture_decode.h"   // gos_GetTextureTexelsByGlName
// Define Win32 surface entrypoints before volk.h (vulkan_context.cpp).
#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include "vulkan_context.h"
#include "vk_gpu_profile.h"   // VK-GPU-ZONES: MC2_VK_GPU_SCOPE
#include "mc2_diag_verbose.h"
#include "vk_image_resource.h"
#include "vulkan_spatial_aa.h"
#include "engine_present_vk_internal.h"   // EnginePresentVk::Impl, fillLoadPassDeps, the ImGui piece (engine_present_vk_imgui.cpp)
#include "Gates/render_options.h"
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"
#include "vulkan_mech_island.h"   // mc2_vk_mech_island_object_motion_written
#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"
#include "vk_shared_gbuffer_gate.h"
#include "imgui.h"
#include "present_imgui_snapshot.h"
#include "imgui_impl_vulkan.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#ifdef _WIN32
#include <SDL2/SDL_syswm.h>   // P13b: SDL_GetWindowWMInfo -> HWND / HINSTANCE
#endif
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>   // VK-QWI-TO-FENCE-1: composite wait telemetry
#include <tracy/Tracy.hpp>   // VK-ISLAND-TRACY-1: present-path CPU attribution zones
#include <memory>
#include <utility>
#include <unordered_map>
#include <vector>
extern uint32_t g_mc2RenderFrame;
extern uint32_t g_mc2FxFireCaptureFrame;
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

// struct EnginePresentVk::Impl: engine_present_vk_internal.h (shared with engine_present_vk_imgui.cpp).

namespace {

bool waitCompositeSlot(EnginePresentVk::Impl& s, EnginePresentHealth& health) {
    ZoneScopedN("VKIsl.Present.CompositeWait");
    mc2::battlebench::NsScope benchWait(mc2::battlebench::PresCompositeWaitNs, 1);
    const auto start = std::chrono::steady_clock::now();
    const VkFence fence = s.curInFlight().get();
    if (fence == VK_NULL_HANDLE) return false;   // a refused submit whose replacement fence failed to create
    const VkResult result = vkWaitForFences(s.dev, 1, &fence, VK_TRUE, 5ull * 1000 * 1000 * 1000);
    health.composite_wait_us_last = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    if (!present_thread::onWorker()) mc2_hitch::RecordFenceWait(health.composite_wait_us_last / 1000.0);
    // The slot ledger and the slot-wait split count the composite fence on either thread (the present worker is the default).
    mc2_hitch2::noteWait(mc2_hitch2::kSlot, "present_composite", health.composite_wait_us_last / 1000.0);
    mc2_hitch2::stats().compositeWaitNs.fetch_add(static_cast<uint64_t>(health.composite_wait_us_last * 1000.0), std::memory_order_relaxed);
    if (result != VK_SUCCESS) return false;
    health.composite_wait_us_accum += health.composite_wait_us_last;
    ++health.composite_wait_samples;
    if ((health.composite_wait_samples % 600) == 1)
        plog("[QWI-WAIT] composite wait_us_last=%.1f wait_us_avg=%.1f samples=%u",
             health.composite_wait_us_last, health.composite_wait_us_accum / health.composite_wait_samples, health.composite_wait_samples);
    return true;
}

bool objectIdDirectChainGateEnabled() {
    static const bool on = []() {
        return false;
    }();
    return on;
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

std::string spv_dir() { return vk_island_util::spvDir(); }

mc2vk::OwnedShaderModule loadSpv(VkDevice device, const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { plog("cannot open spv '%s'.", path.c_str()); return {}; }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0 || (len % 4) != 0) { std::fclose(f); return {}; }
    std::vector<uint32_t> code(static_cast<size_t>(len) / 4);
    size_t got = std::fread(code.data(), 1, static_cast<size_t>(len), f);
    std::fclose(f);
    if (got != static_cast<size_t>(len)) return {};
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = static_cast<size_t>(len);
    ci.pCode = code.data();
    mc2vk::OwnedShaderModule mod;
    mc2vk::create(mod, device, [&](auto* h) { return vkCreateShaderModule(device, &ci, nullptr, h); });
    if (mod) mc2vk::infra::shader(mod.get(), path, code);
    return mod;
}

// -------- swapchain (re)build against the SHARED device -------------------
bool chooseQueuesAndPresentMode(EnginePresentVk::Impl& s, std::string& reason) {
    if (s.offscreen) {   // no usable surface: every surface query is skipped
        s.presentFamily = s.gfxFamily;
        s.presentQ = s.gfxQ;
        s.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        return true;
    }
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
    // Same family: the render-finished binary semaphore orders the two queues without ownership transfers.
    s.presentQ = s.ctx->presentQueue();

    // Present mode mapping (plan §6/R8): FIFO when MC2_VSYNC=1 (vsync-equivalent,
    // matches the GL swap-interval), else MAILBOX if available else IMMEDIATE
    // (preserve today's uncapped GL default). FIFO is always supported.
    uint32_t pmCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(s.phys, s.surface.get(), &pmCount, nullptr);
    std::vector<VkPresentModeKHR> pms(pmCount);
    if (pmCount) vkGetPhysicalDeviceSurfacePresentModesKHR(s.phys, s.surface.get(), &pmCount, pms.data());
    bool haveMailbox = false, haveImmediate = false, haveRelaxed = false;
    for (auto m : pms) {
        if (m == VK_PRESENT_MODE_FIFO_RELAXED_KHR) haveRelaxed = true;
        if (m == VK_PRESENT_MODE_MAILBOX_KHR)   haveMailbox = true;
        if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) haveImmediate = true;
    }
    const bool vsyncOn = mc2::gate::get<"MC2_VSYNC">();
    if (vsyncOn) {
        // FIFO_RELAXED (AMD guide): a late frame presents at once instead of waiting a whole extra vblank; FIFO when the surface lacks it.
        s.presentMode = haveRelaxed ? VK_PRESENT_MODE_FIFO_RELAXED_KHR : VK_PRESENT_MODE_FIFO_KHR;
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
    if (s.offscreen) {   // one placeholder image index, never touched: the tail skips the blit and the present
        s.scFormat = VK_FORMAT_R8G8B8A8_UNORM;
        s.scExtent = {w, h};
        s.scImages.assign(1, VK_NULL_HANDLE);
        return true;
    }
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
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
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
    // WSI allocations are opaque. Charge a conservative image bound, including tiling/alignment.
    const uint32_t chargedImages=caps.maxImageCount ? std::min(caps.maxImageCount,imgCount+2) : imgCount+2;
    const uint64_t imageBytes=((static_cast<uint64_t>(extent.width)*extent.height*8+65535)/65536)*65536;
    mc2vk::infra::Admission admission(imageBytes*chargedImages,"targets.swapchain");
    if(!admission) { reason="swapchain residency refused; skip present"; return false; }
    const VkResult createResult = mc2vk::createChecked(s.swapchain, s.dev, [&](auto* h) { return vkCreateSwapchainKHR(s.dev, &sci, nullptr, h); });
    if (createResult != VK_SUCCESS) {
        reason = "vkCreateSwapchainKHR failed (VkResult=" + std::to_string(static_cast<int>(createResult)) + ")";
        return false;
    }
    admission.commit(reinterpret_cast<uint64_t>(s.swapchain.get()),"targets.swapchain",true);
    s.scFormat = chosen.format;
    s.scExtent = extent;
    s.scTransform = caps.currentTransform;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(s.dev, s.swapchain.get(), &n, nullptr);
    if(n>chargedImages) {
        std::fprintf(stderr,"[VK_RESIDENCY v1] refused=swapchain images=%u admitted=%u fallback=skip_present\n",n,chargedImages);
        reason="swapchain exceeds admitted image count"; s.swapchain.reset(); return false;
    }
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
         n, extent.width, extent.height, static_cast<int>(chosen.format), presentModeName(s.presentMode));
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
    if (s.offscreen) { s.scExtent = {w, h}; return true; }
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
             VkPipelineStageFlags srcS, VkPipelineStageFlags dstS, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = oldL; b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
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
    float x, y, rhw, depth;
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
void destroyCompositeFramebuffer(EnginePresentVk::Impl& s) {
    s.scaledCompositeFramebuffer.reset();
    if (s.scaledComposite.image) vkImageResourceDestroy(&s.scaledComposite, s.dev);
    s.compositeFramebuffer.reset();
}
void destroyShadowDebugFramebuffer(EnginePresentVk::Impl& s) { s.shadowDebugFramebuffer.reset(); }

bool ensureCompositeObjects(EnginePresentVk::Impl& s) {
    if (!s.ctx) return false;
    if (s.compositePipe.get() && s.compositeFramebuffer.get()) return true;
    if (s.compositeVkFailed || !s.dev || !s.sharedImg.view || s.imgW == 0 || s.imgH == 0)
        return false;

    VkImageResource* scene = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_COLOR0);
    if (!scene || !scene->view) return false;

    // AA reads display-encoded color, at scene resolution, before reconstruction.
    if ((scene->w < s.imgW || scene->h < s.imgH) && !s.scaledComposite.image) {
        if (!vkImageResourceCreate(&s.scaledComposite, s.dev, s.phys, scene->w, scene->h,
                VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |   // STORAGE: CMAA2 (spatial AA mode 3) writes its output as a storage image
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                "present.renderResolutionColor")) return false;
    }

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
    if (s.scaledComposite.image && !s.scaledCompositeFramebuffer) {
        VkImageView att = s.scaledComposite.view.get();
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = s.compositeRenderPass.get();
        fb.attachmentCount = 1; fb.pAttachments = &att;
        fb.width = scene->w; fb.height = scene->h; fb.layers = 1;
        if (!mc2vk::create(s.scaledCompositeFramebuffer, s.dev,
            [&](auto* h) { return vkCreateFramebuffer(s.dev, &fb, nullptr, h); })) return false;
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
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 3; pci.poolSizeCount = 1; pci.pPoolSizes = &size;
        if (!mc2vk::create(s.compositePool, s.dev, [&](auto* h) { return vkCreateDescriptorPool(s.dev, &pci, nullptr, h); })) {
            s.compositeVkFailed = true; return false;
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.compositePool.get();
        ai.descriptorSetCount = 1;
        const VkDescriptorSetLayout compositeDslHandle = s.compositeDsl.get();
        ai.pSetLayouts = &compositeDslHandle;
        if (MC2_VK_ALLOC_SETS(s.dev, &ai, &s.compositeSet) != VK_SUCCESS) {
            s.compositeVkFailed = true; return false;
        }
        if (MC2_VK_ALLOC_SETS(s.dev, &ai, &s.upscaleSet) != VK_SUCCESS) {
            s.compositeVkFailed = true; return false;
        }
        if (MC2_VK_ALLOC_SETS(s.dev, &ai, &s.upscalerSet) != VK_SUCCESS) {
            s.compositeVkFailed = true; return false;
        }
        s.upscalerSetView = VK_NULL_HANDLE;
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
    if (s.scaledComposite.image) {
        ii[0].imageView = s.scaledComposite.view.get();
        for (auto& write : wr) write.dstSet = s.upscaleSet;
        vkUpdateDescriptorSets(s.dev, 2, wr, 0, nullptr);
    }
    plog("[RENDER_SIZES] scene=%ux%u aa=%ux%u composite_hud_imgui=%ux%u",
         scene->w, scene->h, scene->w, scene->h, s.imgW, s.imgH);

    if (!s.compositeLayout) {
        VkPushConstantRange pc{};
        pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pc.size = sizeof(float) * 32;
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
        if (!mc2vk::create(s.compositePipe, s.dev, [&](auto* h) { return mc2vk::infra::graphics(s.dev, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
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
                // Color-attachment WAW/RAW against the previous pass on the shared present image (LOAD_OP_LOAD, no layout transition, so no implicit edge).
        VkSubpassDependency deps[2]; fillLoadPassDeps(deps);
VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &color;
        rp.subpassCount = 1; rp.pSubpasses = &sub;
        rp.dependencyCount = 2; rp.pDependencies = deps;
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
        if (MC2_VK_ALLOC_SETS(s.dev, &ai, &s.shadowDebugSet) != VK_SUCCESS) {
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
        if (!mc2vk::create(s.shadowDebugPipe, s.dev, [&](auto* h) { return mc2vk::infra::graphics(s.dev, VK_NULL_HANDLE, 1, &gp, nullptr, h); })) {
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
    mc2vk::infra::Admission admission(req.size,name);
    if(!admission) return false;
    if (!mc2vk::create(outMem, s.dev, [&](auto* h) { return vkAllocateMemory(s.dev, &ai, nullptr, h); }))
        return false;
    admission.commit(reinterpret_cast<uint64_t>(outMem.get()), name, true);
    return vkBindBufferMemory(s.dev, outBuf.get(), outMem.get(), 0) == VK_SUCCESS;
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
                // Color-attachment WAW/RAW against the previous pass on the shared present image (LOAD_OP_LOAD, no layout transition, so no implicit edge).
        VkSubpassDependency deps[2]; fillLoadPassDeps(deps);
VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &color;
        rp.subpassCount = 1; rp.pSubpasses = &sub;
        rp.dependencyCount = 2; rp.pDependencies = deps;
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
        return mc2vk::create(out, s.dev, [&](auto* h) { return mc2vk::infra::graphics(s.dev, VK_NULL_HANDLE, 1, &gp, nullptr, h); });
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
        VkDescriptorSetLayoutBinding b[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            b[i].binding = i;
            b[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 3; ci.pBindings = b;
        if (!mc2vk::create(s.hudTexDsl, s.dev, [&](auto* h) { return vkCreateDescriptorSetLayout(s.dev, &ci, nullptr, h); })) {
            s.hudColorVkFailed = true; return false;
        }
    }
    if (!s.hudTexPool) {
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        // FREE_DESCRIPTOR_SET_BIT: the (handle,serial)-keyed HUD mirror evicts stale
        // entries and frees their descriptor sets back to the pool (VK-HUD-GLYPH-
        // CORRUPTION FIX) -- without this flag vkFreeDescriptorSets is illegal and the
        // pool would exhaust as runtime string serials accumulate.
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = 512; pci.poolSizeCount = 2; pci.pPoolSizes = sizes;
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
    attr[0].location = 0; attr[0].binding = 0; attr[0].format = VK_FORMAT_R32G32B32A32_SFLOAT; attr[0].offset = offsetof(HudTexGpuVertex, x);
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
        return mc2vk::create(out, s.dev, [&](auto* h) { return mc2vk::infra::graphics(s.dev, VK_NULL_HANDLE, 1, &gp, nullptr, h); });
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

constexpr uint64_t kHudMirrorIdleFrames = 4;   // retention of an unreferenced mirror (cache policy, not a safety window)

// An evicted HUD mirror: the descriptor set returns to the HUD pool, then the buffer and its memory go (members release in reverse order).
struct HudMirrorRetired {
    VkDevice dev = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    mc2vk::OwnedDeviceMemory memory;
    mc2vk::OwnedRawBuffer buffer;
    HudMirrorRetired(VkDevice d, VkDescriptorPool p, VkDescriptorSet st, mc2vk::OwnedDeviceMemory&& mem, mc2vk::OwnedRawBuffer&& buf)
        : dev(d), pool(p), set(st), memory(std::move(mem)), buffer(std::move(buf)) {}
    HudMirrorRetired(HudMirrorRetired&& o) noexcept
        : dev(o.dev), pool(o.pool), set(std::exchange(o.set, VK_NULL_HANDLE)), memory(std::move(o.memory)), buffer(std::move(o.buffer)) {}
    HudMirrorRetired(const HudMirrorRetired&) = delete;
    ~HudMirrorRetired() { if (set) vkFreeDescriptorSets(dev, pool, 1, &set); }
};

EnginePresentVk::Impl::HudTextureMirror* ensureHudTextureMirror(EnginePresentVk::Impl& s,
                                                                const VkHudDraw& d,
                                                                uint32_t* uploads) {
    if (d.textureHandle == 0 || d.textureWidth == 0 || d.textureHeight == 0 || d.textureSerial == 0)
        return nullptr;
    // VK-HUD-GLYPH-CORRUPTION FIX: key by (handle, serial) so a handle re-baked to a
    // new content serial within the frame gets a DISTINCT mirror instead of clobbering
    // the shared one that earlier draws already reference.
    const uint64_t key = (static_cast<uint64_t>(d.textureHandle) << 32) | static_cast<uint64_t>(d.textureSerial);
    auto& m = s.hudTextureMirrors[key];
    m.lastFrame = s.hudMirrorFrameCounter;   // touched this frame -> not evictable yet
    const bool mipmapped = d.textureWidth == 2048 && d.textureHeight == 2048;
    auto* normal = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    auto* depth = mc2SharedSceneDepthGetResource();
    if (!normal || !depth) return nullptr;
    const auto sceneGeneration = mc2SharedSceneColorGeneration();
    if (s.hudSceneGeneration != sceneGeneration) {
        VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ci.image = depth->image.get(); ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.format = depth->format;
        ci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (!mc2vk::create(s.hudDepthView, s.dev, [&](auto* h) { return vkCreateImageView(s.dev, &ci, nullptr, h); })) return nullptr;
        s.hudSceneGeneration = sceneGeneration;
    }
    const size_t baseBytes = static_cast<size_t>(d.textureWidth) * d.textureHeight * 4u;
    const VkDeviceSize bytes = mipmapped ? (baseBytes * 4u - 4u) / 3u : baseBytes;
    const bool needsUpload = m.serial != d.textureSerial || m.width != d.textureWidth ||
                             m.height != d.textureHeight || m.bytes != bytes || !m.buffer || m.sceneGeneration != sceneGeneration;
    if (!needsUpload)
        return &m;
    // VK-HUD-MIRRORCOPY-1: the draw carries a shared immutable mirror (refcount
    // bump at queue time); bytes are only dereferenced HERE, on a (handle,serial)
    // change — the >99.9% no-upload case never touches them.
    if (!d.textureRgba || d.textureRgba->size() < baseBytes)
        return nullptr;
    ZoneScopedN("HUD.TextureUpload");
    ZoneValue(d.textureHandle);
    char textureInfo[128];
    const int textureInfoSize = std::snprintf(textureInfo, sizeof(textureInfo), "handle=%u serial=%u size=%ux%u kind=%u", d.textureHandle, d.textureSerial, d.textureWidth, d.textureHeight, d.kind);
    if (textureInfoSize > 0) ZoneText(textureInfo, static_cast<size_t>(textureInfoSize));
    { ZoneScopedN("HUD.TextureUpload.Allocate");
    if (!createHostBuffer(s, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, m.buffer, m.memory, "present.hudTextureMirror"))
        return nullptr;
    }
    const std::vector<uint8_t>& rgba = *d.textureRgba;
    std::vector<uint8_t> mipPixels;
    if (mipmapped) {
        ZoneScopedN("HUD.TextureUpload.Mip2048.Cpu");
        mipPixels = mc2hud::premultipliedMips(rgba, d.textureWidth);
    }
    { ZoneScopedN("HUD.TextureUpload.MapAndCopy");
    void* mapped = nullptr;
    if (vkMapMemory(s.dev, m.memory.get(), 0, bytes, 0, &mapped) != VK_SUCCESS)
        return nullptr;
    std::memcpy(mapped, mipmapped ? mipPixels.data() : rgba.data(), static_cast<size_t>(bytes));
    vkUnmapMemory(s.dev, m.memory.get());
    }
    if (!m.set) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = s.hudTexPool.get();
        ai.descriptorSetCount = 1;
        const VkDescriptorSetLayout hudTexDslHandle = s.hudTexDsl.get();
        ai.pSetLayouts = &hudTexDslHandle;
        if (MC2_VK_ALLOC_SETS(s.dev, &ai, &m.set) != VK_SUCCESS)
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
    VkDescriptorImageInfo images[2] = {
        {s.compositeNearestSampler.get(), normal->view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {s.compositeNearestSampler.get(), s.hudDepthView.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    for (uint32_t i = 0; i < 2; ++i) {
        wr.dstBinding = i + 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pBufferInfo = nullptr; wr.pImageInfo = &images[i];
        vkUpdateDescriptorSets(s.dev, 1, &wr, 0, nullptr);
    }
    m.sceneGeneration = sceneGeneration;
    m.serial = d.textureSerial;
    m.width = d.textureWidth;
    m.height = d.textureHeight;
    m.bytes = bytes;
    if (uploads) ++(*uploads);
    return &m;
}

// ImGui backend + per-texture descriptor bridge: engine_present_vk_imgui.cpp (ensureImguiVulkan, imguiTexReleaseAll, recordPendingImgui).

uint32_t recordPendingHudColorProof(EnginePresentVk::Impl& s, VkCommandBuffer cb,
                                    uint32_t* nativeDraws,
                                    uint32_t* texturedDraws,
                                    uint32_t* textureUploadsOut,
                                    std::vector<VkPipeline>* recordedPipelines = nullptr) {
    if (s.pendingHudDraws.empty())
        return 0;
    const uint32_t draws = static_cast<uint32_t>(s.pendingHudDraws.size());
    if (nativeDraws) *nativeDraws = 0;
    if (texturedDraws) *texturedDraws = 0;
    if (textureUploadsOut) *textureUploadsOut = 0;
    { ZoneScopedN("VKIsl.Present.Tail.Hud.ObjectsAndMirrorRetirement");
    if (!ensureHudColorObjects(s)) {
        s.pendingHudDraws.clear();
        return draws;
    }

    // VK-HUD-GLYPH-CORRUPTION FIX: advance the HUD frame counter and evict mirror
    // entries not referenced for a few frames (their descriptor set + buffer go back
    // to the pool through the deferred-destroy queue). The idle window is a cache
    // policy only; runtime string serials otherwise accumulate unbounded and would
    // exhaust the 512-set pool.
    ++s.hudMirrorFrameCounter;
    {
        const uint64_t now = s.hudMirrorFrameCounter;
        for (auto it = s.hudTextureMirrors.begin(); it != s.hudTextureMirrors.end(); ) {
            if (it->second.lastFrame + kHudMirrorIdleFrames < now) {
                // Idle long enough to count as gone: its descriptor set and buffer leave through the deferred-destroy queue, so a present submit
                // that is still reading them keeps them alive (a ticket, not the idle window, is the proof).
                auto& m = it->second;
                s.ctx->retire(HudMirrorRetired{s.dev, s.hudTexPool.get(), std::exchange(m.set, VK_NULL_HANDLE), std::move(m.memory), std::move(m.buffer)});
                it = s.hudTextureMirrors.erase(it);
            } else {
                ++it;
            }
        }
    }

    }
    using HudNativeCmd = HudNativeCommand<VkDescriptorSet>;
    struct HudScratch {
        std::vector<HudColorGpuVertex> triVerts, lineVerts;
        std::vector<HudTexGpuVertex> texVerts;
        std::vector<HudNativeCmd> commands;
    };
    // CPU scratch is copied into the retired frame ring before returning. Each
    // recording thread owns its slot storage, including the load-time warmup.
    static thread_local HudScratch scratch[mc2vk::kMaxFramesInFlight];
    auto& [triVerts, lineVerts, texVerts, commands] = scratch[s.slot()];
    triVerts.clear();
    lineVerts.clear();
    texVerts.clear();
    commands.clear();
    uint32_t triDraws = 0;
    uint32_t lineDraws = 0;
    uint32_t texDraws = 0;
    uint32_t textureUploads = 0;
    // present-tail-cut: runs of draws on one (handle, serial) -- glyph strings on the font atlas -- resolve the mirror once. Mirrors are
    // only evicted above, before this loop, and unordered_map nodes are stable across inserts, so the memoized pointer stays valid.
    int texObjectsReady = -1;   // ensureHudTexObjects, resolved on the first textured draw
    uint64_t memoKey = 0;
    EnginePresentVk::Impl::HudTextureMirror* memoMirror = nullptr;
    { ZoneScopedN("VKIsl.Present.Tail.Hud.PrepareAndTextureUpload");
    for (const VkHudDraw& d : s.pendingHudDraws) {
        const bool hasTexture = d.textureHandle != 0 && d.textureHandle != 0xffffffffu;
        if (hasTexture) {
            if (texObjectsReady < 0)
                texObjectsReady = ensureHudTexObjects(s) ? 1 : 0;
            if (!texObjectsReady)
                continue;
            const uint64_t key = (static_cast<uint64_t>(d.textureHandle) << 32) | static_cast<uint64_t>(d.textureSerial);
            auto* mirror = (memoMirror && key == memoKey && d.textureWidth == memoMirror->width && d.textureHeight == memoMirror->height)
                ? memoMirror : ensureHudTextureMirror(s, d, &textureUploads);
            memoKey = key;
            memoMirror = mirror;
            if (!mirror || !mirror->set)
                continue;
            HudNativeCmd td;
            td.stream = 2;
            td.firstVertex = static_cast<uint32_t>(texVerts.size());
            td.set = mirror->set;
            td.width = mirror->width;
            td.height = mirror->height;
            td.textMode = (d.kind == 3) ? 1u : 0u;
            if (d.kind != 3 && d.textureWidth == 2048 && d.textureHeight == 2048) td.textMode = 2u;
            if (td.textMode == 2u && !d.vertices.empty() && d.vertices[0].frgb == 0x434f4d50u) td.textMode = 3u;
            td.foreground = d.foregroundColor;
            td.opaque = (d.alphaMode == 1);
            td.oneInvAlpha = (d.alphaMode == 4); // VK-FIELD-VFX-3: additive-glow effect blend
            td.oneOne = (d.alphaMode == 2);      // VK-FIELD-VFX-7: pure-additive sprite blend
            // present-tail-cut: size once, then write in place (no per-vertex capacity check).
            const size_t texCount = (d.kind == 0 && d.vertices.size() >= 4 && (d.vertices.size() % 4) == 0)
                ? (d.vertices.size() / 4u) * 6u
                : ((d.kind == 2 || d.kind == 3) ? d.vertices.size() : 0u);
            const size_t texBase = texVerts.size();
            texVerts.resize(texBase + texCount);
            HudTexGpuVertex* texOut = texVerts.data() + texBase;
            auto addTex = [&](const VkHudVertex& v) {
                *texOut++ = {v.x, v.y, d.kind == 2 ? v.rhw : 1.0f, v.z, v.argb, v.u, v.v};
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
            td.count = static_cast<uint32_t>(texVerts.size()) - td.firstVertex;
            if (td.count != 0) {
                if (!AppendHudCommand(commands, td)) ++texDraws;
            }
            continue;
        }
        if (d.kind != 0 && d.kind != 1 && d.kind != 2)
            continue;
        std::vector<HudColorGpuVertex>& dst = (d.kind == 1) ? lineVerts : triVerts;
        const size_t before = dst.size();
        if (d.kind == 0 && d.vertices.size() >= 4) {
            // present-tail-cut: size once, write in place.
            dst.resize(before + (d.vertices.size() / 4u) * 6u);
            HudColorGpuVertex* out = dst.data() + before;
            for (size_t i = 0; i + 3 < d.vertices.size(); i += 4) {
                const VkHudVertex& v0 = d.vertices[i + 0];
                const VkHudVertex& v1 = d.vertices[i + 1];
                const VkHudVertex& v2 = d.vertices[i + 2];
                const VkHudVertex& v3 = d.vertices[i + 3];
                out[0] = {v0.x, v0.y, v0.argb};
                out[1] = {v1.x, v1.y, v1.argb};
                out[2] = {v2.x, v2.y, v2.argb};
                out[3] = {v0.x, v0.y, v0.argb};
                out[4] = {v2.x, v2.y, v2.argb};
                out[5] = {v3.x, v3.y, v3.argb};
                out += 6;
            }
        } else {
            dst.resize(before + d.vertices.size());
            HudColorGpuVertex* out = dst.data() + before;
            for (const VkHudVertex& v : d.vertices)
                *out++ = {v.x, v.y, v.argb};
            // VK-FIELD-VFX-2 tail trace: log untextured tri-batch (kind==2) bbox+argb so we
            // can see the ACTUAL geometry the tail rasterizes (bars vs weapon beams).
            static const bool s_vfxTailTrace = false;
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
                    mc2::fmt::print_err("[VK_VFX_TAIL] kind={:d} nverts={:d} argb0={:08X} canvas={:d}x{:d} bbox=[{:.1f},{:.1f}]-[{:.1f},{:.1f}] w={:.2f} h={:.2f} alpha={:d}\n", mc2::fmt::as_u32(static_cast<unsigned>(d.kind)), mc2::fmt::as_size(d.vertices.size()), mc2::fmt::as_u32(static_cast<unsigned>(d.vertices[0].argb)), mc2::fmt::as_u32(s.hudCanvasW), mc2::fmt::as_u32(s.hudCanvasH), xmn, ymn, xmx, ymx, xmx-xmn, ymx-ymn, mc2::fmt::as_u32(d.alphaMode));
                }
            }
        }
        if (dst.size() != before) {
            HudNativeCmd cmd;
            cmd.stream = (d.kind == 1) ? 1u : 0u;
            cmd.firstVertex = static_cast<uint32_t>(before);
            cmd.count = static_cast<uint32_t>((dst.size() - before));
            cmd.opaque = (d.alphaMode == 1);
            // VK-FIELD-VFX-3: untextured tri effect ribbon (AlphaMode 4) -> additive pipe.
            // Only the tri stream has an additive variant; lines keep standard alpha.
            cmd.oneInvAlpha = (d.kind != 1) && (d.alphaMode == 4);
            cmd.oneOne = (d.kind != 1) && (d.alphaMode == 2);
            if (!AppendHudCommand(commands, cmd))
                (d.kind == 1) ? ++lineDraws : ++triDraws;
        }
    }

    }
    const size_t triBytes = triVerts.size() * sizeof(HudColorGpuVertex);
    const size_t lineBytes = lineVerts.size() * sizeof(HudColorGpuVertex);
    const size_t texBytes = texVerts.size() * sizeof(HudTexGpuVertex);
    const VkDeviceSize colorBytes = static_cast<VkDeviceSize>((triBytes + lineBytes));
    // Both vertex streams are this frame's transient memory (frame ring): rewritten every frame, never read after the frame completed.
    mc2vk::FrameAlloc colorVb, texVb;
    { ZoneScopedN("VKIsl.Present.Tail.Hud.VertexUpload");
    if (colorBytes != 0) {
        colorVb = s.ctx->frameAlloc(colorBytes, 16);
        if (!colorVb) {
            s.pendingHudDraws.clear();
            return draws;
        }
        if (triBytes) std::memcpy(colorVb.ptr, triVerts.data(), triBytes);
        if (lineBytes) std::memcpy(static_cast<uint8_t*>(colorVb.ptr) + triBytes, lineVerts.data(), lineBytes);
    }
    if (texBytes != 0) {
        texVb = s.ctx->frameAlloc(static_cast<VkDeviceSize>(texBytes), 16);
        if (!texVb) {
            s.pendingHudDraws.clear();
            return draws;
        }
        std::memcpy(texVb.ptr, texVerts.data(), texBytes);
    }

    }
    { ZoneScopedN("VKIsl.Present.Tail.Hud.DrawRecord");
    auto* normal = mc2SharedSceneColorGetResource(MC2_SHARED_SCENE_NORMAL);
    auto* depth = mc2SharedSceneDepthGetResource();
    if (normal) barrier(cb, normal->image.get(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (depth) barrier(cb, depth->image.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = s.hudColorRenderPass.get();
    rp.framebuffer = s.hudColorFramebuffer.get();
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {s.imgW, s.imgH};
    MC2_VK_GPU_SCOPE(gpuScope, cb, "VKIsl.Present.Hud");
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{};
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width = static_cast<float>(s.imgW); vp.height = static_cast<float>(s.imgH);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    VkRect2D sc{{0, 0}, {s.imgW, s.imgH}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    // HUD-CANVAS-EXTENT-FIX: divide HUD vertices by the canvas they were authored in
    // (logical game res), not the present image size (physical drawable). See hudCanvasW.
    const float hudExW = s.hudCanvasW ? static_cast<float>(s.hudCanvasW) : static_cast<float>(s.imgW);
    const float hudExH = s.hudCanvasH ? static_cast<float>(s.hudCanvasH) : static_cast<float>(s.imgH);
    const float extent[2] = {hudExW, hudExH};
    vkCmdPushConstants(cb, s.hudColorLayout.get(), VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(extent), extent);
    VkDeviceSize offset = 0;
    VkPipeline boundPipe = VK_NULL_HANDLE;
    int boundStream = -1;   // 0 = color vertices bound, 1 = textured vertices bound
    VkDescriptorSet texSetBound = VK_NULL_HANDLE;
    HudTexPush texPushLast{};
    bool texPushValid = false;
    for (const HudNativeCmd& td : commands) {
        if (td.stream == 0 || td.stream == 1) {
            const VkPipeline pipe = (td.stream == 0)
                ? (td.opaque ? s.hudColorTriOpaquePipe.get()
                             : (td.oneOne ? s.hudColorTriOneOnePipe.get()
                                          : (td.oneInvAlpha ? s.hudColorTriOneInvAlphaPipe.get() : s.hudColorTriPipe.get())))
                : (td.opaque ? s.hudColorLineOpaquePipe.get() : s.hudColorLinePipe.get());
            if (boundPipe != pipe) {
                mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                if (recordedPipelines) recordedPipelines->push_back(pipe);
                boundPipe = pipe;
            }
            if (boundStream != 0) {
                offset = colorVb.offset;
                vkCmdBindVertexBuffers(cb, 0, 1, &colorVb.buffer, &offset);
                boundStream = 0;
            }
            const uint32_t first = (td.stream == 1)
                ? static_cast<uint32_t>(triVerts.size()) + td.firstVertex
                : td.firstVertex;
            vkCmdDraw(cb, td.count, 1, first, 0);
            texSetBound = VK_NULL_HANDLE;
            texPushValid = false;
        } else {
            const VkPipeline pipe = td.opaque ? s.hudTexOpaquePipe.get()
                                  : (td.oneOne ? s.hudTexOneOnePipe.get()
                                               : (td.oneInvAlpha ? s.hudTexOneInvAlphaPipe.get() : s.hudTexPipe.get()));
            if (boundPipe != pipe) {
                mc2vk::infra::bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
                if (recordedPipelines) recordedPipelines->push_back(pipe);
                boundPipe = pipe;
            }
            if (boundStream != 1) {
                VkDeviceSize texOffset = texVb.offset;
                vkCmdBindVertexBuffers(cb, 0, 1, &texVb.buffer, &texOffset);
                boundStream = 1;
            }
            HudTexPush pc{};
            pc.extent[0] = hudExW;   // HUD-CANVAS-EXTENT-FIX (see hudColor path above)
            pc.extent[1] = hudExH;
            pc.texW = td.width;
            pc.texH = td.height;
            pc.textMode = td.textMode;
            pc.foreground = td.foreground;
            // present-tail-cut: within a run of textured commands the set and push state persist (one layout), so rebind/re-push only on
            // change. Any color command in between resets the tracking (it pushes through the color layout).
            if (texSetBound != td.set) {
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.hudTexLayout.get(),
                                        0, 1, &td.set, 0, nullptr);
                texSetBound = td.set;
            }
            if (!texPushValid || std::memcmp(&texPushLast, &pc, sizeof(pc)) != 0) {
                vkCmdPushConstants(cb, s.hudTexLayout.get(),
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(pc), &pc);
                texPushLast = pc;
                texPushValid = true;
            }
            if (td.textMode == 3u) {
                ZoneScopedN("VKIsl.Present.Compass");
                MC2_VK_GPU_SCOPE(compassScope, cb, "VKIsl.Present.Compass");
                vkCmdDraw(cb, td.count, 1, td.firstVertex, 0);
            } else {
                vkCmdDraw(cb, td.count, 1, td.firstVertex, 0);
            }
        }
    }
    vkCmdEndRenderPass(cb);
    gpuScope.end();
    if (normal) barrier(cb, normal->image.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    if (depth) barrier(cb, depth->image.get(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    }
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
    s.spatialAA.reset();
    if (s.upscaler) s.upscaler->resetHistory();
    s.motionVectors.reset();
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
                               VK_IMAGE_USAGE_STORAGE_BIT |   // CMAA2 (spatial AA mode 3) writes its output as a storage image
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

bool EnginePresentVk::init(VulkanContext& ctx, SDL_Window* window, uint32_t w, uint32_t h) {
    health_ = EnginePresentHealth{};
    health_.attempted = true;
    if (!ctx.valid()) {
        health_.fallback_reason = "engine VulkanContext not valid (present init pre-req)";
        plog("%s. fail-soft.", health_.fallback_reason.c_str());
        return false;
    }
    if (!window) {
        health_.fallback_reason = "P13b: NULL GL game window handed to present init";
        plog("%s. fail-soft.", health_.fallback_reason.c_str());
        return false;
    }
    window_ = window;   // BORROWED (g_sdl_window) -- never destroyed here.
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

    // Create the Vulkan surface on the visible SDL_WINDOW_VULKAN game window.
    // Reuse its HWND so presentation and input belong to the same window.
#ifdef _WIN32
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(window_, &wm) != SDL_TRUE) {
        return fail(std::string("P13b SDL_GetWindowWMInfo: ") + SDL_GetError());
    }
    if (wm.subsystem != SDL_SYSWM_WINDOWS) {
        return fail("P13b: game window subsystem is not SDL_SYSWM_WINDOWS");
    }
    HWND      hwnd  = wm.info.win.window;
    HINSTANCE hinst = wm.info.win.hinstance;
    if (!hwnd) return fail("P13b: SDL_GetWindowWMInfo gave a NULL HWND");
    if (!hinst) hinst = GetModuleHandle(nullptr);   // SDL may leave hinstance NULL

    // Volk lacks the Win32 entrypoint here; resolve it from the shared instance.
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
        return fail("P13b: vkCreateWin32SurfaceKHR on the game window's HWND failed ("
                    + std::to_string(static_cast<int>(sr)) + ")");
    }
    plog("P13b: VK surface created on the VISIBLE GL game window HWND=%p (no hidden "
         "window; GL keeps input+focus).", static_cast<void*>(hwnd));
#else
    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window_, s.inst, &rawSurface))
        return fail(std::string("SDL_Vulkan_CreateSurface: ") + SDL_GetError());
    s.surface = mc2vk::OwnedSurface(s.inst, rawSurface);
    {
        // Xvfb is only presentable from the GPU its loader picks; on the others no queue family supports the window surface.
        // Run offscreen there (no swapchain, no present): the shared image is still composited and the capture readback reads it.
        // Offscreen only when EVERY family answered and none can present. A failed query is an error, not proof of "no present": the
        // offscreen path has no frame pacing, so on a failed query keep the normal swapchain path (chooseQueuesAndPresentMode decides).
        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(s.phys, &qfCount, nullptr);
        bool anyPresent = false, queryFailed = false;
        for (uint32_t q = 0; q < qfCount && !anyPresent; ++q) {
            VkBool32 canPresent = VK_FALSE;
            const VkResult qr = vkGetPhysicalDeviceSurfaceSupportKHR(s.phys, q, s.surface.get(), &canPresent);
            if (qr != VK_SUCCESS) {
                queryFailed = true;
                plog("ERROR: vkGetPhysicalDeviceSurfaceSupportKHR(family %u) failed (%d); not falling back to offscreen.",
                     q, static_cast<int>(qr));
            } else if (canPresent == VK_TRUE) {
                anyPresent = true;
            }
        }
        if (!anyPresent && !queryFailed) {
            s.offscreen = true;
            plog("window surface not presentable from this GPU; running offscreen (capture readback only, no present).");
        }
    }
#endif

    // ---- queue confirm + present mode -------------------------------------
    {
        std::string r;
        if (!chooseQueuesAndPresentMode(s, r)) return fail(r);
        health_.present_mode = static_cast<int>(s.presentMode);
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
        pci.flags = 0;
        pci.queueFamilyIndex = s.gfxFamily;
        if (!mc2vk::create(s.cpool, s.dev, [&](auto* h) { return vkCreateCommandPool(s.dev, &pci, nullptr, h); }))
            return fail("vkCreateCommandPool failed");
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        for (uint32_t i = 0; i < mc2vk::kMaxFramesInFlight; ++i) {
            if (!mc2vk::create(s.tailPool[i], s.dev, [&](auto* h) { return vkCreateCommandPool(s.dev, &pci, nullptr, h); }))
                return fail("vkCreateCommandPool(tail) failed");
            if (!mc2vk::create(s.blitPools[i], s.dev, [&](auto* h) { return vkCreateCommandPool(s.dev, &pci, nullptr, h); }))
                return fail("vkCreateCommandPool(blit) failed");
            cai.commandPool = s.blitPools[i].get();
            if (vkAllocateCommandBuffers(s.dev, &cai, &s.blitCbufs[i]) != VK_SUCCESS)
                return fail("vkAllocateCommandBuffers(blit) failed");
            cai.commandPool = s.tailPool[i].get();
            if (vkAllocateCommandBuffers(s.dev, &cai, &s.cbufs[i]) != VK_SUCCESS)
                return fail("vkAllocateCommandBuffers failed");
        }
        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (uint32_t i = 0; i < mc2vk::kMaxFramesInFlight; ++i) {
            if (!mc2vk::create(s.acquireSems[i], s.dev, [&](auto* h) { return vkCreateSemaphore(s.dev, &semci, nullptr, h); }))
                return fail("vkCreateSemaphore(acquire) failed");
            if (!mc2vk::create(s.blitFences[i], s.dev, [&](auto* h) { return vkCreateFence(s.dev, &fci, nullptr, h); }))
                return fail("vkCreateFence(blit) failed");
            if (!mc2vk::create(s.inFlights[i], s.dev, [&](auto* h) { return vkCreateFence(s.dev, &fci, nullptr, h); }))
                return fail("vkCreateFence(inFlight) failed");
        }
        ctx.setFramesInFlight(static_cast<uint32_t>((std::max)(1, mc2::gate::get<"MC2_VK_FRAMES_IN_FLIGHT">())));
        s.slotCount = ctx.frameContext().framesInFlight;
        if (mc2DiagVerbose()) plog("[VK_FRAMES_IN_FLIGHT v1] frames_in_flight=%u (MC2_VK_FRAMES_IN_FLIGHT=1 is the kill switch).", s.slotCount);
        // VK-PRESENT-SINGLESUBMIT-1: the composite's own command buffer (recorded at
        // composite time, submitted together with the tail cbuf in ONE submit).
        VkCommandBufferAllocateInfo cai2{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai2.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai2.commandBufferCount = 1;
        for (uint32_t i = 0; i < mc2vk::kMaxFramesInFlight; ++i) {
            if (!mc2vk::create(s.compositeCmdPool[i], s.dev, [&](auto* h) { return vkCreateCommandPool(s.dev, &pci, nullptr, h); }))
                return fail("vkCreateCommandPool(composite) failed");
            cai2.commandPool = s.compositeCmdPool[i].get();
            if (vkAllocateCommandBuffers(s.dev, &cai2, &s.compositeCbufs[i]) != VK_SUCCESS)
                return fail("vkAllocateCommandBuffers(composite) failed");
        }
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

// Arm the native composite against the present owner's actual image readiness.
bool EnginePresentVk::beginNativeComposite() {
    ++health_.native_composite_requests;
    nativeCompositeArmed_ = created_ && impl_ && impl_->sharedImg.image.get() != VK_NULL_HANDLE;
    presentImageVkOwned_ = false;
    health_.native_composite_armed = nativeCompositeArmed_;
    if (nativeCompositeArmed_) ++health_.native_composite_grants;
    return nativeCompositeArmed_;
}

bool EnginePresentVk::renderNativeCompositeVisual(float exposure,
                                                  int viewMode,
                                                  int engineIdxBase,
                                                  float lowLightGain,
                                                  const float lowLightTint[3],
                                                  bool casEnabled, float casSharpness, int tonemapMode,
                                                  int viewportX, int viewportY,
                                                  int viewportW, int viewportH) {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "renderNativeCompositeVisual");
    if (!created_ || !impl_ || !nativeCompositeArmed_)
        return false;
    Impl& s = *impl_;
    if (viewportW <= 0 || viewportH <= 0 ||
        viewportX < 0 || viewportY < 0 ||
        viewportX + viewportW > (int)s.imgW ||
        viewportY + viewportH > (int)s.imgH)
        return false;
    const bool objectIdDirectChainCandidate = objectIdDirectChainGateEnabled();
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

    // The worker retires the next slot before releasing main; inline mode retains the original wait here.
    if (!present_thread::enabled() && !waitCompositeSlot(s, health_)) return false;

    // Record into the composite's OWN cbuf; the submit happens in presentFrame as
    // half of the single {compositeCbuf, cbuf} submission.
    // The explicit temporal-AA preference replaces spatial AA and CAS; spatial modes never register a jitter consumer.
    VkImageResource* upDepth = mc2SharedSceneDepthGetResource();
    const bool scaled = static_cast<bool>(s.scaledComposite.image);
    RenderCore::UpscalerFrame upFrame{};
    RenderCore::IUpscalerProvider* const up = mc2vk::selectUpscaler(
        s.upscaler, *scene, upDepth, !scaled && mc2::options::current().temporalAA(), s.captureFrame, upFrame);
    mc2vk::infra::materialBiasTick();   // provider failure / AA toggle: log the effective bias; material owners rebuild stale samplers
    const int aaMode = up ? 0 : mc2::options::current().spatialAA;
    if (up) casEnabled = false;
    const auto& aaOutput = scaled ? s.scaledComposite : s.sharedImg;
    const bool aaReady = s.spatialAA.prepare(*s.ctx, aaOutput, aaMode);
    ZoneNamedN(zCompRec, "VKIsl.Present.CompositeRecord", true);   // VK-ISLAND-TRACY-1 (to fn return)
    VkCommandBuffer cc = s.curCompositeCbuf();
    s.compositePending = false;
    vkResetCommandPool(s.dev, s.compositeCmdPool[s.slot()].get(), 0);   // cc is s.compositeCbufs[slot]
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cc, &bi) != VK_SUCCESS)
        return false;

    // Camera motion vectors (temporal track, docs/plans/2026-11-06-temporal-aa.md), before the
    // composite. Demand-driven: the only consumer so far is the debug inset in the present tail,
    // so the pass (and its image) cost nothing until that inset or a resolve asks for it.
    const bool mvOk = (s.showMotionVectorsDebug || up) && upDepth && upDepth->image && upDepth->w && upDepth->h &&
                      s.motionVectors.prepare(*s.ctx, *upDepth);
    if (mvOk) s.motionVectors.record(cc, *upDepth, s.captureFrame, mc2_vk_mech_island_object_motion_written() != 0);   // present-side copy of the scene's g_mc2RenderFrame

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

    // Upscaler resolve between the scene and the composite; the composite then tonemaps the resolved image once.
    VkDescriptorSet sceneSet = s.compositeSet;
    if (up && mvOk && mc2vk::dispatchUpscaler(*up, upFrame, cc, s.motionVectors.image(), s.motionVectors.reactiveImage())) {
        const VkImageView upView = reinterpret_cast<VkImageView>(up->output().view);
        // Rewrite when either bound view may have moved: the provider output, or the shared targets (binding 1) were recreated.
        const unsigned long long targetsGen = mc2SharedSceneColorGeneration();
        if (s.upscalerSetView != upView || s.upscalerSetTargetsGen != targetsGen) {
            s.upscalerSetView = upView;
            s.upscalerSetTargetsGen = targetsGen;
            const VkDescriptorImageInfo fi[2]{{s.compositeSampler.get(), upView, VK_IMAGE_LAYOUT_GENERAL},
                {s.compositeNearestSampler.get(), boundObjectId ? boundObjectId->view.get() : scene->view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkWriteDescriptorSet fw[2]{};
            for (uint32_t i = 0; i < 2; ++i) {
                fw[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                fw[i].dstSet = s.upscalerSet; fw[i].dstBinding = i; fw[i].descriptorCount = 1;
                fw[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                fw[i].pImageInfo = &fi[i];
            }
            vkUpdateDescriptorSets(s.dev, 2, fw, 0, nullptr);
        }
        sceneSet = s.upscalerSet;
    }

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = s.compositeRenderPass.get();
    rp.framebuffer = scaled ? s.scaledCompositeFramebuffer.get() : s.compositeFramebuffer.get();
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {aaOutput.w, aaOutput.h};
    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    MC2_VK_GPU_SCOPE(gpuScope, cc, "VKIsl.Present.Composite");
    vkCmdBeginRenderPass(cc, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{static_cast<float>(viewportX), static_cast<float>(viewportY),
                  static_cast<float>(viewportW), static_cast<float>(viewportH), 0.0f, 1.0f};
    VkRect2D sc{{viewportX, viewportY}, {static_cast<uint32_t>(viewportW), static_cast<uint32_t>(viewportH)}};
    if (scaled) {
        vp.x *= static_cast<float>(scene->w) / s.imgW; vp.y *= static_cast<float>(scene->h) / s.imgH;
        vp.width *= static_cast<float>(scene->w) / s.imgW; vp.height *= static_cast<float>(scene->h) / s.imgH;
        sc = {{static_cast<int32_t>(vp.x), static_cast<int32_t>(vp.y)}, {static_cast<uint32_t>(vp.width), static_cast<uint32_t>(vp.height)}};
    }
    vkCmdSetViewport(cc, 0, 1, &vp);
    vkCmdSetScissor(cc, 0, 1, &sc);
    mc2vk::infra::bind(cc, VK_PIPELINE_BIND_POINT_GRAPHICS, s.compositePipe.get());
    vkCmdBindDescriptorSets(cc, VK_PIPELINE_BIND_POINT_GRAPHICS, s.compositeLayout.get(),
                            0, 1, &sceneSet, 0, nullptr);
    const auto& output = visualTuning_environment().output;
    const bool upscale = mc2::gate::get<"MC2_RENDER_SCALE">() < 1.0 &&
                         (scene->w < s.imgW || scene->h < s.imgH);
    // Low/Potato (baseQuality < 2) upscale with the single bilinear tap: the 9-tap Catmull-Rom costs 2.1 ms of 3.4 ms on an HD 630 at 720p.
    const bool cubicUpscale = mc2::options::current().baseQuality >= 2;
    float pc[32] = {
        exposure,
        1.0f / static_cast<float>(upscale ? scene->w : s.imgW),
        1.0f / static_cast<float>(upscale ? scene->h : s.imgH),
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        (!scaled && casEnabled) ? 1.0f : 0.0f,
        static_cast<float>(viewMode),
        lowLightGain,
        lowLightTint ? lowLightTint[0] : 0.7f,
        lowLightTint ? lowLightTint[1] : 1.0f,
        lowLightTint ? lowLightTint[2] : 0.6f,
        static_cast<float>(engineIdxBase),
        casSharpness,
        static_cast<float>(tonemapMode),
        output.grade[0], output.grade[1], output.grade[2], output.vignette,
        output.shadows[0], output.shadows[1], output.shadows[2], output.topWarmthStrength,
        output.highlights[0], output.highlights[1], output.highlights[2], 0,
        output.topWarmth[0], output.topWarmth[1], output.topWarmth[2], 0
    };
    vkCmdPushConstants(cc, s.compositeLayout.get(), VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), pc);
    vkCmdDraw(cc, 3, 1, 0, 0);
    vkCmdEndRenderPass(cc);
    gpuScope.end();

    if (aaReady) s.spatialAA.record(cc, aaOutput);
    if (scaled) {
        barrier(cc, s.scaledComposite.image.get(),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        rp.framebuffer = s.compositeFramebuffer.get();
        rp.renderArea.extent = {s.imgW, s.imgH};
        MC2_VK_GPU_SCOPE(upscaleScope, cc, "VKIsl.Present.Upscale");
        vkCmdBeginRenderPass(cc, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vp = {static_cast<float>(viewportX), static_cast<float>(viewportY), static_cast<float>(viewportW), static_cast<float>(viewportH), 0.0f, 1.0f};
        sc = {{viewportX, viewportY}, {static_cast<uint32_t>(viewportW), static_cast<uint32_t>(viewportH)}};
        vkCmdSetViewport(cc, 0, 1, &vp); vkCmdSetScissor(cc, 0, 1, &sc);
        mc2vk::infra::bind(cc, VK_PIPELINE_BIND_POINT_GRAPHICS, s.compositePipe.get());
        vkCmdBindDescriptorSets(cc, VK_PIPELINE_BIND_POINT_GRAPHICS, s.compositeLayout.get(),
                               0, 1, &s.upscaleSet, 0, nullptr);
        pc[3] = (upscale && cubicUpscale) ? 1.0f : 0.0f;
        pc[4] = 1.0f; // already tone-mapped and graded; only reconstruct/sharpen
        pc[7] = casEnabled ? 1.0f : 0.0f;
        vkCmdPushConstants(cc, s.compositeLayout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
        vkCmdDraw(cc, 3, 1, 0, 0);
        vkCmdEndRenderPass(cc);
        barrier(cc, s.scaledComposite.image.get(),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }

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

    // VK-PRESENT-SINGLESUBMIT-1: NO submit and NO drain here. The recorded cbuf is
    // submitted by presentFrame together with the tail (HUD/ImGui + swapchain
    // blit) in ONE vkQueueSubmit; GPU-side composite-before-tail ordering comes from
    // command-buffer order within the submit + the tail's entry barriers.
    s.compositePending = true;
    ++health_.composite_vk_frames;
    presentImageVkOwned_ = true;
    return true;
}

void EnginePresentVk::setMotionVectorsDebug(bool on) {
    if (impl_) impl_->showMotionVectorsDebug = on;
}

bool EnginePresentVk::renderNativeCsmShadowDebugOverlay(void* csmArrayView, int layer) {
    if (!created_ || !impl_ || !nativeCompositeArmed_)
        return false;
    Impl& s = *impl_;
    VkImageView arrayView = reinterpret_cast<VkImageView>(csmArrayView);
    if (!ensureShadowDebugOverlayObjects(s, arrayView))
        return false;

    // VK-PRESENT-SINGLESUBMIT-1: the overlay used to be its OWN submit here (and it
    // reset s.curCbuf() mid-frame and left s.curInFlight().get() unsignaled after its wait — a latent
    // hang against the present tail's fence wait). It is now only STASHED; the
    // present tail records it into the single {compositeCbuf, cbuf} submission,
    // after the composite (correct paint order — with the composite no longer
    // executed at composite time, an own-submit overlay would run BEFORE the
    // composite on the GPU and be overwritten by it).
    s.pendingShadowDebugView = arrayView;
    s.pendingShadowDebugLayer = layer < 0 ? 0 : layer;
    return true;
}

bool EnginePresentVk::prewarmPipelines() {
    if (!created_ || !impl_) return false;
    Impl& s=*impl_;
    const bool ready = ensureCompositeObjects(s) && ensureHudColorObjects(s) && ensureHudTexObjects(s) &&
        (!ImGui::GetCurrentContext() || ensureImguiVulkan(s)) &&
        s.spatialAA.prepare(*s.ctx, s.scaledComposite.image ? s.scaledComposite : s.sharedImg,
                            mc2::options::current().spatialAA);
    if (!ready) return false;
    // Pipeline creation alone leaves driver work for the first command recording.
    // This load-only composite is overwritten by the first scene; it never presents
    // or advances the frame clock, and the bounded submit retires its ring data.
    ZoneScopedN("VKIsl.Present.WarmRecord");
    const bool armed = nativeCompositeArmed_, owned = presentImageVkOwned_;
    const auto composites = health_.composite_vk_frames;
    nativeCompositeArmed_ = true;
    const bool recorded = renderNativeCompositeVisual(1.0f, 0, 0, 1.0f, nullptr,
        false, 0.0f, 0, 0, 0, s.imgW, s.imgH);
    bool submitted = false;
    if (recorded) {
        const VkCommandBuffer cb = s.curCompositeCbuf();
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cb;
        submitted = submitAndWait(*s.ctx, si, 5ull * 1000 * 1000 * 1000, "present_load_record");
    }
    s.compositePending = false;
    nativeCompositeArmed_ = armed;
    presentImageVkOwned_ = owned;
    health_.composite_vk_frames = composites;
    if (!recorded || !submitted) return false;
    auto pendingHud = std::move(s.pendingHudDraws);
    // Degenerate off-screen triangles exercise the HUD's color and descriptor
    // paths without changing any pixels or borrowing a live UI texture identity.
    constexpr uint32_t warmTexture = UINT32_MAX - 1;
    VkHudDraw draw;
    draw.kind = 2;
    draw.vertices.resize(3);
    for (auto& v : draw.vertices) v.x = v.y = -1000000.0f;
    draw.textureSerial = warmTexture;
    draw.textureWidth = draw.textureHeight = 1;
    draw.textureRgba = std::make_shared<const std::vector<uint8_t>>(4, 255);
    s.pendingHudDraws.clear();
    // Each distinct triangle, line and textured pipeline, including opaque.
    struct WarmVariant { uint32_t kind, texture, alpha; };
    constexpr WarmVariant variants[] = {
        {2, 0, 1}, {2, 0, 3}, {2, 0, 2}, {2, 0, 4},
        {1, 0, 1}, {1, 0, 3},
        {2, warmTexture, 1}, {2, warmTexture, 3},
        {2, warmTexture, 2}, {2, warmTexture, 4},
    };
    for (const auto& variant : variants) {
        draw.alphaMode = variant.alpha;
        draw.kind = variant.kind;
        draw.textureHandle = variant.texture;
        s.pendingHudDraws.push_back(draw);
    }
    std::vector<VkPipeline> recordedPipelines;
    const bool hudSubmitted = submitOneShot(*s.ctx, [&](VkCommandBuffer cb) {
        barrier(cb, s.sharedImg.image.get(),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        recordPendingHudColorProof(s, cb, nullptr, nullptr, nullptr, &recordedPipelines);
    }, "present_load_hud");
    s.pendingHudDraws = std::move(pendingHud);
    const uint64_t key = (static_cast<uint64_t>(warmTexture) << 32) | warmTexture;
    if (auto it = s.hudTextureMirrors.find(key); it != s.hudTextureMirrors.end()) {
        auto& [identity, mirror] = *it;
        vkFreeDescriptorSets(s.dev, s.hudTexPool.get(), 1, &mirror.set);
        s.hudTextureMirrors.erase(it);
    }
    const VkPipeline expected[] = {
        s.hudColorTriOpaquePipe.get(), s.hudColorTriPipe.get(),
        s.hudColorTriOneOnePipe.get(), s.hudColorTriOneInvAlphaPipe.get(),
        s.hudColorLineOpaquePipe.get(), s.hudColorLinePipe.get(),
        s.hudTexOpaquePipe.get(), s.hudTexPipe.get(),
        s.hudTexOneOnePipe.get(), s.hudTexOneInvAlphaPipe.get(),
    };
    const bool covered = std::all_of(std::begin(expected), std::end(expected), [&](VkPipeline pipe) {
        return pipe && std::find(recordedPipelines.begin(), recordedPipelines.end(), pipe) != recordedPipelines.end();
    });
    std::fprintf(stderr, "[VK_PRESENT_WARM] hud_variants=%zu complete=%d submitted=%d\n",
        recordedPipelines.size(), static_cast<int>(covered), static_cast<int>(hudSubmitted));
    return hudSubmitted && covered;
}

bool EnginePresentVk::prepareImguiVulkanFrame() {
    if (!created_ || !impl_)
        return false;
    Impl& s = *impl_;
    if (!s.imguiVkInited || ImGui::GetIO().Fonts->TexID == 0)
        present_thread::waitIdle(); // The backend's font upload submits directly to the shared queue.
    if (!ensureImguiVulkan(s))
        return false;
    health_.imgui_vk_ready = true;
    ImGui_ImplVulkan_NewFrame();
    return true;
}

bool EnginePresentVk::renderImguiVulkanDrawData(ImDrawData* drawData) {
    if (!created_ || !impl_ || !drawData)
        return false;
    Impl& s = *impl_;
    if (!ensureImguiVulkan(s))
        return false;
    health_.imgui_vk_ready = true;
    s.clearImgui();
    if (s.ownedImgui.assign(*drawData)) {
        s.pendingImguiDrawData = &s.ownedImgui.data;
    } else {
        present_thread::stop();
        plog("INLINE FALLBACK: ImGui callback requires main ownership");
        s.pendingImguiDrawData = drawData;
    }
    return true;
}

bool EnginePresentVk::queueHudVulkanFrame(std::vector<VkHudDraw>&& draws,
                                          uint32_t canvasW, uint32_t canvasH) {
    if (!created_ || !impl_ || draws.empty())
        return false;
    Impl& s = *impl_;
    s.pendingHudDraws = std::move(draws);
    s.hudCanvasW = canvasW;   // HUD-CANVAS-EXTENT-FIX: authoring canvas (logical res)
    s.hudCanvasH = canvasH;
    health_.hud_vk_draws_last = static_cast<uint32_t>(s.pendingHudDraws.size());
    health_.hud_vk_textured_last = 0;
    health_.hud_vk_serialized_last = 0;
    for (const VkHudDraw& d : s.pendingHudDraws) {
        if (d.textureHandle != 0 && d.textureRgba && d.textureWidth != 0 && d.textureHeight != 0) {
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
    imguiTexReleaseAll(s);   // before the backend (and its descriptor pool) goes
    s.imguiTexSampler.reset();
    if (s.imguiVkInited) {
        // Teardown can follow context/backend destruction or failed init.
        // Guard both lifetimes: the backend shutdown dereferences its data.
        if (ImGui::GetCurrentContext() != nullptr &&
            ImGui::GetIO().BackendRendererUserData != nullptr) {
            ImGui_ImplVulkan_Shutdown();
        }
        s.imguiVkInited = false;
    }
    s.clearImgui();
    s.pendingHudDraws.clear();
    health_.imgui_vk_ready = false;
}

// The worker owns acquire, composite submission, tail recording and frame retirement.
bool EnginePresentVk::presentFrame(uint32_t drawableW, uint32_t drawableH,
                                     bool verifyReadback) {
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "presentFrame");
    if (impl_) impl_->frameFinished = false;
    const bool ok = presentFrameBody(drawableW, drawableH, verifyReadback);
    if (!ok) present_thread::reportFailure(health_.fallback_reason);   // worker only: main tears down and re-inits at the next frame top
    // Declined frames still retire their allocations and release the producer.
    if (impl_ && !impl_->frameFinished) finishFrame();
    if (present_thread::onWorker() && mc2::overlap::isOwner(mc2::overlap::Owner::Render)) present_thread::seal();
    return ok;
}

void EnginePresentVk::setCaptureFrame(uint32_t renderFrame, uint32_t fireFrame) {
    if (impl_) { impl_->captureFrame = renderFrame; impl_->captureFireFrame = fireFrame; }
}

bool EnginePresentVk::finishFrame() {
    Impl& s = *impl_;
    s.compositePending = false;
    presentImageVkOwned_ = false;
    health_.validation_errors = s.ctx->health().validationErrors;
    ZoneScopedN("VKIsl.Present.Tail.Finish");
    { ZoneScopedN("VKIsl.Present.Tail.Finish.Collect"); mc2::battlebench::NsScope ns(mc2::battlebench::PresCollectNs);
      gpuprof::frameEnd(); }
    { ZoneScopedN("VKIsl.Present.Tail.Finish.Boundary"); mc2::battlebench::NsScope ns(mc2::battlebench::PresBoundaryNs);
      s.ctx->frameBoundary(); }
    const bool ready = !present_thread::onWorker() || waitCompositeSlot(s, health_);
    if (!ready) {
        health_.fallback_reason = "composite slot fence wait failed";
        present_thread::reportFailure(health_.fallback_reason);
    }
    s.frameFinished = true;
    if (present_thread::onWorker() && (health_.present_frames % 300) == 1) logHealth();
    return ready && !mc2vk::infra::deviceLostLatched();
}

bool EnginePresentVk::presentFrameBody(uint32_t drawableW, uint32_t drawableH,
                                         bool verifyReadback) {
    if (!created_ || !impl_) {
        health_.fallback_reason = "present path not initialized";
        return false;
    }
    ZoneScopedN("VKIsl.Present.Frame");   // VK-ISLAND-TRACY-1: presentFrame total (CPU)
    Impl& s = *impl_;

    // Drawable dimensions are captured by main; the worker never pumps SDL. Minimized: present nothing and keep any WSI result pending,
    // so the swapchain is rebuilt for the restored size instead of failing a zero-extent rebuild here.
    if (drawableW == 0 || drawableH == 0) {
        return true;   // nothing to present; try again next frame
    }

    // WSI results are consumed only after main has transferred the next frame.
    if (s.lastWsiResult != VK_SUCCESS) {
        const VkResult previous = std::exchange(s.lastWsiResult, VK_SUCCESS);
        // vk-present-instance 2a: SUBOPTIMAL rebuilds only when the surface extent or transform differs from the chain's; otherwise the
        // chain is still the best the surface offers and a rebuild would loop (a drain per frame). Logged once.
        bool suboptimalOnly = false;
        if (previous == VK_SUBOPTIMAL_KHR) {
            VkSurfaceCapabilitiesKHR caps{};
            if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s.phys, s.surface.get(), &caps) == VK_SUCCESS) {
                VkExtent2D cur = caps.currentExtent;
                if (cur.width == 0xFFFFFFFFu) cur = {drawableW, drawableH};
                cur.width  = std::clamp(cur.width,  caps.minImageExtent.width,  caps.maxImageExtent.width);
                cur.height = std::clamp(cur.height, caps.minImageExtent.height, caps.maxImageExtent.height);
                suboptimalOnly = cur.width == s.scExtent.width && cur.height == s.scExtent.height && caps.currentTransform == s.scTransform;
            }
            if (suboptimalOnly) {
                static bool logged = false;
                if (!logged) { logged = true; plog("SUBOPTIMAL with unchanged extent/transform: keeping the swapchain (logged once)."); }
            }
        }
        if (suboptimalOnly) {
            // keep going
        } else if (previous == VK_ERROR_OUT_OF_DATE_KHR || previous == VK_SUBOPTIMAL_KHR) {
            std::string reason;
            if (!rebuildSwapchain(s, drawableW, drawableH, health_, reason)) {
                health_.fallback_reason = reason;
                return false;
            }
        } else {
            if (previous == VK_ERROR_DEVICE_LOST) infra::deviceLost();
            health_.fallback_reason = "vkQueuePresentKHR failed (" + std::to_string(static_cast<int>(previous)) + ")";
            return false;
        }
    }

    // Resize: if the GL drawable changed, rebuild the shared image + swapchain to
    // match the game window's new size (OS-driven resize / DPI change).
    if (drawableW != s.imgW || drawableH != s.imgH) {
        const bool hadNativeComposite = (nativeCompositeArmed_);
        // VK-PRESENT-SINGLESUBMIT-1: (a) retire any in-flight prior submit before
        // buildSharedImage destroys the image it may still reference (the old
        // composite-time queue drain used to provide this incidentally; the wait is
        // free when nothing is pending — fence stays SIGNALED, no reset). (b) DROP
        // the pending composite recording: it references the about-to-be-destroyed
        // shared image / framebuffers and must never be submitted.
        {
            VkFence fences[mc2vk::kMaxFramesInFlight]{};
            s.ctx->waitIdle("present_resize_blit");
            s.ctx->poll();   // vk-sync-arch: flush the deferred submit batch before the image goes away
            for (uint32_t i = 0; i < s.slotCount; ++i) fences[i] = s.inFlights[i].get();
            VkResult done = vkWaitForFences(s.dev, s.slotCount, fences, VK_TRUE, 5ull * 1000 * 1000 * 1000);
            // A timeout proves nothing: the in-flight frame may still read the image. Only an idle device proves it; a failed idle (device lost) leaves
            // the image alone and fails the present, so buildSharedImage never destroys an image the GPU may still use.
            if (done != VK_SUCCESS) { s.ctx->waitIdle("resize"); done = mc2vk::infra::deviceLostLatched() ? VK_ERROR_DEVICE_LOST : VK_SUCCESS; }
            if (done != VK_SUCCESS) { health_.fallback_reason = "resize: in-flight frame not proven complete"; return false; }
        }
        drainReadbackBeforeImageDestroy();   // a pending image->buffer copy reads the shared image buildSharedImage is about to destroy
        s.compositePending = false;
        std::string r;
        if (!buildSharedImage(s, drawableW, drawableH, r)) {
            health_.fallback_reason = "resize: " + r; return false;
        }
        if (!rebuildSwapchain(s, drawableW, drawableH, health_, r)) {
            health_.fallback_reason = "resize: " + r; return false;
        }
        ++health_.resize_count;
        health_.extent_w = s.scExtent.width;
        health_.extent_h = s.scExtent.height;
        if (hadNativeComposite) {
            nativeCompositeArmed_ = false;
            presentImageVkOwned_ = false;
            return true;
        }
    }

    if (!presentImageVkOwned_ || !s.compositePending) {
        presentNativeStrictAbort("present_image_not_vk_owned");
        // Tolerate bounded warmup, but never submit stale image contents.
        static const unsigned int kNotOwnedBudget = 240u;   // ~a few seconds @60fps
        if (notOwnedStreak_ < kNotOwnedBudget) {
            ++notOwnedStreak_;
            return true;   // transient: retry next frame
        }
        health_.fallback_reason =
            "present image not VK-owned (native VK composite required post item-8)";
        return false;
    }
    notOwnedStreak_ = 0;   // VK produced the present image this frame
    // Legacy handoff counters remain trip-wires: present == skip, copy1_blit == 0.
    ++health_.copy1_skip_frames;
    ++health_.gl_handoff_skip_frames;
    if (health_.gl_handoff_skip_frames == 1) {
        plog("present image is VK-only; copy-1 + GL->VK handoff deleted (item-8).");
    }

    // A failed rebuild retires the old chain and leaves none: never acquire from a null swapchain.
    if (!s.offscreen && !s.swapchain) {
        health_.fallback_reason = "no swapchain (an earlier rebuild failed)";
        return false;
    }
    ZoneNamedN(zTailRec, "VKIsl.Present.TailRecord", true);   // VK-ISLAND-TRACY-1: overlay/HUD/ImGui + blit record (to fn return)
    mc2::battlebench::NsScope benchTail(mc2::battlebench::PresTailNs);
    vkResetCommandPool(s.dev, s.tailPool[s.slot()].get(), 0);   // curCbuf() is s.cbufs[slot]
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.curCbuf(), &bi);

    // VK-PRESENT-SINGLESUBMIT-1: order the composite's color writes (executed as
    // the FIRST command buffer of this same submit) before any tail attachment
    // write. Needed by the overlay/HUD/ImGui render passes; the blit path is
    // covered by its own COLOR_ATTACHMENT->TRANSFER_SRC barrier below.
    {
        barrier(s.curCbuf(), s.sharedImg.image.get(),
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
        MC2_VK_GPU_SCOPE(gpuScope, s.curCbuf(), "VKIsl.Present.ShadowDebug");
        vkCmdBeginRenderPass(s.curCbuf(), &srp, VK_SUBPASS_CONTENTS_INLINE);
        const float quadSize = 256.0f;
        const float margin = 16.0f;
        VkViewport svp{margin, static_cast<float>(s.imgH) - quadSize - margin, quadSize, quadSize, 0.0f, 1.0f};
        VkRect2D ssc{{static_cast<int32_t>(margin), static_cast<int32_t>((static_cast<float>(s.imgH) - quadSize - margin))},
                     {static_cast<uint32_t>(quadSize), static_cast<uint32_t>(quadSize)}};
        vkCmdSetViewport(s.curCbuf(), 0, 1, &svp);
        vkCmdSetScissor(s.curCbuf(), 0, 1, &ssc);
        mc2vk::infra::bind(s.curCbuf(), VK_PIPELINE_BIND_POINT_GRAPHICS, s.shadowDebugPipe.get());
        vkCmdBindDescriptorSets(s.curCbuf(), VK_PIPELINE_BIND_POINT_GRAPHICS, s.shadowDebugLayout.get(),
                                0, 1, &s.shadowDebugSet, 0, nullptr);
        int spc[4] = { s.pendingShadowDebugLayer, 0, 0, 0 };
        vkCmdPushConstants(s.curCbuf(), s.shadowDebugLayout.get(), VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(spc), spc);
        vkCmdDraw(s.curCbuf(), 3, 1, 0, 0);
        vkCmdEndRenderPass(s.curCbuf());
        gpuScope.end();
        ++health_.shadow_debug_vk_frames;
    }
    s.pendingShadowDebugView = VK_NULL_HANDLE;
    // Motion-vector debug inset (Graphics Options): after the composite, before HUD/ImGui.
    if (s.showMotionVectorsDebug)
        (void)s.motionVectors.recordDebug(s.curCbuf(), s.sharedImg.view.get(), s.imgW, s.imgH);

    // VK-TAIL-HOOK-1: final native-tail seam. The composite left the shared image
    // in COLOR_ATTACHMENT layout; native HUD/ImGui commands attach here before the
    // present blit consumes the final image.
    {
        ++health_.tail_hook_frames;
        uint32_t hudColorDraws = 0;
        uint32_t hudTexturedDraws = 0;
        uint32_t hudTextureUploads = 0;
        uint32_t hudDraws = 0;
        { ZoneScopedN("VKIsl.Present.Tail.Hud"); mc2::battlebench::NsScope benchHud(mc2::battlebench::PresTailHudNs);
          hudDraws = recordPendingHudColorProof(s, s.curCbuf(), &hudColorDraws, &hudTexturedDraws, &hudTextureUploads); }
        if (hudDraws) {
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
        bool imguiDrawn = false;
        { ZoneScopedN("VKIsl.Present.Tail.ImGui"); mc2::battlebench::NsScope benchImgui(mc2::battlebench::PresTailImguiNs);
          imguiDrawn = recordPendingImgui(s, s.curCbuf()); }
        if (imguiDrawn)
            ++health_.imgui_vk_frames;
    }

    // The Render ticket covers every frame allocation and query, independently of the later WSI blit.
    { ZoneScopedN("VKIsl.Present.Tail.Blit.ProfileEnd"); gpuprof::recordFrameEnd(s.curCbuf()); }
    if (vkEndCommandBuffer(s.curCbuf()) != VK_SUCCESS) {
        health_.fallback_reason = "tail command buffer end failed";
        return false;
    }
    VkCommandBuffer commands[] = {s.curCompositeCbuf(), s.curCbuf()};
    VkSubmitInfo tail{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    tail.commandBufferCount = 2;
    tail.pCommandBuffers = commands;
    const VkFence tailFence = s.curInFlight().get();
    vkResetFences(s.dev, 1, &tailFence);
    s.compositePending = false;
    VkResult result;
    { ZoneScopedN("VKIsl.Present.Submit");
      mc2::battlebench::NsScope benchSubmit(mc2::battlebench::PresTailSubmitNs);
      result = s.ctx->submit(tail, tailFence); } // A fence forces the deferred prefix onto the queue before seal.
    if (result != VK_SUCCESS) {
        s.ctx->waitIdle("present_tail_failed");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        mc2vk::create(s.curInFlight(), s.dev, [&](auto* h) { return vkCreateFence(s.dev, &fci, nullptr, h); });
        health_.fallback_reason = "Render tail submit failed";
        return false;
    }
    gpuprof::frameEndSubmitted();
    s.spatialAA.submitted();
    if (verifyReadback) verifyReadbackContent();
    ++health_.present_frames;

    // Capture everything before frameBoundary advances the slot. Only these handles and worker-owned WSI result survive seal.
    const uint32_t slot = s.slot();
    const VkDevice dev = s.dev;
    VulkanContext* const ctx = s.ctx;
    const bool offscreen = s.offscreen;
    const VkSwapchainKHR swapchain = s.swapchain.get();
    const VkQueue presentQueue = s.presentQ;
    const auto images = s.scImages;
    std::vector<VkSemaphore> rendered;
    for (const auto& sem : s.renderSems) rendered.push_back(sem.get());
    const VkSemaphore acquireSem = s.acquireSems[slot].get();
    const VkFence blitFence = s.blitFences[slot].get();
    const VkCommandBuffer blitCb = s.blitCbufs[slot];
    const VkImage sharedImage = s.sharedImg.image.get();
    const uint32_t imgW = s.imgW, imgH = s.imgH;
    const VkExtent2D extent = s.scExtent;
    // Blit resources are not in the Render ticket. One-slot reuse still waits their own last use.
    if (!present_thread::detail::prepareBlitSlot(slot, [&](uint32_t) {
        return vkWaitForFences(dev, 1, &blitFence, VK_TRUE, 5ull * 1000 * 1000 * 1000) == VK_SUCCESS;
    }, [&](uint32_t capturedSlot) {
        return vkResetCommandPool(dev, s.blitPools[capturedSlot].get(), 0) == VK_SUCCESS;
    })) {
        health_.fallback_reason = "blit slot completion/reset failed";
        return false;
    }
    return present_thread::detail::afterRenderTail([&] { return finishFrame(); }, [] { present_thread::seal(); }, [&, dev, ctx, offscreen, swapchain, presentQueue,
        images, rendered, acquireSem, blitFence, blitCb, sharedImage, imgW, imgH, extent] {
    // Acquire leaves the fence signaled until submit, so declines can retry.
    uint32_t imageIndex = 0;
    VkResult r;
    { ZoneScopedN("VKIsl.Present.Acquire");   // VK-ISLAND-TRACY-1: vkAcquireNextImageKHR (can block on WSI)
      mc2::battlebench::NsScope benchAcquire(mc2::battlebench::PresAcquireNs, 0);
      const auto acqT0 = std::chrono::steady_clock::now();
      r = offscreen ? VK_SUCCESS : vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, acquireSem, VK_NULL_HANDLE, &imageIndex);
      mc2_hitch2::noteAcquire(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - acqT0).count())); }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        s.lastWsiResult = r; // Rebuild only after the next Render handoff.
        return true;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        if (r == VK_ERROR_DEVICE_LOST) mc2vk::infra::deviceLost();
        health_.fallback_reason = "acquire failed (" + std::to_string(static_cast<int>(r)) + ")";
        return false;
    }
    const VkImage scImg = images[imageIndex];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(blitCb, &begin) != VK_SUCCESS) {
        health_.fallback_reason = "blit command buffer begin failed";
        return false;
    }
    { ZoneScopedN("VKIsl.Present.Tail.Blit");
      mc2::battlebench::NsScope benchBlit(mc2::battlebench::PresTailBlitNs);
    { ZoneScopedN("VKIsl.Present.Tail.Blit.BarriersAndBlit");
    // The composite's color writes precede the swapchain blit.
    barrier(blitCb, sharedImage,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    if (!offscreen) {
    // The acquire wait and this barrier both use TRANSFER, chaining WSI's image availability to the blit.
    barrier(blitCb, scImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[0] = {0, 0, 0};
    region.srcOffsets[1] = {static_cast<int32_t>(imgW), static_cast<int32_t>(imgH), 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    // The native composite has a bottom-left origin; WSI has a top-left origin.
    region.dstOffsets[0] = {0, static_cast<int32_t>(extent.height), 0};
    region.dstOffsets[1] = {static_cast<int32_t>(extent.width), 0, 1};
    vkCmdBlitImage(blitCb,
                   sharedImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   scImg,             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region, VK_FILTER_LINEAR);

    // swapchain -> PRESENT_SRC.
    barrier(blitCb, scImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    // Return the shared image to COLOR_ATTACHMENT for the next native composite.
    barrier(blitCb, sharedImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    }
    if (vkEndCommandBuffer(blitCb) != VK_SUCCESS) {
        health_.fallback_reason = "blit command buffer end failed";
        return false;
    }
    const VkSemaphore renderSem = offscreen ? VK_NULL_HANDLE : rendered[imageIndex];
    vkResetFences(dev, 1, &blitFence);
    r = ctx->submitPresentTail(blitCb, offscreen ? VK_NULL_HANDLE : acquireSem, renderSem, blitFence);
    if (r != VK_SUCCESS) {
        // No replacement fence can retire the earlier Render tail. Stop WSI and let main drain the worker then idle before destruction.
        // The acquired semaphore is deliberately not reused; teardown destroys it after the idle.
        health_.fallback_reason = "blit submit failed";
        return false;
    }
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderSem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &imageIndex;
    { ZoneScopedN("VKIsl.Present.QueuePresent");
      mc2::battlebench::NsScope benchPresent(mc2::battlebench::PresQueuePresentNs);
      s.lastWsiResult = offscreen ? VK_SUCCESS : ctx->present(presentQueue, pi); }
    return true;
    });
}

// Readback of the shared image (strided checksum, optional PPM dump, capture and screenshot delivery). Never blocks: the copy is requested at one present
// and consumed at a later one through the readback ring, and while a request is in flight no second one is made. Only called when verifyReadback is set.
void EnginePresentVk::verifyReadbackContent() {
    Impl& s = *impl_;
    mc2vk::VulkanContext& ctx = *s.ctx;
    // First-five-frame proofs need consecutive images. Normal readbacks stay
    // asynchronous; this explicit capture mode retires each copy in its frame.
    const bool consecutive = mc2::gate::get<"MC2_SCREENSHOT_AT_FRAME">() == -5 && gos::screenshot::pending();

    // ---- consume the outstanding request, if its copy has finished ----
    auto consume = [&](const unsigned char* px, uint32_t rw, uint32_t rh, uint64_t readbackTag) {
        const VkDeviceSize needed = static_cast<VkDeviceSize>(rw) * rh * 4u;
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
        // A frame pin captures once at or after N; otherwise overwrite each verify.
        {
            if (mc2::gate::isNonEmpty<"MC2_VK_PRESENT_DUMP_FRAME">()) {
                const unsigned long pin = static_cast<unsigned long>(mc2::gate::get<"MC2_VK_PRESENT_DUMP_FRAME">());
                if (s_ppmDone || static_cast<unsigned long>(s.captureFrame) < pin)
                    ppmPath = nullptr; // not yet at the pinned frame, or already captured
            } else if (mc2::gate::get<"MC2_VK_PRESENT_DUMP_ON_FIRE">()) {
                // The fire fixture arms a mid-volley frame; zero suppresses capture.
                if (s_ppmDone || s.captureFireFrame == 0 ||
                    s.captureFrame < s.captureFireFrame)
                    ppmPath = nullptr;
            }
        }
        if (ppmPath) {
            FILE* pf = fopen(ppmPath, "wb");
            if (pf) {
                mc2::fmt::print(pf, "P6\n{:d} {:d}\n255\n", mc2::fmt::as_u32(rw), mc2::fmt::as_u32(rh));
                std::vector<unsigned char> rgb(static_cast<size_t>(rw) * rh * 3u);
                for (size_t i = 0, o = 0; i < static_cast<size_t>(rw) * rh; ++i) {
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
    // A capture or screenshot takes only a readback requested at or after the frame it was asked in; an older in-flight one shows an earlier
    // frame than its sidecar. The request stays pending and the next readback (requested right after this one is consumed) serves it.
    if (gos::visual_capture::vkCapturePending() && gos::screenshot::Slot::acceptsReadback(gos::visual_capture::vkCapturePendingSince(), readbackTag))
        gos::visual_capture::vkCaptureDeliver(rw, rh, px);
    if (gos::screenshot::pending() && gos::screenshot::Slot::acceptsReadback(gos::screenshot::pendingSince(), readbackTag))
        gos::screenshot::deliver(rw, rh, px);

    health_.prev_readback_checksum = health_.last_readback_checksum;
    health_.last_readback_checksum = h;
    if (health_.prev_readback_checksum != 0 &&
        health_.prev_readback_checksum != health_.last_readback_checksum) {
        health_.readback_changed = true;
    }
    };
    if (s.readbackToken) {
        bool stale = false;
        const void* bytes = consecutive
            ? ctx.readbacks().wait(ctx, s.readbackToken, s.readbackTag, 5000000000ull)
            : ctx.readbacks().poll(ctx, s.readbackToken, s.readbackTag, &stale);
        if (bytes) {
            consume(static_cast<const unsigned char*>(bytes), s.readbackW, s.readbackH, s.readbackTag);
            ctx.readbacks().release(ctx, s.readbackToken);
            s.readbackToken = {};
        } else if (stale) {
            s.readbackToken = {};   // recycled or for another frame: discarded, never used
        } else {
            return;                 // still in flight
        }
    }

    // ---- request the next one ----
    const uint32_t rw = s.imgW, rh = s.imgH;
    const VkDeviceSize needed = static_cast<VkDeviceSize>(rw) * rh * 4u;
    if (needed == 0) return;
    const uint64_t tag = s.captureFrame;
    VkBuffer dst = VK_NULL_HANDLE;
    const mc2vk::ReadbackToken tok = ctx.readbacks().request(ctx, needed, tag, &dst);
    if (!tok) return;
    if (!s.readbackCbuf) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = s.cpool.get(); cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(s.dev, &cai, &s.readbackCbuf) != VK_SUCCESS) { ctx.readbacks().release(ctx, tok); return; }
    }
    vkResetCommandPool(s.dev, s.cpool.get(), 0);   // cpool holds only readbackCbuf
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s.readbackCbuf, &bi);
    // shared image is in COLOR_ATTACHMENT (post-present barrier) -> TRANSFER_SRC.
    barrier(s.readbackCbuf, s.sharedImg.image.get(),
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // A recycled slot: its last copy (TRANSFER_WRITE) and the host read of it are ordered before this copy on the queue as well, so the reuse
    // never relies on the host wait alone (the ring recycles only after a host sync point, see ReadbackRing).
    VkBufferMemoryBarrier reuse{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    reuse.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT; reuse.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    reuse.srcQueueFamilyIndex = reuse.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    reuse.buffer = dst; reuse.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(s.readbackCbuf, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &reuse, 0, nullptr);
    VkBufferImageCopy bic{};
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {rw, rh, 1};
    vkCmdCopyImageToBuffer(s.readbackCbuf, s.sharedImg.image.get(),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &bic);
    // Make the copy visible to the host read after the fence wait (model: vk_scene_readback.cpp).
    VkBufferMemoryBarrier toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    toHost.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; toHost.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = dst; toHost.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(s.readbackCbuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &toHost, 0, nullptr);
    barrier(s.readbackCbuf, s.sharedImg.image.get(),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    vkEndCommandBuffer(s.readbackCbuf);

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &s.readbackCbuf;
    uint64_t value = 0;
    if (ctx.submit(si, VK_NULL_HANDLE, &value) != VK_SUCCESS) { ctx.readbacks().release(ctx, tok); return; }
    ctx.readbacks().seal(tok, mc2vk::GpuTicket{value, 0});
    s.readbackToken = tok;
    s.readbackTag = tag;
    s.readbackW = rw;
    s.readbackH = rh;
    if (consecutive) {
        if (const void* bytes = ctx.readbacks().wait(ctx, tok, tag, 5000000000ull)) {
            consume(static_cast<const unsigned char*>(bytes), rw, rh, tag);
            ctx.readbacks().release(ctx, tok);
            s.readbackToken = {};
        }
    }
}

void EnginePresentVk::drainReadbackBeforeImageDestroy() {
    if (!impl_ || !impl_->readbackToken) return;
    Impl& s = *impl_;
    mc2vk::VulkanContext& ctx = *s.ctx;
    ctx.wait(ctx.readbacks().ticketOf(s.readbackToken), 5ull * 1000 * 1000 * 1000, "present_resize_readback");
    if (!ctx.isComplete(ctx.readbacks().ticketOf(s.readbackToken))) ctx.waitIdle("present_resize_readback_timeout");
    ctx.readbacks().release(ctx, s.readbackToken);
    s.readbackToken = {};
}

void EnginePresentVk::destroy() {
    if (impl_) {
        Impl& s = *impl_;
        if (s.dev) {
            s.ctx->waitIdle("present_destroy");
            if (s.readbackToken) { s.ctx->readbacks().release(*s.ctx, s.readbackToken); s.readbackToken = {}; }   // idle: its copy is done, the slot rejoins the pool
        }
        shutdownImguiVulkanRenderer();
        if (s.dev) s.ctx->drainDeferred("present_destroy");   // retired HUD mirrors name this object descriptor pool
        // VK-PRESENT-SINGLESUBMIT-1: compositeCbuf is pool-allocated (freed with the pool); just drop the pending state.
        for (auto& cb : s.compositeCbufs) cb = VK_NULL_HANDLE;
        s.compositePending = false;
        s.pendingShadowDebugView = VK_NULL_HANDLE;
        // Every owned object (pipelines, layouts, buffers and their memory, the swapchain, the surface, the command pool) is released by ~Impl, in
        // reverse declaration order. The device and instance belong to the engine context and are never destroyed here.
        delete impl_;
        impl_ = nullptr;
    }
    // P13b: window_ is BORROWED (g_sdl_window, owned by gos_render). Never
    // destroy it here -- just drop our reference.
    window_ = nullptr;
    created_ = false;
}

void EnginePresentVk::logHealth() const {
    mc2::fmt::print_err("[VK_PRESENT] created={:d} attempted={:d} present_frames={:d} resize_count={:d} resize_recreate_ok={:d} resize_recreate_retries={:d} resize_recreate_failsoft={:d} validation_errors={:d} extent={:d}x{:d} present_mode={} readback_changed={:d} last_checksum=0x{:x} native_composite_requests={:d} native_composite_grants={:d} copy1_blit_frames={:d} copy1_skip_frames={:d} gl_handoff_skip_frames={:d} tail_hook_frames={:d} composite_vk_frames={:d} shadow_debug_vk_frames={:d} hud_vk_queued_frames={:d} hud_vk_consumed_frames={:d} hud_vk_draws_last={:d} hud_vk_textured_last={:d} hud_vk_serialized_last={:d} hud_vk_color_frames={:d} hud_vk_color_draws_last={:d} hud_vk_textured_frames={:d} hud_vk_textured_draws_last={:d} hud_vk_texture_uploads={:d} imgui_vk_ready={:d} imgui_vk_frames={:d} native_composite_armed={:d} composite_wait_us_last={:.1f} composite_wait_us_avg={:.1f} composite_wait_samples={:d} fallback_reason=\"{}\"\n",
        mc2::fmt::as_i32(health_.created ? 1 : 0), mc2::fmt::as_i32(health_.attempted ? 1 : 0),
        mc2::fmt::as_u32(health_.present_frames), mc2::fmt::as_u32(health_.resize_count),
        mc2::fmt::as_u32(health_.resize_recreate_ok), mc2::fmt::as_u32(health_.resize_recreate_retries),
        mc2::fmt::as_u32(health_.resize_recreate_failsoft), mc2::fmt::as_u32(health_.validation_errors),
        mc2::fmt::as_u32(health_.extent_w), mc2::fmt::as_u32(health_.extent_h),
        health_.present_mode_name.empty() ? "?" : health_.present_mode_name.c_str(),
        mc2::fmt::as_i32(health_.readback_changed ? 1 : 0),
        mc2::fmt::as_u64(static_cast<unsigned long long>(health_.last_readback_checksum)),
        mc2::fmt::as_u32(health_.native_composite_requests), mc2::fmt::as_u32(health_.native_composite_grants),
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
        mc2::fmt::as_u32(health_.native_composite_armed),
        health_.composite_wait_us_last,
        health_.composite_wait_samples
            ? health_.composite_wait_us_accum / static_cast<double>(health_.composite_wait_samples) : 0.0,
        mc2::fmt::as_u32(health_.composite_wait_samples),
        health_.fallback_reason.empty() ? "" : health_.fallback_reason.c_str());
    std::fflush(stderr);
}

EnginePresentVk& getOrCreateEnginePresentVk() {
    present_thread::waitSealed();
    mc2::overlap::assertOwner(mc2::overlap::Owner::Render, "getOrCreateEnginePresentVk");
    static EnginePresentVk g_enginePresent;
    return g_enginePresent;
}

}  // namespace mc2vk

