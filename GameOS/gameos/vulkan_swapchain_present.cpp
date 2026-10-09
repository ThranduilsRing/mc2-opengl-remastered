// VULKAN-SWAPCHAIN-PRESENT-1 -- Layer-5 milestone: the engine OWNS a Vulkan
// swapchain and PRESENTS a controlled frame safely, fail-soft to GL.
//
// L6.6-COMPOSITE-PRESENT-SCAFFOLD (2026-07-02): the present MACHINERY that used
// to live inline here is now the reusable, PERSISTENT mc2vk::VulkanPresentObject
// (vulkan_present_object.{h,cpp}). This TU now:
//   (1) DRIVES that object for the 16-frame + mid-loop-resize probe
//       (mc2_vulkan_probe_swapchain_present) -- the probe still passing is the
//       regression check that the extraction is faithful (per-image render
//       semaphores + OUT_OF_DATE/SUBOPTIMAL rebuild preserved VERBATIM); and
//   (2) exposes mc2_vulkan_composite_present_scaffold() -- the CompositePresent
//       TEST region's driver: init the persistent object -> present N frames ->
//       a resize -> destroy, to its OWN separate Vulkan window.
//
// NARROW SCOPE (unchanged, supervisor ruling). This TU creates a SEPARATE visible
// SDL Vulkan window (distinct from the GL window g_sdl_window / RenderWindow::
// window_) that never shares the GL context. It does NOT migrate the game
// renderer, does NOT make Vulkan default, does NOT touch the GL swap path /
// FBO-0, and routes NO game render pass through the swapchain. Full game-window
// Vulkan present is BLOCKED on L7 shared-image interop / ownership migration
// (ledgered). Whole TU compiles to nothing unless MC2_VULKAN is defined -> the
// GL build is byte-identical. Fail-soft at every step (log + return false,
// never crash -- this is also the no-Vulkan-runtime path).

#include "vulkan_backend_skeleton.h"

#ifdef MC2_VULKAN

#include "vulkan_present_object.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

void sp_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[VULKAN_SKELETON] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

const char* sp_format_str(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_SRGB:  return "B8G8R8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB:  return "R8G8B8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        default:                       return "other";
    }
}

const char* sp_present_mode_str(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default:                               return "other";
    }
}

// Optional debug_state dump of the health line (best-effort; never fatal).
void maybe_dump_debug_state(const mc2vk::PresentHealth& h, const char* fname) {
    const char* dumpEnv = std::getenv("MC2_DEBUG_STATE_DUMP");
    if (!(dumpEnv && dumpEnv[0] && dumpEnv[0] != '0')) return;
    std::FILE* f = std::fopen(fname, "w");
    if (!f) return; // debug_state dir may not exist; best-effort only.
    std::fprintf(f,
        "swapchain_available=%d surface_created=%d format=%s present_mode=%s "
        "image_count=%u extent=%ux%u resize_count=%u present_frames=%u "
        "fallback_reason=%s validation_errors=%u\n",
        h.swapchain_available ? 1 : 0, h.surface_created ? 1 : 0,
        sp_format_str(h.format), sp_present_mode_str(h.present_mode),
        h.image_count, h.extent_w, h.extent_h, h.resize_count, h.present_frames,
        h.fallback_reason.empty() ? "(none)" : h.fallback_reason.c_str(),
        h.validation_errors);
    std::fclose(f);
}

// Shared driver: stand up the persistent object, present `frames` frames,
// forcing a resize at `resizeAtFrame` (SDL_SetWindowSize -> next acquire/present
// returns OUT_OF_DATE and the object rebuilds). Fills `outHealth`. Returns true
// iff all `frames` presented VK_SUCCESS with zero validation errors and no
// fallback. Used by both the L5 probe and the L6.6 CompositePresent scaffold.
bool drive_present(const char* spvDir, uint32_t frames, uint32_t resizeAtFrame,
                   mc2vk::PresentHealth& outHealth) {
    const bool hidden = std::getenv("MC2_VULKAN_SWAPCHAIN_PRESENT_HIDDEN") != nullptr;

    mc2vk::VulkanPresentObject present;
    if (!present.init(spvDir ? spvDir : "shaders/vulkan", hidden, 800, 600)) {
        outHealth = present.health();
        return false;
    }

    bool ok = true;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (frame == resizeAtFrame && resizeAtFrame < frames) {
            sp_log("swapchain-present: frame %u -- forcing resize to 640x480.", frame);
            // handleResize drives the rebuild deterministically (mirrors the L5
            // SDL_SetWindowSize -> OUT_OF_DATE path; the object also rebuilds on
            // OUT_OF_DATE from a real window resize).
            SDL_SetWindowSize(present.window(), 640, 480);
            if (!present.handleResize(640, 480)) { ok = false; break; }
        }
        if (!present.acquireAndPresentOneFrame()) { ok = false; break; }
    }

    outHealth = present.health();
    ok = ok && (outHealth.present_frames == frames) && outHealth.fallback_reason.empty();

    if (ok) sp_log("swapchain-present: PASS -- presented %u frames, %u resize(s), %u validation error(s).",
                   outHealth.present_frames, outHealth.resize_count, outHealth.validation_errors);
    else    sp_log("swapchain-present: FAIL/partial -- present_frames=%u fallback='%s'.",
                   outHealth.present_frames, outHealth.fallback_reason.c_str());

    present.destroy();
    outHealth.validation_errors = present.health().validation_errors;
    return ok;
}

} // namespace

