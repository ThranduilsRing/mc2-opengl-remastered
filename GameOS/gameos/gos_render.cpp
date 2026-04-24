#include "gos_render.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <SDL2/SDL_syswm.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <SDL2/SDL.h>
#include <GL/glew.h>
#include <GL/gl.h>
#include "utils/logging.h"

// FIXME: think how to make it better when different parts need window
SDL_Window* g_sdl_window = NULL;
static bool g_mouse_grabbed = false;

namespace graphics {

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
   SDL_GLContext glcontext_;
   RenderWindow* render_window_;
};

static void apply_mouse_grab(SDL_Window* window, bool grabbed)
{
#ifdef PLATFORM_WINDOWS
    if (!grabbed) {
        if (!ClipCursor(NULL)) {
            log_error("ClipCursor release failed: %lu\n", (unsigned long)GetLastError());
        }
    }
#endif

    if (!window) {
        return;
    }

    SDL_SetWindowMouseGrab(window, grabbed ? SDL_TRUE : SDL_FALSE);

#ifdef PLATFORM_WINDOWS
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (SDL_GetWindowWMInfo(window, &wm_info) != SDL_TRUE) {
        log_error("SDL_GetWindowWMInfo: %s\n", SDL_GetError());
        return;
    }

    if (wm_info.subsystem != SDL_SYSWM_WINDOWS || !wm_info.info.win.window) {
        return;
    }

    if (!grabbed) {
        return;
    }

    RECT client_rect;
    if (!GetClientRect(wm_info.info.win.window, &client_rect)) {
        log_error("GetClientRect failed: %lu\n", (unsigned long)GetLastError());
        return;
    }

    POINT top_left = { client_rect.left, client_rect.top };
    POINT bottom_right = { client_rect.right, client_rect.bottom };
    if (!ClientToScreen(wm_info.info.win.window, &top_left) ||
        !ClientToScreen(wm_info.info.win.window, &bottom_right)) {
        log_error("ClientToScreen failed: %lu\n", (unsigned long)GetLastError());
        return;
    }

    RECT clip_rect = {
        top_left.x,
        top_left.y,
        bottom_right.x,
        bottom_right.y,
    };

    if (!ClipCursor(&clip_rect)) {
        log_error("ClipCursor apply failed: %lu\n", (unsigned long)GetLastError());
    }
#endif
}

void set_mouse_grab(bool grabbed)
{
    g_mouse_grabbed = grabbed;
    apply_mouse_grab(g_sdl_window, grabbed);
}

void refresh_mouse_grab()
{
    apply_mouse_grab(g_sdl_window, g_mouse_grabbed);
}

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

    // initialize using 0 videodriver
    if (SDL_VideoInit(nullptr) < 0) {
        fprintf(stderr, "Couldn't initialize video driver: %s\n", SDL_GetError());
        return NULL;
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

    {
        window = SDL_CreateWindow(pwinname ? pwinname : "--", 
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_OPENGL|SDL_WINDOW_ALLOW_HIGHDPI);

        if (!window) {
            fprintf(stderr, "Couldn't create window: %s\n", SDL_GetError());
            return NULL;
        }
        SDL_GetWindowSize(window, &width, &height);

        // NULL to use window width and height and display refresh rate
        // only need to set mode if wanted fullscreen
        if (SDL_SetWindowDisplayMode(window, NULL) < 0) {
            fprintf(stderr, "Can't set up display mode: %s\n", SDL_GetError());
            SDL_DestroyWindow(window);
            return NULL;
        }

        SDL_ShowWindow(window);

        // Hide the OS cursor. MC2 renders its own in-game cursor sprite, so
        // the default arrow would otherwise double up on top of it.
        SDL_ShowCursor(SDL_DISABLE);
    }

    RenderWindow* rw = new RenderWindow();
    rw->window_ = window;
    rw->width_ = width;
    rw->height_ = height;

    g_sdl_window = window;
    set_mouse_grab(true);

    return rw;
}

//==============================================================================
void swap_window(RenderWindowHandle h)
{
    RenderWindow* rw = (RenderWindow*)h;
    assert(rw && rw->window_);
    SDL_GL_SwapWindow(rw->window_);
}

