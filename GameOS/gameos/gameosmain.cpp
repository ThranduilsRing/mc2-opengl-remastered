#include "gameos.hpp"
#include "gos_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// sebi 2026-04-22: unhandled-exception filter that symbolizes the stack via
// DbgHelp (PDB-based). Needed because release/RelWithDebInfo builds otherwise
// die silently with "read violation at 0xNN" and no frames — Tracy only resolves
// the top CRT frame, and the real null-deref callsite is invisible.
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
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
#include "gos_static_prop_killswitch.h"
#include "asset_scale.h"
#include "gos_crashbundle.h"
#include "gos_smoke.h"

#include <signal.h>
#include "gos_profiler.h"
#include "tgl.h"   // drainTglPoolStats / drainTglPoolStatsOnShutdown (Tier-1 instr)

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
                // Repurposed from SSAO toggle to GPU static prop frag debug
                // cycle. SSAO infrastructure is preserved in code; rebind
                // elsewhere if needed.
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
                g_useGpuStaticProps = !g_useGpuStaticProps;
                fprintf(stderr, "GPU Static Props: %s\n",
                        g_useGpuStaticProps ? "ON" : "OFF");
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
    while( SDL_PollEvent( &event ) ) {

        if(g_focus_lost) {
            if(event.type != SDL_WINDOWEVENT_FOCUS_GAINED) {
                continue;
            } else {
                g_focus_lost = false;
            }
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
		case SDL_WINDOWEVENT_RESIZED:
			{
				float w = (float)event.window.data1;
				float h = (float)event.window.data2;
				glViewport(0, 0, (GLsizei)w, (GLsizei)h);
                SPEW(("INPUT", "resize event: w: %f h:%f\n", w, h));
			}
			break;
        case SDL_WINDOWEVENT_FOCUS_LOST:
            g_focus_lost = true;
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

	const int viewport_w = Environment.drawableWidth;
	const int viewport_h = Environment.drawableHeight;

    if (pp) {
        pp->resize(viewport_w, viewport_h);
        pp->beginScene();
    }

    glViewport(0, 0, viewport_w, viewport_h);
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
    glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT );

    // Skybox disabled — terrain fog provides atmosphere, bright sky looked jarring
    // if (pp) pp->renderSkybox(0.3f, 0.7f, 0.2f);

    {
        ZoneScopedN("Camera.UpdateRenderers");
        gos_RendererBeginFrame();
        Environment.UpdateRenderers();
        gos_RendererEndFrame();
    }

    glUseProgram(0);

    // Composite post-processed scene to default framebuffer
    if (pp) {
        pp->endScene();
    }

    // Replay buffered HUD draws to FB 0 (after post-process)
    gos_RendererFlushHUDBatch();
    drainGLErrors("hud");
    //CHECK_GL_ERROR;
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
    {
        const bool tgl     = (getenv("MC2_TGL_POOL_TRACE")       != nullptr);
        const bool destr   = (getenv("MC2_DESTROY_TRACE")        != nullptr);
        // GL_ERROR is default-on; the env var suppresses it.
        const bool glprint = (getenv("MC2_GL_ERROR_DRAIN_SILENT") == nullptr);
        const bool smoke   = (getenv("MC2_SMOKE_MODE")           != nullptr);
        const char* build  =
#ifdef MC2_BUILD_HASH
            MC2_BUILD_HASH
#else
            "UNKNOWN"
#endif
            ;
        char _cbbuf[320];
        snprintf(_cbbuf, sizeof(_cbbuf),
            "[INSTR v1] enabled: tgl_pool=%d destroy=%d gl_error_print=%d smoke=%d build=%s",
            tgl ? 1 : 0, destr ? 1 : 0, glprint ? 1 : 0, smoke ? 1 : 0, build);
        puts(_cbbuf);
        crashbundle_append(_cbbuf);
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

    GLenum err = glewInit();
    if (GLEW_OK != err)
    {
        SPEW(("GLEW", "Error: %s\n", glewGetErrorString(err)));
        return 1;
    }

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

    while( !g_exit ) {
        ZoneScopedN("Frame");

		uint64_t start_tick = timing::gettickcount();

        if (g_focus_lost) {
            ZoneScopedN("Frame.BackgroundThrottle");
            timing::sleep(10 * 1000000);
        }

        {
            ZoneScopedN("GameLogic");
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

        {
            ZoneScopedN("DrawScreen");
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
            drainGLErrors("frame");
        }

        {
            ZoneScopedN("SwapWindow");
            graphics::swap_window(win);
            static bool s_first_frame_logged = false;
            if (!s_first_frame_logged) {
                s_first_frame_logged = true;
                startup_phase("first_frame_presented");
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
		frameRate = dt ? (1000.0f / (float)dt) : 0.0f;

        // Validation mode: record frame and check exit condition
        if (getValidateConfig().enabled) {
            validateRecordFrame((float)dt);
            if (validateShouldExit()) {
                validateWriteResults(Environment.drawableWidth, Environment.drawableHeight);
                break;
            }
        }
    }

    // Write validation results if game exited before frame limit
    if (getValidateConfig().enabled) {
        validateWriteResults(Environment.drawableWidth, Environment.drawableHeight);
    }

    // Tier-1 instrumentation (stability spec §2.5): final monotonic summary
    // before tearing down render/audio. Always emitted regardless of env gate.
    drainTglPoolStatsOnShutdown();

    Environment.TerminateGameEngine();
    AssetScale::shutdown();

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
