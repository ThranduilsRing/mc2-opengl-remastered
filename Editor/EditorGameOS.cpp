/***************************************************************
* FILENAME: EditorGameOS.cpp
* DESCRIPTION: Provides the Editor GameOS compatibility shim for the SDL/OpenGL remaster.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/27/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster attribution header and human-maintenance comments.
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
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cctype>

#include <gameos.hpp>          // gos_RendererBeginFrame / EndFrame / HandleEvents
#include "gos_render.h"        // graphics::make_current_context etc.
#include "SDL.h"
#include "gos_input.h"         // input::beginUpdateMouseState etc.
#include "editorinterface.h"

#include <SDL2/SDL.h>

namespace graphics {
    void set_editor_parent_window(void* hwnd);
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
            GLenum glewErr = glewInit();
            EditorGameOSTrace("InitGameOS: after glewInit result=%u", (unsigned)glewErr);

            if (glewErr == GLEW_OK)
            {
                EditorGameOSTrace("InitGameOS: before gos_CreateRenderer context=%p window=%p", g_editorRenderContext, g_editorRenderWindow);
                gos_CreateRenderer(g_editorRenderContext, g_editorRenderWindow, w, h);
                EditorGameOSTrace("InitGameOS: after gos_CreateRenderer renderer=%p", getGosRenderer());

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
        if (w > 0 && h > 0 &&
            (Environment.drawableWidth != w || Environment.drawableHeight != h))
        {
            Environment.screenWidth = w;
            Environment.screenHeight = h;
            Environment.drawableWidth = w;
            Environment.drawableHeight = h;
            graphics::resize_window(g_editorRenderWindow, w, h);
            EditorGameOSTrace("RunGameOSLogic: resize w=%d h=%d", w, h);
        }
    }

    // --- Pump SDL events ---------------------------------------------------
    input::beginUpdateMouseState();

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
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
    glViewport(0, 0, viewport_w, viewport_h);

    // Minimal per-frame GL state: depth test on and clear buffers.
    // Do NOT set up fixed-function projection/modelview matrices here —
    // the Remastered terrain shader manages its own uniform matrices and
    // setting glMatrixMode/glFrustum would shadow them, leaving terrain gray.
    // gos_RendererBeginFrame() is responsible for all per-frame shader state.
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    gos_RendererHandleEvents();

    // by Methuselas: Editor terrain rendering is sensitive to this frame-order
    // handoff. Do not reorder BeginFrame/DoGameLogic/UpdateRenderers/EndFrame
    // while chasing shader bugs unless the Editor terrain path is revalidated.
    gos_RendererBeginFrame();

    if (Environment.DoGameLogic)
    {
        Environment.DoGameLogic();
    }

    Environment.UpdateRenderers();
    gos_RendererEndFrame();

    // Do NOT call glUseProgram(0) here.  The Remastered terrain shader program
    // is managed by the gosRenderer; forcibly unbinding it after EndFrame tears
    // down state the renderer expects to persist across the swap.  Let the
    // renderer own its own program lifecycle.
    graphics::swap_window(g_editorRenderWindow);

    return 0;
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
