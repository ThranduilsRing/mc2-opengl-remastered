/***************************************************************
* FILENAME: EditorGosRender.cpp
* DESCRIPTION: Hosts the remastered SDL/OpenGL renderer inside the MFC Editor window.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 05/16/2026
* UPDATED BY: Methuselas
* CHANGES: Remove temporary Editor renderer trace calls after shader/runtime validation.
****************************************************************/

#include "gos_render.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>   // timeBeginPeriod / timeGetTime (editor frame limiter)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <SDL2/SDL.h>

#ifdef MC2_IMGUI
#include "imgui.h"
// g_imguiInitialized is defined in GuiRuntime/GuiRuntime.cpp.
// Declared here rather than pulling in GuiRuntime.h to keep the
// renderer layer independent of the GUI layer headers.
extern bool g_imguiInitialized;
#endif
#include <SDL2/SDL_syswm.h>
#include "utils/mc2log.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // str<>()
#include "mc2fmt.h"

// FIXME: think how to make it better when different parts need window
SDL_Window* g_sdl_window = nullptr;

#ifdef PLATFORM_WINDOWS
static HWND g_editor_parent_hwnd = nullptr;
static HWND g_editor_child_hwnd = nullptr;

// by Methuselas: the OpenGL child HWND sits on top of the MFC editor client
// area, so Windows delivers mouse button/drag messages to the child first.
// Forward those messages back to the MFC EditorInterface HWND; that keeps the
// original Editor tool/camera handlers authoritative while the GL child remains
// only a render surface.
static LPARAM EditorGosRender_MapMousePointToEditor(HWND childHwnd, LPARAM lParam)
{
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    if (g_editor_parent_hwnd && ::IsWindow(g_editor_parent_hwnd))
        ::MapWindowPoints(childHwnd, g_editor_parent_hwnd, &pt, 1);

    return MAKELPARAM((short)pt.x, (short)pt.y);
}

static bool EditorGosRender_ForwardMouseToEditor(HWND childHwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (!g_editor_parent_hwnd || !::IsWindow(g_editor_parent_hwnd))
        return false;

    //
    // Mouse position is tracked every frame via GetCursorPos+ScreenToClient in
    // GuiRuntime::NewFrame, so ImGui always knows hover state before messages arrive.
    //
    // Button routing uses IsWindowHovered (spatial, no one-frame lag) rather than
    // WantCaptureMouse (which is tainted by mouse_any_down across frames):
    //   - Cursor over ImGui window at DOWN time  -> inject into ImGui, consume (don't
    //     forward to MFC).  Track flag = false so UP is also consumed.
    //   - Cursor over viewport at DOWN time      -> inject into ImGui (so its internal
    //     state stays consistent), forward to MFC.  Track flag = true so UP is forwarded.
    //   - UP always mirrors DOWN via the tracking flag so press/release are correlated.
    //
    // WM_MOUSEWHEEL is always injected because the scroll delta is only available from
    // the message; GetAsyncKeyState has no wheel equivalent.
    //
    // NOTE: injecting button events on viewport clicks sets ImGui's mouse_any_down=true
    // for the *next* frame, making WantCaptureMouse=true.  The inspector pick gate in
    // missiongui.cpp uses IsWindowHovered(AnyWindow) -- not WantCaptureMouse -- so it
    // is unaffected by that transient state.
#ifdef MC2_IMGUI
    if (g_imguiInitialized) {
        // Per-button tracking: true while MFC received the matching button-down.
        // Ensures button-up is forwarded to MFC iff its button-down was, preventing
        // orphaned press or release messages in either consumer.
        static bool s_lbFwdToMfc = false;
        static bool s_rbFwdToMfc = false;
        static bool s_mbFwdToMfc = false;

        bool wantMouse = ImGui::GetIO().WantCaptureMouse;
        ImGuiIO& io    = ImGui::GetIO();

        switch (msg)
        {
        case WM_MOUSEWHEEL:
            io.AddMouseWheelEvent(
                0.0f, (float)GET_WHEEL_DELTA_WPARAM(wParam) / (float)WHEEL_DELTA);
            if (wantMouse) return true;
            break;

        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
            io.AddMouseButtonEvent(0, true);
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
                s_lbFwdToMfc = false;
                return true;   // ImGui owns this click; do not forward to MFC
            }
            s_lbFwdToMfc = true;   // viewport click; forward to MFC below
            break;
        case WM_LBUTTONUP:
            io.AddMouseButtonEvent(0, false);
            if (!s_lbFwdToMfc) return true;
            s_lbFwdToMfc = false;
            break;

        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
            io.AddMouseButtonEvent(1, true);
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
                s_rbFwdToMfc = false;
                return true;
            }
            s_rbFwdToMfc = true;
            break;
        case WM_RBUTTONUP:
            io.AddMouseButtonEvent(1, false);
            if (!s_rbFwdToMfc) return true;
            s_rbFwdToMfc = false;
            break;

        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
            io.AddMouseButtonEvent(2, true);
            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow)) {
                s_mbFwdToMfc = false;
                return true;
            }
            s_mbFwdToMfc = true;
            break;
        case WM_MBUTTONUP:
            io.AddMouseButtonEvent(2, false);
            if (!s_mbFwdToMfc) return true;
            s_mbFwdToMfc = false;
            break;

        default:
            break;
        }
    }
