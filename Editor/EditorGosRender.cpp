/***************************************************************
* FILENAME: EditorGosRender.cpp
* DESCRIPTION: Hosts the remastered SDL/OpenGL renderer inside the MFC Editor window.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Updated Editor Remaster attribution header and human-maintenance comments.
****************************************************************/

#include "gos_render.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <windowsx.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <stdarg.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>
#include <GL/glew.h>
#include <GL/gl.h>
#include "utils/logging.h"

static void EditorGosRenderTrace(const char* fmt, ...)
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


// FIXME: think how to make it better when different parts need window
SDL_Window* g_sdl_window = NULL;

#ifdef PLATFORM_WINDOWS
static HWND g_editor_parent_hwnd = NULL;
static HWND g_editor_child_hwnd = NULL;

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
        return NULL;

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo))
        return NULL;

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

struct RenderContext {
   SDL_GLContext glcontext_;   // used on non-Windows / legacy path
   RenderWindow* render_window_;
#ifdef PLATFORM_WINDOWS
   // On Windows the editor creates its GL context via WGL directly, because
   // SDL_GL_CreateContext refuses to work on SDL_CreateWindowFrom windows
   // (they lack the internal SDL_WINDOW_OPENGL flag).  Store the WGL handles
   // here so make_current / destroy / swap can use them without SDL involvement.
   HGLRC  wglContext_;   // the core-profile WGL context; NULL on non-editor builds
   HDC    wglDC_;        // the DC for the GL child HWND; owned here, released on destroy
#endif
};

static void PrintRenderer(SDL_RendererInfo * info);


//==============================================================================
void set_verbose(bool is_verbose)
{
    VERBOSE_VIDEO = is_verbose;
    VERBOSE_RENDER = is_verbose;
    VERBOSE_MODES = is_verbose;
}

//==============================================================================
RenderWindow* create_window(const char* pwinname, int width, int height)
{
    EditorGosRenderTrace("EditorGosRender::create_window: enter name=%s w=%d h=%d",
        pwinname ? pwinname : "<null>", width, height);
	int i, j, m, n;
	SDL_DisplayMode fullscreen_mode;
    SDL_Window* window = NULL; 

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
    EditorGosRenderTrace("EditorGosRender::create_window: before SDL_SetMainReady");
    SDL_SetMainReady();
    EditorGosRenderTrace("EditorGosRender::create_window: after SDL_SetMainReady wasInit=0x%08x",
        (unsigned)SDL_WasInit(0));

    if ((SDL_WasInit(0) & SDL_INIT_VIDEO) == 0) {
        EditorGosRenderTrace("EditorGosRender::create_window: before SDL_InitSubSystem VIDEO|EVENTS");
        if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
            EditorGosRenderTrace("EditorGosRender::create_window: SDL_InitSubSystem failed: %s", SDL_GetError());
            fprintf(stderr, "Couldn't initialize SDL video/events: %s\n", SDL_GetError());
            return NULL;
        }
        EditorGosRenderTrace("EditorGosRender::create_window: after SDL_InitSubSystem VIDEO|EVENTS wasInit=0x%08x",
            (unsigned)SDL_WasInit(0));
    } else if ((SDL_WasInit(0) & SDL_INIT_EVENTS) == 0) {
        EditorGosRenderTrace("EditorGosRender::create_window: before SDL_InitSubSystem EVENTS");
        if (SDL_InitSubSystem(SDL_INIT_EVENTS) != 0) {
            EditorGosRenderTrace("EditorGosRender::create_window: SDL_InitSubSystem EVENTS failed: %s", SDL_GetError());
            fprintf(stderr, "Couldn't initialize SDL events: %s\n", SDL_GetError());
            return NULL;
        }
        EditorGosRenderTrace("EditorGosRender::create_window: after SDL_InitSubSystem EVENTS wasInit=0x%08x",
            (unsigned)SDL_WasInit(0));
    } else {
        EditorGosRenderTrace("EditorGosRender::create_window: SDL video/events already initialized wasInit=0x%08x",
            (unsigned)SDL_WasInit(0));
    }

    if (VERBOSE_VIDEO) {
        fprintf(stderr, "Video driver: %s\n", SDL_GetCurrentVideoDriver());
    }

    //not really related to video, but let it be here
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

    SDL_GL_SetAttribute( SDL_GL_RED_SIZE, 8 );
    SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE, 8 );
    SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE, 8 );
    SDL_GL_SetAttribute( SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 16 );
    SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );

    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);

    // 4x multisampling :P
    // disable, and add as setting later.
    //SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    //SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);


    // select core profile if needed
    // COMPATIBILITY, ES,...
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    //SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    // 4.3 required for core SSBO (GL_SHADER_STORAGE_BUFFER) + std430 layout,
    // used by the GPU static-prop renderer. AMD RX 7900 XTX supports up to
    // 4.6 core; 4.3 is the minimum feature level we need.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);

    // MC2_GL_DEBUG=1 enables the OpenGL debug context AND the debug-message
    // callback (installed in gameosmain.cpp). Debug contexts run driver-side
    // validation on every GL call and can cost 10-30% perf, especially on
    // NVIDIA; the callback also floods stdout with harmless AMD-driver
    // warnings in our workload. Off by default in shipped builds;
    // env-gated rather than NDEBUG-gated so it can be flipped on a
    // deployed binary without rebuilding.
    const bool gl_debug = (getenv("MC2_GL_DEBUG") != nullptr);
    if (gl_debug) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
        printf("[GL_DEBUG] MC2_GL_DEBUG=1 -- GL debug context active. This reduces performance.\n");
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

            /* Print available fullscreen video modes */
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
    //fullscreen_mode.refresh_rate = state->refresh_rate;

