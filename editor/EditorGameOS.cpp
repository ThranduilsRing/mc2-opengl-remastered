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
#include <atomic>
#include "stdafx.h"
#include "EditorRenderTimer.h"
#include "gos_render_pass_timer.h"  // EditorFramePhase_* whole-frame timing (MC2_EDITOR_GPU_TIMERS)
#include "EditorWatchdog.h"  // EditorWatchdog_Heartbeat (MC2_EDITOR_WATCHDOG)
#include <cstdarg>
#include <vector>
#include <stdlib.h>
#include <cstdio>
#include <cstring>
#include <cctype>

#include <gameos.hpp>          // gos_RendererBeginFrame / EndFrame / HandleEvents
#include "gos_render.h"        // graphics::make_current_context etc.
#include "render_host_kind.h"  // InitializeRenderContextConventions (shared game/editor parity)
#include "render_frame_driver.h" // Shared world-render dispatch
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
#include "Gates/gate.h"
#include "Gates/render_options_publish.h"

#ifdef TRACY_ENABLE
#  include "tracy/Tracy.hpp"
#endif

extern std::atomic<uint32_t> g_mc2FrameCounter;   // engine logic-pass epoch (mclib/tgl.cpp), advanced by gos::host::advanceFrameCounter
extern uint32_t g_mc2RenderFrame;    // render-frame clock (mclib/tgl.cpp), once per presented frame

#ifdef MC2_IMGUI
#include "GuiRuntime.h"
#include "gos_host_vk.h"          // initVulkanBackend / presentOrSwap: the game's VK bring-up and present, shared
#include "gos_screenshot.h"        // -capture on the Vulkan backend
#include "render_backend_select.h"
#include "imgui.h"
#include "imgui_impl_sdl2.h"
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

    // Shared graphics code requires the off-screen window hooks.
    // The editor never runs the off-screen harness window mode: no-ops.
    bool window_offscreen_requested() { return false; }
    void enforce_window_offscreen(RenderWindowHandle /*rw*/) {}
}


#ifdef PLATFORM_WINDOWS
#else
#endif

namespace graphics { void limit_editor_frame(); }
extern void gos_CreateRenderer(graphics::RenderWindowHandle win_h, int w, int h);
extern void gos_DestroyRenderer();
extern bool gos_CreateAudio();
extern void gos_DestroyAudio();
class gosRenderer;
extern gosRenderer* getGosRenderer();

static graphics::RenderWindowHandle  g_editorRenderWindow  = nullptr;
static bool g_editorAudioCreated = false;


