// gos_host_vk.cpp - the Vulkan host glue the game and the editor share.
//
// WHAT: engine Vulkan bring-up on the host's window (initVulkanBackend) and the per-frame present-or-swap (presentOrSwap).
// WHY: the editor viewport must run the same present path as the game; a second copy would diverge the way the editor's old forked
// render loop did. The game's gameosmain and the editor's EditorGameOS both call these.
#include <atomic>
#include "gos_host_vk.h"
#include "mc2fmt.h"
#include "gos_render.h"
#include "gos_screenshot.h"
#include "gos_visual_capture.h"
#include "render_backend_select.h"
#include "mc2_hitch_trace.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "../../RenderWorld/RenderWorld.h"   // advanceFrame: the handle-slot clock
#include <SDL2/SDL.h>
#include <string>
#include <cstdio>
#include <tracy/Tracy.hpp>
#ifdef _WIN32
#include <windows.h>
#endif
#ifdef MC2_VULKAN
#include "vulkan_context.h"
#include "engine_present_vk.h"
#endif

extern SDL_Window* g_sdl_window;
extern uint32_t g_mc2FxFireCaptureFrame;   // mclib/tgl.cpp
extern std::atomic<uint32_t> g_mc2FrameCounter;         // mclib/tgl.cpp: logic-pass epoch
extern uint32_t g_mc2RenderFrame;          // mclib/tgl.cpp: render-frame clock, once per presented frame

#if defined(MC2_VULKAN) && defined(MC2_VULKAN_ISLAND)
// Render-island teardown entry points (each defined in its vulkan_*_island*.cpp; no-ops if the island was never born).
extern "C" void mc2_vk_water_island_destroy();
extern "C" void mc2_vk_vfx_island_destroy();
extern "C" void mc2_vk_color_prop_island_destroy();
extern "C" void mc2_vk_terrain_island_destroy();
extern "C" void mc2_vk_mech_island_destroy();
extern "C" void mc2_vk_legacy_shape_island_destroy();
extern "C" void mc2_vk_sky_island_destroy();
extern "C" void mc2_vk_shadow_island_destroy();
#endif

// Both hosts report the actual Vulkan adapter; no GL vendor-string fallback.
bool gos_IsNvidiaGPU() {
#ifdef MC2_VULKAN
    if (!mc2vk::engineVulkanContextValid()) return false;
    // A process chooses one physical device. Local-static initialization is thread safe.
    static const bool nvidia = [] {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(mc2vk::getOrCreateEngineVulkanContext().physicalDevice(), &properties);
        return properties.vendorID == 0x10de;
    }();
    return nvidia;
#else
    return false;
#endif
}

// VK-FAILSOFT-DISARM-1: present init hit a zero surface extent (window minimized at boot) and was DEFERRED instead of fail-softing
// the session to GL. The frame loop retries EnginePresentVk::init once the drawable is non-zero.
bool g_vkPresentInitDeferred = false;
// Failed deferred-retry attempts while the drawable was visible (non-zero). Minimized frames never count.
unsigned g_vkPresentRetryFails = 0;

