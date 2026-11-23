#include "mc2_czstring.h"
#include "../../code/net/runtime.h"
#include "load_scope.h"
#include "diagnostic_trace.h"
#include "mclib/pak_zip.h"
#include "loading_pump_meter.h"
#include "../../GuiRuntime/PlayerFrontEnd.h"
#include <memory>
#include <atomic>
#include <print>
#include "mc2fmt.h"
#include "gameos.hpp"
#include "hitch_stats.h"
#include "../../RenderCore/TextureService.h"
#include "mc2_diag_verbose.h"  // QUIET-DEFAULT-BUILD-1
#include "gos_render.h"
#include "render_host_kind.h"
#include "present_thread.h"
#include "render_frame_driver.h"       // RenderFrameDriver_RenderWorld — Slice 6 shared render seam (gated)
#include "render_snapshot.h"
#include "../../code/tick_commit.h"
#include "../../code/render_view.h"
#include "draw_packet_emitter.h"       // DrawPacket v0
#include "gos_static_prop_batcher.h"   // batcher_getSortedPacketCount — explicit, do not rely on transitive
#include "../../RenderCore/MaterialGpu.h"  // RenderCore::MaterialGpu for g_dpSelProp albedoTex fill
#include "build_identity.h"          // Stable build identity declarations
#include "../../RenderCore/ResolutionContract.h"  // META-FIX design D: MC2_RES_ASSERT audit
#include "vk_shared_gbuffer_gate.h"        // mc2vkgate::umbrellaOn (the VK umbrella adapter)
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <unordered_set>
#include <cstring>
#include <time.h>
#include "gameosmain_internal.h"
#include "utils/timing_math.h"

