// gos_host_vk.cpp - the Vulkan host glue the game and the editor share.
//
// WHAT: engine Vulkan bring-up on the host's window (initVulkanBackend) and the per-frame present-or-swap (presentOrSwap).
// WHY: the editor viewport must run the same present path as the game; a second copy would diverge the way the editor's old forked
// render loop did. The game's gameosmain and the editor's EditorGameOS both call these.
#include "gos_host_vk.h"
#include "mc2fmt.h"
#include "gos_render.h"
#include "gos_screenshot.h"
#include "gos_visual_capture.h"
#include "render_backend_select.h"
#include "mc2_hitch_trace.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include <SDL2/SDL.h>
#include <string>
#ifdef MC2_VULKAN
#include "vulkan_context.h"
#include "engine_present_vk.h"
#endif

extern SDL_Window* g_sdl_window;

// VK-FAILSOFT-DISARM-1: present init hit a zero surface extent (window minimized at boot) and was DEFERRED instead of fail-softing
// the session to GL. The frame loop retries EnginePresentVk::init once the drawable is non-zero.
bool g_vkPresentInitDeferred = false;
// Failed deferred-retry attempts while the drawable was visible (non-zero). Minimized frames never count.
unsigned g_vkPresentRetryFails = 0;

// Birth the engine Vulkan context and present path when the Vulkan backend is selected.
void initVulkanBackend(graphics::RenderWindowHandle win, int w, int h) {
    // VK-P13-SELECTOR-1 / T2+T4: birth the standalone shared engine Vulkan
    // context when MC2_RENDER_BACKEND=vulkan. The GL window + GL context above
    // are UNTOUCHED (approach A'); this only additionally stands up the shared
    // VK device that P6/P8/P9/P14 will reuse. Present is still GL swap (T3 wires
    // the VK present intercept later). On =gl/unset this whole block is skipped
    // -> byte-identical GL behavior.
    if (mc2::renderBackend() == mc2::RenderBackend::Vulkan) {
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
        if (!vkok || !vkctx.valid()) {
            // Fail-soft: force the whole session back to GL. renderBackend() stays
            // queryable but now returns GL; the VK device is null; every consumer
            // takes the GL path. Present was never changed, so nothing to revert.
            mc2::forceGLFallback(vkctx.health().fallbackReason.empty()
                                     ? "engine Vulkan context did not come up"
                                     : vkctx.health().fallbackReason.c_str());
        } else {
            // VK-P13 T3: stand up the whole-frame GL->VK present intercept on the
            // shared device. Sized to the GL drawable (backbuffer, incl. pillarbox
            // by construction: source is FBO-0 at swap). Fail-soft: if present init
            // fails, force GL -- the GL swap path is untouched.
            int dw = 0, dh = 0;
            graphics::get_drawable_size(win, &dw, &dh);
            if (dw <= 0) dw = w;
            if (dh <= 0) dh = h;
            mc2vk::EnginePresentVk& vkpres = mc2vk::getOrCreateEnginePresentVk();
            // P13b: present onto the EXISTING visible GL game window (g_sdl_window)
            // via raw vkCreateWin32SurfaceKHR on its HWND -- no hidden VK window.
            const bool presok = vkpres.init(vkctx, g_sdl_window, (uint32_t)dw, (uint32_t)dh);
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
                // the frame loop; the session stays VK. forceGLFallback remains
                // for context/device-level failures only (handled above).
                const std::string& fr = vkpres.health().fallback_reason;
                g_vkPresentInitDeferred = true;
                mc2::fmt::print_out("[VK_PRESENT] init deferred: {} -- will retry from the frame loop (session stays VK; present-level failures never force GL).\n", fr.c_str());
                fflush(stdout);
            }
        }
#else
        // MC2_RENDER_BACKEND=vulkan requested but this build has no Vulkan
        // support compiled in (GL-only build). Fail-soft to GL.
        mc2::forceGLFallback("MC2_RENDER_BACKEND=vulkan but build has no MC2_VULKAN support");
#endif
    }
}

namespace gos { namespace host {

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
    if (mc2::renderBackend() == mc2::RenderBackend::Vulkan) {
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
                    vkpres.init(rctx, g_sdl_window, (uint32_t)rdw, (uint32_t)rdh) &&
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
                    // surface is not coming back — take the
                    // best-effort GL fail-soft rather than showing
                    // torn frames forever.
                    mc2::forceGLFallback(
                        "VK present init failed for ~60s with a "
                        "visible drawable (deferred-retry budget)");
                }
            }
        }
        if (vkpres.valid()) {
            int pdw = 0, pdh = 0;
            graphics::get_drawable_size(win, &pdw, &pdh);
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
            extern uint32_t g_mc2FxFireCaptureFrame;
            static const bool s_dumpOnFire = mc2::gate::get<"MC2_VK_PRESENT_DUMP_ON_FIRE">();
            const unsigned long firePin =
                s_dumpOnFire ? (unsigned long)g_mc2FxFireCaptureFrame : 0ul;
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
            if (vkpres.presentGLFrame((uint32_t)pdw, (uint32_t)pdh, verify)) {
                vkPresented = true;
                static unsigned long s_vkHealthCadence = 0;
                if ((s_vkHealthCadence++ % 300) == 0) vkpres.logHealth();
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
                // remaining forceGL is the structural not-VK-owned
                // decline (config plainly cannot composite; retry
                // cannot help).
                const std::string fr = vkpres.health().fallback_reason;
                vkpres.logHealth();
                if (fr.find("not VK-owned") != std::string::npos) {
                    mc2::forceGLFallback(fr.c_str());
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
    if (!vkPresented) {
        // VK-FAILSOFT-DISARM-2b: NEVER GL-swap while VK present init
        // is deferred. A GL SwapBuffers on the shared HWND claims the
        // window's WSI binding, and every VK swapchain re-creation
        // then fails NATIVE_WINDOW_IN_USE — observed 120/120 re-init
        // failures over 60s with GL swapping between attempts (the
        // deferral deadlock: GL swaps because VK isn't up; VK can't
        // come up because GL swaps). Skipping the swap frees the HWND
        // so the next retry succeeds; the window just holds its last
        // frame for the sub-second gap (or stays blank while
        // minimized, where nothing is visible anyway). After the
        // retry-budget forceGL, renderBackend() returns GL and the
        // swap resumes.
        if (mc2::renderBackend() == mc2::RenderBackend::Vulkan &&
            g_vkPresentInitDeferred) {
            // hold the frame: no GL present while VK retries the HWND
        } else {
            graphics::swap_window(win);
        }
    }
#else
    graphics::swap_window(win);
#endif
}

}}  // namespace gos::host
