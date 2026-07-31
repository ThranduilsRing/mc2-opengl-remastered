#ifndef GAMEOS_GRAPHICS_H
#define GAMEOS_GRAPHICS_H

#include <SDL2/SDL.h>

// SMOKE-NO-CURSOR-STEAL-1: true when this process must NOT touch the OS cursor
// (no SDL_WarpMouseInWindow, no window mouse-grab, no Win32 ClipCursor).
// Automated runs (MC2_SMOKE_MODE=1) default to suppressed so agent-driven
// smoke / visual_verify / capture launches never yank the user's pointer out of
// whatever window they are working in. MC2_NO_CURSOR_GRAB is the explicit
// override in BOTH directions: =1 forces suppression on (even interactive),
// =0 forces it off (even under smoke). Defined in gameos_input.cpp so the MFC
// editor shell (which replaces gos_render.cpp) links too.
bool gos_CursorControlSuppressed();

// CURSOR-GRAB-FIX-1 -----------------------------------------------------------
// THE RULE: a window that is not visible to the user must never capture the
// user's input. No grab, no clip, no warp, no foreground activation -- and it
// must STAY that way across focus events, not just at startup.
//
// gos_CursorControlSuppressed() above expresses only ONE of the two intents that
// mean "unattended", so keying cursor policy on it alone shipped a real bug:
// MC2_WINDOW_OFFSCREEN=1 without MC2_NO_CURSOR_GRAB parked the window at
// (-32000,-32000), SHOWED it, and then clipped the user's physical pointer to a
// rectangle on no monitor. The functions below are the single answer to "may
// this process take the pointer?", and they are keyed on BOTH intent and ACTUAL
// visibility -- intent cannot see a window that became invisible some other way.
// All are defined in gameos_input.cpp so the MFC editor shell (which replaces
// gos_render.cpp wholesale) links too.

// True when MC2_WINDOW_OFFSCREEN is set. The one parse of that variable in the
// engine; graphics::window_offscreen_requested() delegates here.
bool gos_WindowOffscreenRequested();

// True when this process was started to run WITHOUT a user watching it: either
// cursor control is suppressed or the window is parked offscreen. Use for
// anything that would take the user's attention (modal dialogs, foreground
// activation), not just the pointer.
bool gos_UnattendedRunRequested();

// True only when `window` actually occupies pixels a user could be looking at:
// non-null, not HIDDEN, not MINIMIZED, non-degenerate, and intersecting at
// least one display's bounds. Evaluated live -- never cached -- because
// visibility changes at runtime and a startup-only answer is what let the
// FOCUS_GAINED re-arm defeat the previous fix.
bool gos_WindowUserVisible(SDL_Window* window);

// THE chokepoint predicate. True when this process must not grab, clip, confine
// or warp the OS pointer. `reason` (optional) receives a static string naming
// which condition fired, for one-shot diagnostics.
bool gos_CursorCaptureForbidden(SDL_Window* window, const char** reason);

namespace graphics {

struct RenderWindow;
struct RenderContext;

typedef RenderWindow*   RenderWindowHandle;
typedef RenderContext*   RenderContextHandle;

void set_verbose(bool is_verbose);

// ISSUE-49: windowed_hint = the engine's windowed boot state
// (Environment.fullScreen == 0, i.e. options.cfg FullScreen=FALSE or
// MC2_WINDOWED). Makes the window resizable for options.cfg-windowed boots
// too, not only when the MC2_WINDOWED env var is set. Editor ignores it.
RenderWindowHandle  create_window           (const char* pwinname, int width, int height, bool windowed_hint);
bool                resize_window           (RenderWindowHandle rw_handle, int width, int height);
void                get_window_size         (RenderWindowHandle rw_handle, int* width, int* height);
// may be different than window size (e.g. when switching to fullscreen)
void                get_drawable_size       (RenderWindowHandle rw_handle, int* width, int* height);
void                set_mouse_grab          (bool grabbed);
void                refresh_mouse_grab      ();
void                swap_window             (RenderWindowHandle h);
void                destroy_window          (RenderWindowHandle rw_handle);
bool                set_window_fullscreen   (RenderWindowHandle rw_handle, bool fullscreen);
// NOFOCUS-HARDEN-1: true when MC2_WINDOW_OFFSCREEN is set -- the window is
// created off the visible desktop and must be kept there. See gos_render.cpp.
bool                window_offscreen_requested();
// Re-assert the offscreen origin after any external window-mode change. No-op
// unless MC2_WINDOW_OFFSCREEN is set. Synchronous by design: post-hoc POLLING
// is what let a desktop-sized window flash on screen for seconds.
void                enforce_window_offscreen(RenderWindowHandle rw_handle);
bool                is_mode_supported       (int width, int height, int bpp);
int                 get_window_display_index(RenderContextHandle ctx_h);
bool                get_desktop_display_mode(int display_index, int* width, int* height, int* bpp);
int                 get_num_display_modes   (int display_index);
bool                get_display_mode_by_index(int display_index, int mode_index, int* width, int* height, int* bpp);

RenderContextHandle init_render_context     (RenderWindowHandle render_window);
void                make_current_context    (RenderContextHandle ctx_h);
void                destroy_render_context  (RenderContextHandle rc_handle);

SDL_Window*         getSDLWindow            () noexcept;
SDL_GLContext       getSDLGLContext          () noexcept;

};

#endif // GAMEOS_GRAPHICS_H


