// vulkan_shared_scene_color.cpp — VK-P11 TIER-1 SHARED SCENE COLOR/NORMAL impl.
// =============================================================================
// Builds a VK-ONLY (non-exportable) R16G16B16A16_SFLOAT (or R32_UINT object-id)
// VkImageResource on the SHARED engine Vulkan context. The VK terrain/prop/mech/
// water islands co-write the scene color(MRT0)/normal(MRT1)/object-id(MRT2) into
// these SAME images in place (direct-chain, no GL<->VK handoff); the GL scene never
// renders into them. There is NO GL import — the FBO color attachments are plain-GL
// placeholders (gos_postprocess.cpp).
//
// The active owners are held in a small file-static registry keyed by slot so the
// VK islands can fetch the SAME image and build a LOAD color attachment over it.
//
// VK-RUNG8-ITEM5 (GL-import-half deletion): the former exportable/glInteropView
// half is gone. This substrate arms solely under the full native-present stack
// (mc2VkSharedSceneDirectResourceEnabled); otherwise gos_postprocess declines the
// whole shared scene to pure GL.
//
// Whole TU is inert unless MC2_VULKAN_ISLAND is defined (island discipline). It also
// needs the shared engine context (MC2_VULKAN) — island TUs are compiled with BOTH.
// =============================================================================

#ifdef MC2_VULKAN_ISLAND

#if defined(_WIN32) && !defined(VK_USE_PLATFORM_WIN32_KHR)
#define VK_USE_PLATFORM_WIN32_KHR
#endif

// This TU is BOTH the GL-facing producer AND the island-facing registry, so it
// defines the island accessors — pull in their declarations.
#define MC2_VULKAN_SHARED_SCENE_COLOR_IMPL_ISLAND
#include "vulkan_shared_scene_color.h"

#include <volk.h>

#include "vk_first_layout.h"
#include "vk_image_resource.h"
#include "vulkan_context.h"
#include "Gates/render_options.h"

#include <cstdio>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <new>
#include "mc2fmt.h"

// Opaque owner: the VK-only (non-exportable) image the islands share. No GL import
// — the FBO color attachment is a plain-GL placeholder owned by gos_postprocess.
struct Mc2SharedSceneColor {
    VkImageResource            res{};      // VK-only R16G16B16A16_SFLOAT / R32_UINT
    VkDevice                   device = VK_NULL_HANDLE;  // cached for destroy
    Mc2SharedSceneColorSlot    slot   = MC2_SHARED_SCENE_COLOR0;
    bool                       resValid  = false;
};

namespace {

// Active owners by slot (for the island to fetch the SAME shared image). Only one
// gosPostProcess FBO exists, so a flat by-slot registry is sufficient.
Mc2SharedSceneColor* g_activeBySlot[MC2_SHARED_SCENE_COLOR_SLOT_COUNT] = {nullptr, nullptr, nullptr, nullptr};
// Bumped on every create and destroy: a cached descriptor that names a shared view rewrites when it moves.
std::atomic<uint64_t> g_generation{0};

void slog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    mc2::fmt::print_out("[VK_SHARED_SCENE_COLOR] {}\n", mc2::fmt::safe(buf));
    std::fflush(stdout);
}

const char* slotName(Mc2SharedSceneColorSlot slot) {
    if (slot == MC2_SHARED_SCENE_NORMAL) return "normal(MRT1)";
    if (slot == MC2_SHARED_SCENE_OBJECT_ID) return "objectId(MRT2)";
    if (slot == MC2_SHARED_SCENE_OBJECT_MV) return "objectMotion";
    return "color(MRT0)";
}

VkFormat vkFormatForSlot(Mc2SharedSceneColorSlot slot) {
    // Scene consumers read RGB only; keep HDR range while halving color traffic.
    if (slot == MC2_SHARED_SCENE_COLOR0 && mc2::options::lowCost()) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(mc2vk::getOrCreateEngineVulkanContext().physicalDevice(), VK_FORMAT_B10G11R11_UFLOAT_PACK32, &props);
        constexpr auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                  VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((props.optimalTilingFeatures & required) == required) return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    }
    // Normals/classes are bounded [0,1]: halve MRT1 traffic on shared DDR4.
    if (slot == MC2_SHARED_SCENE_NORMAL && mc2::options::lowCost()) return VK_FORMAT_R8G8B8A8_UNORM;
    if (slot == MC2_SHARED_SCENE_OBJECT_MV) return VK_FORMAT_R16G16_SFLOAT;
    return slot == MC2_SHARED_SCENE_OBJECT_ID
        ? VK_FORMAT_R32_UINT
        : VK_FORMAT_R16G16B16A16_SFLOAT;
}