#endif
    // ── SDL mouse-position injection ─────────────────────────────────────────
    // The GL child uses a custom WndProc, so SDL never generates
    // SDL_MOUSEMOTION events for it.  Push a synthetic event on every
    // WM_MOUSEMOVE so the GameOS input system (gos_GetMouseInfo) returns
    // accurate coordinates — needed by missiongui inspector picks.
    // This is independent of the ImGui path above.
    if (msg == WM_MOUSEMOVE) {
        SDL_Event motionEv = {};
        motionEv.type = SDL_MOUSEMOTION;
        motionEv.motion.x = (Sint32)GET_X_LPARAM(lParam);
        motionEv.motion.y = (Sint32)GET_Y_LPARAM(lParam);
        SDL_PushEvent(&motionEv);
    }


    LPARAM editorLParam = lParam;

    switch (msg)
    {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
            editorLParam = EditorGosRender_MapMousePointToEditor(childHwnd, lParam);
            break;

        case WM_MOUSEWHEEL:
            // WM_MOUSEWHEEL already carries screen coordinates in lParam.
            // MFC's OnMouseWheel expects that shape, so forward it unchanged.
            break;

        default:
            return false;
    }

    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
        msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK)
    {
        ::SetFocus(g_editor_parent_hwnd);
    }

    ::SendMessage(g_editor_parent_hwnd, msg, wParam, editorLParam);
    return true;
}

static LRESULT CALLBACK EditorGLChildWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (EditorGosRender_ForwardMouseToEditor(hwnd, msg, wParam, lParam))
        return 0;

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}
#endif

// by Methuselas: MFC owns the Editor shell, menus, focus, accelerators,
// tool input, and shutdown. This file owns only the embedded SDL/OpenGL
// render surface; do not move Editor UI/input authority into the renderer.
namespace graphics {

void set_editor_parent_window(void* hwnd)
{
#ifdef PLATFORM_WINDOWS
    g_editor_parent_hwnd = (HWND)hwnd;
#else
    (void)hwnd;
#endif
}

#ifdef PLATFORM_WINDOWS
static HWND get_sdl_hwnd(SDL_Window* window)
{
    if (!window)
        return nullptr;

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo))
        return nullptr;

    return wmInfo.info.win.window;
}