// sebi 2026-04-22: unhandled-exception filter that symbolizes the stack via
// DbgHelp (PDB-based). Needed because release/RelWithDebInfo builds otherwise
// die silently with "read violation at 0xNN" and no frames — Tracy only resolves
// the top CRT frame, and the real null-deref callsite is invisible.
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI mc2_unhandled_exception_filter(EXCEPTION_POINTERS* ep)
{
    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "[CRASH] code=0x%08lX flags=0x%08lX addr=%p\n",
        ep->ExceptionRecord->ExceptionCode,
        ep->ExceptionRecord->ExceptionFlags,
        ep->ExceptionRecord->ExceptionAddress);
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        ep->ExceptionRecord->NumberParameters >= 2) {
        fprintf(stderr, "[CRASH] %s violation at 0x%p\n",
            ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "READ" :
            ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" : "EXEC",
            reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]));
    }

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);

    // Walk back from the faulting context rather than capturing current stack —
    // CaptureStackBackTrace would start from this filter function and miss the
    // pre-exception frames we actually care about.
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 frame{};
#if defined(_M_X64) || defined(_M_AMD64)
    frame.AddrPC.Offset = ctx.Rip;
    frame.AddrFrame.Offset = ctx.Rbp;
    frame.AddrStack.Offset = ctx.Rsp;
    DWORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
    frame.AddrPC.Offset = ctx.Eip;
    frame.AddrFrame.Offset = ctx.Ebp;
    frame.AddrStack.Offset = ctx.Esp;
    DWORD machine = IMAGE_FILE_MACHINE_I386;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    fprintf(stderr, "[CRASH] stack:\n");
    for (int i = 0; i < 32; ++i) {
        if (!StackWalk64(machine, proc, GetCurrentThread(), &frame, &ctx,
                          nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
            break;
        if (frame.AddrPC.Offset == 0) break;

        char symBuf[sizeof(SYMBOL_INFO) + 512];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 512;
        DWORD64 disp64 = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD dispL = 0;

        const char* symName = "?";
        const char* fileName = "?";
        DWORD lineNum = 0;
        if (SymFromAddr(proc, frame.AddrPC.Offset, &disp64, sym))
            symName = sym->Name;
        if (SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &dispL, &line)) {
            fileName = line.FileName;
            lineNum = line.LineNumber;
        }
        fprintf(stderr, "  #%02d 0x%016llX  %s  (%s:%u)\n",
            i, static_cast<unsigned long long>(frame.AddrPC.Offset), symName, fileName, lineNum);
    }
    fflush(stderr);
    SymCleanup(proc);

    // EXCEPTION_EXECUTE_HANDLER would swallow the crash; we want to keep the
    // debugger-catch / crash-dialog behavior. Returning EXCEPTION_CONTINUE_SEARCH
    // lets the OS do whatever it would have (watson, just-in-time debugger, etc.)
    // after we've printed our trace.
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#include <SDL2/SDL.h>
#ifdef _WIN32
#include <SDL2/SDL_system.h>
#endif
#include "gos_input.h"

#include "utils/color_utils.h"
#include "utils/timing.h"
#include "gos_postprocess.h"
#include "gos_validate.h"
#include "gos_screenshot.h"   // TGA capture of the Vulkan present readback
#include "gos_dev_shell.h"    // DEV-SHELL-1: localhost dev command socket (MC2_DEV_SHELL)
#include "gos_visual_capture.h"  // [VISUAL_CAPTURE v1] S9 PNG+sidecar + bookmark replay
#include "gos_static_prop_killswitch.h"
#include "gos_static_prop_registry.h"  // Stage 3.C: isEnabled() for [INSTR v1]
#include "../../RenderWorld/RenderWorld.h"  // M1 Task 14
#include "../../RenderCore/FramePackets.h"
#include "../../mclib/render_contract.h"   // Phase 2 assert init
#include "../../mclib/camera.h"   // CAMERA-PARK-1: extern CameraPtr eye (pose pin fixture)
#include "asset_scale.h"
#include "gos_crashbundle.h"
#include "gos_smoke.h"
#include "gpu_memory_report.h"   // [VRAM v1] load_end / run_end lines and the Tracy VRAM plot

#include <signal.h>
#include "gos_profiler.h"
#include "tgl.h"
#include "gos_object_recon_tracy.h"  // [OBJECT_RECON v1] slice-2 recon zero (env-gated)
#include "projectz_trace.h"  // projectz_trace_init/frame_tick/shutdown (PROJECTZ v1)
#include "battle_bench.h"
#include "projectz_overlay.h" // RAlt+P debug overlay (commit 4)
#include "render_backend_select.h"  // VK-P13-SELECTOR-1 / T1: MC2_RENDER_BACKEND
#ifdef MC2_VULKAN
#include "vulkan_context.h"         // VK-P13-SELECTOR-1 / T2: shared engine VK device
#include "vk_gpu_profile.h"          // VK-GPU-ZONES: per-frame Tracy GPU collect / pass timestamps
#include "vk_desc_write_cache.h"     // per-frame descriptor write/skip counter (Tracy plot + [VK_DESC_WRITES v1])
#include "vk_render_pass_timer.h"    // Arm(): the timestamp pool is born before a worker thread brackets a pass
#include "../../code/frame_jobs.h"   // frameJobsEnabled(): MC2_FRAME_JOBS=0 keeps recorded islands on the render thread
#include "../../code/job_system.h"   // the Render-domain pool the recorded islands run on
#include "engine_present_vk.h"      // VK-P13-SELECTOR-1 / T3: GL->VK present intercept
#endif
#include "gos_visual_diff.h"  // Stage 2.E pre-HUD capture + Ctrl+Shift+P record
#include "gos_rdoc_capture.h"  // Tier 5: env-gated in-process RenderDoc capture
#include "gos_terrain_indirect.h"  // [INSTR v1] banner: terrain_indirect{,_parity} fields
#include "gpu_cull_record.h"       // C0-1: GpuActorRecord schema selftest
#include "gpu_cull_readback.h"    // C2: async readback ring buffer selftest
#include "object_admission_predicate.h"  // Track A1: init probe + selftest gate
#include "view_registry.h"
#include "debug_state_dump.h"                     // DEBUG-STATE-DUMP-1
// TerrainPatchStream accessors feed the unconditional RenderSnapshot terrain-
// pass fill (TERRAIN-PASS-PACKET-0) below, so this header must NOT be gated on
// MC2_IMGUI — otherwise a non-IMGUI build fails to compile gameosmain.cpp.
#include "gos_terrain_patch_stream.h"             // TERRAIN-SPINE-0
#ifdef MC2_IMGUI
#include "../../GuiRuntime/GuiRuntime.h"
#include "../../GuiRuntime/EditorInspector.h"     // TERRAIN-SPINE-0
#include "imgui_impl_sdl2.h"
#endif
#include "mc2_hitch_trace.h"
#include "gos_render_pass_timer.h"
#include "gos_frame_pass_stats.h"   // [FRAME_PASS_STATS v1] collector
#include "utils/mc2log.h"   // LOGGING-FACADE-P0: mc2::log::init() before the NUL redirect
#include "Gates/gate_cold.h"   // typed gate registry: main-entered marker, invalidateAll, [CONFIG v1]
#include "mc2fmt.h"
#include "../../mclib/sim_overlap.h"

// Tier-1 instrumentation (stability spec §5.1): single source of truth for
// the frame=... field used by TGL_POOL, DESTROY, and GL_ERROR log lines.
// Definition lives in mclib/tgl.cpp so data tools (aseconv, pak, makefst,
// makersp) that link mclib without gameosmain still resolve the symbol.
// This TU owns the per-frame increment; everyone else is a read-only extern.

// Force discrete GPU selection on hybrid-graphics laptops (NVIDIA Optimus,
// AMD PowerXpress). Without these exports, an unknown OpenGL executable is
// routed to the Intel integrated GPU by default, which is catastrophic for
// our terrain/shadow/post-process workload. These symbols are looked up by
// the driver by exported name; they do not need to be referenced from code.
#if defined(_WIN32)
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 0x00000001;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif
static bool g_exit = false;
static std::string_view g_exitWhy = "game_request";   // first requester wins; written once as the EXIT_REASON trace event when the frame loop ends
static void request_exit(std::string_view why) { if (!g_exit) g_exitWhy = why; g_exit = true; }
static bool g_focus_lost = false;

// Owned by gos_render.cpp; needed here to query SDL_WINDOW_MINIMIZED so the
// background-throttle path can distinguish "window invisible" from merely
// "another window has keyboard focus" (which is normal during gameplay when
// the user clicks a chat / browser / profiler window).

// Global runtime toggle for the GPU static-prop renderer.
// Definition lives in gos_static_prop_batcher.cpp (in the gameos lib) so
// data-tool executables that link mclib but not gameos_main still resolve
// the symbol. Toggled at runtime via RAlt+0 (see handle_key_down).

static void handle_key_down( SDL_Keysym* keysym ) {
    const bool alt_debug = (keysym->mod & KMOD_ALT) != 0;
    switch( keysym->sym ) {
        case SDLK_ESCAPE:
            // Documented quit is RAlt+Escape (docs/modding-guide.md). LAlt+Esc is the Windows window-cycle shortcut and must not end the game.
            if((keysym->mod & KMOD_RALT) != 0)
                request_exit("ralt_escape");
            break;
        case 'd':
            if(alt_debug)
                gos_RenderEnableDebugDrawCalls();
            break;
        case SDLK_F2:
            if (alt_debug) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->showShadowDebug_ = !pp->showShadowDebug_;
                    mc2::fmt::print_err("Shadow Debug: {}\n", pp->showShadowDebug_ ? "CSM view" : "OFF");
                }
            }
            break;
        case SDLK_F3:
            if (alt_debug) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->shadowsEnabled_ = !pp->shadowsEnabled_;
                    mc2::fmt::print_err("Shadows: {}\n", pp->shadowsEnabled_ ? "ON" : "OFF");
                }
            }
            break;
        case SDLK_F5:
            if (alt_debug) {
                bool cur = gos_GetTerrainDrawEnabled();
                gos_SetTerrainDrawEnabled(!cur);
                mc2::fmt::print_err("Terrain Draw: {}\n", !cur ? "ON" : "OFF");
            }
            break;
        case SDLK_1:
            if (alt_debug) {
                // Toggle cement-word diagnostic visualization (terrain frag
                // mode 8).  R = CEMENT_LAYER_VALID bit, G = layer index low
                // byte, B = useCementAtlas == 0.  See gos_terrain.frag.
                float cur = gos_GetTerrainDebugMode();
                float next = (cur > 7.5f) ? 0.0f : 8.0f;
                gos_SetTerrainDebugMode(next);
                mc2::fmt::print_err("Surface Debug: {}\n",
                    (next > 7.5f)
                        ? "CEMENT-WORD VIZ (R=valid, G=layer&0xFF, B=!useCementAtlas)"
                        : "OFF");
            }
            break;
        case SDLK_4:
            if (alt_debug) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    if (!pp->screenShadowEnabled_) {
                        pp->screenShadowEnabled_ = true;
                        pp->screenShadowDebug_ = 0;
                        mc2::fmt::print_err("Screen Shadows: ON\n");
                    } else if (pp->screenShadowDebug_ == 0) {
                        pp->screenShadowDebug_ = 1;
                        mc2::fmt::print_err("Screen Shadows: DEBUG (red=terrain, green=lit, blue=shadowed, black=sky)\n");
                    } else {
                        pp->screenShadowEnabled_ = false;
                        pp->screenShadowDebug_ = 0;
                        mc2::fmt::print_err("Screen Shadows: OFF\n");
                    }
                }
            }
            break;
        case SDLK_8:
            if (alt_debug) {
                int mode = (static_cast<int>(gos_GetTerrainDebugMode()) + 1) % 8;
                gos_SetTerrainDebugMode(static_cast<float>(mode));
                switch (mode) {
                    case 0: mc2::fmt::print_err("Surface Debug: OFF\n"); break;
                    case 1: mc2::fmt::print_err("Surface Debug: DEPTH (R=actual G=undisplaced)\n"); break;
                    case 2: mc2::fmt::print_err("Surface Debug: RAW colormap (all terrain)\n"); break;
                    case 3: mc2::fmt::print_err("Surface Debug: BLURRED colormap\n"); break;
                    case 4: mc2::fmt::print_err("Surface Debug: MATERIAL weights (RGB=rock/grass/dirt)\n"); break;
                    case 5: mc2::fmt::print_err("Surface Debug: NORMAL lighting\n"); break;
                    case 6: mc2::fmt::print_err("Surface Debug: TERRAIN shadow factor\n"); break;
                    case 7: mc2::fmt::print_err("Surface Debug: CLOUD factor\n"); break;
                }
            }
            break;
        case SDLK_9:
            if (alt_debug) {
                // Repurposed from SSAO toggle to GPU static prop frag debug cycle.
                // SSAO infrastructure removed entirely in F1 unified-projection
                // retirement (2026-05-22 spec). Key no longer toggles SSAO.
                gos_GpuPropsCycleDebugMode();
                int m = gos_GpuPropsGetDebugMode();
                const char* name = "?";
                switch (m) {
                    case 0: name = "normal"; break;
                    case 1: name = "addr-gradient"; break;
                    case 2: name = "addr-hash"; break;
                    case 3: name = "WHITE"; break;
                    case 4: name = "ARGB-only"; break;
                    case 5: name = "TEX-only"; break;
                    case 6: name = "HIGHLIGHT-only"; break;
                    case 7: name = "TEX+HIGHLIGHT"; break;
                }
                mc2::fmt::print_err("GPU Props Debug: {} ({})\n", m, name);
            }
            break;
        case SDLK_5:
            if (alt_debug) {
                // Cycle HUD scale: 1.0 -> 0.90 -> 0.85 -> 0.80 -> 1.0
                float s = gos_GetHudScale();
                if      (s > 0.99f) s = 0.90f;
                else if (s > 0.88f) s = 0.85f;
                else if (s > 0.83f) s = 0.80f;
                else                s = 1.00f;
                gos_SetHudScale(s);
                mc2::fmt::print_err("HUD Scale: {:.2f}\n", s);
            }
            break;
        case SDLK_7:
            if (alt_debug) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->shorelineEnabled_ = !pp->shorelineEnabled_;
                    mc2::fmt::print_err("Shorelines: {}\n", pp->shorelineEnabled_ ? "ON" : "OFF");
                }
            }
            break;
        case 'p':
            // RAlt+P: cycle the projectZ debug overlay through candidate
            // predicates (off -> legacyRectFinite -> homogClip -> rectSignedW
            // -> rectNearFar -> rectGuard -> off ...). See projectz_overlay.h.
            if (alt_debug) {
                projectz_overlay_advance();
            }
            break;
        case 'c':
            // RAlt+Shift+C: deliberate crash-bundle smoke test.
            // Must be gated with both ALT and SHIFT to avoid colliding with
            // normal gameplay 'c' bindings.
            if (alt_debug && (keysym->mod & KMOD_SHIFT) != 0) {
                crashbundle_trigger_test_crash();
            }
            break;
    }
}

