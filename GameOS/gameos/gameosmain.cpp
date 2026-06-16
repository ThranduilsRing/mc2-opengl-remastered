#include "gameos.hpp"
#include "gos_render.h"
#include "render_snapshot.h"
#include "draw_packet_emitter.h"       // DrawPacket v0
#include "gos_static_prop_batcher.h"   // batcher_getSortedPacketCount — explicit, do not rely on transitive
#include "../../RenderCore/MaterialGpu.h"  // RenderCore::MaterialGpu for g_dpSelProp albedoTex fill
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <unordered_set>
#include <cstring>
#include <time.h>

// SHADOW-SPINE-0: C-linkage accessors from batcher TUs. Declared at file
// scope because `extern "C"` is illegal at function/block scope.
extern "C" uint32_t gos_getMechShadowProgramId();
extern "C" uint32_t gos_getMechShadowTypesDrawn();
extern "C" uint32_t gos_getMechShadowInstDrawn();
extern "C" uint32_t gos_getStaticPropShadowTypesDrawn();
extern "C" uint32_t gos_getStaticPropShadowInstDrawn();

// VFX-SPINE-0: C-linkage accessors from the particle bridge + batcher TUs.
// Same file-scope rule as the shadow accessors above (extern "C" cannot live
// at block scope). Read-only — these only return file-static counters.
extern "C" unsigned int       gos_vfx_getParticleProgramId();
extern "C" unsigned int       gos_vfx_getSsboCapacity();
extern "C" int                gos_vfx_getInitFailed();
extern "C" int                gos_vfx_getCameraSetThisFrame();
extern "C" int                gos_vfx_getDebugMode();
extern "C" int                mc2_vfx_isEnabled();
extern "C" int                mc2_vfx_isLogEnabled();
extern "C" unsigned int       mc2_vfx_getBudget();
extern "C" int                mc2_vfx_getOverflowReported();
extern "C" unsigned long long mc2_vfx_getEmitTotal();
extern "C" unsigned long long mc2_vfx_getFlushTotal();
extern "C" unsigned long long mc2_vfx_getNonemptyFlushTotal();
extern "C" unsigned long long mc2_vfx_getRecordsFlushedTotal();
extern "C" unsigned int       mc2_vfx_getRecordsPerFlushMax();
extern "C" unsigned long long mc2_vfx_getTrailSpawnTotal();
extern "C" unsigned long long mc2_vfx_getTrailHeadTotal();

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
            (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    }

    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, NULL, TRUE);

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
                          NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
            break;
        if (frame.AddrPC.Offset == 0) break;

        char symBuf[sizeof(SYMBOL_INFO) + 512];
        SYMBOL_INFO* sym = (SYMBOL_INFO*)symBuf;
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
            i, (unsigned long long)frame.AddrPC.Offset, symName, fileName, lineNum);
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
#include "gos_input.h"

#include "utils/camera.h"
#include "utils/shader_builder.h"
#include "utils/gl_utils.h"
#include "utils/timing.h"
#include "gos_postprocess.h"
#include "gos_validate.h"
#include "gos_screenshot.h"   // deterministic backbuffer->TGA capture (oracle)
#include "gos_visual_capture.h"  // [VISUAL_CAPTURE v1] S9 PNG+sidecar + bookmark replay
#include "gos_static_prop_killswitch.h"
#include "gos_static_prop_registry.h"  // Stage 3.C: isEnabled() for [INSTR v1]
#include "../../RenderWorld/RenderWorld.h"  // M1 Task 14
#include "../../mclib/render_contract.h"   // Phase 2 assert init
#include "asset_scale.h"
#include "gos_crashbundle.h"
#include "gos_smoke.h"

#include <signal.h>
#include "gos_profiler.h"
#include "tgl.h"   // drainTglPoolStats / drainTglPoolStatsOnShutdown (Tier-1 instr)
#include "gos_object_recon_tracy.h"  // [OBJECT_RECON v1] slice-2 recon zero (env-gated)
#include "projectz_trace.h"  // projectz_trace_init/frame_tick/shutdown (PROJECTZ v1)
#include "projectz_overlay.h" // RAlt+P debug overlay (commit 4)
#include "gos_visual_diff.h"  // Stage 2.E pre-HUD capture + Ctrl+Shift+P record
#include "gos_rdoc_capture.h"  // Tier 5: env-gated in-process RenderDoc capture
#include "gos_terrain_indirect.h"  // [INSTR v1] banner: terrain_indirect{,_parity} fields
#include "terrain_surface_trace.h" // [INSTR v1] banner: terrain_surface_trace field (PR-0)
#include "gpu_cull_record.h"       // C0-1: GpuActorRecord schema selftest
#include "gpu_cull_readback.h"    // C2: async readback ring buffer selftest
#include "object_admission_predicate.h"  // Track A1: init probe + selftest gate
#include "view_uniforms_gl.h"                     // getCurrentView (unconditional: used by debug_state_dump)
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

// Tier-1 instrumentation (stability spec §5.1): single source of truth for
// the frame=... field used by TGL_POOL, DESTROY, and GL_ERROR log lines.
// Definition lives in mclib/tgl.cpp so data tools (aseconv, pak, makefst,
// makersp) that link mclib without gameosmain still resolve the symbol.
// This TU owns the per-frame increment; everyone else is a read-only extern.
extern uint32_t g_mc2FrameCounter;

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

extern void gos_GetTerrainCameraPos(float* x, float* y, float* z);

extern void gos_CreateRenderer(graphics::RenderContextHandle ctx_h, graphics::RenderWindowHandle win_h, int w, int h);
extern void gos_DestroyRenderer();
extern void gos_RendererBeginFrame();
extern void gos_RendererEndFrame();
extern void gos_RendererFlushHUDBatch();
extern void gos_RendererHandleEvents();
extern void gos_RenderUpdateDebugInput();
extern void gos_RenderEnableDebugDrawCalls();
extern bool gos_RenderGetEnableDebugDrawCalls();

extern bool gosExitGameOS();

extern bool gos_CreateAudio();
extern void gos_DestroyAudio();

static bool g_exit = false;
static bool g_focus_lost = false;

// Owned by gos_render.cpp; needed here to query SDL_WINDOW_MINIMIZED so the
// background-throttle path can distinguish "window invisible" from merely
// "another window has keyboard focus" (which is normal during gameplay when
// the user clicks a chat / browser / profiler window).
extern SDL_Window* g_sdl_window;

// Global runtime toggle for the GPU static-prop renderer.
// Definition lives in gos_static_prop_batcher.cpp (in the gameos lib) so
// data-tool executables that link mclib but not gameos_main still resolve
// the symbol. Toggled at runtime via RAlt+0 (see handle_key_down).
extern bool g_useGpuStaticProps;
#if 0
static camera g_camera;
#endif

