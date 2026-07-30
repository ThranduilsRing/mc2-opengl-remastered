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