#if defined(_WIN32) && defined(MC2_VULKAN)
static void SDLCALL displayMessageHook(void*, void*, unsigned int message, Uint64, Sint64) {
    if (message == WM_DISPLAYCHANGE) mc2vk::notifyDisplayChanged();
}
#endif

void process_events( void ) {

    input::beginUpdateMouseState();

#if defined(_WIN32) && defined(MC2_VULKAN)
    SDL_SetWindowsMessageHook(displayMessageHook, nullptr);
#endif
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
#ifdef MC2_VULKAN
        // Observe display notifications before the unfocused-input filter.
        const bool displayChange = event.type == SDL_DISPLAYEVENT ||
            (event.type == SDL_WINDOWEVENT && (event.window.event == SDL_WINDOWEVENT_MOVED ||
                                              event.window.event == SDL_WINDOWEVENT_DISPLAY_CHANGED));
        if (displayChange) mc2vk::notifyDisplayChanged();
#endif
#ifdef MC2_IMGUI
        if (g_imguiInitialized) {
            ImGui_ImplSDL2_ProcessEvent(&event);
        }
#endif
        // While unfocused, drop input events but let window events through
        // so FOCUS_GAINED can propagate to the switch below and clear the
        // flag. The prior form compared event.type against a subevent value
        // (SDL_WINDOWEVENT_FOCUS_GAINED, always .window.event never .type),
        // which could never match and would have jammed this loop if the
        // FOCUS_LOST handler had ever actually fired.
        if (g_focus_lost && event.type != SDL_WINDOWEVENT) {
            continue;
        }

        switch( event.type ) {
        case SDL_KEYDOWN:
            handle_key_down( &event.key.keysym );
            // fallthrough
        case SDL_KEYUP:
            input::handleKeyEvent(&event);
            break;
        case SDL_QUIT:
            request_exit("sdl_quit");   // WM_CLOSE / Alt+F4 / the OS closing a ghosted window
            break;
        case SDL_WINDOWEVENT:
            // Window subevents live in event.window.event, not event.type.
            // The prior top-level cases `SDL_WINDOWEVENT_RESIZED` (enum
            // value 5) and `SDL_WINDOWEVENT_FOCUS_LOST` (value 13) never
            // actually fired because event.type for any window-related
            // event is SDL_WINDOWEVENT (0x200). Fixed to dispatch properly.
            switch (event.window.event) {
            case SDL_WINDOWEVENT_RESIZED:
            case SDL_WINDOWEVENT_SIZE_CHANGED: {
                float w = static_cast<float>(event.window.data1);
                float h = static_cast<float>(event.window.data2);

                // FREE-RESIZE-1: refresh the physical drawable size — every
                // per-frame consumer (postprocess FBO resize in draw_screen,
                // UI canvas box, mouse normalize, camera aspect) keys off
                // Environment.drawableWidth/Height, so updating them + the
                // ImGui display size is the whole live-resize story. The
                // logical 800x600 canvas (Environment.screenWidth) stays
                // untouched by design.
                {
                    extern SDL_Window* g_sdl_window;
                    if (g_sdl_window) {
                        SDL_GetWindowSizeInPixels(g_sdl_window,
                            &Environment.drawableWidth, &Environment.drawableHeight);
#ifdef MC2_IMGUI
                        GuiRuntime::NotifyResize(
                            Environment.drawableWidth, Environment.drawableHeight,
                            Environment.screenWidth, Environment.screenHeight);
#endif
                    }
                }
                graphics::refresh_mouse_grab();
                graphics::apply_os_cursor_policy("resize", !g_focus_lost);
                MC2_LOG(Core, Debug, "resize event: w: {:f} h:{:f}", w, h);
                break;
            }
            case SDL_WINDOWEVENT_MOVED:
                graphics::refresh_mouse_grab();
                break;
            case SDL_WINDOWEVENT_FOCUS_GAINED:
                graphics::window_checkpoint("focus_gained_before");
                graphics::window_checkpoint("focus_gained_after", true);
                // Re-assert the mouse grab on focus regain. The initial
                // grab at window-creation can be only partially applied on
                // multi-monitor setups (symptom: cursor escapes to the
                // leftmost column of the adjacent monitor without losing
                // focus). Re-asserting once focus is actually confirmed
                // makes the clamp stick.
                //
                // CURSOR-GRAB-FIX-1: this re-arm is what made every previous
                // one-shot suppression useless -- an offscreen window still
                // receives FOCUS_GAINED and re-clipped the user's pointer to a
                // rect at (-32000,-32000). It stays unconditional on purpose:
                // graphics::apply_mouse_grab() re-evaluates
                // gos_CursorCaptureForbidden() on EVERY call, so the refusal
                // survives arbitrarily many focus events. Do not add a second
                // copy of the policy here.
                graphics::set_mouse_grab(true);
                graphics::apply_os_cursor_policy("focus_gained", true);
                g_focus_lost = false;
                input::setInputFocus(true);
                break;
            case SDL_WINDOWEVENT_ENTER:
                graphics::apply_os_cursor_policy("enter", !g_focus_lost);
                break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
                graphics::window_checkpoint("focus_lost");
                // Release grab so Alt-Tab and other windows behave cleanly.
                graphics::set_mouse_grab(false);
                graphics::apply_os_cursor_policy("focus_lost", false);
                g_focus_lost = true;
                input::setInputFocus(false);
                break;
            }
            break;
        case SDL_MOUSEMOTION:
            input::handleMouseMotion(&event); 
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            //input::handleMouseButton(&event);
            break;
        case SDL_MOUSEWHEEL:
            input::handleMouseWheel(&event);
            break;
        }
    }

    input::updateMouseState();
    input::updateKeyboardState();
}

extern bool g_disable_quads;

// META-FIX design D (MC2_RES_ASSERT) chokepoint (ii): per-frame frame-level
// resolution contract. Only called when the gate is armed (default OFF =
// tier1 byte-identical; the call site is one static-latch bool test).
//   (a) HUD clamp invariant — while gos_GetHudResClampEnabled() the logical
//       canvas (Environment.screenWidth/Height) is exactly 800x600. Catches
//       the HUD-RES-CLAMP merge-drop class at runtime (the handleEvents
//       `Environment.screenWidth = width_` rewrite un-clamps on the first
//       mode change if the gos_SetScreenMode clamp is lost).
//   (b) drawable validity + freshness — the cached Environment.drawable* is
//       positive and matches a fresh graphics::get_drawable_size probe.
//       3-frame grace: the cache refreshes only in handleEvents, so a
//       WM-initiated resize between events must not false-fatal.
//   (c) #49 windowed oracle — an MC2_WINDOWED boot with MC2_WIDTH/MC2_HEIGHT
//       must keep the requested physical drawable (the pre-fix #49 trap
//       forced the window back to 800x600 and passed every self-consistent
//       check). Downgraded to a non-fatal note when the desktop is smaller
//       than the request (SDL clamps legitimately there).
static void mc2resFrameAudit(int viewport_w, int viewport_h)
{
    char resDetail[192];

    // (a) HUD clamp invariant.
    if (gos_GetHudResClampEnabled() &&
        (Environment.screenWidth != 800 || Environment.screenHeight != 600))
    {
        mc2::fmt::to_buf(resDetail,
                 "screen={}x{} expected=800x600 (clamp enabled)",
                 Environment.screenWidth, Environment.screenHeight);
        mc2res::violation("hud_clamp", resDetail);
    }

    // (b) drawable validity + freshness (3-frame grace).
    if (viewport_w <= 0 || viewport_h <= 0)
    {
        mc2::fmt::to_buf(resDetail,
                 "drawable={}x{} (must be positive)", viewport_w, viewport_h);
        mc2res::violation("drawable_nonpositive", resDetail);
    }
    else
    {
        int probeW = 0, probeH = 0;
        static int s_staleFrames = 0;
        if (gos_ProbeDrawableSize(&probeW, &probeH))
        {
            if (probeW != viewport_w || probeH != viewport_h)
            {
                if (++s_staleFrames > 3)
                {
                    mc2::fmt::to_buf(resDetail,
                             "cached={}x{} probe={}x{} for {} frames",
                             viewport_w, viewport_h, probeW, probeH,
                             s_staleFrames);
                    s_staleFrames = 0;
                    mc2res::violation("drawable_stale", resDetail);
                }
            }
            else
                s_staleFrames = 0;
        }
    }

    // (c) #49 windowed-resolution oracle.
    static const int s_reqW = mc2::gate::get<"MC2_WIDTH">();
    static const int s_reqH = mc2::gate::get<"MC2_HEIGHT">();
    static const bool s_windowed = mc2::gate::get<"MC2_WINDOWED">();
    if (s_windowed && s_reqW > 0 && s_reqH > 0)
    {
        static int s_oracleFrames = 0;
        if (viewport_w != s_reqW || viewport_h != s_reqH)
        {
            if (++s_oracleFrames > 3)
            {
                s_oracleFrames = 0;
                bool desktopSmaller = false;
                SDL_DisplayMode dm;
                if (SDL_GetDesktopDisplayMode(0, &dm) == 0)
                    desktopSmaller = (dm.w < s_reqW || dm.h < s_reqH);
                mc2::fmt::to_buf(resDetail,
                         "drawable={}x{} requested={}x{} (MC2_WINDOWED)",
                         viewport_w, viewport_h, s_reqW, s_reqH);
                if (desktopSmaller)
                    mc2res::note("windowed_size_desktop_clamp", resDetail);
                else
                    mc2res::violation("windowed_size", resDetail);
            }
        }
        else
            s_oracleFrames = 0;
    }
}

