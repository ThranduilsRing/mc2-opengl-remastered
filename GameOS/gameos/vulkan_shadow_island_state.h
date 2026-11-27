// Private to vulkan_shadow_island_live.cpp: the health snapshot, readiness flags and forbidden-readback counters (file-size split, W116).
#pragma once
#include <cstring>

namespace {

struct ShadowIslandHealth {
    int   vulkanAvailable = 0;
    int   buildEnabled    = 1;   // this TU only compiles when the island is built
    char  deviceName[256] = {0};
    unsigned long validationErrors = 0;
    unsigned long attempted   = 0;
    unsigned long usedVulkan  = 0;
    char  fallbackReason[256] = {0};
    unsigned long vkDraws     = 0;
    int   directChain    = 0;
    unsigned long instRecords = 0;   // caster instances fed this frame
    unsigned long typeRuns    = 0;   // per-type instanced draws this frame
    unsigned long geomVerts   = 0;   // shared VBO verts copied once
    double stageInstUs = 0.0, drawUs = 0.0, totalUs = 0.0;
};
ShadowIslandHealth g_health;
// VK-SHADOW-DIRECT-CONSUME-1: true only between a successful mc2_vk_shadow_island_run()
// (fence-waited, layout-transitioned) and the next call to mc2_vk_shadow_island_run()
// (which resets it before doing any work) -- so a same-frame-later VK consumer (the
// terrain island) can tell "is this frame's depth ready right now" without racing a
// stale prior-frame flag into a later frame where the shadow island declined.
bool g_dynShadowReadyThisFrame = false;
// One value per frame (bumped by the first pass, the dynamic map): the mech casters stage their feed once per epoch and every later pass reuses it.
uint64_t g_shadowFrameEpoch = 0;

void setFallback(const char* reason) {
    std::strncpy(g_health.fallbackReason, reason ? reason : "",
                 sizeof(g_health.fallbackReason) - 1);
    g_health.fallbackReason[sizeof(g_health.fallbackReason) - 1] = '\0';
}

// Forbidden DEPTH-readback counters (zero-copy invariant). VK PRODUCES the shadow
// map; GL SAMPLES it in place. Nothing should ever pull the shadow-map bytes back
// to the CPU or copy them across a staging buffer. Any of these firing on the
// interop path means the zero-copy invariant was violated -> fail the frame.
struct InteropForbidden {
    unsigned long vkCmdCopyImageToBuffer = 0;  // shadow-map readback   (must be 0)
    unsigned long glGetTexImage          = 0;  // shadow-map CPU pull   (must be 0)
    unsigned long glReadPixels           = 0;  // shadow-map CPU pull   (must be 0)
};
InteropForbidden g_forbidden;

}  // namespace
