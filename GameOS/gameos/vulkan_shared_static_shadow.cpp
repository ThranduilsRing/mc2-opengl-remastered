#ifdef MC2_VULKAN_ISLAND

#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#define MC2_VULKAN_SHARED_STATIC_SHADOW_IMPL_ISLAND
#include "vulkan_shared_static_shadow.h"

#include <volk.h>
#include <GL/glew.h>

#include "vk_image_resource.h"
#include "gl_interop_view.h"
#include "vulkan_context.h"

#include <cstdarg>
#include <cstdio>
#include <new>

struct Mc2SharedStaticShadow {
    VkImageResource res{};
    GlInteropView   view{};
    VkDevice        device = VK_NULL_HANDLE;
    bool            resValid = false;
    bool            viewValid = false;
};

namespace {
Mc2SharedStaticShadow* g_activeStaticShadow = nullptr;

void slog(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::printf("[VK_SHARED_STATIC_SHADOW] %s\n", buf);
    std::fflush(stdout);
}
} // namespace

Mc2SharedStaticShadow* mc2SharedStaticShadowCreate(int w, int h, GLuint* outGlTex,
                                                   const char** outReason) {
    auto bail = [&](const char* reason) -> Mc2SharedStaticShadow* {
        if (outReason) *outReason = reason;
        slog("fallback reason=%s (using plain GL GL_DEPTH_COMPONENT24)", reason);
        return nullptr;
    };
    if (w <= 0 || h <= 0 || !outGlTex) return bail("bad_args");
    if (!mc2vk::ensureEngineVulkanContextForInterop())
        return bail("engine_vk_context_birth_failed");
    mc2vk::VulkanContext& ctx = mc2vk::getOrCreateEngineVulkanContext();
    if (!ctx.valid()) return bail("engine_vk_context_not_valid");

    VkDevice device = ctx.device();
    VkPhysicalDevice phys = ctx.physicalDevice();
    if (device == VK_NULL_HANDLE || phys == VK_NULL_HANDLE)
        return bail("engine_vk_device_null");

    Mc2SharedStaticShadow* s = new (std::nothrow) Mc2SharedStaticShadow();
    if (!s) return bail("alloc_failed");
    s->device = device;

    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!vkImageResourceCreate(&s->res, device, phys, (uint32_t)w, (uint32_t)h,
                               VK_FORMAT_D32_SFLOAT, usage, /*exportable*/ true)) {
        mc2SharedStaticShadowDestroy(s);
        return bail("vk_image_resource_create_failed");
    }
    s->resValid = true;

    if (!glInteropViewCreate(&s->view, &s->res, phys, GL_DEPTH_COMPONENT32F)) {
        mc2SharedStaticShadowDestroy(s);
        return bail("gl_interop_view_create_failed");
    }
    s->viewValid = true;

    if (s->view.glTex == 0) {
        mc2SharedStaticShadowDestroy(s);
        return bail("imported_gl_tex_zero");
    }

    *outGlTex = s->view.glTex;
    if (outReason) *outReason = "active";
    g_activeStaticShadow = s;
    slog("active: VK D32_SFLOAT %dx%d imported to GL_DEPTH_COMPONENT32F tex=%u",
         w, h, (unsigned)s->view.glTex);
    return s;
}

void mc2SharedStaticShadowDestroy(Mc2SharedStaticShadow* s) {
    if (!s) return;
    if (g_activeStaticShadow == s) g_activeStaticShadow = nullptr;
    if (s->viewValid) glInteropViewDestroy(&s->view);
    if (s->resValid && s->device != VK_NULL_HANDLE)
        vkImageResourceDestroy(&s->res, s->device);
    delete s;
}

VkImageResource* mc2SharedStaticShadowGetResource() {
    Mc2SharedStaticShadow* s = g_activeStaticShadow;
    return (s && s->resValid) ? &s->res : nullptr;
}

GlInteropView* mc2SharedStaticShadowGetGlView() {
    Mc2SharedStaticShadow* s = g_activeStaticShadow;
    return (s && s->viewValid) ? &s->view : nullptr;
}

#endif // MC2_VULKAN_ISLAND