//==============================================================================
RenderContextHandle init_render_context(RenderWindowHandle render_window)
{
    RenderWindow* rw = (RenderWindow*)render_window;
    assert(rw && rw->window_);

    SDL_GLContext glcontext = SDL_GL_CreateContext(rw->window_);
    if (!glcontext ) {
        fprintf(stderr, "SDL_GL_CreateContext(): %s\n", SDL_GetError());
        return NULL;
    }

    if (SDL_GL_MakeCurrent(rw->window_, glcontext) < 0) {
        SDL_GL_DeleteContext(glcontext);
        return NULL;
    } 

    // MC2_VSYNC: "1" forces vsync on, "0" or unset leaves it off.
    // Off by default so a GPU that misses 60 Hz is not rounded down
    // to 30/20/15 FPS.
    const char* vsync_env = getenv("MC2_VSYNC");
    const bool vsync_on = (vsync_env && vsync_env[0] == '1');
    SDL_GL_SetSwapInterval(vsync_on ? 1 : 0);
    printf("[VSYNC] MC2_VSYNC=%s -- vsync %s.\n",
           vsync_env ? vsync_env : "(unset, default 0)",
           vsync_on ? "ON" : "OFF");

    // Print GPU identity unconditionally with a distinctive prefix so it is
    // impossible to miss when a user pastes their console log for triage.
    // On hybrid-graphics laptops this line is the single most valuable
    // diagnostic: it says which GPU OpenGL actually selected.
    printf("[GPU] Vendor   : %s\n", glGetString(GL_VENDOR));
    printf("[GPU] Renderer : %s\n", glGetString(GL_RENDERER));
    printf("[GPU] Version  : %s\n", glGetString(GL_VERSION));

    // GL capability limits -- useful to rule out SSBO / texture-size / unit
    // ceilings when a user reports rendering issues on unusual hardware.
    {
        GLint maxTex = 0, maxSSBO = 0, maxTexUnits = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
        glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxSSBO);
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxTexUnits);
        printf("[GL] max_texture_size=%d max_ssbo_block=%d max_combined_texture_units=%d\n",
               maxTex, maxSSBO, maxTexUnits);
    }

    // Drawable (physical, post-HiDPI) vs logical (window coords) size.
    // Divergence indicates the backbuffer is larger than the window, which
    // multiplies fragment cost.
    {
        int draw_w = 0, draw_h = 0, logical_w = 0, logical_h = 0;
        SDL_GL_GetDrawableSize(rw->window_, &draw_w, &draw_h);
        SDL_GetWindowSize(rw->window_, &logical_w, &logical_h);
        printf("[WINDOW] drawable=%dx%d logical=%dx%d%s\n",
               draw_w, draw_h, logical_w, logical_h,
               (draw_w != logical_w || draw_h != logical_h) ? " (HiDPI)" : "");
    }

    // Single-line summary of effective runtime mode. Anchor for log pastes:
    // grep [MODE] and you see every toggle state at a glance.
    // NB: gl_debug is set on the attribute in create_window()'s scope; we
    // re-read the env here because that local doesn't carry across functions.
    const bool gl_debug_mode = (getenv("MC2_GL_DEBUG") != nullptr);
    printf("[MODE] gl_debug=%d vsync=%d tracy=on-demand\n",
           gl_debug_mode ? 1 : 0,
           vsync_on ? 1 : 0);
    printf("[TRACY] on-demand mode -- profiler listening on TCP 8086, no capture until a GUI attaches.\n");

    if(VERBOSE_RENDER) {
        SDL_DisplayMode mode;
        SDL_GetCurrentDisplayMode(0, &mode);
        printf("Current Display Mode:\n");
        printf("Screen BPP: %d\n", SDL_BITSPERPIXEL(mode.format));
        printf("\n");
        printf("Vendor     : %s\n", glGetString(GL_VENDOR));
        printf("Renderer   : %s\n", glGetString(GL_RENDERER));
        printf("Version    : %s\n", glGetString(GL_VERSION));
        const GLubyte* exts = glGetString(GL_EXTENSIONS);
        printf("Extensions : %s\n", exts);
        printf("\n");

        int value;
        int status = 0;

        /*
           status = SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &value);
           if (!status) {
           printf("SDL_GL_RED_SIZE: requested %d, got %d\n", 5, value);
           } else {
           printf("Failed to get SDL_GL_RED_SIZE: %s\n", SDL_GetError());
           }
           status = SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &value);
           if (!status) {
           printf("SDL_GL_GREEN_SIZE: requested %d, got %d\n", 5, value);
           } else {
           printf("Failed to get SDL_GL_GREEN_SIZE: %s\n", SDL_GetError());
           }
           status = SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &value);
           if (!status) {
           printf("SDL_GL_BLUE_SIZE: requested %d, got %d\n", 5, value);
           } else {
           printf("Failed to get SDL_GL_BLUE_SIZE: %s\n", SDL_GetError());
           }
           */
        //status = SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &value);
        //if (!status) {
        //    printf("SDL_GL_DEPTH_SIZE: requested %d, got %d\n", 16, value);
        //} else {
        //    printf("Failed to get SDL_GL_DEPTH_SIZE: %s\n", SDL_GetError());
        //}

		/*
        status = SDL_GL_GetAttribute(SDL_GL_MULTISAMPLEBUFFERS, &value);
        if (!status) {
            printf("SDL_GL_MULTISAMPLEBUFFERS: %d\n", value);
        } else {
            printf("Failed to get SDL_GL_MULTISAMPLEBUFFERS: %s\n",
                    SDL_GetError());
        }

        status = SDL_GL_GetAttribute(SDL_GL_MULTISAMPLESAMPLES, &value);
        if (!status) {
            printf("SDL_GL_MULTISAMPLESAMPLES: %d\n", value);
        } else {
            printf("Failed to get SDL_GL_MULTISAMPLESAMPLES: %s\n",
                    SDL_GetError());
        }
		*/
        status = SDL_GL_GetAttribute(SDL_GL_ACCELERATED_VISUAL, &value);
        if (!status) {
            printf("SDL_GL_ACCELERATED_VISUAL: %d\n", value);
        } else {
            printf("Failed to get SDL_GL_ACCELERATED_VISUAL: %s\n",
                    SDL_GetError());
        }
		
        status = SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &value);
        if (!status) {
            printf("SDL_GL_CONTEXT_MAJOR_VERSION: %d\n", value);
        } else {
            printf("Failed to get SDL_GL_CONTEXT_MAJOR_VERSION: %s\n", SDL_GetError());
        }

        status = SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &value);
        if (!status) {
            printf("SDL_GL_CONTEXT_MINOR_VERSION: %d\n", value);
        } else {
            printf("Failed to get SDL_GL_CONTEXT_MINOR_VERSION: %s\n", SDL_GetError());
        }
    }

    RenderContext* rc = new RenderContext();
    rc->glcontext_ = glcontext;
    rc->render_window_ = render_window;

	return rc;
}