#ifdef PLATFORM_WINDOWS
    {
        // Editor/MFC host path — Windows only.
        //
        // SDL_CreateWindow with SDL_WINDOW_OPENGL registers its own WNDCLASS and calls
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
            EditorGosRenderTrace("EditorGosRender::create_window: ERROR no valid parent HWND");
            fprintf(stderr, "EditorGosRender::create_window: no valid parent HWND\n");
            return NULL;
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
            wc.style         = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
            // by Methuselas: this child HWND is still render-only, but it must
            // forward mouse messages so MFC EditorInterface keeps ownership of
            // camera rotation, painting, selection, and capture behavior.
            wc.lpfnWndProc   = EditorGLChildWndProc;
            wc.hInstance     = ::GetModuleHandleW(NULL);
            wc.lpszClassName = s_glClassName;
            ATOM a = ::RegisterClassExW(&wc);
            if (a == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            {
                EditorGosRenderTrace("EditorGosRender::create_window: RegisterClassEx FAILED err=%lu",
                    ::GetLastError());
                fprintf(stderr, "EditorGosRender::create_window: RegisterClassEx failed\n");
                return NULL;
            }
            s_classRegistered = true;
            EditorGosRenderTrace("EditorGosRender::create_window: WNDCLASS registered");
        }

        // Size the GL child to fill the current client area of the parent.
        RECT parentRC = {};
        ::GetClientRect(g_editor_parent_hwnd, &parentRC);
        int childW = (parentRC.right  > 0) ? parentRC.right  : width;
        int childH = (parentRC.bottom > 0) ? parentRC.bottom : height;

        EditorGosRenderTrace("EditorGosRender::create_window: before CreateWindowEx GL child parent=%p w=%d h=%d",
            (void*)g_editor_parent_hwnd, childW, childH);

        HWND glHwnd = ::CreateWindowExW(
            0,
            s_glClassName,
            L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
            0, 0, childW, childH,
            g_editor_parent_hwnd,
            NULL,
            ::GetModuleHandleW(NULL),
            NULL);

        EditorGosRenderTrace("EditorGosRender::create_window: after CreateWindowEx GL child hwnd=%p err=%lu",
            (void*)glHwnd, ::GetLastError());

        if (!glHwnd)
        {
            fprintf(stderr, "EditorGosRender::create_window: CreateWindowEx GL child failed\n");
            return NULL;
        }

        g_editor_child_hwnd = glHwnd;

        // SDL_CreateWindowFrom wraps an existing HWND but does NOT call SDL_GL_LoadLibrary
        // (which SDL_CreateWindow with SDL_WINDOW_OPENGL would do automatically).
        // Without it, SDL's WGL function pointers (wglChoosePixelFormatARB etc.) are null,
        // so SDL_GL_CreateContext fails silently with no error string.
        // Call it explicitly before SDL_CreateWindowFrom so the WGL extension chain is
        // bootstrapped. NULL = use the default OpenGL library (opengl32.dll).
        EditorGosRenderTrace("EditorGosRender::create_window: before SDL_GL_LoadLibrary");
        if (SDL_GL_LoadLibrary(NULL) < 0)
        {
            EditorGosRenderTrace("EditorGosRender::create_window: SDL_GL_LoadLibrary failed: %s", SDL_GetError());
            // Non-fatal: SDL may already have the library loaded; proceed anyway.
        }
        else
        {
            EditorGosRenderTrace("EditorGosRender::create_window: SDL_GL_LoadLibrary OK");
        }

        // Wrap our pre-built HWND in an SDL window.  SDL sees a valid Win32 window with
        // CS_OWNDC; it will set the pixel format and create a GL context on it.
        EditorGosRenderTrace("EditorGosRender::create_window: before SDL_CreateWindowFrom hwnd=%p",
            (void*)glHwnd);

        window = SDL_CreateWindowFrom((void*)glHwnd);

        EditorGosRenderTrace("EditorGosRender::create_window: after SDL_CreateWindowFrom window=%p error=%s",
            (void*)window, SDL_GetError() ? SDL_GetError() : "none");

        if (!window)
        {
            fprintf(stderr, "SDL_CreateWindowFrom failed: %s\n", SDL_GetError());
            ::DestroyWindow(glHwnd);
            g_editor_child_hwnd = NULL;
            return NULL;
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
        // Non-Windows fallback: plain SDL_CreateWindow.
        Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
        window = SDL_CreateWindow(pwinname ? pwinname : "--",
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, windowFlags);
        if (!window) {
            fprintf(stderr, "Couldn't create window: %s\n", SDL_GetError());
            return NULL;
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

//==============================================================================
void swap_window(RenderWindowHandle h)
{
    RenderWindow* rw = (RenderWindow*)h;
    assert(rw && rw->window_);
#ifdef PLATFORM_WINDOWS
    if (g_editor_child_hwnd && ::IsWindow(g_editor_child_hwnd))
    {
        size_sdl_child_to_editor_client(g_editor_child_hwnd);
        // On the WGL path the DC is owned by the RenderContext (rc->wglDC_).
        // g_editor_child_hwnd has CS_OWNDC so GetDC returns the same persistent DC.
        HDC dc = ::GetDC(g_editor_child_hwnd);
        if (dc)
        {
            ::SwapBuffers(dc);
            ::ReleaseDC(g_editor_child_hwnd, dc);
        }
        return;
    }
#endif
    SDL_GL_SwapWindow(rw->window_);
}

//==============================================================================
RenderContextHandle init_render_context(RenderWindowHandle render_window)
{
    RenderWindow* rw = (RenderWindow*)render_window;
    assert(rw && rw->window_);

#ifdef PLATFORM_WINDOWS
    // SDL_GL_CreateContext checks for the internal SDL_WINDOW_OPENGL flag and refuses
    // to create a context on SDL_CreateWindowFrom windows (which don't have it).
    // Bypass SDL entirely: set the pixel format on our CS_OWNDC child HWND and create
    // a WGL core-profile context directly.

    if (!g_editor_child_hwnd || !::IsWindow(g_editor_child_hwnd))
    {
        EditorGosRenderTrace("init_render_context: ERROR no GL child HWND");
        return NULL;
    }

    HDC hdc = ::GetDC(g_editor_child_hwnd);
    if (!hdc)
    {
        EditorGosRenderTrace("init_render_context: GetDC failed err=%lu", ::GetLastError());
        return NULL;
    }

    // Step 1: pick and set a pixel format using the classic (non-ARB) path.
    // This must happen before any wgl call.
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;

    int fmt = ::ChoosePixelFormat(hdc, &pfd);
    EditorGosRenderTrace("init_render_context: ChoosePixelFormat fmt=%d", fmt);
    if (fmt == 0)
    {
        EditorGosRenderTrace("init_render_context: ChoosePixelFormat failed err=%lu", ::GetLastError());
        ::ReleaseDC(g_editor_child_hwnd, hdc);
        return NULL;
    }

    if (!::SetPixelFormat(hdc, fmt, &pfd))
    {
        EditorGosRenderTrace("init_render_context: SetPixelFormat failed err=%lu", ::GetLastError());
        ::ReleaseDC(g_editor_child_hwnd, hdc);
        return NULL;
    }
    EditorGosRenderTrace("init_render_context: SetPixelFormat OK fmt=%d", fmt);

    // Step 2: create a legacy GL 1.x context so we can query wglCreateContextAttribsARB.
    HGLRC legacyCtx = ::wglCreateContext(hdc);
    if (!legacyCtx)
    {
        EditorGosRenderTrace("init_render_context: wglCreateContext (legacy) failed err=%lu", ::GetLastError());
        ::ReleaseDC(g_editor_child_hwnd, hdc);
        return NULL;
    }

    if (!::wglMakeCurrent(hdc, legacyCtx))
    {
        EditorGosRenderTrace("init_render_context: wglMakeCurrent (legacy) failed err=%lu", ::GetLastError());
        ::wglDeleteContext(legacyCtx);
        ::ReleaseDC(g_editor_child_hwnd, hdc);
        return NULL;
    }

    // Step 3: load wglCreateContextAttribsARB and wglChoosePixelFormatARB through
    // the legacy context, then upgrade to a core 4.3 context.
    typedef HGLRC (WINAPI* PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int*);
    PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)::wglGetProcAddress("wglCreateContextAttribsARB");

    EditorGosRenderTrace("init_render_context: wglCreateContextAttribsARB=%p", (void*)wglCreateContextAttribsARB);

    HGLRC coreCtx = NULL;
    if (wglCreateContextAttribsARB)
    {
        const int attribs[] = {
            0x2091, 4,          // WGL_CONTEXT_MAJOR_VERSION_ARB = 4
            0x2092, 3,          // WGL_CONTEXT_MINOR_VERSION_ARB = 3
            0x9126, 0x00000001, // WGL_CONTEXT_PROFILE_MASK_ARB = WGL_CONTEXT_CORE_PROFILE_BIT_ARB
            0x2094, 0,          // WGL_CONTEXT_FLAGS_ARB = 0 (set bit 1 for debug)
            0
        };
        coreCtx = wglCreateContextAttribsARB(hdc, NULL, attribs);
        EditorGosRenderTrace("init_render_context: wglCreateContextAttribsARB core 4.3 ctx=%p err=%lu",
            (void*)coreCtx, ::GetLastError());
    }

    // Detach legacy context; we use coreCtx (or fall back to legacyCtx) from here.
    ::wglMakeCurrent(NULL, NULL);
    ::wglDeleteContext(legacyCtx);

    HGLRC finalCtx = coreCtx ? coreCtx : NULL;
    if (!finalCtx)
    {
        EditorGosRenderTrace("init_render_context: failed to create core 4.3 context");
        ::ReleaseDC(g_editor_child_hwnd, hdc);
        return NULL;
    }

    if (!::wglMakeCurrent(hdc, finalCtx))
    {
        EditorGosRenderTrace("init_render_context: wglMakeCurrent (core) failed err=%lu", ::GetLastError());
        ::wglDeleteContext(finalCtx);
        ::ReleaseDC(g_editor_child_hwnd, hdc);
        return NULL;
    }
    EditorGosRenderTrace("init_render_context: WGL core context current OK");

    // Step 4: set swap interval via wglSwapIntervalEXT.
    typedef BOOL (WINAPI* PFNWGLSWAPINTERVALEXTPROC)(int);
    PFNWGLSWAPINTERVALEXTPROC wglSwapIntervalEXT =
        (PFNWGLSWAPINTERVALEXTPROC)::wglGetProcAddress("wglSwapIntervalEXT");
    const char* vsync_env = getenv("MC2_VSYNC");
    const bool vsync_on = (vsync_env && vsync_env[0] == '1');
    if (wglSwapIntervalEXT)
        wglSwapIntervalEXT(vsync_on ? 1 : 0);
    printf("[VSYNC] MC2_VSYNC=%s -- vsync %s.\n",
           vsync_env ? vsync_env : "(unset, default 0)",
           vsync_on ? "ON" : "OFF");

    // GPU identity — requires a current context.
    printf("[GPU] Vendor   : %s\n", glGetString(GL_VENDOR));
    printf("[GPU] Renderer : %s\n", glGetString(GL_RENDERER));
    printf("[GPU] Version  : %s\n", glGetString(GL_VERSION));
    {
        GLint maxTex = 0, maxSSBO = 0, maxTexUnits = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxSSBO);
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxTexUnits);
        printf("[GL] max_texture_size=%d max_ssbo_block=%d max_combined_texture_units=%d\n",
               maxTex, maxSSBO, maxTexUnits);
    }
    {
        RECT rc2 = {};
        ::GetClientRect(g_editor_child_hwnd, &rc2);
        printf("[WINDOW] drawable=%dx%d logical=%dx%d\n",
               (int)(rc2.right - rc2.left), (int)(rc2.bottom - rc2.top),
               rw->width_, rw->height_);
    }
    const bool gl_debug_mode = (getenv("MC2_GL_DEBUG") != nullptr);
    printf("[MODE] gl_debug=%d vsync=%d tracy=on-demand\n", gl_debug_mode ? 1 : 0, vsync_on ? 1 : 0);
    printf("[TRACY] on-demand mode -- profiler listening on TCP 8086, no capture until a GUI attaches.\n");

    RenderContext* rc = new RenderContext();
    rc->glcontext_    = NULL;   // not used on Windows editor path
    rc->render_window_ = render_window;
    rc->wglContext_   = finalCtx;
    rc->wglDC_        = hdc;    // keep the DC alive; released in destroy_render_context
    return rc;

#else
    // Non-Windows: use SDL as before.
    SDL_GLContext glcontext = SDL_GL_CreateContext(rw->window_);
    if (!glcontext) {
        fprintf(stderr, "SDL_GL_CreateContext(): %s\n", SDL_GetError());
        return NULL;
    }
    if (SDL_GL_MakeCurrent(rw->window_, glcontext) < 0) {
        SDL_GL_DeleteContext(glcontext);
        return NULL;
    }
    const char* vsync_env = getenv("MC2_VSYNC");
    const bool vsync_on = (vsync_env && vsync_env[0] == '1');
    SDL_GL_SetSwapInterval(vsync_on ? 1 : 0);
    printf("[VSYNC] MC2_VSYNC=%s -- vsync %s.\n",
           vsync_env ? vsync_env : "(unset, default 0)", vsync_on ? "ON" : "OFF");
    printf("[GPU] Vendor   : %s\n", glGetString(GL_VENDOR));
    printf("[GPU] Renderer : %s\n", glGetString(GL_RENDERER));
    printf("[GPU] Version  : %s\n", glGetString(GL_VERSION));

    RenderContext* rc = new RenderContext();
    rc->glcontext_    = glcontext;
    rc->render_window_ = render_window;
    return rc;
#endif
}


