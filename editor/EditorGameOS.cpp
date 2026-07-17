/***************************************************************
* FILENAME: EditorGameOS.cpp
* DESCRIPTION: Provides the Editor GameOS compatibility shim for the SDL/OpenGL remaster.
* AUTHOR: Methuselas
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 05/16/2026
* UPDATED: by Methuselas
* CHANGES: Gate Editor GameOS trace logging behind MC2_EDITOR_TRACE.
****************************************************************/

//===========================================================================//
// EditorGameOS.cpp
//
// Shim layer providing GameOS platform symbols that the Editor requires but
// that the SDL-based gameos port no longer exports from its own compiled
// objects.
//
// Background: the original GameOS shipped a windows.lib (MFC/Win32 platform
// integration) alongside gameos.lib. That library provided InitGameOS,
// GameOSWinProc, RunGameOSLogic and a handful of runtime globals. The
// open-source SDL port dropped windows.lib; its equivalent logic lives in
// gameosmain.cpp (the SDL main loop) which is not linked into the Editor.
//
// The Editor is MFC-based: it owns its own message pump and render window.
// These shims wire the remaining call sites to the correct SDL/gameos entry
// points, or provide safe no-op defaults where the old symbol was pure
// Win32 boilerplate that has no SDL equivalent.
#include "stdafx.h"
#include "EditorGpuTimer.h"  // EditorFramePhase_* whole-frame timing (MC2_EDITOR_GPU_TIMERS)
#include "EditorWatchdog.h"  // EditorWatchdog_Heartbeat (MC2_EDITOR_WATCHDOG)
#include <cstdarg>
#include <stdlib.h>
#include <cstdio>
#include <cstring>
#include <cctype>

#include <gameos.hpp>          // gos_RendererBeginFrame / EndFrame / HandleEvents
#include "gos_render.h"        // graphics::make_current_context etc.
#include "gos_render_context.h"  // InitializeRenderContextConventions (shared game/editor parity)
#include "render_frame_driver.h" // RenderFrameDriver_RenderWorld — Slice 6 shared render seam
#include "../GameOS/gameos/gos_terrain_indirect.h" // RebuildCementAtlasIfDirty() — deferred cement-atlas rebuild at frame-start
#include "gos_input.h"         // input::beginUpdateMouseState etc.
#include "camera.h"            // extern eye + fgetScreenResX (pick-cache coherence diag)
#include "gos_postprocess.h"   // gosPostProcess + getGosPostProcess() — needed for
                               // beginScene/endScene so props render into the scene FBO
                               // with MRT (COLOR_ATTACHMENT2 = object IDs) enabled.
                               // Without this the editor rendered directly to FBO 0 and
                               // lookupAtPixel always read back 0.
#include "editorinterface.h"

#include <SDL2/SDL.h>

#ifdef TRACY_ENABLE
#  include "tracy/Tracy.hpp"
#  include "tracy/TracyOpenGL.hpp"
#endif

#ifdef MC2_IMGUI
#include "GuiRuntime.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#endif

namespace graphics {
    void set_editor_parent_window(void* hwnd);
    SDL_Window* getSDLWindow() noexcept;

    // Editor/MFC render path compatibility:
    // gameos_main expects the renderer layer to provide mouse-grab hooks.
    // The standalone game renderer implements these in gos_render.cpp, but
    // EditRel uses EditorGosRender.cpp instead so the editor can own its MFC
    // window/message pump.  For the editor, grabbing the mouse is unsafe and
    // unnecessary, so these are intentionally harmless no-ops.
    void set_mouse_grab(bool /*grab*/)
    {
        // No-op for the MFC editor shell.
    }

    void refresh_mouse_grab(void)
    {
        // No-op for the MFC editor shell.
    }
}


#ifdef PLATFORM_WINDOWS
#include <GL/glew.h>
#else
#include <GL/glew.h>
#endif

extern void gos_CreateRenderer(graphics::RenderContextHandle ctx_h, graphics::RenderWindowHandle win_h, int w, int h);
extern void gos_DestroyRenderer();
extern bool gos_CreateAudio();
extern void gos_DestroyAudio();
class gosRenderer;
extern gosRenderer* getGosRenderer();

static graphics::RenderWindowHandle  g_editorRenderWindow  = nullptr;
static graphics::RenderContextHandle g_editorRenderContext = nullptr;
static bool g_editorAudioCreated = false;


// ---------------------------------------------------------------------------
// Local startup tracing
// ---------------------------------------------------------------------------
static void EditorGameOSTrace(const char* fmt, ...)
{
	if (getenv("MC2_EDITOR_TRACE") == NULL)
		return;

    FILE* f = fopen("editor-startup.log", "a");
    if (!f)
        return;

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    fputc('\n', f);
    fclose(f);
}