static void draw_screen( void )
{
    g_disable_quads = false;

    gosPostProcess* pp = getGosPostProcess();

    // Apply validation mode feature overrides
    if (pp && getValidateConfig().enabled) {
        ValidateConfig& vc = getValidateConfig();
        if (vc.shadowsOverride >= 0) pp->shadowsEnabled_ = vc.shadowsOverride;
    }


	const int viewport_w = Environment.drawableWidth;
	const int viewport_h = Environment.drawableHeight;

	// META-FIX design D (MC2_RES_ASSERT): per-frame resolution contract audit.
	// Default OFF = one static bool test, tier1 byte-identical.
	if (mc2res::armed())
		mc2resFrameAudit(viewport_w, viewport_h);

    if (pp) {
        pp->resize(viewport_w, viewport_h);
        pp->beginScene();
    }


    // TODO: reset all states to sane defaults!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    // Blue-grey sky during gameplay, black for menus/loading/mech bay
    // Uses previous frame's terrain flag (set during rendering, cleared in beginScene)
    {
        gosPostProcess* ppClear = getGosPostProcess();
        const bool blueGrey = ppClear && ppClear->prevFrameHadTerrain_;
        // The terrain island CLEARs shared color0/normal/depth first; the GL scene FBO is not cleared.
        if (blueGrey)
            mc2_vk_terrain_set_native_prefix_clear_color(0.55f, 0.62f, 0.72f, 1.0f);
        else
            mc2_vk_terrain_set_native_prefix_clear_color(0.0f, 0.0f, 0.0f, 1.0f);
        // No terrain last frame (menu, loading, mech bay): nothing will clear the shared scene before the first island draws, so clear it now. A frame
        // with terrain is cleared by the terrain pass, or by the terrain flush's own guarantee.
        if (!blueGrey) (void)mc2_vk_scene_clear(ppClear ? ppClear->getWidth() : viewport_w,
                                              ppClear ? ppClear->getHeight() : viewport_h);
    }

    // Skybox disabled — terrain fog provides atmosphere, bright sky looked jarring

    {
        ZoneScopedN("Camera.UpdateRenderers");
        // GAME-EDITOR-RENDER-FRAME-DRIVER-1 (Slice 6): gated A/B adoption of the
        // shared RenderFrameDriver seam. Default OFF — MC2_RENDER_FRAME_DRIVER
        // unset keeps the EXACT inline trio below (tier1 smoke stays
        // byte-identical). Set MC2_RENDER_FRAME_DRIVER=1 to route the game's
        // world dispatch through the same RenderFrameDriver_RenderWorld the
        // editor adopts unconditionally, proving the seam is genuinely shared.
        // Exit criteria to flip default-ON + delete the inline path: a build +
        // tier1 smoke A/B that is byte-identical with the gate OFF vs ON.
        // The Tracy sub-zones and frameBannerTick stay OUTSIDE the seam (they
        // are game-only), so both branches keep them identically.
        static const bool s_useRenderFrameDriver = false;
        if (s_useRenderFrameDriver) {
            RenderFrameDesc rfd;
            rfd.host = RenderHostKind::Game;
            RenderFrameDriver_RenderWorld(rfd);
        } else {
            { ZoneScopedN("Camera.UpdateRenderers gos_RendererBeginFrame"); gos_RendererBeginFrame(); }
            Environment.UpdateRenderers();
            { ZoneScopedN("Camera.UpdateRenderers gos_RendererEndFrame"); gos_RendererEndFrame(); }
        }
        RenderWorld::frameBannerTick();  // M1 Task 14 (m2 fix: post-EndFrame)
    }


    // Composite post-processed scene to default framebuffer
    if (pp) {
        render_contract::noteRenderPass(render_contract::PassIdentity::PostProcess,
                                        "gosPostProcess_endScene");
        // [RENDER_PASS_TIME v1] post = whole endScene chain (HZB/SSAO/screen
        // shadow/shoreline/godrays/bloom/composite). Single outer scope: the
        // inner stages call each other, and GL_TIME_ELAPSED cannot nest.
        render_contract::beginPassScope(render_contract::PassIdentity::PostProcess,
                                        "gosPostProcess_endScene");
        pp->endScene();
        render_contract::endPassScope(render_contract::PassIdentity::PostProcess,
                                      "gosPostProcess_endScene");
    }

    // Stage 2.E visual-diff capture hook. Must fire AFTER pp->endScene() so the
    // default framebuffer holds scene+post-process, but BEFORE projectz_overlay
    // and HUD replay so neither leaks into the captured TGA. Default-off
    // (early-return when MC2_VISUAL_DIFF_CAPTURE is unset).
    VisualDiff::onFrameTick();

    // Tier 5: in-process RenderDoc capture trigger. Default-off; activates
    // only when MC2_RDC_CAPTURE_FRAME is set. Shares the post-PP / pre-HUD
    // seam with VisualDiff so the captured frame matches visual-diff
    // semantics (intro-complete + N frames). See gos_rdoc_capture.h.
    RdocCapture::onFrameTick();

    // ProjectZ debug overlay (RAlt+P): drawn on the default framebuffer
    // AFTER post-process composite and BEFORE HUD replay so it sits over the
    // scene but below the HUD. No-op when overlay is off (default).
    projectz_overlay_render(Environment.drawableWidth, Environment.drawableHeight);

    // Replay buffered HUD draws to FB 0 (after post-process)
    gos_RendererFlushHUDBatch();

#ifdef MC2_IMGUI
    // FB 0 depth starts at 1.0 (GL default); only sceneFBO_ gets cleared during
    // draw_screen().  Post-ImGui 3D camera renders (encyclopedia mech viewer) use
    // GL_GEQUAL (reverse-Z), so fragments at depth 0.x all fail "0.x >= 1.0".
    // Clear to 0.0 (reverse-Z far) once per frame so the depth test passes.



    GuiRuntime::Render();
#endif
}

extern float frameRate;


// External linkage: the gameosmain_*.cpp pieces call startup_phase().
    // Startup phase timing. Anchor at the top of main(). Cheap printfs --
    // total cost is microseconds, but the signal for triage is high.
    Uint64 g_startup_t0 = 0;
    double startup_elapsed() {
        const Uint64 now = SDL_GetPerformanceCounter();
        const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
        return static_cast<double>((now - g_startup_t0)) / freq;
    }
    void startup_phase(std::string_view name) {
        static double firstAudio = -1.0;
        const double elapsed = startup_elapsed();
        if (name == "first_audio_queued") {
            if (firstAudio >= 0.0) return;
            firstAudio = elapsed;
        }
        const std::string nameZ(name);   // printf / emitTiming take a NUL-terminated string
        mc2::fmt::print_out("[TIME] t={:6.2f}s  phase={}\n", elapsed, nameZ.c_str());
        if (name == "first_frame_presented")
            mc2::fmt::print_out("[STARTUP] first_present_s={:.3f} first_audio_queued_s={:.3f}\n", elapsed, firstAudio);
        if (SmokeMode::state().enabled) {
            // Emit canonical smoke-line for the same milestone.
            SmokeMode::emitTiming(nameZ.c_str());
        }
    }