const char* debugNameForSlot(Mc2SharedSceneColorSlot slot) {
    if (slot == MC2_SHARED_SCENE_NORMAL) return "shared.scene.normal";
    if (slot == MC2_SHARED_SCENE_OBJECT_ID) return "shared.scene.objectId";
    if (slot == MC2_SHARED_SCENE_OBJECT_MV) return "shared.scene.objectMotion";
    return "shared.scene.color";
}

const char* formatNameForSlot(Mc2SharedSceneColorSlot slot) {
    if (vkFormatForSlot(slot) == VK_FORMAT_B10G11R11_UFLOAT_PACK32) return "R11G11B10F";
    if (slot == MC2_SHARED_SCENE_NORMAL && mc2::options::lowCost()) return "RGBA8";
    if (slot == MC2_SHARED_SCENE_OBJECT_MV) return "RG16F";
    return slot == MC2_SHARED_SCENE_OBJECT_ID ? "R32UI" : "RGBA16F";
}

} // namespace

Mc2SharedSceneColor* mc2SharedSceneColorCreate(Mc2SharedSceneColorSlot slot,
                                               int w, int h,
                                               const char** outReason) {
    auto bail = [&](const char* reason) -> Mc2SharedSceneColor* {
        if (outReason) *outReason = reason;
        slog("%s fallback reason=%s (would decline shared scene to pure GL)",
             slotName(slot), reason);
        return nullptr;
    };
    if (w <= 0 || h <= 0) return bail("bad_args");
    if (slot < 0 || slot >= MC2_SHARED_SCENE_COLOR_SLOT_COUNT) return bail("bad_slot");

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

    Mc2SharedSceneColor* c = new (std::nothrow) Mc2SharedSceneColor();
    if (!c) return bail("alloc_failed");
    c->device = device;
    c->slot   = slot;

    // VK-only GBuffer image: COLOR_ATTACHMENT (VK co-writes/samples it) + SAMPLED
    // (post/native consumers) + TRANSFER_SRC/DST (VK-side copies). No GL import.
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!vkImageResourceCreate(&c->res, device, phys,
                               static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                               vkFormatForSlot(slot),
                               usage)) {
        mc2SharedSceneColorDestroy(c);
        return bail("vk_image_resource_create_failed");
    }
    c->resValid = true;
    mc2vk::nameObject(device, c->res.image.get(), debugNameForSlot(slot));

    // Every island declares the attachment layout from its first frame, so the image must already be in it; the object-id image, which no island
    // writes, also starts at "no object". The object-motion image rests in SHADER_READ_ONLY between frames (the mech island's render pass discards
    // and leaves it there; the motion-vector pass samples it on every frame, also before the first mech pass ran), so it starts there, zeroed.
    const SharedImageStart start = slot == MC2_SHARED_SCENE_OBJECT_ID   ? SharedImageStart::ColorAttachmentCleared
                                   : slot == MC2_SHARED_SCENE_OBJECT_MV ? SharedImageStart::ShaderReadCleared
                                                                        : SharedImageStart::ColorAttachment;
    if (!mc2vkGiveSharedImageFirstLayout(ctx, c->res, start)) {
        mc2SharedSceneColorDestroy(c);
        return bail("first_layout_failed");
    }

    if (outReason) *outReason = "active_vk_only";
    g_activeBySlot[slot] = c;
    g_generation.fetch_add(1, std::memory_order_release);
    slog("%s active: VK-only %s %dx%d (no GL import)", slotName(slot),
         formatNameForSlot(slot), w, h);
    return c;
}

void mc2SharedSceneColorDestroy(Mc2SharedSceneColor* c) {
    if (!c) return;
    if (c->slot >= 0 && c->slot < MC2_SHARED_SCENE_COLOR_SLOT_COUNT &&
        g_activeBySlot[c->slot] == c) {
        g_activeBySlot[c->slot] = nullptr;
    }
    g_generation.fetch_add(1, std::memory_order_release);
    if (c->resValid && c->device != VK_NULL_HANDLE)
        vkImageResourceDestroy(&c->res, c->device);
    delete c;
}

// ---- Island-facing accessor -------------------------------------------------
VkImageResource* mc2SharedSceneColorGetResource(Mc2SharedSceneColorSlot slot) {
    if (slot < 0 || slot >= MC2_SHARED_SCENE_COLOR_SLOT_COUNT) return nullptr;
    Mc2SharedSceneColor* c = g_activeBySlot[slot];
    return (c && c->resValid) ? &c->res : nullptr;
}

unsigned long long mc2SharedSceneColorGeneration() { return g_generation.load(std::memory_order_acquire); }

#endif // MC2_VULKAN_ISLAND