static bool EditorGameOS_EnsureSDLVideo()
{
    EditorGameOSTrace("InitGameOS: before SDL_SetMainReady");
    SDL_SetMainReady();
    EditorGameOSTrace("InitGameOS: after SDL_SetMainReady");

    Uint32 wasInit = SDL_WasInit(0);
    EditorGameOSTrace("InitGameOS: SDL_WasInit=0x%08x", (unsigned)wasInit);

    if ((wasInit & SDL_INIT_VIDEO) == 0)
    {
        EditorGameOSTrace("InitGameOS: before SDL_InitSubSystem VIDEO");
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        {
            EditorGameOSTrace("InitGameOS: SDL_InitSubSystem VIDEO failed: %s", SDL_GetError());
            return false;
        }
        EditorGameOSTrace("InitGameOS: after SDL_InitSubSystem VIDEO");
    }
    else
    {
        EditorGameOSTrace("InitGameOS: SDL video already initialized");
    }

    if ((SDL_WasInit(0) & SDL_INIT_EVENTS) == 0)
    {
        EditorGameOSTrace("InitGameOS: before SDL_InitSubSystem EVENTS");
        if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0)
        {
            EditorGameOSTrace("InitGameOS: SDL_InitSubSystem EVENTS failed: %s", SDL_GetError());
            return false;
        }
        EditorGameOSTrace("InitGameOS: after SDL_InitSubSystem EVENTS");
    }

    return true;
}


// ---------------------------------------------------------------------------
// Runtime globals
// ---------------------------------------------------------------------------

// gActive / gGotFocus: track Editor window activation state.
// WindowProc in EditorInterface.cpp sets these on WM_CREATE / WM_ACTIVATE.
bool gActive    = true;
bool gGotFocus  = true;

// DebuggerActive: used to suppress certain assert dialogs when a debugger is
// attached. We query the OS directly rather than relying on the old GameOS
// debug thread that no longer exists.
bool DebuggerActive = (::IsDebuggerPresent() != 0);

// ProcessingError: re-entrancy guard used by the old GameOS assert handler.
// The SDL gameos assert path is independent; keep this at 0 so the Editor's
// OnPaint guard (`if (ProcessingError ...)`) is a no-op.
volatile int ProcessingError = 0;

// gNoDialogs: game-startup flag parsed from the command line in mechcmd2.cpp.
// The Editor never runs the game's ParseCommandLine so define it here as the
// safe default (dialogs enabled).
bool gNoDialogs = false;

// ObjectTextureSize: defined in Editor.cpp as `int`; mclib objects extern it.
// The single authoritative definition lives in Editor.cpp — this file does
// NOT re-define it.  (If the linker complains of a duplicate, remove the
// definition from Editor.cpp and keep this comment.)

// ---------------------------------------------------------------------------
// InitDW  (Watson / crash-reporting stub)
// ---------------------------------------------------------------------------
// The original InitDW() registered an Office Watson crash-upload handler.
// That infrastructure is not present in this open-source build; make it a
// clean no-op so Editor startup continues normally.
void InitDW(void)
{
    // No-op: Office Watson crash reporter not available in open-source build.
}

// ---------------------------------------------------------------------------
// mc2_hzb_dump_camera_view  (HZB occlusion-cull debug capture stub)
// ---------------------------------------------------------------------------
// PRE-EXISTING editor-link gap, NOT part of EDITOR-GAME-RUNTIME-PARITY-ARC:
// commit fe0f2a9c added an HZB camera capture/replay tool. Its definition lives
// in code/gamecam.cpp (a game TU not linked into EditRel), but GuiRuntime/
// EditorInspector.cpp (shared by the editor) calls it from drawImGui(). EditRel
// has no HZB cull capture, so provide a clean no-op shim — same pattern as the
// other GameOS symbols this file supplies. (Surfaced now because this arc made
// EditRel link/build for the first time in this worktree.)
extern "C" void mc2_hzb_dump_camera_view(void)
{
    // No-op: HZB camera-view capture is a game-only debug tool; absent in EditRel.
}

// ---------------------------------------------------------------------------
// mc2_save_camera_bookmark  (in-game camera-pose bookmark save stub)
// ---------------------------------------------------------------------------
// Same PRE-EXISTING editor-link gap class as mc2_hzb_dump_camera_view above, one
// symbol later in the same commit (560dfc1d, BOOKMARK-SAVE-BUTTON-1): definition
// lives in code/gamecam.cpp (reads the game's global `eye`/`mission`, not linked
// into EditRel), called unconditionally from GuiRuntime/EditorInspector.cpp's
// "Save Camera Bookmark" button. Surfaced by TECHBRAIN-PATROL-VIEWER-1 building
// EditRel for the first time since that commit landed. A real editor-side
// implementation would need the editor's OWN camera/mission model (EditorData),
// not the game's — out of scope here; clean no-op shim so EditRel links, same
// as the HZB stub above.
extern "C" void mc2_save_camera_bookmark(char* outPath, int outPathLen)
{
    // No-op: camera-bookmark save is a game-only debug tool; absent in EditRel.
    if (outPath && outPathLen > 0)
        outPath[0] = 0;
}