// (end of the startup phase helpers)

// Mission-load phase timing. Exposed (file-scope linkage, not namespaced)
// so code/mission.cpp can declare these by forward-decl and call into them
// without a new header. Pattern mirrors the startup timing above.
static Uint64 g_mission_t0 = 0;
static unsigned g_missionTraceFrames = 5;
static Uint64 g_missionReadyTrace = 0;
static graphics::RenderWindowHandle g_loadingWindow = nullptr;
static void (*g_loadingDraw)() = nullptr;

void graphics::set_loading_draw(void (*draw)()) { g_loadingDraw = draw; }

void graphics::redraw_loading_screen()
{
    if (!g_loadingWindow || !g_loadingDraw || loading_phase().empty()) return;
    static Uint64 lastDraw = 0;
    static bool drawing = false;
    const auto now = SDL_GetTicks64();
    if (drawing || now - lastDraw < 100) return;
    lastDraw = now;
    drawing = true;
    // Only explicit safe checkpoints enter here: no world update, input or resource-loading callback.
    mc2vk::present_thread::waitSealed();
    auto* pp = getGosPostProcess();
    if (pp) pp->beginScene();
    (void)mc2_vk_scene_clear(pp ? pp->getWidth() : Environment.drawableWidth,
                            pp ? pp->getHeight() : Environment.drawableHeight);
    gos_RendererBeginFrame();
    gos_SetRenderState(gos_State_IsHUD, 1);
    g_loadingDraw();
    gos_SetRenderState(gos_State_IsHUD, 0);
    gos_RendererEndFrame();
    if (pp) pp->endScene();
    gos_RendererFlushHUDBatch();
    // The present owns a frame boundary; retain the ordinary host clocks and handle retirement.
    gos::host::advanceFrameCounter();
    gos::host::presentOrSwap(g_loadingWindow, g_mc2RenderFrame);
    mc2vk::present_thread::waitSealed();
    gos::host::retirePresentHandles(g_mc2RenderFrame);
    drawing = false;
}

// Loading runs on the window's main thread. Service OS messages without
// dispatching game input against the partially constructed mission.
extern "C" void mission_load_pump()
{
    graphics::pump_loading_events();
    if (SmokeMode::state().headless) return;
    static mc2::loadpump::CursorThrottle cursorThrottle;
    if (!cursorThrottle.due(SDL_GetTicks64())) return;
    graphics::redraw_loading_screen();
    // The sprite cannot advance during synchronous loading; use the OS arrow.
    // The central policy preserves the unattended harness cursor guard.
    graphics::apply_os_cursor_policy("mission_load", false);
}

// LOAD-PHASE-FACTS-1: coarse wall-clock phase breakdown for the mission-load
// critical path (FST/pak parse, texture prewarm, object/actor spawn, etc).
// Cheap fixed-size ring of (name, elapsed_ms) pairs recorded by every
// mission_phase_mark() call; overhead is a handful of printf-equivalent
// stores per mission load (~microseconds total) so this stays always-on,
// no gate needed. Consolidated into one [LOAD_PHASES v1] line at
// mission_phase_report(), called from Mission::start() right after
// mission_ready is marked.
namespace {
    struct LoadPhaseSample { const char* name; double t_ms; };
    static const int kMaxLoadPhaseSamples = 16;
    static LoadPhaseSample g_loadPhaseSamples[kMaxLoadPhaseSamples];
    static int g_loadPhaseCount = 0;
}

extern "C" void mission_phase_begin()
{
    mc2::vfs::resetPakIo();
    graphics::begin_loading_events();
    mc2::loadpump::meter.mission.begin(SDL_GetTicks64(), "mission_load_start", SmokeMode::state().headless);
    mission_load_pump();
    g_missionTraceFrames = 5;
    g_missionReadyTrace = 0;
    g_mission_t0 = SDL_GetPerformanceCounter();
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    (void)freq; // suppress unused-var if compiler gets clever
    mc2::fmt::print_out("[MISSION] t=  0.00s  phase=mission_load_start\n");
    g_loadPhaseCount = 0;
    mc2::load::begin();
}

extern "C" void mission_phase_mark(mc2::czstring name)
{
    mission_load_pump();
    graphics::loading_events_phase(name);
    auto& pumpScope = mc2::loadpump::meter.mission;
    pumpScope.mark(SDL_GetTicks64(), name);
    if (strcmp(name, "mission_ready") == 0) pumpScope.end(SDL_GetTicks64());
    const Uint64 now = SDL_GetPerformanceCounter();
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    const double elapsed = static_cast<double>((now - g_mission_t0)) / freq;
    mc2::fmt::print_out("[MISSION] t={:6.2f}s  phase={}\n", elapsed, mc2::fmt::safe(name));

    if (g_loadPhaseCount < kMaxLoadPhaseSamples) {
        g_loadPhaseSamples[g_loadPhaseCount].name = name;
        g_loadPhaseSamples[g_loadPhaseCount].t_ms = elapsed * 1000.0;
        g_loadPhaseCount++;
    }
}

// LOAD-PHASE-FACTS-1: emit one consolidated line summarizing the phase
// breakdown recorded since mission_phase_begin(). Each phaseN=NAME:MS is
// the wall-clock delta since the PREVIOUS mark (or since mission_load_start
// for the first mark), so the numbers sum to the final mark's total.
extern "C" void mission_phase_report()
{
    mc2::vfs::reportPakIo();
    graphics::end_loading_events();
    if (g_sdl_window)
        graphics::apply_os_cursor_policy("mission_ready", !g_focus_lost);
    g_missionTraceFrames = 0;
    char buf[1024];
    int off = 0;
    double prev_ms = 0.0;
    const double total_ms = (g_loadPhaseCount > 0) ? g_loadPhaseSamples[g_loadPhaseCount - 1].t_ms : 0.0;

    off += mc2::fmt::to_buf_n(buf + off, sizeof(buf) - off, "[LOAD_PHASES v1] total={:.1f}ms", total_ms);
    for (int i = 0; i < g_loadPhaseCount && off < static_cast<int>(sizeof(buf)); i++) {
        const double delta_ms = g_loadPhaseSamples[i].t_ms - prev_ms;
        off += mc2::fmt::to_buf_n(buf + off, sizeof(buf) - off, " phase{}={}:{:.1f}ms",
                         i + 1, mc2::fmt::safe(g_loadPhaseSamples[i].name), delta_ms);
        prev_ms = g_loadPhaseSamples[i].t_ms;
    }
    std::printf("%s\n", buf);
    const auto& pumpScope = mc2::loadpump::meter.mission;
    if (mc2_diag::tagRequested("LOAD_PUMP"))
        MC2_LOG(Io, Info, "[LOAD_PUMP v1] max_gap_ms={} phase={} scope=mission mode={} services={}",
            pumpScope.maxGap, pumpScope.worst, pumpScope.attempted ? "attempted-pump" : "actual-service", pumpScope.services);
    {
        const auto scopes = mc2::load::report();
        std::printf("[LOAD_SCOPES v1]%s\n", scopes.scopes.c_str());
        std::printf("[LOAD_SPLIT v1]%s phase_delta_ms=%.3f\n", scopes.split.c_str(), scopes.total / 1e6 - total_ms);
        std::printf("[LOAD_TOP v1]%s\n", scopes.top.c_str());
    }
    fflush(stdout);
    // Mission load is over: the GPU memory every later frame starts from (the smoke report's load-end VRAM line).
    mc2::gpumem::log("load_end");
    g_missionReadyTrace = SDL_GetPerformanceCounter();
}

#ifndef DISABLE_GAMEOS_MAIN
namespace {

}  // namespace