//==============================================================================
void destroy_render_context(RenderContextHandle rc_handle)
{
    RenderContext* rc = (RenderContext*)rc_handle;
    assert(rc);

#ifdef PLATFORM_WINDOWS
    if (rc->wglContext_)
    {
        ::wglMakeCurrent(NULL, NULL);
        ::wglDeleteContext(rc->wglContext_);
        rc->wglContext_ = NULL;
    }
    if (rc->wglDC_ && g_editor_child_hwnd)
    {
        ::ReleaseDC(g_editor_child_hwnd, rc->wglDC_);
        rc->wglDC_ = NULL;
    }
#else
    SDL_GL_DeleteContext(rc->glcontext_);
#endif
    delete rc;
}

//==============================================================================
void make_current_context(RenderContextHandle ctx_h)
{
    RenderContext* rc = (RenderContext*)ctx_h;
    assert(rc && rc->render_window_);

    RenderWindow* rw = rc->render_window_;
    assert(rw && rw->window_);

#ifdef PLATFORM_WINDOWS
    if (rc->wglContext_ && rc->wglDC_)
    {
        ::wglMakeCurrent(rc->wglDC_, rc->wglContext_);
        return;
    }
#endif
    SDL_GL_MakeCurrent(rw->window_, rc->glcontext_);
}

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
        log_error("SDL_SetWindowFullscreen: %s\n", SDL_GetError());
        return false;
    }

    return true;
}