static void handle_key_down( SDL_Keysym* keysym ) {
    const bool alt_debug = (keysym->mod & KMOD_ALT) != 0;
    switch( keysym->sym ) {
        case SDLK_ESCAPE:
            if(alt_debug)
                g_exit = true;
            break;
        case 'd':
            if(alt_debug)
                gos_RenderEnableDebugDrawCalls();
            break;
        case SDLK_F1:
            if (alt_debug) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->bloomEnabled_ = !pp->bloomEnabled_;
                    fprintf(stderr, "Bloom: %s\n", pp->bloomEnabled_ ? "ON" : "OFF");
                }
            }
            break;
        case SDLK_F2:
            if (alt_debug) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    if (!pp->showShadowDebug_) {
                        pp->showShadowDebug_ = true;
                        pp->shadowDebugMode_ = 0;
                        fprintf(stderr, "Shadow Debug: STATIC map\n");
                    } else if (pp->shadowDebugMode_ == 0) {
                        pp->shadowDebugMode_ = 1;
                        fprintf(stderr, "Shadow Debug: DYNAMIC map\n");
                    } else {
                        pp->showShadowDebug_ = false;
                        fprintf(stderr, "Shadow Debug: OFF\n");
                    }
                }
            }
            break;
        case SDLK_F3:
            if (alt_debug) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->shadowsEnabled_ = !pp->shadowsEnabled_;
                    fprintf(stderr, "Shadows: %s\n", pp->shadowsEnabled_ ? "ON" : "OFF");
                }
            }
            break;
        case SDLK_F5:
            if (alt_debug) {
                bool cur = gos_GetTerrainDrawEnabled();
                gos_SetTerrainDrawEnabled(!cur);
                fprintf(stderr, "Terrain Draw: %s\n", !cur ? "ON" : "OFF");
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
                fprintf(stderr, "Surface Debug: %s\n",
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
                        fprintf(stderr, "Screen Shadows: ON\n");
                    } else if (pp->screenShadowDebug_ == 0) {
                        pp->screenShadowDebug_ = 1;
                        fprintf(stderr, "Screen Shadows: DEBUG (red=terrain, green=lit, blue=shadowed, black=sky)\n");
                    } else {
                        pp->screenShadowEnabled_ = false;
                        pp->screenShadowDebug_ = 0;
                        fprintf(stderr, "Screen Shadows: OFF\n");
                    }
                }
            }
            break;
        case SDLK_8:
            if (alt_debug) {
                int mode = ((int)gos_GetTerrainDebugMode() + 1) % 8;
                gos_SetTerrainDebugMode((float)mode);
                switch (mode) {
                    case 0: fprintf(stderr, "Surface Debug: OFF\n"); break;
                    case 1: fprintf(stderr, "Surface Debug: DEPTH (R=actual G=undisplaced)\n"); break;
                    case 2: fprintf(stderr, "Surface Debug: RAW colormap (all terrain)\n"); break;
                    case 3: fprintf(stderr, "Surface Debug: BLURRED colormap\n"); break;
                    case 4: fprintf(stderr, "Surface Debug: MATERIAL weights (RGB=rock/grass/dirt)\n"); break;
                    case 5: fprintf(stderr, "Surface Debug: NORMAL lighting\n"); break;
                    case 6: fprintf(stderr, "Surface Debug: TERRAIN shadow factor\n"); break;
                    case 7: fprintf(stderr, "Surface Debug: CLOUD factor\n"); break;
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
                fprintf(stderr, "GPU Props Debug: %d (%s)\n", m, name);
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
                fprintf(stderr, "HUD Scale: %.2f\n", s);
            }
            break;
        case SDLK_6:
            if (alt_debug) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->godrayEnabled_ = !pp->godrayEnabled_;
                    fprintf(stderr, "God Rays: %s\n", pp->godrayEnabled_ ? "ON" : "OFF");
                }
            }
            break;
        case SDLK_7:
            if (alt_debug) {
                gosPostProcess* pp = getGosPostProcess();
                if (pp) {
                    pp->shorelineEnabled_ = !pp->shorelineEnabled_;
                    fprintf(stderr, "Shorelines: %s\n", pp->shorelineEnabled_ ? "ON" : "OFF");
                }
            }
            break;
        case SDLK_0:
            if (alt_debug) {
                if (g_useGpuObjects) {
                    // Slice 1 is active — legacy killswitch is mutually
                    // exclusive (spec R1). Ignore the toggle and log once.
                    static bool s_loggedBlock = false;
                    if (!s_loggedBlock) {
                        fprintf(stderr, "[OBJBATCHER v1] event=legacy_toggle_blocked "
                                        "reason=g_useGpuObjects_active\n");
                        fflush(stderr);
                        s_loggedBlock = true;
                    }
                } else {
                    g_useGpuStaticProps = !g_useGpuStaticProps;
                    fprintf(stderr, "GPU Static Props: %s\n",
                            g_useGpuStaticProps ? "ON" : "OFF");
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

static void process_events( void ) {

    input::beginUpdateMouseState();

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
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
            g_exit = true;
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
                float w = (float)event.window.data1;
                float h = (float)event.window.data2;
                glViewport(0, 0, (GLsizei)w, (GLsizei)h);
                graphics::refresh_mouse_grab();
                SPEW(("INPUT", "resize event: w: %f h:%f\n", w, h));
                break;
            }
            case SDL_WINDOWEVENT_MOVED:
                graphics::refresh_mouse_grab();
                break;
            case SDL_WINDOWEVENT_FOCUS_GAINED:
                // Re-assert the mouse grab on focus regain. The initial
                // grab at window-creation can be only partially applied on
                // multi-monitor setups (symptom: cursor escapes to the
                // leftmost column of the adjacent monitor without losing
                // focus). Re-asserting once focus is actually confirmed
                // makes the clamp stick.
                graphics::set_mouse_grab(true);
                g_focus_lost = false;
                break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
                // Release grab so Alt-Tab and other windows behave cleanly.
                graphics::set_mouse_grab(false);
                g_focus_lost = true;
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

bool g_force43Aspect = true;

static void draw_screen( void )
{
    g_disable_quads = false;

    gosPostProcess* pp = getGosPostProcess();

    // Apply validation mode feature overrides
    if (pp && getValidateConfig().enabled) {
        ValidateConfig& vc = getValidateConfig();
        if (vc.bloomOverride >= 0) pp->bloomEnabled_ = vc.bloomOverride;
        if (vc.shadowsOverride >= 0) pp->shadowsEnabled_ = vc.shadowsOverride;
        if (vc.fxaaOverride >= 0) pp->fxaaEnabled_ = vc.fxaaOverride;
    }

    glCullFace(GL_FRONT);

	int viewport_w, viewport_h;
	if (g_force43Aspect) {
		const float kTargetAspect = 4.0f / 3.0f;
		const int drawW = Environment.drawableWidth;
		const int drawH = Environment.drawableHeight;
		if ((float)drawW / (float)drawH > kTargetAspect) {
			viewport_h = drawH;
			viewport_w = (int)(drawH * kTargetAspect);
		} else {
			viewport_w = drawW;
			viewport_h = (int)(drawW / kTargetAspect);
		}
	} else {
		viewport_w = Environment.drawableWidth;
		viewport_h = Environment.drawableHeight;
	}

    if (pp) {
        pp->resize(viewport_w, viewport_h);
        pp->beginScene();
    }

    glViewport(0, 0, viewport_w, viewport_h);
    glClearDepth(0.0f);   // reverse-Z (U2): far plane = depth 0
    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
#if 0
    mat4 proj;
    g_camera.get_projection(&proj);
    mat4 viewM;
    g_camera.get_view(&viewM);
#endif

#if 0
    gos_VERTEX q[4];
    q[0].x = 10; q[0].y = 10;
    q[0].z = 0.0;
    q[0].rhw = 1;
    q[0].argb = 0xffff0000;
    q[1] = q[2] = q[3] = q[0];

    q[1].x = 210; q[1].y = 10;
    q[2].x = 110; q[2].y = 210;
    q[3].x = 10; q[3].y = 210;

    g_disable_quads = false;
    gos_DrawQuads(&q[0], 4);
    g_disable_quads = true;
#endif
	/*
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadTransposeMatrixf((const float*)proj);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadTransposeMatrixf((const float*)viewM);
	*/

    CHECK_GL_ERROR;

    // TODO: reset all states to sane defaults!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    glDepthMask(GL_TRUE);
    // Blue-grey sky during gameplay, black for menus/loading/mech bay
    // Uses previous frame's terrain flag (set during rendering, cleared in beginScene)
    {
        gosPostProcess* ppClear = getGosPostProcess();
        if (ppClear && ppClear->prevFrameHadTerrain_)
            glClearColor(0.55f, 0.62f, 0.72f, 1.0f);
        else
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    }
    glClearDepth(0.0f);   // reverse-Z (U2): far plane = depth 0
    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );

    // F3: overwrite attachment 1 (GBuffer1) with the post-shadow-eligible sentinel.
    // glClearColor sets alpha=1.0 in both gameplay and menu paths, which would
    // otherwise leave non-overwritten pixels reading "shadow handled". This
    // sentinel ensures defense-in-depth coherence per F3 design v2 §5A.
    if (pp) pp->clearGBuffer1();

    // Skybox disabled — terrain fog provides atmosphere, bright sky looked jarring
    // if (pp) pp->renderSkybox(0.3f, 0.7f, 0.2f);

    {
        ZoneScopedN("Camera.UpdateRenderers");
        { ZoneScopedN("Camera.UpdateRenderers gos_RendererBeginFrame"); gos_RendererBeginFrame(); }
        Environment.UpdateRenderers();
        { ZoneScopedN("Camera.UpdateRenderers gos_RendererEndFrame"); gos_RendererEndFrame(); }
        RenderWorld::frameBannerTick();  // M1 Task 14 (m2 fix: post-EndFrame)
    }

    glUseProgram(0);

    // Composite post-processed scene to default framebuffer
    if (pp) {
        render_contract::noteRenderPass(render_contract::PassIdentity::PostProcess,
                                        "gosPostProcess_endScene");
        // [RENDER_PASS_TIME v1] post = whole endScene chain (HZB/SSAO/screen
        // shadow/shoreline/godrays/bloom/composite). Single outer scope: the
        // inner stages call each other, and GL_TIME_ELAPSED cannot nest.
        gos_render_pass_timer::Begin(gos_render_pass_timer::Pass_Post);
        pp->endScene();
        gos_render_pass_timer::End(gos_render_pass_timer::Pass_Post);
    }

    // Stage 2.E visual-diff capture hook. Must fire AFTER pp->endScene() so the
    // default framebuffer holds scene+post-process, but BEFORE projectz_overlay
    // and HUD replay so neither leaks into the captured TGA. Default-off
    // (early-return when MC2_VISUAL_DIFF_CAPTURE is unset).
    VisualDiff::onFrameTick(Environment.drawableWidth, Environment.drawableHeight);

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
    drainGLErrors("hud");
    //CHECK_GL_ERROR;
#ifdef MC2_IMGUI
    GuiRuntime::Render();
#endif
}

extern float frameRate;


const char* getStringForType(GLenum type)
{
	switch (type)
	{
	case GL_DEBUG_TYPE_ERROR: return "DEBUG_TYPE_ERROR";
	case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "DEBUG_TYPE_DEPRECATED_BEHAVIOR";
	case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: return "DEBUG_TYPE_UNDEFINED_BEHAVIOR";
	case GL_DEBUG_TYPE_PERFORMANCE: return "DEBUG_TYPE_PERFORMANCE";
	case GL_DEBUG_TYPE_PORTABILITY: return "DEBUG_TYPE_PORTABILITY";
	case GL_DEBUG_TYPE_MARKER: return "DEBUG_TYPE_MARKER";
	case GL_DEBUG_TYPE_PUSH_GROUP: return "DEBUG_TYPE_PUSH_GROUP";
	case GL_DEBUG_TYPE_POP_GROUP: return "DEBUG_TYPE_POP_GROUP";
	case GL_DEBUG_TYPE_OTHER: return "DEBUG_TYPE_OTHER";
	default: return "(undefined)";
	}
}

const char* getStringForSource(GLenum type)
{
	switch (type)
	{
	case GL_DEBUG_SOURCE_API: return "DEBUG_SOURCE_API";
	case GL_DEBUG_SOURCE_SHADER_COMPILER: return "DEBUG_SOURCE_SHADER_COMPILER";
	case GL_DEBUG_SOURCE_WINDOW_SYSTEM: return "DEBUG_SOURCE_WINDOW_SYSTEM";
	case GL_DEBUG_SOURCE_THIRD_PARTY: return "DEBUG_SOURCE_THIRD_PARTY";
	case GL_DEBUG_SOURCE_APPLICATION: return "DEBUG_SOURCE_APPLICATION";
	case GL_DEBUG_SOURCE_OTHER: return "DEBUG_SOURCE_OTHER";
	default: return "(undefined)";
	}
}

const char* getStringForSeverity(GLenum type)
{
	switch (type)
	{
	case GL_DEBUG_SEVERITY_HIGH: return "DEBUG_SEVERITY_HIGH";
	case GL_DEBUG_SEVERITY_MEDIUM: return "DEBUG_SEVERITY_MEDIUM";
	case GL_DEBUG_SEVERITY_LOW: return "DEBUG_SEVERITY_LOW";
	case GL_DEBUG_SEVERITY_NOTIFICATION: return "DEBUG_SEVERITY_NOTIFICATION";
	default: return "(undefined)";
	}
}
namespace {
    // Startup phase timing. Anchor at the top of main(). Cheap printfs --
    // total cost is microseconds, but the signal for triage is high.
    static Uint64 g_startup_t0 = 0;
    static double startup_elapsed() {
        const Uint64 now = SDL_GetPerformanceCounter();
        const double freq = (double)SDL_GetPerformanceFrequency();
        return (double)(now - g_startup_t0) / freq;
    }
    static void startup_phase(const char* name) {
        printf("[TIME] t=%6.2fs  phase=%s\n", startup_elapsed(), name);
        if (SmokeMode::state().enabled) {
            // Emit canonical smoke-line for the same milestone.
            SmokeMode::emitTiming(name);
        }
    }
}

// Mission-load phase timing. Exposed (file-scope linkage, not namespaced)
// so code/mission.cpp can declare these by forward-decl and call into them
// without a new header. Pattern mirrors the startup timing above.
static Uint64 g_mission_t0 = 0;


extern "C" void mission_phase_begin()
{
    g_mission_t0 = SDL_GetPerformanceCounter();
    const double freq = (double)SDL_GetPerformanceFrequency();
    (void)freq; // suppress unused-var if compiler gets clever
    printf("[MISSION] t=  0.00s  phase=mission_load_start\n");
}

extern "C" void mission_phase_mark(const char* name)
{
    const Uint64 now = SDL_GetPerformanceCounter();
    const double freq = (double)SDL_GetPerformanceFrequency();
    const double elapsed = (double)(now - g_mission_t0) / freq;
    printf("[MISSION] t=%6.2fs  phase=%s\n", elapsed, name);
}

//typedef void (GLAPIENTRY *GLDEBUGPROCARB)(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam);
#ifdef PLATFORM_WINDOWS
void GLAPIENTRY OpenGLDebugLog(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
#else
void GLAPIENTRY OpenGLDebugLog(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar* message, GLvoid* userParam)
#endif
{
	if (severity != GL_DEBUG_SEVERITY_NOTIFICATION && severity != GL_DEBUG_SEVERITY_LOW)
	{
		printf("Type: %s; Source: %s; ID: %d; Severity : %s\n",
			getStringForType(type),
			getStringForSource(source),
			id,
			getStringForSeverity(severity)
		);
		printf("Message : %s\n", message);
	}
	// Tier 1.2 (docs/testing-strategy.md): opt-in fatal abort on GL_DEBUG_SEVERITY_HIGH.
	// Default behavior unchanged. Catches the GL state-leak / sampler-inheritance /
	// depth-state-inheritance bug class documented in
	// memory/{blend,sampler,gpu_direct_depth}_state_inheritance.md.
	// MEDIUM/LOW are too noisy on AMD (perf warnings on every TGL stream).
	// Function-scope static: 99.9% of callback invocations are a single load+branch.
	static const bool s_glDebugFatal = (std::getenv("MC2_GL_DEBUG_FATAL") != nullptr);
	if (s_glDebugFatal && severity == GL_DEBUG_SEVERITY_HIGH)
	{
		std::fprintf(stderr,
			"[MC2_GL_DEBUG_FATAL] severity=HIGH source=0x%x type=0x%x "
			"id=%u\n  %.*s\n",
			source, type, id, (int)length, message);
		std::fflush(stderr);
		std::abort();
	}
}

#ifndef DISABLE_GAMEOS_MAIN
int main(int argc, char** argv)
{
    // Make stdout line-buffered (was fully buffered on Windows when redirected, hiding
    // output past the last explicit fflush before a crash). Harmless for interactive
    // runs; invaluable for diagnosing startup crashes when stdout is piped to a file.
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

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
    if (const char* st = std::getenv("MC2_OBJECT_ADMISSION_SELFTEST")) {
        if (std::strcmp(st, "1") == 0) {
            int fails = objectAdmissionPredicate_selftest();
            std::printf("[OBJECT_ADMISSION v1] event=selftest_summary fails=%d\n", fails);
            std::fflush(stdout);
            if (fails != 0) {
                // Hard fail — selftest is opt-in (env-gated); when an operator
                // turned it on and a case failed, the predicate body has a real
                // boundary error and we must NOT continue into rendering.
                // Failing loudly here surfaces the bug; smoke runner reports
                // the abort as a failed mission.
                gosASSERT(false);
                std::abort();
            }
        }
    }
    {
        const bool tgl     = (getenv("MC2_TGL_POOL_TRACE")       != nullptr);
        const bool destr   = (getenv("MC2_DESTROY_TRACE")        != nullptr);
        // GL_ERROR is default-on; the env var suppresses it.
        const bool glprint = (getenv("MC2_GL_ERROR_DRAIN_SILENT") == nullptr);
        const bool smoke   = (getenv("MC2_SMOKE_MODE")           != nullptr);
        const bool waterFp = (getenv("MC2_RENDER_WATER_FASTPATH")     != nullptr);
        const bool waterPc = (getenv("MC2_RENDER_WATER_PARITY_CHECK") != nullptr);
        const bool vpFast  = (getenv("MC2_VERTEX_PROJECT_FAST")       != nullptr);
        const bool vpPar   = (getenv("MC2_VERTEX_PROJECT_PARITY")     != nullptr);
        // MC2_GPU_OBJECTS env override: "0" disables (opt-out from the
        // 2026-05-04 default-on flip), any other value (including "1")
        // enables. Unset leaves the compile-time default (true).
        // Slice 1 invariant: mutually exclusive with legacy killswitch.
        // Setting g_useGpuObjects at startup here happens before any code
        // path can read it; legacy g_useGpuStaticProps starts false.
        const char* gpuObjEnv = getenv("MC2_GPU_OBJECTS");
        if (gpuObjEnv) {
            g_useGpuObjects = (gpuObjEnv[0] != '0');
        }
        const bool gpuObj = g_useGpuObjects;
        // [OBJECT_RECON v1] read MC2_OBJECT_RECON_TRACY here so the gate is
        // live before any update kernel runs. drainPerFrame() lazy-inits
        // too, but eager init avoids missing the very-first-frame data.
        mc2_object_recon::initFromEnv();
        const bool objRecon = mc2_object_recon::g_enabled;
        const bool tInd    = gos_terrain_indirect::IsEnabled();
        const bool tIndP   = gos_terrain_indirect::IsParityCheckEnabled();
        const bool tIndM   = gos_terrain_indirect::IsMineEnabled();   // PR2c
        const bool tIndO   = gos_terrain_indirect::IsOverlayEnabled();  // PR2b
        const bool tIndOP  = gos_terrain_indirect::IsOverlayParityCheckEnabled();  // PR2b
        // ParseEnvBool semantics: "0"/"false"/"off"/"no" → false, anything else → true.
        // Must match the ParseEnvBool logic in code/terrobj.cpp so the banner
        // accurately reflects the actual gate state (getenv!=nullptr would report
        // MC2_STATIC_UPDATE_SKIP=0 as enabled, breaking operator trust in the banner).
        auto suParseBool = [](const char* name, bool def = false) -> bool {
            const char* v = getenv(name);
            if (!v || !*v) return def;
            if (v[0] == '0' && !v[1]) return false;
            if (!_stricmp(v, "false") || !_stricmp(v, "off") || !_stricmp(v, "no")) return false;
            return true;
        };
        const bool suTrace = suParseBool("MC2_STATIC_UPDATE_TRACE");
        // 2026-05-11: MC2_STATIC_UPDATE_SKIP defaults ON; mirror in terrobj.cpp:88.
        const bool suSkip  = suParseBool("MC2_STATIC_UPDATE_SKIP", true);
        // Default ON; MC2_GPU_CULL_SUBSTRATE=0 opts out.
        const char* _gcs = getenv("MC2_GPU_CULL_SUBSTRATE");
        bool gpuCullSubstrate = (_gcs == nullptr || _gcs[0] != '0');
        bool gpuCullParity    = (getenv("MC2_GPU_CULL_AABB_PARITY") != nullptr && getenv("MC2_GPU_CULL_AABB_PARITY")[0] != '0');
        const bool shrHR      = (getenv("MC2_SHADER_HOT_RELOAD")    != nullptr);
        const bool revZ       = (getenv("MC2_REVERSE_Z_TRACE")      != nullptr);
        // PR-0: dormant [TERRAIN_SURFACE v1] lifecycle channel gate. Trace-only,
        // default-OFF, separate from the MC2_TERRAIN_SURFACE path-select switch.
        const bool tSurfTrc   = mc2_terrain_surface_trace::enabled();
        // T3.1: (E) SpotLight_ -> real illumination env gate deleted. The
        // new TG_Light registration path is the unconditional production
        // behavior. MC2_SPOTLIGHT_REAL_TRACE counters remain (Debug
        // Instrumentation Rule: demote, don't delete) for future diagnostics.
        const char* build  =
#ifdef MC2_BUILD_HASH
            MC2_BUILD_HASH
#else
            "UNKNOWN"
#endif
            ;
        // Grew 384 -> 512 -> 640 to absorb terrain_indirect{,_parity}
        // and gpu_objects fields without truncation.
        // Grew 640 -> 720 to absorb gpu_cull_substrate and gpu_cull_aabb_parity.
        // Grew 720 -> 768 to absorb terrain_indirect_mine (PR2c Stage 0c).
        // Grew 768 -> 832 to absorb terrain_indirect_overlay{,_parity} (PR2b).
        // Grew 832 -> 896 to absorb reverse_z_trace (reverse-Z float depth).
        // Grew 896 -> 960 to absorb terrain_surface_trace (PR-0 Wave 0).
        // Grew 960 -> 1024 to absorb spotlight_real (T1.2, (E) SpotLight_ -> real).
        // Shrank back to 960 effective use post-T3.1 (spotlight_real field
        // removed); buffer kept at 1024 for future absorbers.
        // (water_skip_env field was tentatively added during the closed
        // water-projection-skip slice attempt; removed when the slice
        // closed — premise invalidated by Stage 0 M3 audit.)
        char _cbbuf[1024];
        snprintf(_cbbuf, sizeof(_cbbuf),
            "[INSTR v1] enabled: tgl_pool=%d destroy=%d gl_error_print=%d "
            "smoke=%d water_fp=%d water_parity=%d vp_fast=%d vp_parity=%d "
            "terrain_indirect=%d terrain_indirect_parity=%d terrain_indirect_mine=%d "
            "terrain_indirect_overlay=%d terrain_indirect_overlay_parity=%d "
            "gpu_objects=%d obj_recon_tracy=%d "
            "static_update_trace=%d static_update_skip=%d "
            "static_prop_registry=%d "
            "gpu_cull_substrate=%d gpu_cull_aabb_parity=%d "
            "shader_hot_reload=%d "
            "reverse_z_trace=%d "
            "terrain_surface_trace=%d "
            "camera_motion=1 "
            "build=%s",
            tgl ? 1 : 0, destr ? 1 : 0, glprint ? 1 : 0, smoke ? 1 : 0,
            waterFp ? 1 : 0, waterPc ? 1 : 0, vpFast ? 1 : 0, vpPar ? 1 : 0,
            tInd ? 1 : 0, tIndP ? 1 : 0, tIndM ? 1 : 0,
            tIndO ? 1 : 0, tIndOP ? 1 : 0,
            gpuObj ? 1 : 0, objRecon ? 1 : 0,
            suTrace ? 1 : 0, suSkip ? 1 : 0,
            GpuStaticPropRegistry::isEnabled() ? 1 : 0,
            gpuCullSubstrate ? 1 : 0, gpuCullParity ? 1 : 0,
            shrHR ? 1 : 0,
            revZ ? 1 : 0,
            tSurfTrc ? 1 : 0,
            build);
        puts(_cbbuf);
        crashbundle_append(_cbbuf);

        // [MATERIAL_GPU v4] startup banner — separate from [INSTR v1] to keep
        // that buffer size stable. Duplicates the getenv check because
        // s_materialGpuEnabled is a private file-scope static in the batcher
        // (not accessible cross-TU — Option A from MaterialGpu-2 spec §3).
        // v5: MC2_MATERIAL_GPU defaults ON; set to "0" to disable.
        {
            const char* matGpuEnv  = getenv("MC2_MATERIAL_GPU");
            const bool  matGpuOn   = (matGpuEnv == nullptr || matGpuEnv[0] != '0');
            const char* matSampleEnv = getenv("MC2_MATERIAL_GPU_SAMPLE");
            const bool  matSampleOn  = (matSampleEnv == nullptr || matSampleEnv[0] != '0');
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "[MATERIAL_GPU v4] enabled=%d sample=%d binding=5 (default-ON; MC2_MATERIAL_GPU=0 disables)\n",
                          (int)matGpuOn, (int)matSampleOn);
            std::fputs(buf, stderr);
        }

        // [UNIFIED_PROJ v1] Warn when MC2_DISABLE_GOSFX=0 dev-override is active
        // under unified projection. Default MC2_DISABLE_GOSFX=1 is unaffected.
        {
            const char* gosfxEnv = getenv("MC2_DISABLE_GOSFX");
            if (gosfxEnv != nullptr && strcmp(gosfxEnv, "0") == 0) {
                fprintf(stderr,
                    "[UNIFIED_PROJ v1] WARN: MC2_DISABLE_GOSFX=0 active under unified "
                    "projection. gosFX particles will render incorrectly (MLR clipper "
                    "uses stale convention). See CLAUDE.md Known Issues.\n");
            }
        }

        // GPU cull record schema selftest.
        {
            int gcFail = gpu_cull::gpu_cull_record_selftest();
            if (gcFail > 0) {
                fprintf(stderr, "[GPU_CULL v1] FATAL: selftest failed (%d); "
                                "GpuActorRecord layout corrupt, see log above.\n", gcFail);
                fflush(stderr);
                gosASSERT(false);
                abort();
            }
        }

        // C2: readback ring buffer three-tier selftest is called from within
        // readback_init() (at mission load) — not here. readback_init() requires GL
        // context + a valid maxActors count, so it cannot run at engine startup.
        // The selftest emits [GPU_CULL v1] event=readback_selftest pass=3 fail=0
        // to the mission log when MC2_GPU_CULL_READBACK=1 is set.
        // (No call needed here — readback_selftest() in readback.cpp is self-contained.)

        if (getenv("MC2_STATIC_PROP_BAKE_SELFTEST")) {
            // Stub: full self-test wired in Task 5 after buildRecipeFromShape
            // has callers and a registered type to test against.
            fprintf(stderr, "[STATIC_PROP_BAKE v1] event=selftest_stub note=full_test_wired_task5\n");
            fflush(stderr);
        }

        if (g_pzTrace) {
            char _pzbuf[256];
            snprintf(_pzbuf, sizeof(_pzbuf),
                "[PROJECTZ v1] enabled: trace=%d heatmap=%d summary=%d guard_px=%d",
                g_pzDoTrace ? 1 : 0, g_pzDoHeatmap ? 1 : 0, g_pzDoSummary ? 1 : 0, g_pzGuardPx);
            puts(_pzbuf);
        }
    }

    //signal(SIGTRAP, SIG_IGN);

#ifdef _WIN32
    // crashbundle_init installs the richer SEH filter (crash bundle +
    // diagnostic dialog). It supersedes the legacy mc2_unhandled_exception_filter
    // above; the old function is retained in this TU for reference but is
    // no longer the registered filter.
    crashbundle_init();
    (void)&mc2_unhandled_exception_filter; // silence "unused" warning
#endif
    g_startup_t0 = SDL_GetPerformanceCounter();
    startup_phase("process_start");

    // gather command line
	size_t cmdline_len = 0;
    for(int i=0;i<argc;++i) {
        cmdline_len += strlen(argv[i]);
        cmdline_len += 1; // ' '
    }
    char* cmdline = new char[cmdline_len + 1];
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

    delete[] cmdline;
    cmdline = NULL;

    // [HUD-RES-CLAMP v1] Force the startup render base to the canonical 800x600
    // regardless of options.cfg ResolutionX/Y. The legacy 2D HUD only lays out
    // correctly at the discrete tuned widths; a non-tuned options value breaks
    // it. 800x600 + FULLSCREEN_DESKTOP upscale matches the known-good shipped 4K
    // config and is immune to the options-menu resolution rewrite. Mirrors the
    // runtime clamp in gos_SetScreenMode. Memory: hud_scene_resolution_separation.
    Environment.screenWidth  = 800;
    Environment.screenHeight = 600;

    int w = Environment.screenWidth;
    int h = Environment.screenHeight;

    graphics::RenderWindowHandle win = graphics::create_window("mc2", w, h);
    if(!win)
        return 1;

    startup_phase("window_created");

    graphics::RenderContextHandle ctx = graphics::init_render_context(win);
    if(!ctx)
        return 1;

    graphics::make_current_context(ctx);

    // GLEW-CORE-PROFILE-1: we request a CORE-profile context (gos_render.cpp:190).
    // In a core profile glGetString(GL_EXTENSIONS) returns NULL, and without
    // glewExperimental GLEW falls back to that legacy string for glewIsSupported().
    // NVIDIA strictly returns NULL there, so glewIsSupported("GL_ARB_*") reports
    // FALSE for extensions that are actually present (AMD is lenient and still
    // returns the string, which is why this only bit NVIDIA). That made
    // glewIsSupported("GL_ARB_shader_draw_parameters") false on NVIDIA ->
    // s_hasShaderDrawParams=false -> the static-prop coalesce COLOR path was
    // disabled -> trees/buildings drew only into the shadow map and were invisible
    // in the scene (root-caused from an RTX 3080 RenderDoc capture). Setting
    // glewExperimental forces GLEW to use the core-correct glGetStringi() +
    // GL_NUM_EXTENSIONS query, fixing glewIsSupported() for BOTH vendors.
    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (GLEW_OK != err)
    {
        SPEW(("GLEW", "Error: %s\n", glewGetErrorString(err)));
        return 1;
    }
    // glewExperimental can leave a spurious GL_INVALID_ENUM queued from its own
    // probing; clear it so the first real frame's error checks start clean.
    (void)glGetError();

    render_contract::initRenderContractAssert();
    render_contract::initRenderPassTelemetry();   // [RENDER_PASS v1] (MC2_RENDER_PASS_TELEMETRY=1)

    if (GLEW_ARB_parallel_shader_compile) {
        glMaxShaderCompilerThreadsARB(0xFFFFFFFF);
        printf("[INSTR v1] parallel_shader_compile=enabled\n");
    } else {
        printf("[INSTR v1] parallel_shader_compile=unsupported\n");
    }
    fflush(stdout);

	// Install GL debug callback only when MC2_GL_DEBUG is set. In shipping
	// builds this keeps stdout free of harmless driver warnings (esp. the
	// AMD ~glsl_program double-detach chatter) and saves the sync-debug
	// overhead. Paired with the context-flag gate in gos_render.cpp.
	if (getenv("MC2_GL_DEBUG") != nullptr) {
		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageControlARB(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
		glDebugMessageCallbackARB((GLDEBUGPROC)&OpenGLDebugLog, NULL);
	}

    // GL_ARB_clip_control: align hardware depth convention with engine's
    // existing D3D-style projection matrices. cameraToClip
    // (mclib/camera.cpp:1942-1965) produces clip-space [0, w] by deliberate
    // design; without clip control, hardware default expects [-w, w] and
    // compresses our output into window depth [0.5, 1.0] — half precision
    // wasted. With ZERO_TO_ONE, hardware natively expects [0, w], NDC z is
    // [0, 1], window depth uses full [0, 1] range.
    //
    // **Fail-closed contract:** the four shader edits in this slice
    // (gos_terrain.tese, gos_terrain_thin.vert, shadow_screen.frag,
    // ssao.frag) remove their depth-range workaround remaps unconditionally.
    // Running without glClipControl(GL_ZERO_TO_ONE) would feed [0,1] NDC z
    // to hardware expecting [-1,1] and produce garbage depth. So if the
    // extension is somehow unavailable at runtime, we MUST refuse to start
    // rather than ship broken rendering.
    //
    // Plan: docs/superpowers/plans/2026-05-06-clip-control-adoption.md
    if (GLEW_ARB_clip_control || GLEW_VERSION_4_5) {
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        printf("[INSTR v1] clip_control=enabled origin=lower_left depth=zero_to_one\n");
        fflush(stdout);
    } else {
        printf("[INSTR v1] clip_control=unsupported fatal=1\n");
        fflush(stdout);
        gosASSERT(false);
        abort();
    }

    SPEW(("GRAPHICS", "Status: Using GLEW %s\n", glewGetString(GLEW_VERSION)));
    //if ((!GLEW_ARB_vertex_program || !GLEW_ARB_fragment_program))
    //{
     //   SPEW(("GRAPHICS", "No shader program support\n"));
      //  return 1;
    //}

    if(!glewIsSupported("GL_VERSION_3_0")) {
        SPEW(("GRAPHICS", "Minimum required OpenGL version is 3.0\n"));
        return 1;
    }

    const char* glsl_version = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    SPEW(("GRAPHICS", "GLSL version supported: %s\n", glsl_version));

    int glsl_maj = 0, glsl_min = 0;
    sscanf(glsl_version, "%d.%d", &glsl_maj, &glsl_min);

    if(glsl_maj < 3 || (glsl_maj==3 && glsl_min < 30) ) {
        SPEW(("GRAPHICS", "Minimum required OpenGL version is 330 ES, current: %d.%d\n", glsl_maj, glsl_min));
        return 1;
    }

    char version[16] = {0};
    snprintf(version, sizeof(version), "%d%d", glsl_maj, glsl_min);
    SPEW(("GRAPHICS", "Using %s shader version\n", version));

    startup_phase("gl_context_ready");

    gos_CreateRenderer(ctx, win, w, h);
    startup_phase("renderer_created");
#ifdef MC2_IMGUI
    GuiRuntime::Init();
#endif
    if(!gos_CreateAudio())
    {   // not an error
        SPEW(("AUDIO", "Failed to create audio\n"));
    }

    // AssetScale must load BEFORE InitializeGameEngine — that call creates
    // main-menu widgets via aObject::init which queries the manifest for
    // the chrome-flag opt-in. Late init = manifest empty at widget load.
    AssetScale::init("data/art/asset_sizes.csv");
    Environment.InitializeGameEngine();
    startup_phase("engine_init_done");

#if 0
	float aspect = (float)w/(float)h;
	mat4 proj_mat = frustumProjMatrix(-aspect*0.5f, aspect*0.5f, -.5f, .5f, 1.0f, 100.0f);
	g_camera.set_projection(proj_mat);
	g_camera.set_view(mat4::translation(vec3(0, 0, -16)));
#endif

	timing::init();
    TracyGpuContext;

    // MC2_FPS_CAP: max FPS override. Applies to both menu and mission.
    //   unset → menu=90, mission=165 (defaults)
    //   0     → uncapped
    //   N     → N FPS in both states
    // timeBeginPeriod(1) brings Windows Sleep() to 1ms resolution.
    // Cap is skipped entirely when vsync is active (SDL_GL_GetSwapInterval()==1).
    const char* fpsCapEnv = getenv("MC2_FPS_CAP");
    static const int s_fps_cap_menu    = fpsCapEnv ? atoi(fpsCapEnv) : 90;
    static const int s_fps_cap_mission = fpsCapEnv ? atoi(fpsCapEnv) : 165;
    const bool s_vsync_active = (SDL_GL_GetSwapInterval() == 1);
    printf("[FRAMECAP] MC2_FPS_CAP=%s vsync=%s -- menu=%d FPS mission=%d FPS%s\n",
           fpsCapEnv ? fpsCapEnv : "(unset)",
           s_vsync_active ? "ON" : "OFF",
           s_fps_cap_menu, s_fps_cap_mission,
           s_vsync_active ? " (cap inactive, vsync owns pacing)" : "");
#ifdef _WIN32
    timeBeginPeriod(1);
#endif

    static uint32_t s_hitchFrame = 0u;
    while( !g_exit ) {
        ZoneScopedN("Frame");

		uint64_t start_tick = timing::gettickcount();
        mc2_hitch::BeginFrame(s_hitchFrame);

        // Throttle when the window is actually invisible (minimized / hidden),
        // NOT merely when it lacks keyboard focus. Plain focus loss happens
        // constantly during normal play (clicking a chat window, opening
        // Tracy, Windows shifting focus to a notification) and the loop must
        // keep running at full speed in those cases. g_focus_lost is still
        // used as the *input filter* below — different concern, same SDL event
        // source, but they want different predicates.
        bool windowInvisible = false;
        if (g_sdl_window) {
            Uint32 flags = SDL_GetWindowFlags(g_sdl_window);
            windowInvisible = (flags & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
        }
        if (windowInvisible) {
            ZoneScopedN("Frame.BackgroundThrottle");
            timing::sleep(10 * 1000000);
        }

#ifdef MC2_IMGUI
        GuiRuntime::NewFrame();
#endif

        {
            ZoneScopedN("GameLogic");
            mc2_hitch::HitchScope _hitchLogic(mc2_hitch::HitchSpanKind::PhaseGameLogic);
            if(gos_RenderGetEnableDebugDrawCalls()) {
                gos_RenderUpdateDebugInput();
            } else {
                Environment.DoGameLogic();
            }
        }

        {
            ZoneScopedN("Frame.ProcessEvents");
            process_events();
        }

        {
            ZoneScopedN("Frame.RendererHandleEvents");
		    gos_RendererHandleEvents();
        }

        // RenderSnapshot is a shallow struct — spans are (ptr, count) into the
        // ping-pong arena. Copy is safe: arena is not reset until the next
        // ExtractRenderSnapshot() call. Emit must complete before any second extraction.
        RenderSnapshot snap;
        {
            ZoneScopedN("ExtractRenderSnapshot");
            snap = ExtractRenderSnapshot();
        }
        mc2_debug_state::maybeWriteRenderState(snap);
        {
            ZoneScopedN("EmitDrawPackets");

            // Size the candidate buffer to the exact total packet count.
            // batcher_getSortedPacketCount() returns s_sortedPacketOrder.size() — a
            // permutation over global s_packets[], same total count. Falls back to
            // 4096 before finalizeGeometry runs (returns 0 at that point).
            const uint32_t totalPackets = batcher_getSortedPacketCount();
            const uint32_t kMaxPackets  = totalPackets > 0u ? totalPackets : 4096u;

            // Module-static candidate buffer, resized on demand.
            // v0: population only — candidates are emitted but not dispatched.
            static std::vector<StaticPropDrawPacketCandidate> s_candidates;
            if (s_candidates.size() < static_cast<size_t>(kMaxPackets))
                s_candidates.resize(kMaxPackets);

            // Static storage reused every frame; contents are frame-local and invalid
            // after the current frame path. Sized to kMaxPackets on first resize, same
            // as s_candidates — overflow is structurally impossible.
            // sorted_packet_cap observed in tier1 smoke: mc2_01=134, mc2_03=359,
            // mc2_10=417, mc2_17=312, mc2_24=753 (all well under 4096 fallback).
            static std::vector<RenderCore::DrawPacket> s_drawPackets;
            if (s_drawPackets.size() < static_cast<size_t>(kMaxPackets))
                s_drawPackets.resize(kMaxPackets);

            const DrawPacketEmitStats stats =
                emitStaticPropDrawPackets(snap, s_candidates.data(), kMaxPackets);

            // DrawPacket v2: per-frame field compare. Set MC2_DRAW_PACKET_COMPARE=1 to enable.
            static const bool s_dpCompareEnabled = [] {
                const char* v = std::getenv("MC2_DRAW_PACKET_COMPARE");
                return v && v[0] == '1';
            }();
            if (s_dpCompareEnabled) {
                comparePacketsToLegacy(s_candidates.data(), stats.emitted,
                    static_cast<uint32_t>(snap.frameIndex));
            }

            // DrawPacket v3: candidate → RenderCore::DrawPacket[] ABI promotion.
            // No dispatch; legacy flush() unchanged. Set MC2_DRAW_PACKET_V3=1 to enable.
            static const bool s_v3Enabled = [] {
                const char* v = std::getenv("MC2_DRAW_PACKET_V3");
                return v && v[0] == '1';
            }();
            static bool s_finalizeWarnedOnce = false;

            if (s_v3Enabled) {
                // Warn once if finalizeGeometry hasn't run yet. Once-only to avoid per-frame log spam.
                if (totalPackets == 0 && !s_finalizeWarnedOnce) {
                    s_finalizeWarnedOnce = true;
                    std::fprintf(stderr,
                        "[DRAW_PACKET v3] WARNING (once): totalPackets=0"
                        " (finalizeGeometry not yet run, fallback kMaxPackets=4096)\n");
                }

                // candidateCount = stats.emitted, not kMaxPackets: only the v2-emitted slots are
                // initialized; trailing s_candidates entries are stale from prior frames.
                const DrawPacketBuildStats bstats = buildStaticPropDrawPackets(
                    s_candidates.data(), stats.emitted,
                    s_drawPackets.data(), static_cast<uint32_t>(s_drawPackets.size()));
                // maxPackets == s_drawPackets.size() == kMaxPackets >= stats.emitted: overflow impossible

                std::fprintf(stderr,
                    "[DRAW_PACKET v3] frame=%u input=%u emitted=%u"
                    " invalid_pipeline=%u pipeline_oor=%u invalid_index=%u"
                    " invalid_instance=%u overflow=%u"
                    " object_sentinel=%u light_sentinel=%u sorted_packet_cap=%u\n",
                    static_cast<uint32_t>(snap.frameIndex),
                    bstats.inputCandidates, bstats.emitted,
                    bstats.invalidPipeline, bstats.pipelineOutOfRange,
                    bstats.invalidIndexCount, bstats.invalidInstanceCount,
                    bstats.overflow,
                    bstats.objectIndexSentinelCount, bstats.lightIndexSentinelCount,
                    totalPackets);
            }

            // Hard gate — REAL anomalies only, AND default-SILENT (set MC2_DRAW_PACKET_WARN=1
            // to re-enable). The old gate also tripped on `oldExpected != expectedPackets`,
            // but oldExpected is a legacy comparison field that is ALWAYS 0 while
            // expectedPackets grows (68..71+), so it fired the warning EVERY frame on a
            // totally benign mismatch -> console spam. Dropped that condition; the remaining
            // overflow/invalidRanges/materialMismatches are genuine but are now also gated
            // off by default so a shipping/player build is silent.
            static const bool s_drawPacketWarn = []{
                const char* v = std::getenv("MC2_DRAW_PACKET_WARN");
                return v && v[0] == '1';
            }();
            if (s_drawPacketWarn
                    && (stats.overflow
                        || stats.invalidRanges > 0
                        || stats.materialMismatches > 0)) {
                std::fprintf(stderr,
                    "[DRAW_PACKET v1] WARNING: overflow=%d invalidRanges=%u"
                    " skippedRanges=%u old_expected=%u new_expected=%u"
                    " materialMismatches=%u\n",
                    stats.overflow ? 1 : 0,
                    stats.invalidRanges,
                    stats.skippedRanges,
                    stats.oldExpected,
                    stats.expectedPackets,
                    stats.materialMismatches);
            }

            // Full log line under MC2_RENDER_SNAPSHOT_LOG=1.
            static const bool s_logEnabled = []{
                const char* v = std::getenv("MC2_RENDER_SNAPSHOT_LOG");
                return v && v[0] == '1';
            }();
            if (s_logEnabled) {
                std::fprintf(stderr,
                    "[DRAW_PACKET v1] frame=%llu emitted=%u expected=%u"
                    " old_expected=%u distinct_types=%u static_props=%u"
                    " invalid=%u skipped=%u mat_mismatch=%u overflow=%d retired=%u\n",
                    static_cast<unsigned long long>(snap.frameIndex),
                    stats.emitted,
                    stats.expectedPackets,
                    stats.oldExpected,
                    stats.distinctTypes,
                    static_cast<uint32_t>(snap.staticProps.size()),
                    stats.invalidRanges,
                    stats.skippedRanges,
                    stats.materialMismatches,
                    stats.overflow ? 1 : 0,
                    snap.spBuildRetired);
            }

            // Feed ImGui DrawPackets panel (read in GraphicsOptionsWindow::draw).
            g_dpSnapshot.frame              = snap.frameIndex;
            g_dpSnapshot.emitted            = stats.emitted;
            g_dpSnapshot.expected           = stats.expectedPackets;
            g_dpSnapshot.distinctTypes      = stats.distinctTypes;
            g_dpSnapshot.invalidRanges      = stats.invalidRanges;
            g_dpSnapshot.skippedRanges      = stats.skippedRanges;
            g_dpSnapshot.materialMismatches = stats.materialMismatches;
            g_dpSnapshot.overflow           = stats.overflow;
            g_dpSnapshot.typeDescCount      = batcher_getStaticPropTypeDescCount();

            // Selected-prop packet inspector (DrawPackets panel "Selected Prop" section).
            // g_dpSelectedRecipeIndex is written by EditorInspector on Ctrl+Shift+Click;
            // -1 = no selection. Render thread only — no mutex needed.
            {
                DrawPacketSelectedPropSnapshot sel{};
                const int32_t selRecipe = g_dpSelectedRecipeIndex;
                if (selRecipe >= 0) {
                    // Phase 1: find the first snapshot entry with matching recipeIndex.
                    const ExtractedStaticProp* found = nullptr;
                    for (const auto& sp : snap.staticProps) {
                        if (sp.recipeIndex == selRecipe) { found = &sp; break; }
                    }
                    if (found) {
                        const uint32_t selTypeId = found->typeId;

                        // Phase 2: count visible instances of the same typeId.
                        uint32_t instCount = 0u;
                        for (const auto& sp : snap.staticProps)
                            if (sp.typeId == selTypeId) ++instCount;

                        sel.valid         = true;
                        sel.recipeIndex   = selRecipe;
                        sel.typeId        = selTypeId;
                        sel.firstPacket   = found->firstPacket;
                        sel.packetCount   = found->packetCount;
                        sel.instanceCount = instCount;
                        sel.materialIdx   = found->materialIdx;
                        sel.alphaClass    = found->alphaClass;
                        static_assert(sizeof(sel.shapeName) == sizeof(found->shapeName),
                            "shapeName size mismatch");
                        std::memcpy(sel.shapeName, found->shapeName, sizeof(sel.shapeName));
                        sel.shapeName[sizeof(sel.shapeName) - 1] = '\0';

                        // MaterialGpu lookup — per-type (all packets share same materialIdx).
                        RenderCore::MaterialGpu mg{};
                        const bool mgOk = (found->materialIdx != 0xFFFFFFFFu)
                                          && batcher_getMaterialGpuEntry(found->materialIdx, &mg);
                        const uint32_t mgAlbedoTex = mgOk ? mg.albedoTex : 0xFFFFFFFFu;

                        // Per-packet rows.
                        const uint32_t rowCap = DrawPacketSelectedPropSnapshot::kMaxRows;
                        const uint32_t pktEnd = found->firstPacket + found->packetCount;
                        for (uint32_t pi = found->firstPacket;
                                      pi < pktEnd && sel.rowCount < rowCap; ++pi) {
                            DrawPacketPropRow& row = sel.rows[sel.rowCount];
                            row.globalPacketIdx = pi;
                            uint32_t idxCount = 0u, firstIdx = 0u, owningType = 0u;
                            int32_t  baseVtx = 0;
                            if (batcher_getPacketDrawInfo(pi, &idxCount, &firstIdx,
                                                          &baseVtx, &owningType)) {
                                row.indexCount = idxCount;
                                row.firstIndex = firstIdx;
                                row.baseVertex = baseVtx;
                            }
                            uint32_t matFlags = 0u;
                            if (batcher_getPacketMaterialFlags(pi, &matFlags)) {
                                row.materialFlags = matFlags;
                                row.pipelineId    = (matFlags & STATIC_PROP_FLAG_ALPHA_TEST)
                                                    ? 2u : 1u;
                            }
                            // Render-spine v1.1: legacy layer + MaterialGpu albedo + match flag.
                            row.texArrayLayer         = found->texArrayLayer;
                            row.albedoTex             = mgAlbedoTex;
                            row.materialMatchesLegacy = mgOk
                                && (found->texArrayLayer >= 0)
                                && (mgAlbedoTex == static_cast<uint32_t>(found->texArrayLayer));
                            ++sel.rowCount;
                        }
                    }
                }
                g_dpSelProp = sel;
            }

            // TERRAIN-PASS-PACKET-0: promote pass-level terrain facts from
            // live accessors into the RenderSnapshot. Pass-level only — no
            // per-tile identity, no new counters introduced. The inspector
            // (below) becomes a downstream consumer of snap.terrainPass.
            // Read-only — no GL state touched, no mutation of any render path.
            //
            // Free-function accessors defined in gameos_graphics.cpp; full
            // gosRenderer type is private to that TU.
            extern uint32_t gos_getTerrainSurfaceProgramId();
            extern uint32_t gos_getThinTerrainProgramId();
            extern uint32_t gos_getWaterFastProgramId();
            extern uint32_t gos_getTerrainOverlayProgramId();
            {
                const RenderCore::EngineView& view = RenderCore::getCurrentView();
                snap.terrainPass.viewId          = view.id;
                snap.terrainPass.legacyProgramId = gos_getTerrainSurfaceProgramId();
                snap.terrainPass.drawCallCount   = TerrainPatchStream::getLastFlushBucketCount();
                uint32_t flags = 0u;
                // tessellationOn: hard-coded true (matches inspector field semantics).
                flags |= TerrainPassFacts::kFlagTessellationOn;
                // viewUniformsBound: terrain shaders don't consume ViewUniforms (binding=3) yet.
                // (Leave kFlagViewUniformsBound clear.)
                if (TerrainPatchStream::wasLastFlushOverflowed())
                    flags |= TerrainPassFacts::kFlagOverflow;
                snap.terrainPass.flags = flags;
            }

            #ifdef MC2_IMGUI
            // TERRAIN-SPINE-0: inspector filler. Now a downstream consumer of
            // snap.terrainPass for the pass-level fields promoted in
            // TERRAIN-PASS-PACKET-0. Per-program ids + per-flush detail fields
            // that were not promoted to the snapshot continue to read directly
            // from live accessors (no behavior change for those).
            {
                EditorInspector::TerrainPassSnapshot ts;
                ts.surfaceProgramId   = snap.terrainPass.legacyProgramId;
                ts.thinProgramId      = gos_getThinTerrainProgramId();
                ts.waterFastProgramId = gos_getWaterFastProgramId();
                ts.overlayProgramId   = gos_getTerrainOverlayProgramId();
                ts.bucketCount  = snap.terrainPass.drawCallCount;
                ts.vertCount    = TerrainPatchStream::getLastFlushVertCount();
                ts.thinRecCount = TerrainPatchStream::getLastFlushThinRecCount();
                ts.recipeCount  = TerrainPatchStream::getLastFlushRecipeCount();
                ts.overflow     = (snap.terrainPass.flags & TerrainPassFacts::kFlagOverflow) != 0u;
                ts.viewUniformsBoundForTerrain =
                    (snap.terrainPass.flags & TerrainPassFacts::kFlagViewUniformsBound) != 0u;
                const RenderCore::EngineView& view = RenderCore::getCurrentView();
                ts.currentViewId   = snap.terrainPass.viewId;
                ts.currentViewName = view.debugName ? view.debugName : "";
                ts.tessellationOn  =
                    (snap.terrainPass.flags & TerrainPassFacts::kFlagTessellationOn) != 0u;
                EditorInspector::setTerrainPassSnapshot(ts);
            }

            // SHADOW-SPINE-0: pass-level snapshot of the shadow render spine.
            // Mirrors the terrain block above. Read-only — no GL state touched.
            {
                // C++-linkage helpers defined in gameos_graphics.cpp. C-linkage
                // accessors declared at file scope (top of TU).
                extern uint32_t gos_getTerrainShadowProgramId();
                extern uint32_t gos_getStaticPropShadowProgramId();
                extern bool     gos_getShadowsEnabled();
                extern bool     gos_StaticLightMatrixBuilt();
                extern int      gos_getShadowMapSize();
                extern int      gos_getDynShadowMapSize();
                EditorInspector::ShadowPassSnapshot sp;
                sp.terrainShadowProgramId      = gos_getTerrainShadowProgramId();
                sp.mechShadowProgramId         = gos_getMechShadowProgramId();
                sp.staticPropShadowProgramId   = gos_getStaticPropShadowProgramId();
                sp.shadowsEnabled              = gos_getShadowsEnabled();
                sp.staticLightMatrixBuilt      = gos_StaticLightMatrixBuilt();
                sp.shadowMapSize               = gos_getShadowMapSize();
                sp.dynShadowMapSize            = gos_getDynShadowMapSize();
                sp.mechShadowTypesDrawn        = gos_getMechShadowTypesDrawn();
                sp.mechShadowInstDrawn         = gos_getMechShadowInstDrawn();
                sp.staticPropShadowTypesDrawn  = gos_getStaticPropShadowTypesDrawn();
                sp.staticPropShadowInstDrawn   = gos_getStaticPropShadowInstDrawn();
                // v1: shadow shaders do not consume ViewUniforms (binding=3).
                sp.viewUniformsBoundForShadow  = false;
                EditorInspector::setShadowPassSnapshot(sp);
            }

            // VFX-SPINE-0: pass-level snapshot of the GPU particle / VFX
            // render spine. Mirrors the shadow block above. Read-only — no
            // GL state touched, no VFX mutation, no object-IDs.
            {
                EditorInspector::VfxPassSnapshot vs;
                vs.particleProgramId          = (uint32_t)gos_vfx_getParticleProgramId();
                vs.gpuParticlesEnabled        = (mc2_vfx_isEnabled() != 0);
                vs.gpuParticlesLogEnabled     = (mc2_vfx_isLogEnabled() != 0);
                vs.initFailed                 = (gos_vfx_getInitFailed() != 0);
                vs.cameraSetThisFrame         = (gos_vfx_getCameraSetThisFrame() != 0);
                vs.perFrameBudget             = (uint32_t)mc2_vfx_getBudget();
                vs.ssboCapacityRecords        = (uint32_t)gos_vfx_getSsboCapacity();
                vs.overflowReported           = (mc2_vfx_getOverflowReported() != 0);
                vs.emitTotal                  = mc2_vfx_getEmitTotal();
                vs.flushTotal                 = mc2_vfx_getFlushTotal();
                vs.nonemptyFlushTotal         = mc2_vfx_getNonemptyFlushTotal();
                vs.recordsFlushedTotal        = mc2_vfx_getRecordsFlushedTotal();
                vs.recordsPerFlushMax         = (uint32_t)mc2_vfx_getRecordsPerFlushMax();
                vs.trailSpawnTotal            = mc2_vfx_getTrailSpawnTotal();
                vs.trailHeadTotal             = mc2_vfx_getTrailHeadTotal();
                // v1: VFX shaders do not consume ViewUniforms (binding=3).
                vs.viewUniformsBoundForVfx    = false;
                vs.debugMode                  = gos_vfx_getDebugMode();
                EditorInspector::setVfxPassSnapshot(vs);
            }
            #endif // MC2_IMGUI

            // [FRAME_PASS_STATS v1] frame-level aggregates + per-pass draw/
            // instance counts, pulled from already-computed counters (no
            // hot-loop counting). OFF=zero cost (every Set* early-returns).
            // Counters reflect the prior frame's flush (this seam runs before
            // draw_screen); advisory, not gated.
            if (gos_frame_pass_stats::Enabled()) {
                extern uint64_t batcher_getLastFlushSubmitCount(); // gos_mech_batcher
                const uint32_t mechInst = (uint32_t)batcher_getLastFlushSubmitCount();
                const uint32_t spSlots  = batcher_getDrawSlotCount();

                gos_frame_pass_stats::FrameAggregates agg;
                agg.visibleTerrainChunks = 0u; // set by terrain flush producer
                agg.staticPropBatches    = spSlots;
                agg.mechBatchInstances   = mechInst;
                agg.vfxCount             = 0u; // per-frame VFX count deferred (VFX-SPINE-0)
                gos_frame_pass_stats::SetFrameAggregates(agg);

                gos_frame_pass_stats::SetPassCounts(
                    gos_render_pass_timer::Pass_Mechs, mechInst, mechInst);
                gos_frame_pass_stats::SetPassCounts(
                    gos_render_pass_timer::Pass_SpColor, spSlots, spSlots);
                gos_frame_pass_stats::SetPassCounts(
                    gos_render_pass_timer::Pass_ShadowDyn,
                    gos_getMechShadowInstDrawn() + gos_getStaticPropShadowInstDrawn(),
                    gos_getMechShadowInstDrawn() + gos_getStaticPropShadowInstDrawn());
            }

        }

        {
            ZoneScopedN("DrawScreen");
            mc2_hitch::HitchScope _hitchRender(mc2_hitch::HitchSpanKind::PhaseRender);
            graphics::make_current_context(ctx);
            draw_screen();
        }

        {
            // Tier-1 instrumentation (stability spec §2.3): bump canonical
            // frame counter, then drain TGL pool null counters. Per-frame
            // line is env-gated; monotonic summary every 600 frames is not.
            ZoneScopedN("Frame.DrainTglPoolStats");
            g_mc2FrameCounter++;
            drainTglPoolStats();
            // [OBJECT_RECON v1] slice-2 recon-zero accumulator drain.
            // No-op cost (~1 cmp + 1 branch) when env not set.
            mc2_object_recon::drainPerFrame(g_mc2FrameCounter);
            projectz_frame_tick();
            projectz_overlay_begin_frame();
            drainGLErrors("frame");
        }

        {
            // Oracle: deterministic backbuffer screenshot at a target frame.
            // MC2_SCREENSHOT_AT_FRAME=N + MC2_SCREENSHOT_PATH=out.tga. Captures
            // the finished backbuffer just before present (after draw_screen +
            // EndFrame), once, when the frame counter first reaches N. Default
            // OFF, zero cost when env unset. Lets the dynamic-pipeline oracle
            // produce a real pixel image headlessly (TGA) for visual regression.
            static const char* s_ssPath  = getenv("MC2_SCREENSHOT_PATH");
            static const long  s_ssFrame = getenv("MC2_SCREENSHOT_AT_FRAME")
                ? atol(getenv("MC2_SCREENSHOT_AT_FRAME")) : -1L;
            static bool s_ssDone = false;
            if (s_ssPath && s_ssFrame >= 0 && !s_ssDone
                && (long)g_mc2FrameCounter >= s_ssFrame) {
                // Capture the offscreen post-process scene FBO, NOT the window
                // backbuffer (FBO 0) — when the window is minimized the driver
                // never composites to FBO 0 (it reads black), but sceneFBO_ is
                // a real offscreen RGBA16F target that always holds the rendered
                // scene. endScene() samples it (does not clear it), so it is
                // still intact here, just before swap. Reading RGBA16F as
                // GL_UNSIGNED_BYTE clamps HDR->LDR in the driver — fine for a
                // visual oracle.
                gosPostProcess* ppShot = getGosPostProcess();
                GLuint ssFbo = ppShot ? ppShot->getSceneFBO() : 0;
                int ssW = ppShot ? ppShot->getWidth()  : 0;
                int ssH = ppShot ? ppShot->getHeight() : 0;
                if (ssFbo && ssW > 0 && ssH > 0) {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, ssFbo);
                    glReadBuffer(GL_COLOR_ATTACHMENT0);
                    gos::screenshot::writeTGA(s_ssPath, ssW, ssH);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    s_ssDone = true;
                    fprintf(stderr, "[SCREENSHOT v1] frame=%u wrote %s (%dx%d, sceneFBO)\n",
                            g_mc2FrameCounter, s_ssPath, ssW, ssH);
                    fflush(stderr);
                }
            }
        }

        {
            // [VISUAL_CAPTURE v1] S9: deterministic PNG + capture-tuple sidecar,
            // and bookmark teleport/settle/capture replay. Reuses the same
            // offscreen scene FBO source as [SCREENSHOT v1] above (robust to a
            // minimized window). Default-OFF: onPostRenderFrame early-returns on
            // a single int/pointer check with no allocation when neither
            // MC2_VISUAL_CAPTURE_FRAME nor MC2_VISUAL_BOOKMARK_CAPTURE is set,
            // so we resolve the FBO lazily only when one of them is active.
            if (gos::visual_capture::active()) {
                gosPostProcess* ppVc = getGosPostProcess();
                GLuint vcFbo = ppVc ? ppVc->getSceneFBO() : 0;
                int vcW = ppVc ? ppVc->getWidth()  : 0;
                int vcH = ppVc ? ppVc->getHeight() : 0;
                if (vcFbo && vcW > 0 && vcH > 0) {
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, vcFbo);
                    glReadBuffer(GL_COLOR_ATTACHMENT0);
                    gos::visual_capture::onPostRenderFrame(g_mc2FrameCounter, vcW, vcH);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                }
            }
        }

        // [RENDER_PASS_TIME v1] frame boundary: advance the query ring, poll
        // the oldest pending slot (never blocks), emit the aggregated line at
        // cadence. Placed before swap so the frame's queries are all closed.
        gos_render_pass_timer::FrameEnd();

        // [FRAME_PASS_STATS v1] frame boundary: promote this frame's per-pass
        // rows + aggregates, emit the summary line at cadence, reset. Placed
        // after the timer FrameEnd; advisory, OFF=zero cost.
        gos_frame_pass_stats::FrameEnd((unsigned long)g_mc2FrameCounter);

        {
            ZoneScopedN("SwapWindow");
            mc2_hitch::HitchScope _hitchPresent(mc2_hitch::HitchSpanKind::PhasePresent);
            // SwapWindow split: env-gated glFinish probe attributes the present
            // stall. If MC2_PRESWAP_FINISH=1 moves the ~45ms into PreFinish, the
            // cost is GPU-completion backpressure (GPU draining terrain/shadows),
            // not vsync/CPU. SwapWindow.SDL isolates the bare SDL_GL_SwapWindow.
            {
                static const bool s_preSwapFinish = (getenv("MC2_PRESWAP_FINISH") != nullptr);
                if (s_preSwapFinish) { ZoneScopedN("SwapWindow.PreFinish"); glFinish(); }
            }
            { ZoneScopedN("SwapWindow.SDL"); graphics::swap_window(win); }
            render_contract::renderPassTelemetryFrameTick();  // [RENDER_PASS v1] frame boundary
            static bool s_first_frame_logged = false;
            if (!s_first_frame_logged) {
                s_first_frame_logged = true;
                startup_phase("first_frame_presented");
            }
            // Smoke mode: per-frame perf sample.
            {
                static uint64_t s_lastFrameT = SDL_GetPerformanceCounter();
                uint64_t now = SDL_GetPerformanceCounter();
                double ms = 1000.0 * (double)(now - s_lastFrameT) /
                            (double)SDL_GetPerformanceFrequency();
                s_lastFrameT = now;
                SmokeMode::samplePerf(ms);
            }
            // Heartbeat: every ~1s emit a frame-count marker so we can tell
            // whether the render loop is alive or frozen. Gated behind
            // MC2_HEARTBEAT so the default log is quiet — invaluable for
            // diagnosing freezes on content-faulting mod loads when enabled.
            static const bool s_hbTrace = (getenv("MC2_HEARTBEAT") != nullptr);
            if (s_hbTrace) {
                static int s_hb_frame = 0;
                static uint64_t s_hb_last_ms = 0;
                s_hb_frame++;
                uint64_t now_ms = (uint64_t)(SDL_GetTicks64());
                if (s_hb_last_ms == 0) s_hb_last_ms = now_ms;
                if (now_ms - s_hb_last_ms >= 1000) {
                    char _cbbuf[192];
                    snprintf(_cbbuf, sizeof(_cbbuf),
                        "[HEARTBEAT] frames=%d elapsed_ms=%llu fps=%.1f",
                        s_hb_frame, (unsigned long long)(now_ms - s_hb_last_ms),
                        (double)s_hb_frame * 1000.0 / (double)(now_ms - s_hb_last_ms));
                    fprintf(stderr, "%s\n", _cbbuf);
                    crashbundle_append(_cbbuf);
                    fflush(stderr);
                    s_hb_frame = 0;
                    s_hb_last_ms = now_ms;
                }
            }
        }

        // Frame cap: sleep to hold target frame time and reduce GPU thrashing.
        // Skipped when vsync is active (vsync already owns pacing).
        // Uses mission-state-aware cap: 90 FPS in menus, 165 FPS in missions.
        if (!s_vsync_active) {
            bool inMission = SmokeMode::missionHasStarted();
            int cap = inMission ? s_fps_cap_mission : s_fps_cap_menu;
            static bool s_capTraceLast = !inMission;
            static const bool s_capTrace = (getenv("MC2_FRAMECAP_TRACE") != nullptr);
            if (s_capTrace && s_capTraceLast != inMission) {
                fprintf(stderr, "[FRAMECAP] cap switched: inMission=%d cap=%d\n",
                        (int)inMission, cap);
                fflush(stderr);
                s_capTraceLast = inMission;
            }
            if (cap > 0) {
                ZoneScopedN("Frame.FrameCap");
                uint64_t target_ms = 1000u / (uint64_t)cap;
                uint64_t now = timing::gettickcount();
                uint64_t elapsed_ms = timing::ticks2ms(now - start_tick);
                if (elapsed_ms < target_ms) {
                    mc2_hitch::HitchScope _hitchCap(mc2_hitch::HitchSpanKind::PhaseCap);
                    timing::sleep((unsigned int)((target_ms - elapsed_ms) * 1000000u));
                }
            }
        }

        {
            ZoneScopedN("Frame.TracyGpuCollect");
            TracyGpuCollect;
        }
        FrameMark;

        {
            ZoneScopedN("Frame.ExitCheck");
            g_exit |= gosExitGameOS();
        }

		uint64_t end_tick = timing::gettickcount();
		uint64_t dt = timing::ticks2ms(end_tick - start_tick);
		// S9D: pin the per-frame animation clock too. frameRate feeds
		// frameLength (= 1/frameRate) in mechcmd2.cpp, which drives ALL mech /
		// object / GOSFX motion and animation deltas. Without this, fixing
		// scenarioTime alone leaves animation phase tied to variable fps.
		// Single cached bool check; OFF path keeps the wall-clock frameRate.
		if (SmokeMode::fixedTimestepEnabled())
			frameRate = 30.0f;
		else
			frameRate = dt ? (1000.0f / (float)dt) : 0.0f;
        mc2_hitch::EndFrame(s_hitchFrame, static_cast<double>(dt));
        ++s_hitchFrame;

        // Validation mode: record frame and check exit condition
        if (getValidateConfig().enabled) {
            validateRecordFrame((float)dt);
            if (validateShouldExit()) {
                validateWriteResults(Environment.drawableWidth, Environment.drawableHeight);
                break;
            }
        }

        // Smoke mode: timed auto-quit after durationSec seconds past mission_ready.
        if (SmokeMode::shouldQuit()) {
            SmokeMode::emitTiming("mission_quit");
            SmokeMode::emitCleanSummary();
            g_exit = true;
            break;
        }
    }

#ifdef _WIN32
    timeEndPeriod(1);
#endif

    // Write validation results if game exited before frame limit
    if (getValidateConfig().enabled) {
        validateWriteResults(Environment.drawableWidth, Environment.drawableHeight);
    }

    // Tier-1 instrumentation (stability spec §2.5): final monotonic summary
    // before tearing down render/audio. Always emitted regardless of env gate.
    drainTglPoolStatsOnShutdown();
    mc2_object_recon::drainOnShutdown();
    projectz_shutdown();

    Environment.TerminateGameEngine();
    AssetScale::shutdown();

#ifdef MC2_IMGUI
    GuiRuntime::Shutdown();
#endif
    gos_DestroyRenderer();

    graphics::destroy_render_context(ctx);
    graphics::destroy_window(win);

    gos_DestroyAudio();

    // Return validation exit code if in validate mode
    if (getValidateConfig().enabled)
        return getValidateTelemetry().exitCode;

    return 0;
}
#endif // DISABLE_GAMEOS_MAIN