// Engine entry: gate / log / config bring-up, the window and render context, the frame loop and the shutdown.
// The early-exit checks, the gate invalidation, the [CONFIG v1] dump and every other ordering-critical statement stay
// here in their original order; the large self-contained blocks are the file-static helpers above, called at the
// same points.
int main(int argc, char** argv)
{
    g_startup_t0 = SDL_GetPerformanceCounter();
    RenderCore::TextureService textures;
    RenderCore::TextureServiceScope textureScope(textures);
    // Gate reads resolved before this line are "pre-main" (static initialisers).
    mc2::gate::markMainEntered();
    mc2::overlap::registerThread(mc2::overlap::Owner::Main);   // thread registry: this thread owns Main (and Render)
    initDpiAwareness();

    initLauncherEnv();
    try { mc2::net::configure(argc, argv); }
    catch (const std::exception& error) { std::fprintf(stderr, "Invalid multiplayer argument: %s\n", error.what()); return 2; }

    // The launcher_env.json values were just applied: forget every gate a static initialiser cached.
    mc2::gate::invalidateAll();

    // Make stdout line-buffered (was fully buffered on Windows when redirected, hiding
    // output past the last explicit fflush before a crash). Harmless for interactive
    // runs; invaluable for diagnosing startup crashes when stdout is piped to a file.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    // MC2_LOG master log gate. stdout is UNBUFFERED (above), so every
    // per-mission telemetry printf flushes synchronously -> 400ms+ console
    // hitches when stdout is connected (a console or a pipe). Default
    // (MC2_LOG unset): redirect stdout to the NUL device so NORMAL PLAY never
    // pays that cost -- all the [TAG] printf telemetry is silently discarded.
    // MC2_LOG set (to anything): keep stdout live so the smoke harness can
    // parse [SMOKE v1]/[PERF v1]/etc., and so dev + hitch-catcher.bat see
    // output. Default (MC2_LOG unset) ALSO redirects stderr to NUL: the
    // render-modernization oracle/parity telemetry ([MECH_MATERIAL_GPU],
    // [OBJBATCHER], [TERRAIN_INDIRECT_PARITY], [DRAW_PACKET_V6], [RENDER_SNAPSHOT],
    // [RENDER_WORLD], [VISIBILITY], [ENGINE_VIEW], [VIEW_UNIFORMS], ...) prints
    // periodically to stderr and spams the console + causes fflush hitches even
    // with nothing set. Crashes still land in crash.txt (gos_crashbundle writes
    // the file independently of stderr), so nothing debuggable is lost.
    // MC2_LOG=1 restores full stdout+stderr for smoke/dev/hitch-catcher
    // (scripts/run_smoke.py, scripts/run_visual_capture.py set it).
    // LOGGING-FACADE-P0 (A3): the facade parses MC2_LOG here, BEFORE the redirect,
    // and decides stream liveness: set to anything (incl. legacy "1", or a
    // "cat:level" spec) = live; unset = redirect, exactly as before.
    if (!mc2::log::init()) {
#ifdef _WIN32
        const char* nullDevice = "NUL";
#else
        const char* nullDevice = "/dev/null";
#endif
        FILE* sink = freopen(nullDevice, "w", stdout);
        (void)sink;  // NUL is always present on Windows; if it failed, the stream
                     // simply stays connected (correct but hitchy) -- never fatal.
        FILE* esink = freopen(nullDevice, "w", stderr);
        (void)esink;
    }

    mc2::net::redirectLogs();
    // [CONFIG v1]: which MC2_* variables are set, what they parsed to, and the unregistered / bad ones.
    mc2::gate::dumpConfig(stderr);

    // Tier-1 instrumentation: one-line banner so every log file is
    // self-describing about which traces are enabled.
    projectz_trace_init();
    // Track A1: probe object-admission mode; emit [INSTR v1] object_admission_mode= line.
    objectAdmissionPredicate_init();
    // Track A2: probe effect-admission mode; emit [INSTR v1] effect_admission_mode= line.
    effectAdmissionPredicate_init();
    // F3 T1: probe lighting/shadow mode; emit [OBJECT_ADMISSION_PREDICATE v1] mode_select line.
    lightingShadowPredicate_init();
    // F3 T2: probe debug-overlay + selection-picking modes; emit mode_select lines.
    debugOverlayPredicate_init();
    selectionPickingPredicate_init();
    // Optional startup selftest — hard-fails on any boundary violation so the
    // operator knows immediately if the predicate body has a regression.
    {
    }
    initInstrBanner();

    //signal(SIGTRAP, SIG_IGN);

    // crashbundle_init installs the richer SEH filter (crash bundle +
    // diagnostic dialog). It supersedes the legacy mc2_unhandled_exception_filter
    // above; the old function is retained in this TU for reference but is
    // no longer the registered filter. On Linux it only starts the crashpad
    // handler (the minidump writer there).
    crashbundle_init();
#ifdef _WIN32
    (void)&mc2_unhandled_exception_filter; // silence "unused" warning
#endif
    startup_phase("process_start");

    // gather command line
	size_t cmdline_len = 0;
    for(int i=0;i<argc;++i) {
        cmdline_len += strlen(argv[i]);
        cmdline_len += 1; // ' '
    }
    auto cmdlineOwner = std::make_unique_for_overwrite<char[]>(cmdline_len + 1);
    char* cmdline = cmdlineOwner.get();
    size_t offset = 0;
    for(int i=0;i<argc;++i) {
        size_t arglen = strlen(argv[i]);
        memcpy(cmdline + offset, argv[i], arglen);
        cmdline[offset + arglen] = ' ';
        offset += arglen + 1;
    }
    cmdline[cmdline_len] = '\0';

    // Parse validation args before GameOS consumes the command line
    validateParseArgs(argc, argv);
    // Smoke-mode args must be parsed before GetGameOSEnvironment so any exit
    // on bad argv happens with no GL context held. The parser emits the
    // banner line when MC2_SMOKE_MODE=1.
    SmokeMode::parseArgs(argc, argv);
    SmokeMode::installAtexitSummary();

    // fills in Environment structure
    GetGameOSEnvironment(cmdline);
    initRenderOptionsFromConfig();
    initResolutionFromConfig();

    cmdlineOwner.reset();
    cmdline = nullptr;

    // WINDOWED-8006-1: explicit windowed-mode override (launcher "Windowed
    // mode" checkbox). Wins over options.cfg fullscreen.
    if (mc2::gate::get<"MC2_WINDOWED">()) Environment.fullScreen = 0;

    int w = Environment.screenWidth;
    int h = Environment.screenHeight;

    // ISSUE-49: pass the windowed boot state (options.cfg FullScreen=FALSE or
    // MC2_WINDOWED already folded into Environment.fullScreen above) so the
    // SDL window gets a resizable border for ANY windowed boot, not only when
    // the MC2_WINDOWED env var is set.
    // --headless (stage 1, docs/sim/headless-load.md): no window, device, renderer, GUI or audio; the sim runs on the frame loop's logic half.
    const bool headless = SmokeMode::state().headless;
    graphics::RenderWindowHandle win = headless ? graphics::RenderWindowHandle{} : graphics::create_window("mc2", w, h,
        Environment.fullScreen == 0);
    g_loadingWindow = win;
    if(!win && !headless)
        return 1;

    // [HUD-RES-CLAMP v1] (IN-MISSION-DETACH-1 restore): the WINDOW above is
    // created at the real options.cfg resolution (crisp ImGui/defs UI wants a
    // real-res drawable), but the legacy game logic's canvas goes back to the
    // canonical 800x600 — the legacy 2D HUD and the in-mission viewport/MVP
    // plumbing only lay out correctly at the tuned base. The UI-phase1 merge
    // dropped this clamp, which (a) reverted the HUD-scale fix and (b) fed
    // real-res Environment values into legacy in-mission math. Real drawable
    // size stays available via Environment.drawableWidth/Height and
    // GuiRuntime::GetDisplaySize. Memory: hud_scene_resolution_separation.
    Environment.screenWidth  = 800;
    Environment.screenHeight = 600;

    startup_phase("window_created");

    render_contract::initRenderPassTelemetry();   // [RENDER_PASS v1] (MC2_RENDER_PASS_TELEMETRY=1)
    render_contract::initRenderPassOrder();       // CONTRACT-3 (MC2_RENDER_PASS_ORDER=1)
    render_contract::initRenderPassScope();        // ENFORCEMENT-1 (MC2_RENDER_PASS_CONTRACT_TRACE/ASSERT=1)

    if (!headless) initVulkanBackend(win, w, h);

    startup_phase("native_device_ready");
    if (headless) gos_CreateRendererHeadless(w, h); else gos_CreateRenderer(win, w, h);
    startup_phase("renderer_created");
#ifdef MC2_IMGUI
    if (!headless) {
    GuiRuntime::Init();
    // SDL_ImplSDL2_NewFrame sets DisplaySize from SDL_GetWindowSize (logical pixels).
    // Set it explicitly here as well so the very first frame is correct even before
    // the first NewFrame call, and FramebufferScale is initialised for HiDPI paths.
    {
        int fbW = 0, fbH = 0;
        graphics::get_drawable_size(win, &fbW, &fbH);
        GuiRuntime::NotifyResize(fbW, fbH, Environment.screenWidth, Environment.screenHeight);
    }
    }
#endif
    if(!headless && !gos_CreateAudio())
    {   // not an error
        MC2_LOG(Audio, Debug, "Failed to create audio");
    }

    // AssetScale must load BEFORE InitializeGameEngine — that call creates
    // main-menu widgets via aObject::init which queries the manifest for
    // the chrome-flag opt-in. Late init = manifest empty at widget load.
    AssetScale::init("data/art/asset_sizes.csv");
    try { Environment.InitializeGameEngine(); }
    catch (const mc2::net::SessionStopped&) { g_exit = true; }
    startup_phase("engine_init_done");

	timing::init();


    // MC2_FPS_CAP: max FPS override. Applies to both menu and mission.
    //   unset → menu=90, mission=165 (defaults)
    //   0     → uncapped
    //   N     → N FPS in both states
    // timeBeginPeriod(1) brings Windows Sleep() to 1ms resolution.
    // Cap is skipped entirely when vsync is active (native FIFO presentation).
    // A set value (even an empty or non-numeric one: atoi -> 0, uncapped) overrides both caps; the row's own
    // default (90) is the menu cap, the mission cap stays 165 when unset.
    const bool fpsCapIsSet = mc2::gate::isSet<"MC2_FPS_CAP">();
    const int fpsCapSet = fpsCapIsSet
        ? (mc2::gate::isNonEmpty<"MC2_FPS_CAP">() ? mc2::gate::get<"MC2_FPS_CAP">() : 0) : -1;
    static const int s_fps_cap_menu    = fpsCapIsSet ? fpsCapSet : 90;
    static const int s_fps_cap_mission = fpsCapIsSet ? fpsCapSet : 165;
    bool s_vsync_active = false;
#ifdef MC2_VULKAN
    const auto& nativePresent = mc2vk::getOrCreateEnginePresentVk();
    s_vsync_active = nativePresent.valid() && nativePresent.health().present_mode == VK_PRESENT_MODE_FIFO_KHR;
#endif
    mc2::fmt::print_out("[FRAMECAP] MC2_FPS_CAP={} vsync={} -- menu={} FPS mission={} FPS{}\n",
           mc2::fmt::safe(fpsCapIsSet ? mc2::gate::raw<"MC2_FPS_CAP">() : "(unset)"),   // echoed as typed
           s_vsync_active ? "ON" : "OFF",
           s_fps_cap_menu, s_fps_cap_mission,
           s_vsync_active ? " (cap inactive, vsync owns pacing)" : "");
#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    static uint32_t s_hitchFrame = 0u;
#ifdef MC2_VULKAN
    if (mc2vk::engineVulkanContextValid()) {
        mc2vk::VulkanContext& vkCtx = mc2vk::getOrCreateEngineVulkanContext();
        // Recorded islands (VulkanContext::submitRecorded) record on the Render-domain pool, one island per block; with the pool off or
        // MC2_FRAME_JOBS=0 they record on this thread.
        vkCtx.setRecordExecutor({
            [] { return mc2::jobs::enabled() && frameJobsEnabled(); },
            [](int count, const std::function<void(int)>& body) {
                mc2::jobs::parallel_for(mc2::jobs::Domain::Render, count, 1, 100000, [&body](int begin, int end) { for (int i = begin; i < end; ++i) body(i); });
            }});
        vkCtx.beginFrame();   // frame 1 of the completion tracker
    }
    if (!headless) mc2vk::present_thread::start();
#endif
    bool presentJoinPending = false;
    while( !g_exit ) {
        ZoneScopedN("Frame");
        mc2::battlebench::beginFrame();
        const auto benchFrameStart = mc2::battlebench::output ? mc2::battlebench::Clock::now() : mc2::battlebench::Clock::time_point{};

		uint64_t start_tick = timing::gettickcount();
		const uint64_t frameClockStart = SDL_GetPerformanceCounter();
        mc2_hitch::BeginFrame(s_hitchFrame);
        // The frame containing mission load is excluded: time the first five
        // complete gameplay frames, with the existing hitch phase breakdown.
        const bool traceMissionFrame = g_mc2HitchEnabled && g_missionTraceFrames < 5;
        const auto missionFrameStart = traceMissionFrame ? SDL_GetPerformanceCounter() : 0;

    {
        ZoneScopedN("Frame.WaitSealed");
        mc2vk::present_thread::waitSealed();   // charges mc2::battlebench::WaitSealedNs inside
    }
    gos::host::recoverAsyncPresentFailure();
    if (presentJoinPending) {
        gos::host::retirePresentHandles(g_mc2RenderFrame);
        {
            ZoneScopedN("Frame.WaitWorker");
            mc2::battlebench::WaitScope waitSim(mc2::battlebench::WaitSimNs);
            const auto joinT0 = std::chrono::steady_clock::now();
            mc2::overlap::joinTail();
            mc2_hitch2::stats().simJoinNs.fetch_add(static_cast<uint64_t>(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - joinT0).count()), std::memory_order_relaxed);
        }
        presentJoinPending = false;
    }
    try { frameThrottleLogicEvents(); }
    catch (const mc2::net::SessionStopped&) { g_exit = true; break; }
        // Logic may have loaded another mission since the frame began.
        const bool completeMissionFrame = traceMissionFrame && g_missionTraceFrames < 5 && !g_missionReadyTrace;

        // DEV-SHELL-1: dev command socket poll. Single static-bool check when
        // MC2_DEV_SHELL unset. Commands run here, on the GL-owning main thread.
        if (gos_dev_shell::pollCommands(g_mc2FrameCounter.load(std::memory_order_relaxed)))
            g_exit = true;

        if (!headless) {   // headless: the logic half above is the whole frame
        const auto packetSequence = mc2::tick::retainRenderPacket();
        RenderCore::framePackets().retain(packetSequence);
        // Value arenas and typed frees share one CPU admission sequence. The
        // inline consumer retires here; S4 must report worker completion first.
        RenderSnapshot snap;
        {
            ZoneScopedN("ExtractRenderSnapshot");
            mc2::overlap::assertSimQuiescent("ExtractRenderSnapshot");
            snap = ExtractRenderSnapshot(packetSequence);
            mc2::overlap::extractRender();   // G7b K3: game render list + clock (zone ExtractRenderList)
            snap.draws = mc2::render_view::freezeDrawFrame(packetSequence);
        }
        mc2_debug_state::maybeWriteRenderState(snap);
    frameEmitDrawPackets(snap);

        {
            mc2::render_view::DrawFrameScope drawFrame(*snap.draws, &snap.frameArena);
            mc2::overlap::beginFrameDraw();
            ZoneScopedN("DrawScreen");
            mc2_hitch::HitchScope _hitchRender(mc2_hitch::HitchSpanKind::PhaseRender);

#ifdef MC2_VULKAN
            vk_render_pass_timer::Arm();   // frame top, outside every submit lock (see Arm)
#endif
            draw_screen();
        }
        // The frame's batched plain submits (scene, post) go to the queue now, not after the sim join and the worker's acquire.
        if (mc2vk::engineVulkanContextValid()) mc2vk::getOrCreateEngineVulkanContext().kick();
        // Release copied containers and marker holders on Main before retiring
        // their CPU sequence and draining typed resource destruction.
        snap.draws.reset();
        RetireRenderSnapshot(packetSequence);
        RenderCore::framePackets().retire(packetSequence);

        // Draw reads only the extracted snapshot. Wait before probes and frame counters touch live state;
        // the battle CPU timer includes any uncovered tick time rather than hiding it behind the frame cap.
        // Resource queues still commit after present, when this frame's GPU references have been submitted.
        {
            ZoneScopedN("Frame.WaitWorker");
            mc2::battlebench::WaitScope waitSim(mc2::battlebench::WaitSimNs);
            const auto joinT0 = std::chrono::steady_clock::now();
            mc2::overlap::waitWorker();
            mc2_hitch2::stats().simJoinNs.fetch_add(static_cast<uint64_t>(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - joinT0).count()), std::memory_order_relaxed);
        }
        // Drain the graveyard only once the sim worker is joined: its commands destroy appearances and effects that
        // the worker may be referencing (AppearanceTypeList refcounts are not atomic).
        mc2::tick::retireRenderPacket(packetSequence);

        {
            // Bump the canonical frame counter.
            ZoneScopedN("Frame.AdvanceFrameCounter");
            gos::host::advanceFrameCounter();
            // [OBJECT_RECON v1] slice-2 recon-zero accumulator drain.
            // No-op cost (~1 cmp + 1 branch) when env not set.
            mc2_object_recon::drainPerFrame(g_mc2FrameCounter.load(std::memory_order_relaxed));
            projectz_frame_tick();
            projectz_overlay_begin_frame();
        }

    frameScreenshotOracle();
    frameLoadRaceVerdict();
    frameLoadRaceProbes();
    frameCameraPark(win);

        // [RENDER_PASS_TIME v1] frame boundary: advance the query ring, poll
        // the oldest pending slot (never blocks), emit the aggregated line at
        // cadence. Placed before swap so the frame's queries are all closed.
        gos_render_pass_timer::FrameEnd();

        // [FRAME_PASS_STATS v1] frame boundary: promote this frame's per-pass
        // rows + aggregates, emit the summary line at cadence, reset. Placed
        // after the timer FrameEnd; advisory, OFF=zero cost.
        gos_frame_pass_stats::FrameEnd(static_cast<unsigned long>(g_mc2FrameCounter.load(std::memory_order_relaxed)));

    const bool asyncPresent = mc2vk::present_thread::enabled();
    if (asyncPresent) frameFrameCap(s_vsync_active, s_fps_cap_mission, s_fps_cap_menu, start_tick);
    frameSwapWindow(win);   // the shared host present also ends the Vulkan frame and advances the handle-slot clock
    presentJoinPending = asyncPresent;
    if (benchFrameStart != mc2::battlebench::Clock::time_point{}) mc2::battlebench::endFrame(mc2::battlebench::milliseconds(benchFrameStart));
    if (!asyncPresent) {
        frameFrameCap(s_vsync_active, s_fps_cap_mission, s_fps_cap_menu, start_tick);
        mc2::overlap::joinTail();
    }
        } else {
            mc2::overlap::beginFrameDraw(); // keep the early worker path active in headless determinism/save harnesses
            mc2::overlap::waitWorker();
            gos::host::advanceFrameCounter();
            mc2::overlap::joinTail();
        }   // !headless

        FrameMark;