static void size_sdl_child_to_editor_client(HWND child)
{
    if (!child || !::IsWindow(child) || !g_editor_parent_hwnd || !::IsWindow(g_editor_parent_hwnd))
        return;

    RECT rc;
    ::GetClientRect(g_editor_parent_hwnd, &rc);

    const int clientWidth = rc.right - rc.left;
    const int clientHeight = rc.bottom - rc.top;

    ::SetWindowPos(
        child,
        HWND_TOP,
        0,
        0,
        clientWidth,
        clientHeight,
        SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
}

static void attach_sdl_window_to_editor_parent(SDL_Window* window, int width, int height)
{
    (void)width;
    (void)height;

    if (!window || !g_editor_parent_hwnd || !::IsWindow(g_editor_parent_hwnd))
        return;

    HWND child = get_sdl_hwnd(window);
    if (!child || !::IsWindow(child))
        return;

    g_editor_child_hwnd = child;

    // The editor must remain an MFC-owned application. SDL is only the
    // OpenGL child surface hosted by the MFC view/client HWND.
    ::SetParent(child, g_editor_parent_hwnd);

    LONG_PTR style = ::GetWindowLongPtr(child, GWL_STYLE);
    style &= ~(WS_POPUP | WS_OVERLAPPED | WS_CAPTION | WS_THICKFRAME |
               WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    style |= (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
    ::SetWindowLongPtr(child, GWL_STYLE, style);

    LONG_PTR exStyle = ::GetWindowLongPtr(child, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_APPWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED |
                 WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    ::SetWindowLongPtr(child, GWL_EXSTYLE, exStyle);

    size_sdl_child_to_editor_client(child);

    // Do not let SDL grab/capture the mouse away from the MFC editor shell.
    SDL_SetWindowGrab(window, SDL_FALSE);
    SDL_SetRelativeMouseMode(SDL_FALSE);
#if SDL_VERSION_ATLEAST(2, 0, 4)
    SDL_CaptureMouse(SDL_FALSE);
#endif
}
#endif


static bool VERBOSE_VIDEO = true;   // unchanged (keep current behavior)
static bool VERBOSE_RENDER = false; // mode/driver/extensions dump silenced; GPU identity prints unconditionally
static bool VERBOSE_MODES = false;  // display-mode enumeration silenced
static bool ENABLE_VSYNC = false;  // default off; overridden by MC2_VSYNC env var in init_render_context

struct RenderWindow {
    SDL_Window* window_;
    int width_;
    int height_;
};


static void PrintRenderer(SDL_RendererInfo * info);

// MFC owns cursor selection and message dispatch. Shared game loading/cursor
// hooks must not apply the standalone SDL window policy to the editor shell.
void set_native_cursor(SDL_Cursor*, bool) {}
void pump_loading_events() {}


void set_verbose(bool is_verbose)
{
    VERBOSE_VIDEO = is_verbose;
    VERBOSE_RENDER = is_verbose;
    VERBOSE_MODES = is_verbose;
}

RenderWindow* create_window(const char* pwinname, int width, int height, bool windowed_hint)
{
    // windowed_hint is a game-side SDL resizable-border hint; the
    // editor renders into a Win32 child HWND, so it does not apply here.
    (void)windowed_hint;
	int i, j, m, n;
	SDL_DisplayMode fullscreen_mode;
    SDL_Window* window = nullptr; 

    if (VERBOSE_VIDEO) {
        n = SDL_GetNumVideoDrivers();
        if (n == 0) {
            fprintf(stderr, "No built-in video drivers\n");
        } else {
            fprintf(stderr, "Built-in video drivers:");
            for (i = 0; i < n; ++i) {
                if (i > 0) {
                    fprintf(stderr, ",");
                }
                fprintf(stderr, " %s", SDL_GetVideoDriver(i));
            }
            fprintf(stderr, "\n");
        }
    }

    // MFC owns the process entry point now.  SDL2main is intentionally not
    // linked into the editor target, so the editor renderer must initialize
    // SDL video/events explicitly and safely here.  Do not call SDL_VideoInit()
    // directly from the MFC path; SDL_InitSubSystem keeps SDL's subsystem state
    // coherent for window creation, events, and shutdown.
    SDL_SetMainReady();

    if ((SDL_WasInit(0) & SDL_INIT_VIDEO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            fprintf(stderr, "Couldn't initialize SDL video/events: %s\n", SDL_GetError());
            return nullptr;
        }
    } else if ((SDL_WasInit(0) & SDL_INIT_EVENTS) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0) {
            fprintf(stderr, "Couldn't initialize SDL events: %s\n", SDL_GetError());
            return nullptr;
        }
    } else {
    }

    if (VERBOSE_VIDEO) {
        fprintf(stderr, "Video driver: %s\n", SDL_GetCurrentVideoDriver());
    }

    if (VERBOSE_VIDEO) {
        printf("SDL revision: %s\n", SDL_GetRevision());

        SDL_version compiled;
        SDL_version linked;

        SDL_VERSION(&compiled);
        SDL_GetVersion(&linked);
        fprintf(stderr, "We compiled against SDL version %d.%d.%d \n",
            compiled.major, compiled.minor, compiled.patch);
        fprintf(stderr, "But we are linking against SDL version %d.%d.%d.\n",
           linked.major, linked.minor, linked.patch);
    }

    if (VERBOSE_MODES) {
        SDL_DisplayMode mode;
        int bpp;
        Uint32 Rmask, Gmask, Bmask, Amask;

        n = SDL_GetNumVideoDisplays();
        fprintf(stderr, "Number of displays: %d\n", n);
        for (i = 0; i < n; ++i) {
            fprintf(stderr, "Display %d:\n", i);

            SDL_GetDesktopDisplayMode(i, &mode);
            SDL_PixelFormatEnumToMasks(mode.format, &bpp, &Rmask, &Gmask,
                    &Bmask, &Amask);
            fprintf(stderr,
                    "  Current mode: %dx%d@%dHz, %d bits-per-pixel (%s)\n",
                    mode.w, mode.h, mode.refresh_rate, bpp,
                    SDL_GetPixelFormatName(mode.format));
            if (Rmask || Gmask || Bmask) {
                fprintf(stderr, "      Red Mask   = 0x%.8x\n", Rmask);
                fprintf(stderr, "      Green Mask = 0x%.8x\n", Gmask);
                fprintf(stderr, "      Blue Mask  = 0x%.8x\n", Bmask);
                if (Amask)
                    fprintf(stderr, "      Alpha Mask = 0x%.8x\n", Amask);
            }

            m = SDL_GetNumDisplayModes(i);
            if (m == 0) {
                fprintf(stderr, "No available fullscreen video modes\n");
            } else {
                fprintf(stderr, "  Fullscreen video modes:\n");
                for (j = 0; j < m; ++j) {
                    SDL_GetDisplayMode(i, j, &mode);
                    SDL_PixelFormatEnumToMasks(mode.format, &bpp, &Rmask,
                            &Gmask, &Bmask, &Amask);
                    fprintf(stderr,
                            "    Mode %d: %dx%d@%dHz, %d bits-per-pixel (%s)\n",
                            j, mode.w, mode.h, mode.refresh_rate, bpp,
                            SDL_GetPixelFormatName(mode.format));
                    if (Rmask || Gmask || Bmask) {
                        fprintf(stderr, "        Red Mask   = 0x%.8x\n",
                                Rmask);
                        fprintf(stderr, "        Green Mask = 0x%.8x\n",
                                Gmask);
                        fprintf(stderr, "        Blue Mask  = 0x%.8x\n",
                                Bmask);
                        if (Amask)
                            fprintf(stderr,
                                    "        Alpha Mask = 0x%.8x\n",
                                    Amask);
                    }
                }
            }
        }
    }

    if (VERBOSE_RENDER) {
        SDL_RendererInfo info;

        n = SDL_GetNumRenderDrivers();
        if (n == 0) {
            fprintf(stderr, "No built-in render drivers\n");
        } else {
            fprintf(stderr, "Built-in render drivers:\n");
            for (i = 0; i < n; ++i) {
                SDL_GetRenderDriverInfo(i, &info);
                PrintRenderer(&info);
            }
        }
    }

    SDL_zero(fullscreen_mode);
    switch (/*state->depth*/0) {
        case 8:
            fullscreen_mode.format = SDL_PIXELFORMAT_INDEX8;
            break;
        case 15:
            fullscreen_mode.format = SDL_PIXELFORMAT_RGB555;
            break;
        case 16:
            fullscreen_mode.format = SDL_PIXELFORMAT_RGB565;
            break;
        case 24:
            fullscreen_mode.format = SDL_PIXELFORMAT_RGB24;
            break;
        default:
            fullscreen_mode.format = SDL_PIXELFORMAT_RGB888;
            break;
    }

#ifdef PLATFORM_WINDOWS
    {
        //
        // SDL_CreateWindow with SDL_WINDOW_VULKAN registers its own WNDCLASS and calls
        // CreateWindowEx, which triggers WGL pixel-format negotiation synchronously on
        // the calling thread.  That negotiation crashes on some AMD/NVIDIA drivers when
        // the window is top-level and subsequently reparented (SetParent invalidates the
        // WGL surface), and it also fails silently when the calling thread has no active
        // message queue (pre-pump calls from InitInstance).
        //
        // Fix: create a plain Win32 child HWND ourselves — with CS_OWNDC so WGL can
        // allocate a dedicated device context — then hand that HWND to
        // SDL_CreateWindowFrom().  SDL wraps our HWND without going through its own
        // window-class registration or WGL pixel-format selection path, so none of the
        // above can occur.  The child is already parented under the MFC EditorInterface
        // view; no SetParent or style fixup is needed.

        if (!g_editor_parent_hwnd || !::IsWindow(g_editor_parent_hwnd))
        {
            fprintf(stderr, "EditorGosRender::create_window: no valid parent HWND\n");
            return nullptr;
        }

        // Register a minimal window class with CS_OWNDC (required for OpenGL).
        // The class is registered once per process; subsequent calls to RegisterClassEx
        // with the same name return 0 but GetLastError returns ERROR_CLASS_ALREADY_EXISTS
        // which we treat as success.
        static bool s_classRegistered = false;
        static const wchar_t* s_glClassName = L"MC2EditorGLChild";
        if (!s_classRegistered)
        {
            WNDCLASSEXW wc = {};
            wc.cbSize        = sizeof(wc);
            wc.style         = CS_HREDRAW | CS_VREDRAW;
            // by Methuselas: this child HWND is still render-only, but it must
            // forward mouse messages so MFC EditorInterface keeps ownership of
            // camera rotation, painting, selection, and capture behavior.
            wc.lpfnWndProc   = EditorGLChildWndProc;
            wc.hInstance     = ::GetModuleHandleW(nullptr);
            wc.lpszClassName = s_glClassName;
            ATOM a = ::RegisterClassExW(&wc);
            if (a == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                fprintf(stderr, "EditorGosRender::create_window: RegisterClassEx failed\n");
                return nullptr;
            }
            s_classRegistered = true;
        }

        RECT parentRC = {};
        ::GetClientRect(g_editor_parent_hwnd, &parentRC);
        int childW = (parentRC.right  > 0) ? parentRC.right  : width;
        int childH = (parentRC.bottom > 0) ? parentRC.bottom : height;


        HWND glHwnd = ::CreateWindowExW(
            0,
            s_glClassName,
            L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            0, 0, childW, childH,
            g_editor_parent_hwnd,
            nullptr,
            ::GetModuleHandleW(nullptr),
            nullptr);


        if (!glHwnd)
        {
            fprintf(stderr, "EditorGosRender::create_window: CreateWindowEx GL child failed\n");
            return nullptr;
        }

        g_editor_child_hwnd = glHwnd;

        // The native Vulkan present owner creates its Win32 surface on this child.
        SDL_SetHint(SDL_HINT_VIDEO_FOREIGN_WINDOW_VULKAN, "1");
        window = SDL_CreateWindowFrom((void*)glHwnd);


        if (!window)
        {
            fprintf(stderr, "SDL_CreateWindowFrom failed: %s\n", SDL_GetError());
            ::DestroyWindow(glHwnd);
            g_editor_child_hwnd = nullptr;
            return nullptr;
        }

        // SDL_CreateWindowFrom does not call SDL_VideoInit internally; the subsystem
        // init we already did is sufficient.  Update width/height from the actual child.
        width  = childW;
        height = childH;

        // Do not call SDL_SetWindowGrab / SDL_SetRelativeMouseMode here — the child
        // is already well-behaved (DefWindowProc, no capture) and the MFC frame owns
        // mouse focus through its normal message routing.
        SDL_SetWindowGrab(window, SDL_FALSE);
        SDL_SetRelativeMouseMode(SDL_FALSE);
    }
#else
    {
        Uint32 windowFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
        window = SDL_CreateWindow(pwinname ? pwinname : "--",
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, windowFlags);
        if (!window) {
            fprintf(stderr, "Couldn't create window: %s\n", SDL_GetError());
            return nullptr;
        }
        SDL_GetWindowSize(window, &width, &height);
    }
#endif

    RenderWindow* rw = new RenderWindow();
    rw->window_ = window;
    rw->width_ = width;
    rw->height_ = height;

    g_sdl_window = window;
    SDL_ShowCursor(SDL_ENABLE);

    return rw;
}

SDL_Window* getSDLWindow() noexcept
{
    return g_sdl_window;
}



//==============================================================================
// Editor frame-rate limiter. The editor has no gameplay clock, so its render
// loop runs as fast as the GPU allows (hundreds of fps) and spins the card for
// no benefit. Cap to ~60fps. Override with MC2_EDITOR_FPS_CAP (0 = uncapped).
// Called once per presented frame from swap_window (the single present site).
void limit_editor_frame()
{
    static int   s_capFps = -2;   // -2 = uninitialized
    static DWORD s_lastMs = 0;
    if (s_capFps == -2)
    {
        s_capFps = 60;
        s_capFps = mc2::gate::get<"MC2_EDITOR_FPS_CAP">();   // row default 60
        timeBeginPeriod(1);       // 1ms Sleep() granularity (editor main() skips gameosmain's call)
        s_lastMs = timeGetTime();
        return;                   // first frame: just stamp
    }
    if (s_capFps <= 0) return;    // uncapped
    const DWORD targetMs = (DWORD)(1000 / s_capFps);   // 60 -> 16ms (~62fps)
    DWORD elapsed = timeGetTime() - s_lastMs;
    if (elapsed < targetMs)
        ::Sleep(targetMs - elapsed);
    s_lastMs = timeGetTime();
}

//==============================================================================


//==============================================================================



//==============================================================================


//==============================================================================


//==============================================================================
bool resize_window(RenderWindowHandle rw_handle, int width, int height)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw);

#ifdef PLATFORM_WINDOWS
    if (g_editor_parent_hwnd && g_editor_child_hwnd && ::IsWindow(g_editor_child_hwnd))
    {
        ::SetWindowPos(g_editor_child_hwnd, HWND_TOP, 0, 0, width, height,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    else
#endif
    {
        SDL_SetWindowSize(rw->window_, width, height);
    }

    rw->width_ = width;
    rw->height_ = height;

    return true;
}

//==============================================================================
bool set_window_fullscreen(RenderWindowHandle rw_handle, bool fullscreen)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw);

    Uint32 flags = fullscreen ? /*SDL_WINDOW_FULLSCREEN*/ SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
    
    if(0 != SDL_SetWindowFullscreen(rw->window_, flags)) {
        MC2_LOG(Editor, Error, "SDL_SetWindowFullscreen: {}", mc2::fmt::safe(SDL_GetError()));
        return false;
    }

    return true;
}

//==============================================================================
bool is_mode_supported(int width, int height, int bpp) {

    int displayIndex = 0;

    SDL_DisplayMode desired;
	desired.format = bpp==16 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_RGB888;
	desired.w = width;
	desired.h = height;
	desired.refresh_rate = 0;
	desired.driverdata = 0;

    SDL_DisplayMode returned;
    
    if(nullptr == SDL_GetClosestDisplayMode(displayIndex, &desired, &returned)) {
        MC2_LOG(Editor, Error, "resize_window: {}", mc2::fmt::safe(SDL_GetError()));
        return false;
    }


    if(returned.w == desired.w && returned.h == desired.h && returned.format == desired.format)
        return true;

    return false;
}

//==============================================================================
int get_window_display_index(RenderWindowHandle window)
{
    RenderWindow* rw = window;
    assert(rw && rw->window_);
    return SDL_GetWindowDisplayIndex(rw->window_);
}

//==============================================================================
int get_num_display_modes(int display_index)
{
    return SDL_GetNumDisplayModes(display_index);
}

//==============================================================================
bool get_desktop_display_mode(int display_index, int* width, int* height, int* bpp)
{
    assert(width && height && bpp);

    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(display_index, &dm) != 0) {
        MC2_LOG(Editor, Error, "SDL_GetDesktopDisplayMode failed: {}", mc2::fmt::safe(SDL_GetError()));
        return false;
    }

    *width = dm.w;
    *height = dm.h;
    *bpp = SDL_BITSPERPIXEL(dm.format);
    return true;
}