// ---------------------------------------------------------------------------
// InitGameOS
// ---------------------------------------------------------------------------
// In the original windows.lib this initialised DirectX, registered the
// GameOS window class and hooked the Win32 message pump.  The SDL port
// performs equivalent setup in its main() via gos_CreateRenderer /
// gos_CreateAudio.  For the MFC Editor the renderer is initialised
// separately by EditorMFC / EditorInterface, so this call happens after the
// MFC window already exists.  We defer to GetGameOSEnvironment (which fills
// in the Environment struct) and leave hardware init to the Editor's own
// startup sequence.
void __stdcall InitGameOS(HINSTANCE /*hInstance*/, HWND hWindow, char* commandLine)
{
    EditorGameOSTrace("InitGameOS: enter commandLine=%s",
        (commandLine && *commandLine) ? commandLine : "<empty>");

    // Always register the editor's GameOS environment callbacks.  The old
    // guard skipped this on an empty command line, leaving the MFC shell alive
    // but the core editor callbacks unregistered.
    GetGameOSEnvironment(commandLine ? commandLine : "");
    EditorGameOSTrace("InitGameOS: after GetGameOSEnvironment InitializeGameEngine=%p",
        Environment.InitializeGameEngine);

    // The SDL GameOS main() normally creates the render window/context before
    // InitializeGameEngine().  The MFC editor does not run that main(), so do
    // the same renderer bootstrap here before font/texture/editor init.
    if (!getGosRenderer())
    {
        if (!hWindow || !::IsWindow(hWindow))
        {
            EditorGameOSTrace("InitGameOS: missing/invalid EditorInterface HWND before renderer bootstrap");
            return;
        }

        RECT rc = {};
        ::GetClientRect(hWindow, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        if (w <= 0) w = 1024;
        if (h <= 0) h = 768;

        Environment.screenWidth = w;
        Environment.screenHeight = h;
        Environment.drawableWidth = w;
        Environment.drawableHeight = h;

        EditorGameOSTrace("InitGameOS: before renderer bootstrap w=%d h=%d", w, h);

        // Keep this bootstrap simple. graphics::create_window owns SDL_VideoInit(),
        // so do not pre-initialize SDL video here; double video init can hang on
        // some Windows/SDL setups before create_window returns.
        EditorGameOSTrace("InitGameOS: before SDL_SetMainReady");
        SDL_SetMainReady();
        EditorGameOSTrace("InitGameOS: after SDL_SetMainReady");

        EditorGameOSTrace("InitGameOS: before graphics::set_verbose(false)");
        graphics::set_verbose(false);
        EditorGameOSTrace("InitGameOS: after graphics::set_verbose(false)");

        EditorGameOSTrace("InitGameOS: before graphics::create_window");

        /*
            Editor migration note:

            Keep GameOS/gos_render untouched for the game executable.  The
            editor target links EditorGosRender.cpp instead, which implements
            the same graphics:: API but reparents the SDL/OpenGL render window
            into the MFC EditorInterface HWND.

            That keeps MFC as the temporary shell of record: menus, splash,
            focus, accelerator routing, and shutdown remain MFC-owned while the
            remastered renderer draws only inside the editor client area.
        */
        if (!hWindow || !::IsWindow(hWindow))
        {
            EditorGameOSTrace("InitGameOS: invalid editor HWND; refusing detached renderer startup");
            return;
        }

        graphics::set_editor_parent_window((void*)hWindow);
        g_editorRenderWindow = graphics::create_window("MC2 Editor (Remastered)", w, h);

        EditorGameOSTrace("InitGameOS: after graphics::create_window renderWindow=%p", g_editorRenderWindow);

        if (g_editorRenderWindow)
        {
            EditorGameOSTrace("InitGameOS: before graphics::init_render_context");
            g_editorRenderContext = graphics::init_render_context(g_editorRenderWindow);
            EditorGameOSTrace("InitGameOS: after graphics::init_render_context renderContext=%p", g_editorRenderContext);
        }

        if (g_editorRenderContext)
        {
            EditorGameOSTrace("InitGameOS: before graphics::make_current_context");
            graphics::make_current_context(g_editorRenderContext);
            EditorGameOSTrace("InitGameOS: after graphics::make_current_context");

            EditorGameOSTrace("InitGameOS: before glewInit");
            // GLEW-CORE-PROFILE-1: core-profile context needs glewExperimental so
            // glewIsSupported() uses glGetStringi() instead of the (NULL on core)
            // legacy extension string. Without it, NVIDIA reports ARB extensions as
            // absent -> static-prop coalesce color path disabled -> invisible
            // trees/buildings. Same fix as gameosmain.cpp. Must precede glewInit().
            glewExperimental = GL_TRUE;
            GLenum glewErr = glewInit();
            EditorGameOSTrace("InitGameOS: after glewInit result=%u", (unsigned)glewErr);
            (void)glGetError();  // drain spurious GL_INVALID_ENUM from glew probing

            if (glewErr == GLEW_OK)
            {
#ifdef TRACY_ENABLE
                TracyGpuContext;
#endif
                // GAMEOS-RENDER-CONTEXT-PARITY-1: the editor shares the game's
                // shaders, which assume the reverse-Z [0,1] NDC depth convention.
                // Both hosts now establish clip-control + the reverse-Z depth
                // baseline through ONE shared function so they cannot diverge
                // (this class of bug is what Slice 0 fixed inline). The
                // fail-closed contract (abort if glClipControl is unavailable)
                // lives inside the shared function. Must precede gos_CreateRenderer.
                EditorGameOSTrace("InitGameOS: before InitializeRenderContextConventions(Editor)");
                InitializeRenderContextConventions(RenderHostKind::Editor);
                EditorGameOSTrace("InitGameOS: after InitializeRenderContextConventions(Editor)");

                EditorGameOSTrace("InitGameOS: before gos_CreateRenderer context=%p window=%p", g_editorRenderContext, g_editorRenderWindow);
                gos_CreateRenderer(g_editorRenderContext, g_editorRenderWindow, w, h);
                EditorGameOSTrace("InitGameOS: after gos_CreateRenderer renderer=%p", getGosRenderer());

#ifdef MC2_IMGUI
                EditorGameOSTrace("InitGameOS: MC2_IMGUI defined -- before GuiRuntime::Init sdlWin=%p", graphics::getSDLWindow());
                // Editor opts into editor-mode ImGui (docking dockspace + auto-dock)
                // BEFORE Init() so DockingEnable is set. The game never calls this, so
                // its ImGui windows float. Must precede Init().
                GuiRuntime::SetEditorMode(true);
                GuiRuntime::Init();
                EditorGameOSTrace("InitGameOS: after GuiRuntime::Init g_imguiInitialized=%d", g_imguiInitialized ? 1 : 0);
#endif

                EditorGameOSTrace("InitGameOS: before gos_CreateAudio");
                g_editorAudioCreated = gos_CreateAudio();
                EditorGameOSTrace("InitGameOS: after gos_CreateAudio created=%d", g_editorAudioCreated ? 1 : 0);
            }
            else
            {
                EditorGameOSTrace("InitGameOS: glewInit failed; renderer not created");
            }
        }
        else
        {
            EditorGameOSTrace("InitGameOS: renderer bootstrap skipped because window/context failed");
        }
    }
    else
    {
        EditorGameOSTrace("InitGameOS: renderer already exists renderer=%p", getGosRenderer());
    }

    if (Environment.InitializeGameEngine)
    {
        EditorGameOSTrace("InitGameOS: before InitializeGameEngine");
        Environment.InitializeGameEngine();
        EditorGameOSTrace("InitGameOS: after InitializeGameEngine");
    }
    else
    {
        EditorGameOSTrace("InitGameOS: InitializeGameEngine is null");
    }

    // Renderer and audio are created by EditorMFC / EditorInterface directly.
}

// ---------------------------------------------------------------------------
// GameOSWinProc
// ---------------------------------------------------------------------------
// In the original windows.lib this forwarded selected messages to GameOS so
// it could update its internal input / focus state.  The SDL port manages
// input through SDL_PollEvent in its own loop; it has no Win32 WndProc hook.
//
// For the Editor the critical messages are keyboard and mouse events, which
// EditorInterface already handles directly through MFC's message map.  We
// therefore forward only the input-relevant messages to the SDL input layer
// and return 0 (pass to DefWindowProc) for everything else.
LRESULT CALLBACK GameOSWinProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    // Key events: synthesise an SDL_Event so gos_GetKey() sees them.
    // For mouse events we rely on EditorInterface's own MFC handlers which
    // call gos_GetMouseInfo internally; no extra forwarding needed.
    switch (uMsg)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    {
        SDL_Event ev;
        ev.type = (uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN)
                      ? SDL_KEYDOWN : SDL_KEYUP;
        ev.key.keysym.scancode = static_cast<SDL_Scancode>(MapVirtualKey(
            static_cast<UINT>(wParam), MAPVK_VK_TO_VSC));
        ev.key.keysym.sym      = SDL_GetKeyFromScancode(ev.key.keysym.scancode);
        ev.key.keysym.mod      = KMOD_NONE;
        SDL_PushEvent(&ev);
        break;
    }
    case WM_ACTIVATE:
    case WM_ACTIVATEAPP:
        gActive   = (LOWORD(wParam) != WA_INACTIVE);
        gGotFocus = gActive;
        break;
    default:
        break;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// RunGameOSLogic
// ---------------------------------------------------------------------------
// In the original windows.lib this was the per-frame GameOS tick: pump SDL
// events, advance the renderer, swap buffers.  In the SDL port those steps
// live inside the gameosmain.cpp while-loop.
//
// For the Editor, SafeRunGameOSLogic() (in EditorInterface.cpp) calls this
// from MFC paint / scroll / timer handlers — i.e. when MFC decides it is
// time to update the 3D viewport.  We replicate the relevant subset of the
// SDL main-loop body:
//   1. Pump SDL events (updates input state seen by gos_GetKey etc.)
//   2. Tick the GameOS renderer (begins/ends frame, calls UpdateRenderers
//      which in turn calls Environment.DoGameLogic via the Editor's callback)
//   3. Swap the OpenGL buffer.
//
// We deliberately do NOT sleep here — MFC already throttles calls through
// its message loop.
extern void gos_RendererBeginFrame();
extern void gos_RendererEndFrame();
extern void gos_RendererHandleEvents();
extern bool gosExitGameOS();

DWORD __stdcall RunGameOSLogic()
{
    EditorWatchdog_Heartbeat();  // main-thread liveness for the stall watchdog

    if (!g_editorRenderWindow || !g_editorRenderContext || !getGosRenderer())
    {
        EditorGameOSTrace("RunGameOSLogic: skipped window=%p context=%p renderer=%p",
            g_editorRenderWindow, g_editorRenderContext, getGosRenderer());
        return 0;
    }

    // Keep GameOS dimensions synchronized with the embedded MFC child window.
    HWND hwnd = EditorInterface::instance() ? EditorInterface::instance()->m_hWnd : NULL;
    if (hwnd && ::IsWindow(hwnd))
    {
        RECT rc = {};
        ::GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;
        // Compare against the last REAL client size (not Environment, which we shrink
        // below for the docked scene viewport -- otherwise this fires every frame).
        static int s_lastClientW = -1, s_lastClientH = -1;
        if (w > 0 && h > 0 && (w != s_lastClientW || h != s_lastClientH))
        {
            s_lastClientW = w;
            s_lastClientH = h;
            Environment.screenWidth = w;
            Environment.screenHeight = h;
            Environment.drawableWidth = w;
            Environment.drawableHeight = h;
            graphics::resize_window(g_editorRenderWindow, w, h);
            EditorGameOSTrace("RunGameOSLogic: resize w=%d h=%d", w, h);
        }

#ifdef MC2_IMGUI
        // Dock map-resize: shrink the SCENE viewport to the dockspace CENTRAL node (the
        // area not covered by docked panels) so the GL map fills only that region and
        // the docked right-side panels sit beside it, not over it. The scene FBO +
        // glViewport + camera aspect all follow Environment.drawable/screen dims, and
        // endScene composites to glViewport(0,0,width_,height_) -> the left region;
        // ImGui (DisplaySize = full SDL window) draws panels in the right strip.
        // SceneViewportWidth() lags one frame (built in NewFrame) -- harmless. The
        // scene is left-anchored at (0,0) so mouse-pick coords still map 1:1.
        // MC2_EDITOR_DOCK_RESIZE=0 disables (scene stays full-window behind panels).
        {
            // FIXED-LAYOUT viewport (2026-06-10, replaces the dynamic central-node
            // shrink). Per user direction: reserve a FIXED, DPI-scaled panel column on
            // the right; the scene viewport is the rest of the window, LEFT-ANCHORED at
            // (0,0). Computing the size DIRECTLY from the client rect (not the ImGui
            // central node) removes the one-frame lag AND the moving-target coordinate
            // space that drove the pick divergence -- the scene size is a single known
            // value fed to glViewport + Environment + (via the camera) every width
            // cache. Picking is then exact by construction: mouse - (0,0), normalised
            // by the same fixed width the camera projects with.
            // MC2_EDITOR_RTT=0 disables (full-window scene, panels overlay).
            static int s_panelLogical = -1;   // logical (96-dpi) panel width
            if (s_panelLogical < 0) {
                const char* p = std::getenv("MC2_EDITOR_PANEL_W");
                s_panelLogical = (p && atoi(p) > 0) ? atoi(p) : 320;
            }
            int sw = w, sh = h;
            if (GuiRuntime::RttEnabled() && w > 0 && h > 0) {
                // DPI scale from the GL-child window (1.0 at 96 dpi).
                UINT dpi = ::GetDpiForWindow(hwnd);
                float dpiScale = (dpi > 0) ? (float)dpi / 96.0f : 1.0f;
                int panelPx = (int)(s_panelLogical * dpiScale + 0.5f);
                if (panelPx > w - 256) panelPx = w - 256;   // keep a sane minimum scene
                if (panelPx < 0) panelPx = 0;
                sw = w - panelPx;
                sh = h;
                Environment.screenWidth    = sw;
                Environment.drawableWidth  = sw;
                Environment.screenHeight   = sh;
                Environment.drawableHeight = sh;
                // Publish the fixed scene rect (left-anchored) so GuiRuntime paints the
                // RTT texture there and the pick offset uses the same rect.
                GuiRuntime::SetFixedViewportRect(0, 0, sw, sh);

                // SINGLE SOURCE OF TRUTH: set the GOS projection viewport to the scene
                // sub-rect. gos_GetViewport feeds Camera::update/render -> viewMulX/Y +
                // TG_Shape + userInput + projectZ (the forward-projection every cursor
                // and object pick uses). Patching the per-frame copies one at a time
                // always left one reading the full window (the residual scale/offset).
                // setupViewport only STORES the projection fractions; it does NOT touch
                // the GL render viewport (we set that explicitly via glViewport below),
                // so the render stays scene-sized while ALL projections become coherent.
                // Fractions are relative to the GOS BACKBUFFER width (full window),
                // which differs from the MFC client width. Reading gos_GetViewport
                // after we shrink would feed back the shrunk value (sw/sw=1 -> reset),
                // so capture the TRUE full backbuffer dims once (and re-capture on
                // resize) by restoring the full fraction, reading, then applying scene.
                static float s_gosFullW = 0.0f, s_gosFullH = 0.0f;
                static int s_gosTrackW = -1, s_gosTrackH = -1;
                if (s_gosFullW <= 0.0f || s_gosTrackW != w || s_gosTrackH != h) {
                    gos_SetupViewport(false, 0.0f, false, 0, 0.0f, 0.0f, 1.0f, 1.0f);
                    float a = 0, b = 0, c = 0, d = 0;
                    gos_GetViewport(&a, &b, &c, &d);
                    s_gosFullW = a; s_gosFullH = b;
                    s_gosTrackW = w; s_gosTrackH = h;
                }
                if (s_gosFullW > 0.0f && s_gosFullH > 0.0f) {
                    // top, left, bottom, right as backbuffer fractions; left-anchored.
                    gos_SetupViewport(false, 0.0f, false, 0,
                        0.0f, 0.0f, (float)sh / s_gosFullH, (float)sw / s_gosFullW);
                }
            }
        }
#endif
    }

    // --- Pump SDL events ---------------------------------------------------
    input::beginUpdateMouseState();

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
#ifdef MC2_IMGUI
        if (g_imguiInitialized)
            ImGui_ImplSDL2_ProcessEvent(&event);
#endif
        switch (event.type)
        {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            input::handleKeyEvent(&event);
            break;

        case SDL_MOUSEMOTION:
            input::handleMouseMotion(&event);
            break;

        case SDL_MOUSEWHEEL:
            input::handleMouseWheel(&event);
            break;

        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            input::handleMouseButton(&event);
            break;
        }
    }

    input::updateMouseState();
    input::updateKeyboardState();

    // Match the SDL game loop: bind context, clear, set viewport, run the
    // renderer frame, then swap the embedded WGL window.
    graphics::make_current_context(g_editorRenderContext);

    const int viewport_w = Environment.drawableWidth;
    const int viewport_h = Environment.drawableHeight;

    // Route rendering through the scene FBO so the object-ID buffer
    // (COLOR_ATTACHMENT2) is populated — required for Ctrl+Shift+LMB pick.
    // Without beginScene the editor rendered directly to FBO 0; lookupAtPixel
    // always read 0 from the scene FBO that was never written.
    // Mirrors gameosmain.cpp: resize → beginScene → clear → clearGBuffer1.
    gosPostProcess* pp_editor = getGosPostProcess();
    if (pp_editor) {
        pp_editor->resize(viewport_w, viewport_h);
        pp_editor->beginScene();  // binds sceneFBO_, sets MRT w/ COLOR_ATTACHMENT2
    }

    glViewport(0, 0, viewport_w, viewport_h);

    // Minimal per-frame GL state: depth test on and clear buffers.
    // Do NOT set up fixed-function projection/modelview matrices here —
    // the Remastered terrain shader manages its own uniform matrices and
    // setting glMatrixMode/glFrustum would shadow them, leaving terrain gray.
    // gos_RendererBeginFrame() is responsible for all per-frame shader state.
    // EDITOR-CLIPCONTROL-PARITY-1: the shared scene render path is reverse-Z
    // (U2) — far plane = depth 0, GL_GEQUAL. The game clears the scene to
    // glClearDepth(0.0). The editor previously used GL_LEQUAL + the GL default
    // clearDepth(1.0), which clears the scene-FBO depth to the NEAR plane under
    // reverse-Z -> every GEQUAL draw fails -> garbage/empty depth. Match game.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GEQUAL);   // reverse-Z (U2): was GL_LEQUAL
    glDepthMask(GL_TRUE);

    // When pp_editor->beginScene() ran, the scene FBO is now bound — this
    // glClear hits the scene FBO's color/depth attachments, not FBO 0.
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(0.0f);       // reverse-Z (U2): far plane = depth 0 (match game scene clear)
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // F3 render-contract: stamp GBuffer1 with the post-shadow-eligible
    // sentinel before any draws run. Required by render-contract coherence
    // guarantee (docs/superpowers/specs/render-contract-f3-report.md §5A).
    if (pp_editor) {
        pp_editor->clearGBuffer1();
    }

    gos_RendererHandleEvents();

    // by Methuselas: Editor terrain rendering is sensitive to this frame-order
    // handoff. Do not reorder BeginFrame/DoGameLogic/UpdateRenderers/EndFrame
    // while chasing shader bugs unless the Editor terrain path is revalidated.
#ifdef MC2_IMGUI
    {
        static int s_imguiFrameTrace = 0;
        ++s_imguiFrameTrace;
        if (s_imguiFrameTrace == 1)
            EditorGameOSTrace("RunGameOSLogic: frame=1 g_imguiInitialized=%d", g_imguiInitialized ? 1 : 0);
    }
    GuiRuntime::NewFrame();
    if (g_imguiInitialized) {
        // (Removed the leftover "MC2 Editor" bring-up window that floated at (10,50)
        // and overlapped the top-left status HUD.)
        if (EditorInterface::instance())
            EditorInterface::instance()->renderToolbarImGui();
    }
#endif
    EditorFramePhase_Begin();

    // GAME-EDITOR-RENDER-FRAME-DRIVER-1 (Slice 6): the editor now drives the
    // shared per-frame world-render dispatch through the ONE named seam both
    // hosts call — RenderFrameDriver_RenderWorld — instead of hand-inlining the
    // BeginFrame/UpdateRenderers/EndFrame trio. This is the REAL adoption (the
    // editor is the host that rots). Editor-only steps (DoGameLogic, the !land
    // skip, ImGui/overlays/RTT) stay OUTSIDE the driver, exactly as the parity
    // TD rule requires ("editor may add overlays but not own a parallel render
    // implementation").
    //
    // DoGameLogic must always run — it drives EditorInterface::update() which
    // processes MapGeneratorDialog::TakeAction() (Generate/Preview clicks). It
    // is hoisted to BEFORE the render dispatch so the seam wraps exactly the
    // game's contiguous trio (the game runs GameLogic well before its render
    // bracket; gameosmain.cpp ~1249 vs ~578). This restores the canonical
    // logic-before-render order. NOTE (by Methuselas, carried forward): the
    // editor terrain path was historically sensitive to this BeginFrame/
    // DoGameLogic/UpdateRenderers/EndFrame handoff — hoisting DoGameLogic out
    // of the bracket must be revalidated by an editor build + terrain load
    // test (this is exactly the Slice-6 editor build/load gate).
    if (Environment.DoGameLogic)
        Environment.DoGameLogic();
    EditorFramePhase_Mark("doLogic");

    // UpdateRenderers requires terrain (land != NULL); under MC2_IMGUI we skip
    // the world dispatch when no map is loaded (e.g. while the generator dialog
    // is open at startup). The driver always runs BeginFrame/UpdateRenderers/
    // EndFrame, so when there is no land we keep the editor's pre-existing
    // behavior — run BeginFrame + EndFrame but skip the world update — outside
    // the seam rather than entering the driver. With land present, the full
    // dispatch goes through the shared driver.
    bool runWorldThroughDriver = true;
#ifdef MC2_IMGUI
    if (!land)
        runWorldThroughDriver = false;
#endif

    // Deferred cement-atlas rebuild at a SAFE frame boundary. A cement brush
    // stroke (EditorData::refreshTerrainAfterEdit) marks the atlas dirty rather
    // than rebuilding inline, because RebuildCementAtlas() does a glGetTexImage
    // readback that breaks in-flight GL draw state if run mid-frame (it used to
    // reset normal/detail tiling map-wide). This is the frame-start safe point:
    // the GL context is current and NO terrain/world draws have started yet, so
    // the readback is safe. Must stay BEFORE RenderFrameDriver_RenderWorld (the
    // terrain draw) so newly-painted solid cement quads get an atlas layer index
    // and fill in this frame.
    gos_terrain_indirect::RebuildCementAtlasIfDirty();

    if (runWorldThroughDriver) {
        RenderFrameDesc rfd;
        rfd.host = RenderHostKind::Editor;
        rfd.drawEditorOverlays = true;
        rfd.editorSelectable = true;
        RenderFrameDriver_RenderWorld(rfd);
    } else {
        // No-land editor path: preserve exact prior behavior (Begin + End,
        // world update skipped) without routing through the always-update
        // driver.
        gos_RendererBeginFrame();
        gos_RendererEndFrame();
    }
    EditorFramePhase_Mark("updRend");
    EditorFramePhase_Mark("endFrame");

    // Composite scene FBO → default FB (tone-map, FXAA, bloom, shadow overlay).
    // This is what makes the scene visible on screen; without it the FBO contents
    // are rendered into but never displayed.  Mirrors gameosmain.cpp line 565.
    // endScene() internally calls glBindFramebuffer(GL_FRAMEBUFFER, 0), so the
    // ImGui pass that follows always targets the default FB.
    if (pp_editor) {
        pp_editor->endScene();
    }
    EditorFramePhase_Mark("endScene");

#ifdef MC2_IMGUI
    // RENDER-TO-TEXTURE viewport: endScene() composited the scene into the bottom-left
    // (0,0,viewport_w,viewport_h) region of FBO 0. Capture it into an editor-owned
    // texture so the dockspace can paint it into the central node at the node's exact
    // screen rect (GuiRuntime background draw list). This makes the scene size, the
    // displayed rect, and the pick-coordinate offset all consistent -> retires the
    // legacy DOCK_RESIZE pick divergence. Editor-only; the shared endScene is untouched.
    if (g_imguiInitialized && GuiRuntime::RttEnabled() && viewport_w > 0 && viewport_h > 0) {
        static GLuint s_presentFBO = 0, s_presentTex = 0;
        static int s_presentW = 0, s_presentH = 0;
        if (s_presentFBO == 0)
            glGenFramebuffers(1, &s_presentFBO);
        if (s_presentTex == 0 || s_presentW != viewport_w || s_presentH != viewport_h) {
            if (s_presentTex == 0) glGenTextures(1, &s_presentTex);
            glBindTexture(GL_TEXTURE_2D, s_presentTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, viewport_w, viewport_h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, s_presentFBO);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, s_presentTex, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            s_presentW = viewport_w; s_presentH = viewport_h;
        }
        // Blit the composited scene region of FBO 0 into the present texture.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s_presentFBO);
        glBlitFramebuffer(0, 0, viewport_w, viewport_h,
                          0, 0, viewport_w, viewport_h,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        GuiRuntime::SetViewportTexture(s_presentTex);
    }

    if (g_imguiInitialized) {
        // Ensure ImGui renders to the default framebuffer.
        // Shadow/terrain passes leave non-zero FBOs bound; if we don't reset here
        // ImGui draw calls go into the last-bound shadow-map texture, not the screen.
        // endScene() already bound FBO 0 above; this is belt-and-suspenders.
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        // Restore viewport to the full window in case a render pass shrank it
        // (e.g. shadow atlas pass sets a sub-viewport).
        int vw = 0, vh = 0;
        if (SDL_Window* sw = graphics::getSDLWindow())
            SDL_GetWindowSize(sw, &vw, &vh);
        if (vw > 0 && vh > 0)
            glViewport(0, 0, vw, vh);
        // RTT: the scene now lives in the present texture (painted into the central
        // node by GuiRuntime). Wipe FBO 0 so the raw bottom-left composite left by
        // endScene does not peek out from under the dockspace panels/empty regions.
        if (GuiRuntime::RttEnabled()) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        static bool s_renderTrace = false;
        if (!s_renderTrace) {
            s_renderTrace = true;
            EditorGameOSTrace("RunGameOSLogic: first GuiRuntime::Render fbo=0 viewport=%dx%d", vw, vh);
        }
    }
    GuiRuntime::Render();
#endif
    EditorFramePhase_Mark("gui");

    // Do NOT call glUseProgram(0) here.  The Remastered terrain shader program
    // is managed by the gosRenderer; forcibly unbinding it after EndFrame tears
    // down state the renderer expects to persist across the swap.  Let the
    // renderer own its own program lifecycle.
    graphics::swap_window(g_editorRenderWindow);
    EditorFramePhase_Mark("swap");
    EditorFramePhase_End();

#ifdef TRACY_ENABLE
    TracyGpuCollect;
    FrameMark;
#endif

    return 0;
}


// ---------------------------------------------------------------------------
// Context accessor — used by EditorInterface::OnLButtonUp to ensure the
// WGL context is current before calling tryGameplayPick / glReadPixels.
// Caller must have verified !IsObjectIdBufferEnabled() gating separately.
// ---------------------------------------------------------------------------
graphics::RenderContextHandle EditorGameOS_GetRenderContext()
{
    return g_editorRenderContext;
}

// ---------------------------------------------------------------------------
// Legacy editor/global compatibility symbols
// ---------------------------------------------------------------------------
HSTRRES gameResourceHandle = nullptr;

extern void GetGameOSEnvironment(char* path);

void GetGameOSEnvironment(const char* path)
{
    GetGameOSEnvironment((char*)path);
}

void EditorGameOS_Shutdown(void)
{
    EditorGameOSTrace("EditorGameOS_Shutdown: enter");

    if (g_editorAudioCreated)
    {
        EditorGameOSTrace("EditorGameOS_Shutdown: before gos_DestroyAudio");
        gos_DestroyAudio();
        g_editorAudioCreated = false;
        EditorGameOSTrace("EditorGameOS_Shutdown: after gos_DestroyAudio");
    }

    if (getGosRenderer())
    {
#ifdef MC2_IMGUI
        GuiRuntime::Shutdown();
#endif
        EditorGameOSTrace("EditorGameOS_Shutdown: before gos_DestroyRenderer");
        gos_DestroyRenderer();
        EditorGameOSTrace("EditorGameOS_Shutdown: after gos_DestroyRenderer");
    }

    if (g_editorRenderContext)
    {
        EditorGameOSTrace("EditorGameOS_Shutdown: before destroy_render_context");
        graphics::destroy_render_context(g_editorRenderContext);
        g_editorRenderContext = nullptr;
        EditorGameOSTrace("EditorGameOS_Shutdown: after destroy_render_context");
    }

    if (g_editorRenderWindow)
    {
        EditorGameOSTrace("EditorGameOS_Shutdown: before destroy_window");
        graphics::destroy_window(g_editorRenderWindow);
        g_editorRenderWindow = nullptr;
        EditorGameOSTrace("EditorGameOS_Shutdown: after destroy_window");
    }
}

int S_stricmp(const char* a, const char* b)
{
    return _stricmp(a, b);
}

int S_strnicmp(const char* a, const char* b, size_t n)
{
    return _strnicmp(a, b, n);
}

char* S_strlwr(char* s)
{
    for (char* p = s; *p; ++p)
        *p = (char)std::tolower((unsigned char)*p);
    return s;
}

int S_snprintf(char* buf, size_t size, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int result = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return result;
}