#ifdef MC2_VULKAN
        mc2vk::reportDescWrites();
#endif
        mc2::gpumem::tick(static_cast<unsigned long>(g_mc2FrameCounter.load(std::memory_order_relaxed)));   // VRAM.Process.MB plot beside the frame marks

        {
            ZoneScopedN("Frame.ExitCheck");
            g_exit |= gosExitGameOS();
        }

		const double dt = timing_math::ticks_to_ms_fractional(
			SDL_GetPerformanceCounter() - frameClockStart, SDL_GetPerformanceFrequency());
		// S9D: pin the per-frame animation clock too. frameRate feeds
		// frameLength (= 1/frameRate) in mechcmd2.cpp, which drives ALL mech /
		// object / GOSFX motion and animation deltas. Without this, fixing
		// scenarioTime alone leaves animation phase tied to variable fps.
		// Single cached bool check; OFF path keeps the wall-clock frameRate.
		if (SmokeMode::fixedTimestepEnabled())
			frameRate = 30.0f;
		else
			frameRate = dt ? (1000.0f / static_cast<float>(dt)) : 0.0f;
        mc2_hitch::EndFrame(s_hitchFrame, static_cast<double>(dt));
        if (g_mc2HitchEnabled && g_missionReadyTrace) {
            std::println(stderr, "[MISSION_FRAME] ready_tail_ms={:.3f} frame={}",
                1000.0 * static_cast<double>(SDL_GetPerformanceCounter() - g_missionReadyTrace) /
                    static_cast<double>(SDL_GetPerformanceFrequency()), s_hitchFrame);
            g_missionReadyTrace = 0;
        }
        if (completeMissionFrame) {
            const double frameMs = 1000.0 * static_cast<double>(SDL_GetPerformanceCounter() - missionFrameStart) /
                static_cast<double>(SDL_GetPerformanceFrequency());
            const auto& a = g_mc2HitchAccum;
            std::println(stderr, "[MISSION_FRAME] index={} frame={} ms={:.3f} logic={:.3f} render={:.3f} present={:.3f} cap={:.3f} fence={:.3f}",
                ++g_missionTraceFrames, s_hitchFrame, frameMs, a.phaseLogicMs, a.phaseRenderMs,
                a.phasePresentMs, a.phaseCapMs, a.vkFence.totalMs);
        }
        ++s_hitchFrame;

        // Validation mode: record frame and check exit condition
        if (getValidateConfig().enabled) {
            validateRecordFrame(static_cast<float>(dt));
            if (validateShouldExit()) {
                validateWriteResults();
                break;
            }
        }

        // Smoke mode: timed auto-quit after durationSec seconds past mission_ready.
        if (SmokeMode::shouldQuit()) {
            SmokeMode::emitTiming("mission_quit");
            mc2::gpumem::log("run_end");   // before the summary: the GPU memory the run ended with
            SmokeMode::emitCleanSummary();
            g_exit = true;
            break;
        }
    }

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    {   // stdout/stderr are NUL in normal play: the trace file is the durable record of why the loop ended
        char why[96];
        const int n = mc2::fmt::to_buf(why, "{{\"reason\":\"{}\"}}", g_exitWhy);
        if (n > 0 && n < static_cast<int>(sizeof why)) mc2_diag::writeEvent("EXIT_REASON", 1, g_mc2FrameCounter.load(std::memory_order_relaxed), why);
        mc2_diag::flush();
    }
    mc2vk::present_thread::stop();
    if (presentJoinPending) mc2::overlap::joinTail();
    mc2::overlap::shutdownWorker();   // the sim worker is joined before engine and static teardown
    // Write validation results if game exited before frame limit
    if (getValidateConfig().enabled) {
        validateWriteResults();
    }

    mc2_object_recon::drainOnShutdown();
    projectz_shutdown();
    mc2::fmt::print_err("[SHUTDOWN] engine begin\n"); std::fflush(stderr);
    Environment.TerminateGameEngine();
    mc2::fmt::print_err("[SHUTDOWN] engine end\n"); std::fflush(stderr);
    AssetScale::shutdown();

#ifdef MC2_IMGUI
    if (!headless) GuiRuntime::Shutdown();
#endif
    if (!headless) gos_DestroyRenderer();   // headless: process exit frees the state-only renderer
    mc2::fmt::print_err("[SHUTDOWN] renderer end\n"); std::fflush(stderr);

    // Islands, then the present path, then the shared VulkanContext: one order, shared with the editor (gos_host_vk.cpp).
    if (!headless) gos::host::shutdownVulkanBackend();
    mc2::fmt::print_err("[SHUTDOWN] vulkan end\n"); std::fflush(stderr);

    if (!headless) graphics::destroy_window(win);

    gos_DestroyAudio();
    mc2::fmt::print_err("[SHUTDOWN] audio end\n"); std::fflush(stderr);

    // Return validation exit code if in validate mode
    if (getValidateConfig().enabled)
        return getValidateTelemetry().exitCode;

    if (mc2::net::exitStatus()) return mc2::net::exitStatus();
    return PlayerFrontEnd::restartIfRequested(argc, argv);
}
#endif // DISABLE_GAMEOS_MAIN