//==============================================================================
bool get_display_mode_by_index(int display_index, int mode_index, int* width, int* height, int* bpp)
{
    assert(width && height && bpp);

    SDL_DisplayMode dm;
    if (SDL_GetDisplayMode(display_index, mode_index, &dm) != 0) {
        MC2_LOG(Editor, Error, "SDL_GetDisplayMode failed: {}", mc2::fmt::safe(SDL_GetError()));
        return false;
    }

    *width = dm.w;
    *height = dm.h;
    *bpp = SDL_BITSPERPIXEL(dm.format);
    return true;
}

//==============================================================================
void get_window_size(RenderWindowHandle rw_handle, int* width, int* height)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw && width && height);
    *width = rw->width_;
    *height = rw->height_;
}

//==============================================================================
void get_drawable_size(RenderWindowHandle rw_handle, int* width, int* height)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw && width && height);
	SDL_GetWindowSizeInPixels(rw->window_, width, height);
}

//==============================================================================
void destroy_window(RenderWindowHandle rw_handle)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    SDL_ShowCursor(SDL_ENABLE);
    SDL_DestroyWindow(rw->window_);
    delete rw;

    g_sdl_window = nullptr;
#ifdef PLATFORM_WINDOWS
    // Destroy the Win32 GL child HWND we created in create_window.
    // SDL_DestroyWindow on a SDL_CreateWindowFrom window does NOT destroy the
    // underlying HWND (SDL doesn't own it), so we must do it explicitly.
    if (g_editor_child_hwnd && ::IsWindow(g_editor_child_hwnd))
    {
        ::DestroyWindow(g_editor_child_hwnd);
    }
    g_editor_child_hwnd = nullptr;
