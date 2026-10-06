// render_backend_select.cpp
//
// VK-P13-SELECTOR-1 / T1 -- implementation of the memoized MC2_RENDER_BACKEND
// selector. GL-safe (no Vulkan headers); the whole TU compiles in the shipping
// GL-only build. The selector adds only a single memoized getenv on the =gl /
// unset path, so GL behavior is byte-identical.

#include "render_backend_select.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// VK-FAILSOFT-DISARM-1: defined in mclib/tgl.cpp (data-tool-link home, same as
// g_mc2FrameCounter); set below by forceGLFallback, read by
// mc2vkgate::umbrellaOn() (vk_shared_gbuffer_gate.h).
extern bool g_mc2VkSessionForcedGL;

namespace mc2 {

namespace {

// Cached backend. -1 = not yet resolved; 0 = GL; 1 = Vulkan.
int s_cached = -1;
// Session-sticky force-GL flag set by forceGLFallback(). Once true, renderBackend()
// always returns GL regardless of the cached env selection.
bool s_forceGL = false;

RenderBackend resolveOnce() {
    const char* env = std::getenv("MC2_RENDER_BACKEND");
    if (env == nullptr || env[0] == '\0') {
#ifdef MC2_VULKAN
        // 2026-10-05 VK DEFAULT FLIP: unset -> Vulkan on Vulkan-enabled builds.
        // Fail-soft to GL is unchanged (forceGLFallback) when no VK device/context.
        return RenderBackend::Vulkan;
#else
        return RenderBackend::GL;
#endif
    }
    if (std::strcmp(env, "gl") == 0 || std::strcmp(env, "GL") == 0) {
        return RenderBackend::GL;
    }
    if (std::strcmp(env, "vulkan") == 0 || std::strcmp(env, "VULKAN") == 0 ||
        std::strcmp(env, "vk") == 0) {
        std::printf("[RENDER_BACKEND] MC2_RENDER_BACKEND=%s -> Vulkan backend selected.\n", env);
        std::fflush(stdout);
        return RenderBackend::Vulkan;
    }
    // Unrecognized -> GL, log the coercion once.
    std::printf("[RENDER_BACKEND] MC2_RENDER_BACKEND=%s unrecognized (expected gl|vulkan); "
                "coercing to GL.\n", env);
    std::fflush(stdout);
    return RenderBackend::GL;
}

}  // namespace

RenderBackend renderBackend() {
    if (s_cached < 0) {
        s_cached = static_cast<int>(resolveOnce());
    }
    if (s_forceGL) return RenderBackend::GL;
    return static_cast<RenderBackend>(s_cached);
}

const char* renderBackendName(RenderBackend b) {
    return b == RenderBackend::Vulkan ? "vulkan" : "gl";
}

void forceGLFallback(const char* reason) {
    if (!s_forceGL) {
        std::printf("[RENDER_BACKEND] fail-soft: forcing GL for the session (reason: %s).\n",
                    reason ? reason : "(unspecified)");
        std::fflush(stdout);
    }
    s_forceGL = true;
    // VK-FAILSOFT-DISARM-1: disarm the shared-GBuffer umbrella family too.
    // Post-ITEM8 there is no per-layer GL fallback — if the content islands
    // stay armed while present runs GL, every VK-owned layer VANISHES (torn
    // grey scene with only residual GL-drawn VFX/decals; the S2b signature).
    // mc2vkgate::umbrellaOn() reads this flag per call, so islands that
    // consult their gate per frame fall back to the pure GL path.
    ::g_mc2VkSessionForcedGL = true;
}

}  // namespace mc2
