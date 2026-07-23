#ifndef GAMEOS_GRAPHICS_H
#define GAMEOS_GRAPHICS_H

#include <SDL2/SDL.h>

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


