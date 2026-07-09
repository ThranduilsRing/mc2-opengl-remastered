// vulkan_shared_scene_color.cpp — VK-P11 TIER-1 SHARED SCENE COLOR/NORMAL impl.
// =============================================================================
// Builds an exportable R16G16B16A16_SFLOAT VkImageResource on the SHARED engine
// Vulkan context and imports it into GL as a GL_RGBA16F texture, reusing the proven
// vk_image_resource / gl_interop_view primitives (exactly like
// vulkan_shared_scene_depth, but color). GL-produces (renders terrain into the
// attachment) / VK-imports+co-writes (the prop island) direction.
//
// The active owners are held in a small file-static registry keyed by slot so the
// VK prop island (a sibling island TU) can fetch the SAME exportable image + its GL
// interop view and build a LOAD color attachment over it — the co-write substrate.
//
// Whole TU is inert unless MC2_VULKAN_ISLAND is defined (island discipline). It also
// needs the shared engine context (MC2_VULKAN) — island TUs are compiled with BOTH.
// =============================================================================

#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

// This TU is BOTH the GL-facing producer AND the island-facing registry, so it
// defines the island accessors — pull in their declarations.
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"

#include <volk.h>
#include <GL/glew.h>

#include "vk_image_resource.h"
#include "gl_interop_view.h"
#include "vulkan_context.h"

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <new>

// Opaque owner: the VK-owned exportable RGBA16F image + its GL import adapter.
// Substituted for sceneColorTex_/sceneNormalTex_ via view.glTex; torn down together
// on destroy/resize.
struct Mc2SharedSceneColor {
    VkImageResource            res{};      // exportable R16G16B16A16_SFLOAT
    GlInteropView              view{};     // GL_RGBA16F import over res
    VkDevice                   device = VK_NULL_HANDLE;  // cached for destroy
    Mc2SharedSceneColorSlot    slot   = MC2_SHARED_SCENE_COLOR0;
    bool                       resValid  = false;
    bool                       viewValid = false;
};

namespace {

// Active owners by slot (for the island to fetch the SAME shared image). Only one
// gosPostProcess FBO exists, so a flat by-slot registry is sufficient.
Mc2SharedSceneColor* g_activeBySlot[MC2_SHARED_SCENE_COLOR_SLOT_COUNT] = {nullptr, nullptr, nullptr};

void slog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("[VK_SHARED_SCENE_COLOR] %s\n", buf);
    std::fflush(stdout);
}

const char* slotName(Mc2SharedSceneColorSlot slot) {
    if (slot == MC2_SHARED_SCENE_NORMAL) return "normal(MRT1)";
    if (slot == MC2_SHARED_SCENE_OBJECT_ID) return "objectId(MRT2)";
    return "color(MRT0)";
}

VkFormat vkFormatForSlot(Mc2SharedSceneColorSlot slot) {
    return slot == MC2_SHARED_SCENE_OBJECT_ID
        ? VK_FORMAT_R32_UINT
        : VK_FORMAT_R16G16B16A16_SFLOAT;
}

GLenum glFormatForSlot(Mc2SharedSceneColorSlot slot) {
    return slot == MC2_SHARED_SCENE_OBJECT_ID ? GL_R32UI : GL_RGBA16F;
}

const char* formatNameForSlot(Mc2SharedSceneColorSlot slot) {
    return slot == MC2_SHARED_SCENE_OBJECT_ID ? "R32UI" : "RGBA16F";
}

} // namespace