// Birth the engine Vulkan context and present path when the Vulkan backend is selected.
void initVulkanBackend(graphics::RenderWindowHandle win, int w, int h) {
    // Both hosts create the same native device and present owner on their SDL window.
    mc2::noteBackendEnv();
    {
#ifdef MC2_VULKAN
        // R2 INTERLOCK (plan §6/R2, landed here since the device is born here):
        // volkLoadDevice writes a PROCESS-GLOBAL dispatch table. Both the engine
        // context AND every postprocess-interop island load their own device into
        // that table -> whichever loaded last wins -> wrong function pointers.
        // ENFORCE (logged coercion): if any postprocess-interop island gate is on,
        // force it OFF before we volkLoadDevice the engine device, so the engine
        // device is the sole owner of the global table. (The alternative fail-soft
        // path is available if coercion is ever undesirable.)
        // VK-WATER-TIER1 arms through the shared-GBuffer umbrella path; no raw water interop gate is listed here.
        // The water island borrows the SHARED engine device (no second volkLoadDevice),
        // so it does NOT hit the global-dispatch collision and must NOT be coerced off —
        // water + present coexist on the one shared device. Only add SELF-CREATING-device
        // gates here.
        // Interop-capable device: request the external-memory/-semaphore exts so
        // P6/P8/P14 can import/export shared images against this device.
        mc2vk::VulkanContext& vkctx = mc2vk::getOrCreateEngineVulkanContext();
        const bool vkok = vkctx.init(/*wantPresent=*/true);
        vkctx.logHealth();
#ifdef _WIN32
        std::printf("[NO_GL_RUNTIME v1] context=none opengl32_loaded=%d glew32_loaded=%d\n",
            GetModuleHandleW(L"opengl32.dll") != nullptr ? 1 : 0,
            GetModuleHandleW(L"glew32.dll") != nullptr ? 1 : 0);
        std::fflush(stdout);
#endif
        if (!vkok || !vkctx.valid()) {
            // No GL fallback: show the reason and exit.
            mc2::fatalBackendFailure(vkctx.health().fallbackReason.empty()
                                     ? "engine Vulkan context did not come up"
                                     : vkctx.health().fallbackReason.c_str());
        } else {
            // VK-P13 T3: stand up the whole-frame GL->VK present intercept on the
            // shared device. Sized to the GL drawable (backbuffer, incl. pillarbox
            // by construction: source is FBO-0 at swap). Fail-soft: if present init
            // fails, defer and retry from the frame loop (see below).
            int dw = 0, dh = 0;
            graphics::get_drawable_size(win, &dw, &dh);
            if (dw <= 0) dw = w;
            if (dh <= 0) dh = h;
            mc2vk::EnginePresentVk& vkpres = mc2vk::getOrCreateEnginePresentVk();
            // P13b: present onto the EXISTING visible GL game window (g_sdl_window)
            // via raw vkCreateWin32SurfaceKHR on its HWND -- no hidden VK window.
            const bool presok = vkpres.init(vkctx, g_sdl_window, static_cast<uint32_t>(dw), static_cast<uint32_t>(dh));
            vkpres.logHealth();
            if (!presok || !vkpres.valid()) {
                // VK-FAILSOFT-DISARM-2: present-level init failures are
                // TRANSIENT (zero surface extent = window minimized right now;
                // vkCreateSwapchainKHR NATIVE_WINDOW_IN_USE = another consumer
                // briefly holds the HWND surface — both observed environmental,
                // both clear on retry). Forcing GL for the session here is the
                // torn-scene trap: the umbrella family arms/latches across ~80
                // call sites (fog `static int state` latches, admission modes,
                // mech routing) and a mid-flight disarm can never reach them
                // all — the user-sighted half-grey/streaking + pause-bobbing
                // frame was exactly that half-disarmed state. So: NEVER force
                // GL for a present-level failure — DEFER and retry init from
                // the frame loop; the session stays VK. Context/device-level
                // failures are fatal (handled above).
                const std::string& fr = vkpres.health().fallback_reason;
                g_vkPresentInitDeferred = true;
                mc2::fmt::print_out("[VK_PRESENT] init deferred: {} -- will retry from the frame loop (session stays VK; present-level failures never force GL).\n", fr.c_str());
                fflush(stdout);
            }
        }
#else
        // The Vulkan backend is selected but this build has no Vulkan support
        // compiled in. No fallback: fatal.
        mc2::fatalBackendFailure("this build has no MC2_VULKAN support");
#endif
    }
}