static void EditorGameOSTrace(const char* fmt, ...)
{
	if (!mc2::gate::get<"MC2_EDITOR_TRACE">())
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

// The shared inspector requires this bookmark symbol, but the game camera
// implementation is not linked into the editor. An empty path suppresses
// the bookmark result in the inspector.
extern "C" void mc2_save_camera_bookmark(char* outPath, int outPathLen)
{
    if (outPath && outPathLen > 0)
        outPath[0] = '\0';
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

    // The editor runs no CPrefs::load for options.cfg and no gameosmain, so it publishes the render options itself, with the game's reader
    // and grammar, before any renderer reader runs (without this the editor rendered with the built-in defaults whatever options.cfg said).
    for (const auto& r : mc2::options::publishFromInstall())
        mc2::options::logRejectedOption(r.key, r.text, r.why, mc2::options::find(r.key));

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
        // windowed_hint=false — editor child HWND, hint not applicable.
        g_editorRenderWindow = graphics::create_window("MC2 Editor (Remastered)", w, h, false);

        EditorGameOSTrace("InitGameOS: after graphics::create_window renderWindow=%p", g_editorRenderWindow);

        if (g_editorRenderWindow) {
            // Same VK bring-up as the game (gameosmain.cpp): shared engine context + present on this HWND.
            // A no-op unless the Vulkan backend is selected; a failed bring-up is fatal inside it.
            EditorGameOSTrace("InitGameOS: before initVulkanBackend backend=%s", "vulkan");
            initVulkanBackend(g_editorRenderWindow, w, h);
            EditorGameOSTrace("InitGameOS: after initVulkanBackend");


            EditorGameOSTrace("InitGameOS: before gos_CreateRenderer window=%p", g_editorRenderWindow);
            gos_CreateRenderer(g_editorRenderWindow, w, h);
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
        } else {
            EditorGameOSTrace("InitGameOS: native renderer bootstrap failed: no window");
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

// -capture=<tga>: the shown window (scene + ImGui panels) to a TGA after N loaded frames. GL reads the back
// buffer before the swap; Vulkan queues the present path's readback (the same source the game captures use).
extern const char* EditorCliCapturePath();
extern int EditorCliCaptureFrame();
static bool g_captureDone = false;
static void EditorCaptureDone(const char* path, bool ok, int w, int h)
{
    g_captureDone = true;
    fprintf(stderr, "[EDITOR_CAPTURE v1] path=\"%s\" ok=%d size=%dx%d\n", path ? path : "", ok ? 1 : 0, w, h);
    fflush(stderr);
}
static void EditorViewportCaptureTick()
{
    static int s_frames = 0;
    static bool s_queued = false;
    const char* path = EditorCliCapturePath();
    if (g_captureDone || !path || !path[0] || !land) return;
    if (++s_frames < EditorCliCaptureFrame()) return;
    if (!s_queued) s_queued = gos::screenshot::request(path, EditorCaptureDone);
}

extern void gos_RendererBeginFrame();
extern void gos_RendererEndFrame();
extern void gos_RendererFlushHUDBatch();
extern void gos_RendererHandleEvents();
extern bool gosExitGameOS();

DWORD __stdcall RunGameOSLogic()
{
    EditorWatchdog_Heartbeat();  // main-thread liveness for the stall watchdog

    if (!g_editorRenderWindow || !getGosRenderer())
    {
        EditorGameOSTrace("RunGameOSLogic: skipped window=%p renderer=%p",
            g_editorRenderWindow, getGosRenderer());
        return 0;
    }

    // Keep GameOS dimensions synchronized with the embedded MFC child window.
    HWND hwnd = EditorInterface::instance() ? EditorInterface::instance()->m_hWnd : nullptr;
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

    }

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


    // Minimal per-frame GL state: depth test on and clear buffers.
    // Do NOT set up fixed-function projection/modelview matrices here —
    // the Remastered terrain shader manages its own uniform matrices and
    // setting glMatrixMode/glFrustum would shadow them, leaving terrain gray.
    // gos_RendererBeginFrame() is responsible for all per-frame shader state.
    // Reverse-Z uses depth 0 at the far plane and GL_GEQUAL. Clearing to
    // depth 1 would reject scene draws against the near plane.




    // When pp_editor->beginScene() ran, the scene FBO is now bound — this
    // glClear hits the scene FBO's color/depth attachments, not FBO 0.




    // F3 render-contract: stamp GBuffer1 with the post-shadow-eligible
    // sentinel before any draws run. Required by render-contract coherence
    // guarantee (docs/superpowers/specs/render-contract-f3-report.md §5A).
    if (pp_editor) {
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
        if (EditorInterface::instance())
            EditorInterface::instance()->renderToolbarImGui();
    }
#endif
    EditorFramePhase_Begin();

    // Both hosts share the world-render dispatch so their render order stays
    // consistent. Editor logic and overlays remain outside the scene driver.
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

    // Under Vulkan the selection brackets and health bars route through the HUD batch (VkHudRouteScope, mclib/appear.cpp);
    // the game replays that batch here (gameosmain.cpp), so the editor must too or they never reach the present tail.
    gos_RendererFlushHUDBatch();

#ifdef MC2_IMGUI
    GuiRuntime::Render();
#endif
    EditorFramePhase_Mark("gui");

    // Do NOT call glUseProgram(0) here.  The Remastered terrain shader program
    // is managed by the gosRenderer; forcibly unbinding it after EndFrame tears
    // down state the renderer expects to persist across the swap.  Let the
    // renderer own its own program lifecycle.
    gos_render_pass_timer::FrameEnd();
    EditorViewportCaptureTick();
    // The game's present: VK composite+swapchain present when the Vulkan backend is up, else the GL swap.
    gos::host::advanceFrameCounter();   // the shared engine frame clock (props' light cache, readback tags); the game advances it at the same point
    gos::host::presentOrSwap(g_editorRenderWindow, g_mc2RenderFrame);
    graphics::limit_editor_frame();
    EditorFramePhase_Mark("present");
    EditorFramePhase_End();

#ifdef TRACY_ENABLE
    FrameMark;
#endif

    return 0;
}


// ---------------------------------------------------------------------------
// Context accessor — used by EditorInterface::OnLButtonUp to ensure the
// WGL context is current before calling tryGameplayPick / glReadPixels.
// Caller must have verified !IsObjectIdBufferEnabled() gating separately.
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
        // Same order as the game: islands, present path, VulkanContext, then the GL context and the window below.
        gos::host::shutdownVulkanBackend();
        EditorGameOSTrace("EditorGameOS_Shutdown: after shutdownVulkanBackend");
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