#endif
}

//==============================================================================
static void PrintRendererFlag(Uint32 flag)
{
	switch (flag) {
	case SDL_RENDERER_PRESENTVSYNC:
		fprintf(stderr, "PresentVSync");
		break;
	case SDL_RENDERER_ACCELERATED:
		fprintf(stderr, "Accelerated");
		break;
	default:
		fprintf(stderr, "0x%8.8x", flag);
		break;
	}
}

//==============================================================================
static void PrintPixelFormat(Uint32 format)
{
	switch (format) {
	case SDL_PIXELFORMAT_UNKNOWN:
		fprintf(stderr, "Unknwon");
		break;
	case SDL_PIXELFORMAT_INDEX1LSB:
		fprintf(stderr, "Index1LSB");
		break;
	case SDL_PIXELFORMAT_INDEX1MSB:
		fprintf(stderr, "Index1MSB");
		break;
	case SDL_PIXELFORMAT_INDEX4LSB:
		fprintf(stderr, "Index4LSB");
		break;
	case SDL_PIXELFORMAT_INDEX4MSB:
		fprintf(stderr, "Index4MSB");
		break;
	case SDL_PIXELFORMAT_INDEX8:
		fprintf(stderr, "Index8");
		break;
	case SDL_PIXELFORMAT_RGB332:
		fprintf(stderr, "RGB332");
		break;
	case SDL_PIXELFORMAT_RGB444:
		fprintf(stderr, "RGB444");
		break;
	case SDL_PIXELFORMAT_RGB555:
		fprintf(stderr, "RGB555");
		break;
	case SDL_PIXELFORMAT_BGR555:
		fprintf(stderr, "BGR555");
		break;
	case SDL_PIXELFORMAT_ARGB4444:
		fprintf(stderr, "ARGB4444");
		break;
	case SDL_PIXELFORMAT_ABGR4444:
		fprintf(stderr, "ABGR4444");
		break;
	case SDL_PIXELFORMAT_ARGB1555:
		fprintf(stderr, "ARGB1555");
		break;
	case SDL_PIXELFORMAT_ABGR1555:
		fprintf(stderr, "ABGR1555");
		break;
	case SDL_PIXELFORMAT_RGB565:
		fprintf(stderr, "RGB565");
		break;
	case SDL_PIXELFORMAT_BGR565:
		fprintf(stderr, "BGR565");
		break;
	case SDL_PIXELFORMAT_RGB24:
		fprintf(stderr, "RGB24");
		break;
	case SDL_PIXELFORMAT_BGR24:
		fprintf(stderr, "BGR24");
		break;
	case SDL_PIXELFORMAT_RGB888:
		fprintf(stderr, "RGB888");
		break;
	case SDL_PIXELFORMAT_BGR888:
		fprintf(stderr, "BGR888");
		break;
	case SDL_PIXELFORMAT_ARGB8888:
		fprintf(stderr, "ARGB8888");
		break;
	case SDL_PIXELFORMAT_RGBA8888:
		fprintf(stderr, "RGBA8888");
		break;
	case SDL_PIXELFORMAT_ABGR8888:
		fprintf(stderr, "ABGR8888");
		break;
	case SDL_PIXELFORMAT_BGRA8888:
		fprintf(stderr, "BGRA8888");
		break;
	case SDL_PIXELFORMAT_ARGB2101010:
		fprintf(stderr, "ARGB2101010");
		break;
	case SDL_PIXELFORMAT_YV12:
		fprintf(stderr, "YV12");
		break;
	case SDL_PIXELFORMAT_IYUV:
		fprintf(stderr, "IYUV");
		break;
	case SDL_PIXELFORMAT_YUY2:
		fprintf(stderr, "YUY2");
		break;
	case SDL_PIXELFORMAT_UYVY:
		fprintf(stderr, "UYVY");
		break;
	case SDL_PIXELFORMAT_YVYU:
		fprintf(stderr, "YVYU");
		break;
	default:
		fprintf(stderr, "0x%8.8x", format);
		break;
	}
}

//==============================================================================
static void PrintRenderer(SDL_RendererInfo * info)
{
    size_t i, count;

    fprintf(stderr, "  Renderer %s:\n", info->name);

    fprintf(stderr, "    Flags: 0x%8.8X", info->flags);
    fprintf(stderr, " (");
    count = 0;
    for (i = 0; i < sizeof(info->flags) * 8; ++i) {
        Uint32 flag = (1 << i);
        if (info->flags & flag) {
            if (count > 0) {
                fprintf(stderr, " | ");
            }
            PrintRendererFlag(flag);
            ++count;
        }
    }
    fprintf(stderr, ")\n");

    fprintf(stderr, "    Texture formats (%d): ", info->num_texture_formats);
    for (i = 0; i < info->num_texture_formats; ++i) {
        if (i > 0) {
			fprintf(stderr, ", ");
		}
		PrintPixelFormat(info->texture_formats[i]);
	}
	fprintf(stderr, "\n");

	if (info->max_texture_width || info->max_texture_height) {
		fprintf(stderr, "    Max Texture Size: %dx%d\n",
				info->max_texture_width, info->max_texture_height);
	}
}


}; // namespace graphics
