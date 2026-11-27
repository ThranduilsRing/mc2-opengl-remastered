// vulkan_shared_scene_depth.cpp — VK-TIER1 SHARED SCENE DEPTH substrate impl.
// =============================================================================
// Builds a VK-ONLY (non-exportable) D32_SFLOAT_S8_UINT VkImageResource on the
// SHARED engine Vulkan context. The VK terrain/prop/mech/water islands co-write
// scene depth into this SAME image in place (direct-chain, no GL<->VK handoff);
// the GL scene never renders into it. There is NO GL import: the FBO's GL depth
// attachment is a plain-GL placeholder (see gos_postprocess.cpp) — this owner
// only holds the VK image the islands share.
//
// VK-RUNG8-ITEM5 (GL-import-half deletion): the former exportable/glInteropView
// half is gone. This substrate is the VK-only allocation only; it arms solely
// under the full native-present stack (mc2VkSharedSceneDirectResourceEnabled),
// otherwise gos_postprocess declines the whole shared scene to pure GL.
//
// Built with the Vulkan renderer. It
// also needs the shared engine context, which lives behind MC2_VULKAN — the
// island TUs are compiled with BOTH defined (see CMakeLists island block).
// =============================================================================


#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"

#include <volk.h>

#include "vk_first_layout.h"
#include "vk_image_resource.h"
#include "vulkan_context.h"

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <new>
#include "mc2fmt.h"

// Opaque owner: the VK-owned (non-exportable) scene-depth image the islands share.
// No GL import — sceneDepthTex_ is a plain-GL placeholder owned by gos_postprocess.
struct Mc2SharedSceneDepth {
    VkImageResource res{};      // VK-only D32_SFLOAT_S8_UINT
    VkDevice        device = VK_NULL_HANDLE;  // cached for destroy
    bool            resValid  = false;
};

namespace {
// The single ACTIVE shared scene-depth owner (only one gosPostProcess FBO exists),
// exposed to the VK-P11 Tier-1 prop island via the accessors below so it can build a
// LOAD depth attachment over the SAME shared image.
Mc2SharedSceneDepth* g_activeSharedDepth = nullptr;

void slog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mc2::fmt::print_out("[VK_SHARED_SCENE_DEPTH] {}\n", mc2::fmt::safe(buf));
    std::fflush(stdout);
}
} // namespace

Mc2SharedSceneDepth* mc2SharedSceneDepthCreate(int w, int h, const char** outReason) {
    auto bail = [&](const char* reason) -> Mc2SharedSceneDepth* {
        if (outReason) *outReason = reason;
        slog("fallback reason=%s (would decline shared scene to pure GL)", reason);
        return nullptr;
    };
    if (w <= 0 || h <= 0) return bail("bad_args");

    // Shared engine VK context, on-demand + idempotent. Fail-soft: if it cannot be
    // born the caller declines the whole shared scene to pure GL.
    if (!mc2vk::ensureEngineVulkanContext())
        return bail("engine_vk_context_birth_failed");
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) return bail("engine_vk_context_not_valid");

    VkDevice         device = ctx.device();
    VkPhysicalDevice phys   = ctx.physicalDevice();
    if (device == VK_NULL_HANDLE || phys == VK_NULL_HANDLE)
        return bail("engine_vk_device_null");

    Mc2SharedSceneDepth* d = new (std::nothrow) Mc2SharedSceneDepth();
    if (!d) return bail("alloc_failed");
    d->device = device;

    // VK-only D32_SFLOAT_S8_UINT. There is no GL import, so the former
    // D24_UNORM_S8_UINT byte-exact-with-GL preference (FAR-BAND-BLACK-1) is moot:
    // GL never writes this image (the islands co-write it in VK in place), so the
    // VK-native float32 depth format is the correct and only choice.
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!vkImageResourceCreate(&d->res, device, phys,
                               static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                               VK_FORMAT_D32_SFLOAT_S8_UINT, usage)) {
        mc2SharedSceneDepthDestroy(d);
        return bail("vk_image_resource_create_failed");
    }
    d->resValid = true;
    mc2vk::nameObject(device, d->res.image.get(), "shared.scene.depth");

    // The islands declare DEPTH_STENCIL_ATTACHMENT_OPTIMAL from their first frame, so the image must already be in it.
    if (!mc2vkGiveSharedImageFirstLayout(ctx, d->res, SharedImageStart::DepthStencilAttachment)) {
        mc2SharedSceneDepthDestroy(d);
        return bail("first_layout_failed");
    }

    if (outReason) *outReason = "active_vk_only";
    g_activeSharedDepth = d;
    slog("active: VK-only D32S8 %dx%d (no GL import)", w, h);
    return d;
}

void mc2SharedSceneDepthDestroy(Mc2SharedSceneDepth* d) {
    if (!d) return;
    if (g_activeSharedDepth == d) g_activeSharedDepth = nullptr;
    if (d->resValid && d->device != VK_NULL_HANDLE)
        vkImageResourceDestroy(&d->res, d->device);
    delete d;
}

// ---- Island-facing accessor -------------------------------------------------
VkImageResource* mc2SharedSceneDepthGetResource() {
    Mc2SharedSceneDepth* d = g_activeSharedDepth;
    return (d && d->resValid) ? &d->res : nullptr;
}