Mc2SharedSceneColor* mc2SharedSceneColorCreate(Mc2SharedSceneColorSlot slot,
                                               int w, int h, GLuint* outGlTex,
                                               const char** outReason,
                                               bool vkOnlyNoGlView) {
    auto bail = [&](const char* reason) -> Mc2SharedSceneColor* {
        if (outReason) *outReason = reason;
        slog("%s fallback reason=%s (using plain GL glTexImage2D RGBA16F)",
             slotName(slot), reason);
        return nullptr;
    };
    if (w <= 0 || h <= 0 || !outGlTex) return bail("bad_args");
    if (slot < 0 || slot >= MC2_SHARED_SCENE_COLOR_SLOT_COUNT) return bail("bad_slot");

    // Shared engine VK context, on-demand + idempotent (works on the GL-present path
    // too). Fail-soft coercion to GL if it cannot be born.
    if (!mc2vk::ensureEngineVulkanContextForInterop())
        return bail("engine_vk_context_birth_failed");
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) return bail("engine_vk_context_not_valid");

    VkDevice         device = ctx.device();
    VkPhysicalDevice phys   = ctx.physicalDevice();
    if (device == VK_NULL_HANDLE || phys == VK_NULL_HANDLE)
        return bail("engine_vk_device_null");

    Mc2SharedSceneColor* c = new (std::nothrow) Mc2SharedSceneColor();
    if (!c) return bail("alloc_failed");
    c->device = device;
    c->slot   = slot;

    // Exportable GBuffer image: COLOR_ATTACHMENT (GL renders into it AND VK
    // co-writes/samples it) + SAMPLED (post/native consumers) + TRANSFER_SRC/DST
    // (the glCopyImageSubData scene-color snapshot path).
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!vkImageResourceCreate(&c->res, device, phys,
                               (uint32_t)w, (uint32_t)h,
                               vkFormatForSlot(slot),
                               usage, /*exportable*/ !vkOnlyNoGlView)) {
        mc2SharedSceneColorDestroy(c);
        return bail("vk_image_resource_create_failed");
    }
    c->resValid = true;

    if (vkOnlyNoGlView) {
        *outGlTex = 0;
        if (outReason) *outReason = "active_vk_only";
        g_activeBySlot[slot] = c;
        slog("%s active: VK-only %s %dx%d (no GL import)", slotName(slot),
             formatNameForSlot(slot), w, h);
        return c;
    }

    // Import into GL using the byte-compatible internal format for this slot.
    // GL wrap/filter default to CLAMP/NEAREST inside the view; the engine's own
    // glTexParameteri calls at the seam re-assert the same state.
    if (!glInteropViewCreate(&c->view, &c->res, phys, glFormatForSlot(slot))) {
        mc2SharedSceneColorDestroy(c);
        return bail("gl_interop_view_create_failed");
    }
    c->viewValid = true;

    if (c->view.glTex == 0) {
        mc2SharedSceneColorDestroy(c);
        return bail("imported_gl_tex_zero");
    }

    *outGlTex = c->view.glTex;
    if (outReason) *outReason = "active";
    g_activeBySlot[slot] = c;
    slog("%s active: VK %s %dx%d imported to GL_%s tex=%u", slotName(slot),
         formatNameForSlot(slot), w, h, formatNameForSlot(slot), (unsigned)c->view.glTex);
    return c;
}

bool mc2SharedSceneColorHasGlView(const Mc2SharedSceneColor* c) {
    return c && c->viewValid;
}

void mc2SharedSceneColorDestroy(Mc2SharedSceneColor* c) {
    if (!c) return;
    if (c->slot >= 0 && c->slot < MC2_SHARED_SCENE_COLOR_SLOT_COUNT &&
        g_activeBySlot[c->slot] == c) {
        g_activeBySlot[c->slot] = nullptr;
    }
    if (c->viewValid) glInteropViewDestroy(&c->view);
    if (c->resValid && c->device != VK_NULL_HANDLE)
        vkImageResourceDestroy(&c->res, c->device);
    delete c;
}

// ---- Island-facing accessors ------------------------------------------------
VkImageResource* mc2SharedSceneColorGetResource(Mc2SharedSceneColorSlot slot) {
    if (slot < 0 || slot >= MC2_SHARED_SCENE_COLOR_SLOT_COUNT) return nullptr;
    Mc2SharedSceneColor* c = g_activeBySlot[slot];
    return (c && c->resValid) ? &c->res : nullptr;
}

GlInteropView* mc2SharedSceneColorGetGlView(Mc2SharedSceneColorSlot slot) {
    if (slot < 0 || slot >= MC2_SHARED_SCENE_COLOR_SLOT_COUNT) return nullptr;
    Mc2SharedSceneColor* c = g_activeBySlot[slot];
    return (c && c->viewValid) ? &c->view : nullptr;
}

#endif // MC2_VULKAN_ISLAND
