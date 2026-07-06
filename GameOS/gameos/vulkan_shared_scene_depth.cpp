// vulkan_shared_scene_depth.cpp — VK-TIER1 SHARED SCENE DEPTH substrate impl.
// =============================================================================
// Builds a VK exportable D32_SFLOAT_S8_UINT VkImageResource on the SHARED engine
// Vulkan context and imports it into GL as a GL_DEPTH24_STENCIL8 texture, reusing
// the proven vk_image_resource / gl_interop_view primitives (the exact import
// path the main_d32s8_depthtest spike used). GL-produces / VK-imports direction
// (inverse of the shadow island). This slice ONLY produces the shared backing
// image; there is NO VK depth consumer yet.
//
// Whole TU is inert unless MC2_VULKAN_ISLAND is defined (island discipline). It
// also needs the shared engine context, which lives behind MC2_VULKAN — the
// island TUs are compiled with BOTH defined (see CMakeLists island block).
// =============================================================================

#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#define MC2_VULKAN_SHARED_SCENE_DEPTH_IMPL_ISLAND
#include "vulkan_shared_scene_depth.h"

#include <volk.h>
#include <GL/glew.h>

#include "vk_image_resource.h"
#include "gl_interop_view.h"
#include "vulkan_context.h"

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <new>

#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif

// Opaque owner: the VK-owned exportable image + its GL import adapter. Substituted
// for sceneDepthTex_ via view.glTex; both are torn down together on destroy/resize.
struct Mc2SharedSceneDepth {
    VkImageResource res{};      // exportable D32_SFLOAT_S8_UINT
    GlInteropView   view{};     // GL_DEPTH24_STENCIL8 import over res
    VkDevice        device = VK_NULL_HANDLE;  // cached for destroy
    bool            resValid  = false;
    bool            viewValid = false;
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
    std::printf("[VK_SHARED_SCENE_DEPTH] %s\n", buf);
    std::fflush(stdout);
}
} // namespace

Mc2SharedSceneDepth* mc2SharedSceneDepthCreate(int w, int h, GLuint* outGlTex,
                                               const char** outReason) {
    auto bail = [&](const char* reason) -> Mc2SharedSceneDepth* {
        if (outReason) *outReason = reason;
        slog("fallback reason=%s (using plain GL GL_DEPTH24_STENCIL8)", reason);
        return nullptr;
    };
    if (w <= 0 || h <= 0 || !outGlTex) return bail("bad_args");

    // Shared engine VK context, on-demand + idempotent (works on the GL-present
    // path too). Fail-soft coercion to GL if the context cannot be born.
    if (!mc2vk::ensureEngineVulkanContextForInterop())
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

    // FAR-BAND-BLACK-1 FIX: prefer VK_FORMAT_D24_UNORM_S8_UINT -- this is a
    // bit-exact match for GL_DEPTH24_STENCIL8 (24-bit UNORM depth + 8-bit
    // stencil packed the same way both APIs define it), so GL's per-frame
    // glClear(GL_DEPTH_BUFFER_BIT)/glClearDepth(0.0f) and any GL-side depth
    // write land as the SAME bits VK's terrain-island GEQUAL depth test reads.
    // The prior D32_SFLOAT_S8_UINT export interpreted those same GL-written
    // UNORM24 bits as an IEEE-754 float32 depth on the VK side -- correct only
    // for the exact bit pattern 0x00000000 (GL clear-to-0.0), and GARBAGE for
    // every other depth GL or a prior VK frame wrote, so the terrain island's
    // GEQUAL depth test failed almost everywhere on any large camera jump
    // (bookmark teleport / hard cut), producing an almost-empty co-written
    // frame (root cause of FAR-BAND-BLACK: overview_center/oblique_horizon
    // mean~32-34/ch cov~97% GL-vs-VK divergence, isolated via
    // MC2_VK_TERRAIN_TIER1=0 A/B -- terrain returns instantly with props/mech
    // Tier-1 co-write left ON). Falls back to the old D32_SFLOAT_S8_UINT
    // export if the driver won't export D24_UNORM_S8_UINT (fail-soft, keeps
    // today's behavior on GPUs that don't support it as exportable).
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormat chosenFmt = VK_FORMAT_D24_UNORM_S8_UINT;
    bool created = vkImageResourceCreate(&d->res, device, phys,
                                         (uint32_t)w, (uint32_t)h,
                                         chosenFmt, usage, /*exportable*/ true);
    if (!created) {
        slog("D24_UNORM_S8_UINT not exportable on this device -- falling back "
             "to D32_SFLOAT_S8_UINT (bit-reinterpretation risk; see "
             "FAR-BAND-BLACK-1 comment above).");
        chosenFmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
        created = vkImageResourceCreate(&d->res, device, phys,
                                        (uint32_t)w, (uint32_t)h,
                                        chosenFmt, usage, /*exportable*/ true);
    }
    if (!created) {
        mc2SharedSceneDepthDestroy(d);
        return bail("vk_image_resource_create_failed");
    }
    d->resValid = true;

    // Import into GL as GL_DEPTH24_STENCIL8 (LUID match runs inside). When
    // chosenFmt is D24_UNORM_S8_UINT this is a byte-exact reinterpretation (no
    // value-conversion risk). When it fell back to D32_SFLOAT_S8_UINT this
    // keeps the pre-fix behavior (P5-T0 spike proved the DEPTH aspect samples
    // exactly for that case too, just not depth-test-safe across cross-API
    // writes -- see the FAR-BAND-BLACK-1 comment above). GL wrap/filter
    // default to CLAMP/NEAREST inside the view; the engine's own
    // glTexParameteri calls at the seam re-assert the same state.
    if (!glInteropViewCreate(&d->view, &d->res, phys, GL_DEPTH24_STENCIL8)) {
        mc2SharedSceneDepthDestroy(d);
        return bail("gl_interop_view_create_failed");
    }
    d->viewValid = true;

    if (d->view.glTex == 0) {
        mc2SharedSceneDepthDestroy(d);
        return bail("imported_gl_tex_zero");
    }

    *outGlTex = d->view.glTex;
    if (outReason) *outReason = "active";
    g_activeSharedDepth = d;
    slog("active: VK %s %dx%d imported to GL_DEPTH24_STENCIL8 tex=%u",
         (chosenFmt == VK_FORMAT_D24_UNORM_S8_UINT) ? "D24S8" : "D32S8(fallback)",
         w, h, (unsigned)d->view.glTex);
    return d;
}

void mc2SharedSceneDepthDestroy(Mc2SharedSceneDepth* d) {
    if (!d) return;
    if (g_activeSharedDepth == d) g_activeSharedDepth = nullptr;
    if (d->viewValid) glInteropViewDestroy(&d->view);
    if (d->resValid && d->device != VK_NULL_HANDLE)
        vkImageResourceDestroy(&d->res, d->device);
    delete d;
}

// ---- Island-facing accessors ------------------------------------------------
VkImageResource* mc2SharedSceneDepthGetResource() {
    Mc2SharedSceneDepth* d = g_activeSharedDepth;
    return (d && d->resValid) ? &d->res : nullptr;
}

GlInteropView* mc2SharedSceneDepthGetGlView() {
    Mc2SharedSceneDepth* d = g_activeSharedDepth;
    return (d && d->viewValid) ? &d->view : nullptr;
}

#endif // MC2_VULKAN_ISLAND