//==============================================================================
void destroy_render_context(RenderContextHandle rc_handle)
{
    RenderContext* rc = (RenderContext*)rc_handle;
    assert(rc);

    SDL_GL_DeleteContext(rc->glcontext_);
    delete rc;
}

//==============================================================================
void make_current_context(RenderContextHandle ctx_h)
{
    RenderContext* rc = (RenderContext*)ctx_h;
    assert(rc && rc->render_window_ && rc->glcontext_);

    RenderWindow* rw = rc->render_window_;
    assert(rw && rw->window_);

    SDL_GL_MakeCurrent(rw->window_, rc->glcontext_);
}

//==============================================================================
bool resize_window(RenderWindowHandle rw_handle, int width, int height)
{
    RenderWindow* rw = (RenderWindow*)rw_handle;
    assert(rw);

    SDL_SetWindowSize(rw->window_, width, height);
    rw->width_ = width;
    rw->height_ = height;
    if (rw->window_ == g_sdl_window) {
        refresh_mouse_grab();
    }

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

    if (rw->window_ == g_sdl_window) {
        refresh_mouse_grab();
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
    if (rw->window_ == g_sdl_window) {
        set_mouse_grab(false);
    }
    SDL_ShowCursor(SDL_ENABLE);
    SDL_DestroyWindow(rw->window_);
    delete rw;

    g_sdl_window = NULL;
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