namespace gos { namespace host {

void advanceFrameCounter() { g_mc2FrameCounter.fetch_add(1, std::memory_order_relaxed); ++g_mc2RenderFrame; }

void shutdownVulkanBackend() {
#ifdef MC2_VULKAN
#ifdef MC2_VULKAN_ISLAND
    // Islands first: each frees its Vulkan objects (and the GL interop FBO / imported texture, if any) while the GL context and the shared
    // device are both alive. The shadow island also emits the [VK_SHADOW_INTEROP] health line (forbidden_* counters).
    mc2_vk_water_island_destroy();
    mc2_vk_color_prop_island_destroy();
    mc2_vk_terrain_island_destroy();
    mc2_vk_vfx_island_destroy();
    mc2_vk_mech_island_destroy();
    mc2_vk_legacy_shape_island_destroy();
    mc2_vk_sky_island_destroy();
    mc2_vk_shadow_island_destroy();
#endif
    // Then the present path (copy-1 FBO / imported texture, swapchain), then the shared device.
    mc2vk::getOrCreateEnginePresentVk().destroy();
    mc2vk::getOrCreateEngineVulkanContext().destroy();
#endif
}

// The render-object handle slots reuse an index only after the frame that may name it completed. One clock for every host (game and editor): it runs
// after the present that closed the frame, whatever that present did.
static void advanceHandleClock(unsigned long frameCounter) {
    ZoneScopedN("Host.AdvanceHandleClock");
    uint64_t frameNo = frameCounter, doneFrame = frameNo > 2 ? frameNo - 2 : 0;
#ifdef MC2_VULKAN
    if (mc2vk::engineVulkanContextValid()) {
        const mc2vk::VulkanContext& vkctx = mc2vk::getOrCreateEngineVulkanContext();
        frameNo = vkctx.tracker().frameNo();
        doneFrame = vkctx.completedFrame();
    }
#endif
    RenderWorld::advanceFrame(frameNo, doneFrame);
}

// Present the finished frame: the Vulkan present when the backend is up, else the GL swap.
void presentOrSwap(graphics::RenderWindowHandle win, unsigned long frameCounter) {
    // Outside the Vulkan block so a request expires on a run whose Vulkan present is down or never entered.
    gos::screenshot::tick();
#ifdef MC2_VULKAN
    // VK-P13 T3: present intercept. When MC2_RENDER_BACKEND=vulkan AND
    // the VK present path is up, present the finished GL frame (FBO-0,
    // post-HUD/composite/pillarbox) through the hidden-window VK
    // swapchain instead of SDL_GL_SwapWindow. Any hard error fail-softs
    // to GL for the rest of the session (never breaks the GL path).
    bool vkPresented = false;
    bool vkPresentAttempted = false;   // presentFrame ran (it ends the Vulkan frame itself on every exit)
    {
        mc2vk::EnginePresentVk& vkpres = mc2vk::getOrCreateEnginePresentVk();
        // VK-FAILSOFT-DISARM-1: present init was deferred on a zero
        // surface extent (window minimized at boot). Retry once per
        // frame while the drawable is non-zero; success promotes the
        // session to the normal VK present path mid-run.
        if (!vkpres.valid() && g_vkPresentInitDeferred &&
            (frameCounter % 30 == 0)) {   // retry ~2x/s, not per frame (init logs each attempt)
            int rdw = 0, rdh = 0;
            graphics::get_drawable_size(win, &rdw, &rdh);
            if (rdw > 0 && rdh > 0) {
                mc2vk::VulkanContext& rctx = mc2vk::getOrCreateEngineVulkanContext();
                if (rctx.valid() &&
                    vkpres.init(rctx, g_sdl_window, static_cast<uint32_t>(rdw), static_cast<uint32_t>(rdh)) &&
                    vkpres.valid()) {
                    g_vkPresentInitDeferred = false;
                    g_vkPresentRetryFails = 0;
                    mc2::fmt::print_out("[VK_PRESENT] deferred init recovered: {:d}x{:d} — VK present active.\n", mc2::fmt::as_i32(rdw), mc2::fmt::as_i32(rdh));
                    fflush(stdout);
                    vkpres.logHealth();
                } else if (vkpres.health().fallback_reason.find(
                               "zero surface extent") != std::string::npos) {
                    // Still minimized: the GL drawable reports a
                    // size even for SW_SHOWMINNOACTIVE windows, but
                    // the VK surface caps say zero. This is the
                    // defer-forever-by-design case — never budget it.
                } else if (++g_vkPresentRetryFails >= 120) {
                    // ~60s of NON-minimized failed retries: the
                    // surface is not coming back. No GL fallback:
                    // show the reason, exit.
                    mc2::fatalBackendFailure(
                        "VK present init failed for ~60s with a "
                        "visible drawable (deferred-retry budget)");
                }
            }
        }
        if (vkpres.valid()) {
            int pdw = 0, pdh = 0;
            { ZoneScopedN("Host.PresentDrawableSize"); graphics::get_drawable_size(win, &pdw, &pdh); }
            // T5 content verification: readback on a light cadence for
            // the first stretch (proves real, changing frame content
            // without a per-frame readback stall). A pinned PPM dump
            // (MC2_VK_PRESENT_DUMP_FRAME=N) needs a verify tick AT/after
            // frame N — without the extra window below, a pin >= 600
            // could never fire (verify ticks stopped first), so the
            // oracle silently reported "PPM never wrote" on a healthy
            // present path (the 2026-07-12 grey-regression red herring).
            static const unsigned long s_ppmPinFrame = static_cast<unsigned long>(mc2::gate::get<"MC2_VK_PRESENT_DUMP_FRAME">());
            // VK-FIELD-VFX-5: dynamic fire-event pin (armed at runtime
            // by the MC2_FX_FORCE_SPAWN fixture when
            // MC2_VK_PRESENT_DUMP_ON_FIRE is set) — must be read per
            // frame, not memoized.
            static const bool s_dumpOnFire = mc2::gate::get<"MC2_VK_PRESENT_DUMP_ON_FIRE">();
            const unsigned long firePin =
                s_dumpOnFire ? static_cast<unsigned long>(g_mc2FxFireCaptureFrame) : 0ul;
            const bool inVerifyWindow =
                (frameCounter < 600) ||
                (s_ppmPinFrame > 0 &&
                 frameCounter >= s_ppmPinFrame &&
                 frameCounter < s_ppmPinFrame + 600) ||
                (firePin > 0 &&
                 frameCounter >= firePin &&
                 frameCounter < firePin + 600);
            const bool verify =
                ((mc2::gate::get<"MC2_VK_PRESENT_VERIFY">()) &&
                 inVerifyWindow && (frameCounter % 60 == 0)) ||
                gos::visual_capture::vkCapturePending() ||   // VK-VISUAL-CAPTURE-1
                gos::screenshot::pending();
            vkPresentAttempted = true;
            if (vkpres.presentFrame(static_cast<uint32_t>(pdw), static_cast<uint32_t>(pdh), verify)) {
                vkPresented = true;
                static unsigned long s_vkHealthCadence = 0;
                if ((s_vkHealthCadence++ % 300) == 0) { ZoneScopedN("Host.PresentHealth"); vkpres.logHealth(); }
            } else {
                // VK-FAILSOFT-DISARM-2: a mid-run present failure is
                // almost always the transient swapchain flake
                // (NATIVE_WINDOW_IN_USE at resize; observed
                // environmental, clears in <1s). Forcing GL for the
                // session here produced the torn half-disarmed scene
                // (streaking grey + pause-bobbing mechs). Instead:
                // tear the present stack down and re-enter the
                // DEFERRED-INIT retry path — the session stays VK
                // and recovers when the surface frees up. The ONLY
                // remaining fatal case is the structural not-VK-owned
                // decline (config plainly cannot composite; retry
                // cannot help; it is fatal, there is no GL fallback).
                const std::string fr = vkpres.health().fallback_reason;
                vkpres.logHealth();
                if (fr.find("not VK-owned") != std::string::npos) {
                    mc2::fatalBackendFailure(fr.c_str());
                } else {
                    mc2::fmt::print_out("[VK_PRESENT] present failure treated as transient: {} -- tearing down and retrying init (session stays VK).\n",
                           fr.empty() ? "(unspecified)" : fr.c_str());
                    fflush(stdout);
                    vkpres.destroy();
                    g_vkPresentInitDeferred = true;
                }
            }
        }
    }
    // No present ran (init deferred, present torn down, a retry frame): the Vulkan frame still ends, or the tracker and the ring would stop advancing.
    if (!vkPresentAttempted && mc2vk::engineVulkanContextValid()) mc2vk::getOrCreateEngineVulkanContext().frameBoundary();
#else
    mc2::fatalBackendFailure("Vulkan support is required for presentation");
#endif
    advanceHandleClock(frameCounter);
}

}}  // namespace gos::host