//==============================================================================
bool is_mode_supported(int width, int height, int bpp) {

    int displayIndex = 0;
    //displayIndex = SDL_GetWindowDisplayIndex(win_h);

    SDL_DisplayMode desired;
	desired.format = bpp==16 ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_RGB888;
	desired.w = width;
	desired.h = height;
	desired.refresh_rate = 0;
	desired.driverdata = 0;

    SDL_DisplayMode returned;
    
    if(NULL == SDL_GetClosestDisplayMode(displayIndex, &desired, &returned)) {
        log_error("resize_window: %s\n", SDL_GetError());
        return false;
    }

    //const char* df = SDL_GetPixelFormatName(desired.format);
    //const char* rf = SDL_GetPixelFormatName(returned.format);

    if(returned.w == desired.w && returned.h == desired.h && returned.format == desired.format)
        return true;

    return false;
}

//==============================================================================
int get_window_display_index(RenderContextHandle ctx_h)
{
    RenderContext* rc = (RenderContext*)ctx_h;
    assert(rc);

    RenderWindow* rw = rc->render_window_;
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
        log_error("SDL_GetDesktopDisplayMode failed: %s", SDL_GetError());
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
        log_error("SDL_GetDisplayMode failed: %s", SDL_GetError());
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
	// TOD: does it make sense to cahce this value? probably not
	SDL_GL_GetDrawableSize(rw->window_, width, height);
}

//==============================================================================
void destroy_window(RenderWindowHandle rw_handle)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    SDL_ShowCursor(SDL_ENABLE);
    SDL_DestroyWindow(rw->window_);
    delete rw;

    g_sdl_window = NULL;
#ifdef PLATFORM_WINDOWS
    // Destroy the Win32 GL child HWND we created in create_window.
    // SDL_DestroyWindow on a SDL_CreateWindowFrom window does NOT destroy the
    // underlying HWND (SDL doesn't own it), so we must do it explicitly.
    if (g_editor_child_hwnd && ::IsWindow(g_editor_child_hwnd))
    {
        ::DestroyWindow(g_editor_child_hwnd);
    }
    g_editor_child_hwnd = NULL;
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
