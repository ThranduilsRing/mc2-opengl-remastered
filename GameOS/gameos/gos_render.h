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

// True when cursor control is suppressed OR the window is parked offscreen.
// META4-CLOSE-1 NARROWED THE SCOPE OF THIS ONE. It is now only for things that
// are correct for an interactive MC2_NO_CURSOR_GRAB=1 launch as well -- in
// practice just SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, which is documented
// behaviour of that flag ("does not steal keyboard focus on show").
//
// For anything a PRESENT USER would miss if it were taken away -- his taskbar
// button, his Alt-Tab entry, his crash dialog -- use
// gos_NoUserPresentRequested() below instead. This predicate is true for a
// documented interactive option and must not be used to disable UI.
bool gos_UnattendedRunRequested();

// True only when there is no window a user could be looking at: MC2_WINDOW_
// OFFSCREEN (parked on no monitor) or MC2_SMOKE_MODE (a harness run). NOT
// true for a bare MC2_NO_CURSOR_GRAB=1, which is a documented user-facing
// interactive option -- see the long rationale in gameos_input.cpp.
//
// This is the guard for suppressing user-visible UI and for window styles that
// remove the window from the user's reach (WS_EX_NOACTIVATE, WS_EX_TOOLWINDOW).
bool gos_NoUserPresentRequested();

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

using RenderWindowHandle = RenderWindow*;
using RenderContextHandle = RenderContext*;

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
// The process is about to exit: makes the context non-current and drops the wrapper but does not delete the GL context. Deleting it blocks inside the AMD
// driver (wglDeleteContext waits 1 to 5 s in whole seconds on a kernel object; BUGS 87) and the OS frees the context with the process anyway.
void                release_render_context_at_exit(RenderContextHandle rc_handle);

SDL_Window*         getSDLWindow            () noexcept;
SDL_GLContext       getSDLGLContext          () noexcept;

};

#endif // GAMEOS_GRAPHICS_H