// VULKAN-SWAPCHAIN-PRESENT-1 (Layer-5 probe). Now drives the persistent object;
// 16 frames + a mid-loop resize at frame 8. Returns true iff all 16 frames
// presented VK_SUCCESS with zero validation errors and no fallback.
bool mc2_vulkan_probe_swapchain_present(const char* spvDir) {
    mc2vk::PresentHealth health;
    bool ok = drive_present(spvDir, /*frames=*/16, /*resizeAtFrame=*/8, health);

    sp_log("[VK_SWAPCHAIN_PRESENT_HEALTH] swapchain_available=%d surface_created=%d "
           "format=%s present_mode=%s image_count=%u extent=%ux%u resize_count=%u "
           "present_frames=%u fallback_reason=%s validation_errors=%u",
           health.swapchain_available ? 1 : 0, health.surface_created ? 1 : 0,
           sp_format_str(health.format), sp_present_mode_str(health.present_mode),
           health.image_count, health.extent_w, health.extent_h, health.resize_count,
           health.present_frames,
           health.fallback_reason.empty() ? "(none)" : health.fallback_reason.c_str(),
           health.validation_errors);
    maybe_dump_debug_state(health, "debug_state/vk_swapchain_present_health.txt");

    (void)ok;
    return (health.present_frames == 16) && health.fallback_reason.empty() &&
           (health.validation_errors == 0);
}

// L6.6-COMPOSITE-PRESENT-SCAFFOLD. The CompositePresent TEST region's driver:
// present `frames` frames (default 16) to its OWN separate Vulkan window via the
// persistent object, exercising a resize at the midpoint -- proving the
// region->acquire->present->resize path + the per-image render-semaphore
// discipline WITHOUT touching the GL game window. Fail-soft: any init/acquire
// failure returns false (no crash). *presentFramesOut / *validationErrorsOut
// receive the tallies for the health snapshot. The composited MainColor is NOT
// sampled here (the fullscreen-triangle test pattern stands in) -- bridging the
// real game image is L7 interop, ledgered.
bool mc2_vulkan_composite_present_scaffold(const char* spvDir, uint32_t frames,
                                           uint32_t* presentFramesOut,
                                           uint32_t* resizeCountOut,
                                           uint32_t* validationErrorsOut) {
    if (frames == 0) frames = 16;
    const uint32_t resizeAt = frames / 2;
    mc2vk::PresentHealth health;
    bool ok = drive_present(spvDir, frames, resizeAt, health);

    sp_log("[VK_COMPOSITE_PRESENT_HEALTH] swapchain_available=%d surface_created=%d "
           "format=%s image_count=%u extent=%ux%u resize_count=%u present_frames=%u "
           "fallback_reason=%s validation_errors=%u",
           health.swapchain_available ? 1 : 0, health.surface_created ? 1 : 0,
           sp_format_str(health.format), health.image_count, health.extent_w,
           health.extent_h, health.resize_count, health.present_frames,
           health.fallback_reason.empty() ? "(none)" : health.fallback_reason.c_str(),
           health.validation_errors);
    maybe_dump_debug_state(health, "debug_state/vk_composite_present_health.txt");

    if (presentFramesOut)    *presentFramesOut    = health.present_frames;
    if (resizeCountOut)      *resizeCountOut      = health.resize_count;
    if (validationErrorsOut) *validationErrorsOut = health.validation_errors;
    return ok;
}

#endif // MC2_VULKAN
