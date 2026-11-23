#include "gos_render.h"
#include "loading_pump_meter.h"
#include "gos_smoke.h"
#include "mc2fmt.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>
#include <SDL2/SDL_syswm.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <SDL2/SDL.h>
#include "utils/mc2log.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"   // raw<>()
#include "mc2fmt.h"
#include "diagnostic_trace.h"
#include "present_thread_wait.h"
#include "engine_present_vk.h"
#include "present_thread.h"
#include <atomic>
#include <chrono>
#include <thread>

// FIXME: think how to make it better when different parts need window
SDL_Window* g_sdl_window = nullptr;
static bool g_mouse_grabbed = false;
static SDL_threadID g_window_thread = 0;

// GPU-VENDOR-DETECT-1: -1 = not yet probed, 0 = not NVIDIA, 1 = NVIDIA.
// Set once in init_render_context after the GL context is live. Read via
// gos_IsNvidiaGPU() from mclib (bdactor.cpp) to pick vendor-aware defaults.


namespace graphics {

static bool VERBOSE_VIDEO = true;   // unchanged (keep current behavior)
static bool VERBOSE_RENDER = false; // mode/driver/extensions dump silenced; GPU identity prints unconditionally
static bool VERBOSE_MODES = false;  // display-mode enumeration silenced

struct RenderWindow {
    SDL_Window* window_;
    int width_;
    int height_;
};


#ifdef PLATFORM_WINDOWS
// True when the active clip rectangle is a real clip (smaller than the virtual
// desktop) and overlaps this window -- i.e. a clip this process, or an earlier
// unguarded build of it, left on the pointer.
static bool ourWindowHoldsClip(SDL_Window* window)
{
    if (!window) {
        return false;
    }
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (SDL_GetWindowWMInfo(window, &wm_info) != SDL_TRUE ||
        wm_info.subsystem != SDL_SYSWM_WINDOWS || !wm_info.info.win.window) {
        return false;
    }
    RECT clip, win, overlap;
    if (!GetClipCursor(&clip) || !GetWindowRect(wm_info.info.win.window, &win)) {
        return false;
    }
    const long desktopW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const long desktopH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if ((clip.right - clip.left) >= desktopW && (clip.bottom - clip.top) >= desktopH) {
        return false;   // no clip is active
    }
    return IntersectRect(&overlap, &clip, &win) != 0;
}
#endif

static void apply_mouse_grab(SDL_Window* window, bool grabbed)
{
    // CURSOR-GRAB-FIX-1 -- THE CHOKEPOINT.
    //
    // Every path that can arm the grab funnels through here:
    //   create_window()            -> set_mouse_grab(true)      [startup]
    //   SDL_WINDOWEVENT_FOCUS_GAINED -> set_mouse_grab(true)    [gameosmain]
    //   SDL_WINDOWEVENT_MOVED / RESIZED / SIZE_CHANGED -> refresh_mouse_grab()
    //   resize_window() / set_window_fullscreen() -> refresh_mouse_grab()
    // so guarding HERE closes all of them at once and, critically, keeps them
    // closed: the guard is re-evaluated on every single call, which is what the
    // FOCUS_GAINED re-arm defeats in any startup-only fix. The state variable
    // g_mouse_grabbed records the engine's INTENT, never the decision -- so if a
    // window legitimately becomes visible again, the next refresh re-applies.
    //
    // SMOKE-NO-CURSOR-STEAL-1 gated only on gos_CursorControlSuppressed(), which
    // an MC2_WINDOW_OFFSCREEN run does not set. The result was a shown window at
    // (-32000,-32000) with the user's physical cursor clipped to it. Forcing the
    // RELEASED state (rather than returning early) is deliberate: it also undoes
    // a clip that a previous, unguarded build left behind.
    const char* forbidReason = nullptr;
    if (grabbed && gos_CursorCaptureForbidden(window, &forbidReason)) {
        static const char* s_lastReason = nullptr;
        if (forbidReason != s_lastReason) {
            s_lastReason = forbidReason;
            mc2::fmt::print_out("[CURSOR_GUARD] mouse grab REFUSED ({}) -- the window is not "
                   "visible to the user, so it may not capture the pointer.\n",
                   forbidReason ? forbidReason : "?");
            fflush(stdout);
        }
        grabbed = false;
    }

#ifdef PLATFORM_WINDOWS
    // Release only a clip that lies on OUR window: ClipCursor(NULL) would also drop
    // a clip another application (a game the user is playing) legitimately holds.
    if (!grabbed && ourWindowHoldsClip(window)) {
        if (!ClipCursor(nullptr)) {
            MC2_LOG(Core, Error, "ClipCursor release failed: {:d}", mc2::fmt::as_u32(GetLastError()));
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
        MC2_LOG(Core, Error, "SDL_GetWindowWMInfo: {}", mc2::fmt::safe(SDL_GetError()));
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
        MC2_LOG(Core, Error, "GetClientRect failed: {:d}", mc2::fmt::as_u32(GetLastError()));
        return;
    }

    POINT top_left = { client_rect.left, client_rect.top };
    POINT bottom_right = { client_rect.right, client_rect.bottom };
    if (!ClientToScreen(wm_info.info.win.window, &top_left) ||
        !ClientToScreen(wm_info.info.win.window, &bottom_right)) {
        MC2_LOG(Core, Error, "ClientToScreen failed: {:d}", mc2::fmt::as_u32(GetLastError()));
        return;
    }

    RECT clip_rect = {
        top_left.x,
        top_left.y,
        bottom_right.x,
        bottom_right.y,
    };

    // CURSOR-GRAB-FIX-1 FAILSAFE -- deliberately independent of every gate
    // above. No env var, no SDL flag, no caller intent participates: if the
    // rectangle we are about to confine the user's PHYSICAL pointer to sits on
    // no monitor, the user could not move the mouse back to his own desktop.
    // That outcome is never acceptable, whatever code path asked for it, so this
    // refuses on geometry alone. It is the backstop for a future path that
    // reaches here without consulting gos_CursorCaptureForbidden().
    if (MonitorFromRect(&clip_rect, MONITOR_DEFAULTTONULL) == nullptr) {
        MC2_LOG(Core, Error, "[CURSOR_GUARD] refusing ClipCursor to a rect on no monitor ({:d},{:d})-({:d},{:d}) -- releasing instead.",
                  mc2::fmt::as_i32(clip_rect.left), mc2::fmt::as_i32(clip_rect.top),
                  mc2::fmt::as_i32(clip_rect.right), mc2::fmt::as_i32(clip_rect.bottom));
        ClipCursor(nullptr);
        return;
    }

    if (!ClipCursor(&clip_rect)) {
        MC2_LOG(Core, Error, "ClipCursor apply failed: {:d}", mc2::fmt::as_u32(GetLastError()));
    }
#endif
}

void set_mouse_grab(bool grabbed)
{
    g_mouse_grabbed = grabbed;
    apply_mouse_grab(g_sdl_window, grabbed);
}

static SDL_Cursor* s_nativeCursor = nullptr;
static bool s_cursorVisible = false;

// One owner for shape and visibility, including focus regain and software fallback.
void apply_os_cursor_policy(std::string_view why, bool focused)
{
    if (gos_UnattendedRunRequested() || !g_sdl_window) {
        return;
    }
    const bool forceArrow = mc2::gate::get<"MC2_OS_CURSOR">();
    SDL_SetCursor(focused && s_nativeCursor && !forceArrow ? s_nativeCursor : SDL_GetDefaultCursor());
    const bool hide = focused && !forceArrow && (!s_nativeCursor || !s_cursorVisible);
    SDL_ShowCursor(hide ? SDL_DISABLE : SDL_ENABLE);
    if (!mc2_diag::tagRequested("CURSOR")) return;
    MC2_LOG(Ui, Info, "[CURSOR] os_cursor event={} focused={:d} visible={:d} os_cursor_gate={:d}",
        mc2::fmt::safe(std::string(why).c_str()), mc2::fmt::as_i32(focused ? 1 : 0),
        mc2::fmt::as_i32(SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE ? 1 : 0),
        mc2::fmt::as_i32(mc2::gate::get<"MC2_OS_CURSOR">() ? 1 : 0));
}

void set_native_cursor(SDL_Cursor* cursor, bool visible)
{
    if (s_nativeCursor == cursor && s_cursorVisible == visible) return;
    s_nativeCursor = cursor;
    s_cursorVisible = visible;
    apply_os_cursor_policy("shape", g_sdl_window && (SDL_GetWindowFlags(g_sdl_window) & SDL_WINDOW_INPUT_FOCUS));
}

static LoadingPumpStats s_loadPumps;

void loading_work(const std::function<void()>& work)
{
    if (SDL_ThreadID() != g_window_thread || s_loadPumps.phase.empty()) {
        work();
        return;
    }
    mc2vk::present_thread::detail::workPumping(work, [] {
        mc2vk::present_thread::pumpWindowMessages();
    });
}

std::string_view loading_phase() { return s_loadPumps.phase; }

void begin_loading_events()
{
    s_loadPumps.begin(SDL_GetTicks64());
}

void loading_events_phase(std::string_view phase) {
    if (!s_loadPumps.phase.empty()) s_loadPumps.phase = phase;
}

void end_loading_events()
{
    pump_loading_events();
    mc2::fmt::print_out("[LOAD_PUMP] max_gap_ms={} phase={}\n", s_loadPumps.maxGap,
        s_loadPumps.maxPhase.empty() ? "no_window" : s_loadPumps.maxPhase);
    s_loadPumps.phase = {};
}

void pump_loading_events()
{
    auto& meter = mc2::loadpump::meter;
    if (SmokeMode::state().headless) {
        meter.service(SDL_GetTicks64(), true);
        return;
    }
    if (!g_sdl_window || SDL_ThreadID() != g_window_thread) return;
    if (!meter.due(SDL_GetTicks64())) return;
    const Uint64 stamp = SDL_GetTicks64();
    if (!s_loadPumps.phase.empty() && stamp - s_loadPumps.lastPump >= 250)
        mc2::fmt::print_out("[LOAD_PUMP] gap_ms={} phase={}\n",
            stamp - s_loadPumps.lastPump, s_loadPumps.phase);
    s_loadPumps.pumped(stamp);
    // Pump only: do not consume quit/focus/input events or reenter the game mid-load.
    SDL_PumpEvents();
    note_window_pumped();
    meter.service(SDL_GetTicks64(), false);
}

// unfocused-not-responding breadcrumb: a window thread that pumps nothing for 4 s is about to be ghosted ("not responding"), and a
// ghost the user or Windows closes leaves no dump and no EXIT_REASON. A watchdog writes MAIN_STALL begin/end to the diagnostic trace.
static std::atomic<Uint64> s_lastWindowPumpMs{0};
void note_window_pumped() { s_lastWindowPumpMs.store(SDL_GetTicks64(), std::memory_order_relaxed); }
static void start_main_stall_watchdog()
{
    std::thread([] {
        mc2::loadpump::StallWatch watch;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const Uint64 last = s_lastWindowPumpMs.load(std::memory_order_relaxed);
            const int edge = watch.step(last ? SDL_GetTicks64() - last : 0);
            if (edge == 0 || !mc2_diag::enabled()) continue;
            char data[96];
            mc2::fmt::to_buf(data, "{{\"event\":\"{}\",\"gap_ms\":{}}}", edge > 0 ? "begin" : "end", watch.gap);
            mc2_diag::writeEvent("MAIN_STALL", 1, 0, data);
            mc2_diag::flush();
        }
    }).detach();
}

void begin_variant_load()
{
    mc2::loadpump::meter.variants.begin(SDL_GetTicks64(), "initVariants.catalog", SmokeMode::state().headless);
    pump_loading_events();
}

void end_variant_load()
{
    auto& scope = mc2::loadpump::meter.variants;
    scope.end(SDL_GetTicks64());
    if (mc2_diag::tagRequested("LOAD_PUMP"))
        MC2_LOG(Io, Info, "[LOAD_PUMP v1] max_gap_ms={} phase={} scope=initVariants mode={} services={}",
            scope.maxGap, scope.worst, scope.attempted ? "attempted-pump" : "actual-service", scope.services);
}

void refresh_mouse_grab()
{
    apply_mouse_grab(g_sdl_window, g_mouse_grabbed);
}

static void PrintRenderer(SDL_RendererInfo * info);

//==============================================================================
// NOFOCUS-HARDEN-1 -- correct-at-creation offscreen geometry.
//
// WHY: an agent-launched engine window must never occupy visible desktop area
// at ANY point in its lifetime. The previous design created the window at
// options.cfg size in the middle of the desktop and relied on an EXTERNAL
// python poller (smoke_lib/nofocus.park_window_offscreen, 250ms tick) to move
// it away afterwards. That guarantees a multi-second desktop-covering flash on
// every single launch -- measured live at 1719x995 on a 1707x960 desktop --
// which is precisely the user-visible complaint. Post-hoc repositioning is the
// wrong shape: the only way for the window to never be seen is for it to never
// be created anywhere visible.
//
// MC2_WINDOW_OFFSCREEN=1 makes the position right at CreateWindow time:
//   * the SDL window is created HIDDEN at (-32000,-32000) -- so even the
//     CreateWindowEx call never places an HWND on a visible pixel;
//   * the position is re-asserted immediately before the single SDL_ShowWindow;
//   * fullscreen is refused outright (a fullscreen window has no position to
//     move and would cover the display regardless);
//   * enforce_window_offscreen() re-asserts it SYNCHRONOUSLY after every later
//     mode change (resize_window / set_window_fullscreen -- which is how
//     prefs.applyPrefs()'s SetScreenMode re-issue used to bounce the window
//     back to centre mid-boot).
//
// -32000 is off every physical monitor on a normal multi-mon layout and is
// what Windows itself uses for minimized-window placeholders. The GL drawable
// keeps its full size, so glReadPixels/FBO readback captures (the golden
// visual-capture path) are completely unaffected -- unlike minimizing, which
// makes SDL throttle and can zero the drawable.
static const int kOffscreenWindowX = -32000;
static const int kOffscreenWindowY = -32000;

bool window_offscreen_requested()
{
    // CURSOR-GRAB-FIX-1: delegates to the one parse of MC2_WINDOW_OFFSCREEN
    // (gameos_input.cpp), which the cursor-capture policy also reads. Two
    // independent parses of the same variable is how "offscreen" and "may not
    // take the cursor" drifted apart in the first place -- the window went off
    // the desktop and the pointer went with it.
    return gos_WindowOffscreenRequested();
}

static void apply_offscreen_position(SDL_Window* w)
{
    if (!w || !window_offscreen_requested())
        return;
    SDL_SetWindowPosition(w, kOffscreenWindowX, kOffscreenWindowY);
}

// CURSOR-GRAB-FIX-1 (adversarial pass): the two ways an unattended window could
// still take input AFTER the grab chokepoint was closed.
//
//   1. WS_EX_NOACTIVATE. SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN governs only
//      the ShowWindow at creation. It does nothing about LATER activation --
//      the offscreen window still carried a taskbar button and an Alt-Tab
//      entry, so one Alt-Tab and the user's KEYSTROKES went into the engine.
//      WS_EX_NOACTIVATE makes the window refuse activation for its whole life
//      and, per MSDN, keeps it off the taskbar. The window still receives and
//      paints messages, which is all an offscreen render source needs.
//
//   2. SDL_HINT_MOUSE_AUTO_CAPTURE (default ON) makes SDL call Win32
//      SetCapture() while a mouse button is held over the window -- a capture
//      path that never consults our grab state. Unreachable in practice for a
//      window at (-32000,-32000) because the user cannot click on it, but
//      "unreachable in practice" is exactly the reasoning that shipped the bug
//      being fixed here, so it is turned off explicitly instead.
//
// Applied ONLY when the run already declared itself unattended, so a normal
// interactive launch is byte-identical to before.
//
// META4-CLOSE-1: gated on gos_NoUserPresentRequested(), NOT
// gos_UnattendedRunRequested(). The union included a bare MC2_NO_CURSOR_GRAB=1,
// which is a documented user-facing INTERACTIVE option, so a player who set it
// to play without a cursor clamp got a window with no taskbar button, no
// Alt-Tab entry and no way to give it keyboard focus. Both styles here are
// about putting the window out of the user's REACH, which is only ever right
// when there is no user watching it.
static void apply_unattended_window_styles(SDL_Window* window)
{
    if (!window || !gos_NoUserPresentRequested())
        return;

    SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");

#ifdef PLATFORM_WINDOWS
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    if (SDL_GetWindowWMInfo(window, &wm_info) != SDL_TRUE)
        return;
    if (wm_info.subsystem != SDL_SYSWM_WINDOWS || !wm_info.info.win.window)
        return;

    HWND hwnd = wm_info.info.win.window;
    LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE,
                     ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    mc2::fmt::print_out("[CURSOR_GUARD] unattended run -- window marked WS_EX_NOACTIVATE "
           "(cannot be activated, no taskbar/Alt-Tab entry) and SDL mouse "
           "auto-capture disabled.\n");
    fflush(stdout);
#endif
}

void enforce_window_offscreen(RenderWindowHandle rw_handle)
{
    if (!window_offscreen_requested())
        return;
    RenderWindow* rw = static_cast<RenderWindow*>(rw_handle);
    if (rw)
        apply_offscreen_position(rw->window_);
}

//==============================================================================
void window_checkpoint(std::string_view why, bool restore)
{
    if (!g_sdl_window) return;
#ifdef PLATFORM_WINDOWS
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(g_sdl_window, &wm)) return;
    const HWND hwnd = wm.info.win.window;
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfo(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    // Repair only fullscreen, preserving Z order and the owner's active app.
    if (restore && !window_offscreen_requested() &&
        (SDL_GetWindowFlags(g_sdl_window) & SDL_WINDOW_FULLSCREEN)) {
        const RECT& r = monitor.rcMonitor;
        SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    if (!mc2::gate::get<"MC2_RES_DIAG">()) return;
    RECT window{}, client{};
    GetWindowRect(hwnd, &window);
    GetClientRect(hwnd, &client);
    int dw = 0, dh = 0;
    SDL_GetWindowSizeInPixels(g_sdl_window, &dw, &dh);
    mc2::fmt::print_out("[RES_DIAG] checkpoint={} flags={} window={},{},{},{} client={},{},{},{} monitor={},{},{},{} drawable={}x{}\n",
        why, SDL_GetWindowFlags(g_sdl_window), window.left, window.top, window.right, window.bottom,
        client.left, client.top, client.right, client.bottom,
        monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right, monitor.rcMonitor.bottom, dw, dh);
#ifdef MC2_VULKAN
    // Extents are stable after the worker seals, until main queues the next frame.
    mc2vk::present_thread::waitSealed();
    const auto& health = mc2vk::getOrCreateEnginePresentVk().health();
    mc2::fmt::print_out("[RES_DIAG] checkpoint={} swapchain={}x{}\n", why, health.extent_w, health.extent_h);
#endif
#endif
}

void set_verbose(bool is_verbose)
{
    VERBOSE_VIDEO = is_verbose;
    VERBOSE_RENDER = is_verbose;
    VERBOSE_MODES = is_verbose;
}

//==============================================================================
RenderWindow* create_window(const char* pwinname, int width, int height, bool windowed_hint)
{
	int i, j, m, n;
	SDL_DisplayMode fullscreen_mode;
    SDL_Window* window = nullptr;

    // DPI-AWARE-1 (field-caught 2026-07-18): per-monitor-v2 DPI awareness is
    // declared as the FIRST statement of main() (gameosmain.cpp) — it must
    // precede the VK swapchain probe's hidden SDL window or the per-process
    // DPI context is already locked. Field evidence: on a 4K/225% desktop the
    // DPI-unaware process got its options.cfg 3840x2160 window at LOGICAL size
    // = 2x the desktop (windowRect measured -968,-571..2888,1628), so the user
    // saw a center crop ("zoomed in" menus, top rows cut) while the backbuffer
    // rendered correctly. MC2_DPI_UNAWARE=1 restores the legacy behavior.
    // Alt-tab must retain the desktop-sized surface without staying above other apps.
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_ALLOW_TOPMOST, "0");


    if (VERBOSE_VIDEO) {
        n = SDL_GetNumVideoDrivers();
        if (n == 0) {
            mc2::fmt::print_err("No built-in video drivers\n");
        } else {
            mc2::fmt::print_err("Built-in video drivers:");
            for (i = 0; i < n; ++i) {
                if (i > 0) {
                    mc2::fmt::print_err(",");
                }
                mc2::fmt::print_err(" {}", mc2::fmt::safe(SDL_GetVideoDriver(i)));
            }
            mc2::fmt::print_err("\n");
        }
    }

    // initialize using 0 videodriver
    if (SDL_VideoInit(nullptr) < 0) {
        mc2::fmt::print_err("Couldn't initialize video driver: {}\n", mc2::fmt::safe(SDL_GetError()));
        return nullptr;
    }
    if (VERBOSE_VIDEO) {
        mc2::fmt::print_err("Video driver: {}\n", mc2::fmt::safe(SDL_GetCurrentVideoDriver()));
    }

    //not really related to video, but let it be here
    if (VERBOSE_VIDEO) {
        mc2::fmt::print_out("SDL revision: {}\n", mc2::fmt::safe(SDL_GetRevision()));

        SDL_version compiled;
        SDL_version linked;

        SDL_VERSION(&compiled);
        SDL_GetVersion(&linked);
        mc2::fmt::print_err("We compiled against SDL version {:d}.{:d}.{:d} \n",
            mc2::fmt::as_i32(compiled.major), mc2::fmt::as_i32(compiled.minor), mc2::fmt::as_i32(compiled.patch));
        mc2::fmt::print_err("But we are linking against SDL version {:d}.{:d}.{:d}.\n",
           mc2::fmt::as_i32(linked.major), mc2::fmt::as_i32(linked.minor), mc2::fmt::as_i32(linked.patch));
    }

    if (VERBOSE_MODES) {
        SDL_DisplayMode mode;
        int bpp;
        Uint32 Rmask, Gmask, Bmask, Amask;

        n = SDL_GetNumVideoDisplays();
        mc2::fmt::print_err("Number of displays: {:d}\n", mc2::fmt::as_i32(n));
        for (i = 0; i < n; ++i) {
            mc2::fmt::print_err("Display {:d}:\n", mc2::fmt::as_i32(i));

            SDL_GetDesktopDisplayMode(i, &mode);
            SDL_PixelFormatEnumToMasks(mode.format, &bpp, &Rmask, &Gmask,
                    &Bmask, &Amask);
            mc2::fmt::print_err("  Current mode: {:d}x{:d}@{:d}Hz, {:d} bits-per-pixel ({})\n",
                    mc2::fmt::as_i32(mode.w), mc2::fmt::as_i32(mode.h), mc2::fmt::as_i32(mode.refresh_rate), mc2::fmt::as_i32(bpp),
                    mc2::fmt::safe(SDL_GetPixelFormatName(mode.format)));
            if (Rmask || Gmask || Bmask) {
                mc2::fmt::print_err("      Red Mask   = 0x{:08x}\n", mc2::fmt::as_u32(Rmask));
                mc2::fmt::print_err("      Green Mask = 0x{:08x}\n", mc2::fmt::as_u32(Gmask));
                mc2::fmt::print_err("      Blue Mask  = 0x{:08x}\n", mc2::fmt::as_u32(Bmask));
                if (Amask)
                    mc2::fmt::print_err("      Alpha Mask = 0x{:08x}\n", mc2::fmt::as_u32(Amask));
            }

            /* Print available fullscreen video modes */
            m = SDL_GetNumDisplayModes(i);
            if (m == 0) {
                mc2::fmt::print_err("No available fullscreen video modes\n");
            } else {
                mc2::fmt::print_err("  Fullscreen video modes:\n");
                for (j = 0; j < m; ++j) {
                    SDL_GetDisplayMode(i, j, &mode);
                    SDL_PixelFormatEnumToMasks(mode.format, &bpp, &Rmask,
                            &Gmask, &Bmask, &Amask);
                    mc2::fmt::print_err("    Mode {:d}: {:d}x{:d}@{:d}Hz, {:d} bits-per-pixel ({})\n",
                            mc2::fmt::as_i32(j), mc2::fmt::as_i32(mode.w), mc2::fmt::as_i32(mode.h), mc2::fmt::as_i32(mode.refresh_rate), mc2::fmt::as_i32(bpp),
                            mc2::fmt::safe(SDL_GetPixelFormatName(mode.format)));
                    if (Rmask || Gmask || Bmask) {
                        mc2::fmt::print_err("        Red Mask   = 0x{:08x}\n",
                                mc2::fmt::as_u32(Rmask));
                        mc2::fmt::print_err("        Green Mask = 0x{:08x}\n",
                                mc2::fmt::as_u32(Gmask));
                        mc2::fmt::print_err("        Blue Mask  = 0x{:08x}\n",
                                mc2::fmt::as_u32(Bmask));
                        if (Amask)
                            mc2::fmt::print_err("        Alpha Mask = 0x{:08x}\n",
                                    mc2::fmt::as_u32(Amask));
                    }
                }
            }
        }
    }

    if (VERBOSE_RENDER) {
        SDL_RendererInfo info;

        n = SDL_GetNumRenderDrivers();
        if (n == 0) {
            mc2::fmt::print_err("No built-in render drivers\n");
        } else {
            mc2::fmt::print_err("Built-in render drivers:\n");
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
        // FREE-RESIZE-1 (UI-ASPECT-ANCHOR meta-fix): in windowed mode the
        // window is user-resizable — every consumer (postprocess FBOs, UI
        // canvas box, mouse normalize, camera aspect) re-derives from
        // Environment.drawableWidth/Height each frame, so a live resize just
        // works once the SIZE_CHANGED handler refreshes those values.
        Uint32 winFlags = SDL_WINDOW_VULKAN | SDL_WINDOW_ALLOW_HIGHDPI;
        {
            // ISSUE-49 residual: also honor the engine's windowed boot state
            // (windowed_hint = Environment.fullScreen == 0, set from
            // options.cfg FullScreen=FALSE) — a user booting windowed via
            // options.cfg without the launcher checkbox previously got a
            // fixed, non-draggable border. No change for fullscreen boots
            // (SDL ignores the resizable flag under FULLSCREEN_DESKTOP).
            const bool windowed = mc2::gate::get<"MC2_WINDOWED">() || windowed_hint;
            if (windowed)
                winFlags |= SDL_WINDOW_RESIZABLE;
        }
        // SMOKE-NO-CURSOR-STEAL-1: automated runs should not steal FOCUS either
        // -- the user keeps typing in whatever window was already active.
        // SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN is honored by the Windows
        // backend (ShowWindow SW_SHOWNOACTIVATE); harmless no-op elsewhere.
        // CURSOR-GRAB-FIX-1: this condition WAS spelled out longhand here and
        // nowhere else; gos_UnattendedRunRequested() is the same two intents
        // under one name, shared with the cursor-capture policy so activation
        // and capture cannot disagree about whether a run is unattended.
        if (gos_UnattendedRunRequested()) {
            SDL_SetHint(SDL_HINT_WINDOW_NO_ACTIVATION_WHEN_SHOWN, "1");
        }

        // NOFOCUS-HARDEN-1: geometry correct AT CREATION, not repaired after.
        // HIDDEN + explicit offscreen origin means the HWND is never mapped to
        // a visible pixel, not even for the one frame between CreateWindowEx
        // and the SDL_ShowWindow below.
        int winX = SDL_WINDOWPOS_CENTERED;
        int winY = SDL_WINDOWPOS_CENTERED;
        if (window_offscreen_requested()) {
            winX = kOffscreenWindowX;
            winY = kOffscreenWindowY;
            winFlags |= SDL_WINDOW_HIDDEN;
            // A fullscreen window has no position to move it out of the way:
            // it covers the display by definition. Refuse it here so
            // MC2_WINDOW_OFFSCREEN is self-sufficient and does not depend on
            // the caller also remembering MC2_WINDOWED.
            winFlags &= ~static_cast<Uint32>((SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP));
        }
        window = SDL_CreateWindow(pwinname ? pwinname : "--",
                winX, winY, width, height, winFlags);

        // ISSUE-49 verification diag (existing MC2_RES_DIAG gate): one boot
        // line proving whether the OS window carries the resizable flag.
        if (window && mc2::gate::get<"MC2_RES_DIAG">()) {
            mc2::fmt::print_out("[RES_DIAG] window flags: resizable={} windowed_env={} windowed_hint={}\n",
                (SDL_GetWindowFlags(window) & SDL_WINDOW_RESIZABLE) ? 1 : 0,
                mc2::gate::get<"MC2_WINDOWED">() ? 1 : 0,
                windowed_hint ? 1 : 0);
            fflush(stdout);
        }

        if (!window) {
            mc2::fmt::print_err("Couldn't create window: {}\n", mc2::fmt::safe(SDL_GetError()));
            return nullptr;
        }
        SDL_GetWindowSize(window, &width, &height);

        // NULL to use window width and height and display refresh rate
        // only need to set mode if wanted fullscreen
        if (SDL_SetWindowDisplayMode(window, nullptr) < 0) {
            mc2::fmt::print_err("Can't set up display mode: {}\n", mc2::fmt::safe(SDL_GetError()));
            SDL_DestroyWindow(window);
            return nullptr;
        }

        // NOFOCUS-HARDEN-1: re-assert the origin immediately before the ONLY
        // ShowWindow in the engine. SDL_SetWindowDisplayMode above can move a
        // window; this closes the gap so the first mapped frame is already off
        // the visible desktop.
        apply_offscreen_position(window);
        // CURSOR-GRAB-FIX-1: mark the window non-activating BEFORE it is ever
        // mapped, so there is no interval in which an unattended window could
        // be Alt-Tabbed to and eat the user's keystrokes.
        apply_unattended_window_styles(window);
        SDL_ShowWindow(window);
        apply_offscreen_position(window);
        if (window_offscreen_requested() && mc2::gate::get<"MC2_RES_DIAG">()) {
            int px = 0, py = 0;
            SDL_GetWindowPosition(window, &px, &py);
            mc2::fmt::print_out("[RES_DIAG] offscreen window origin={},{} size={}x{}\n",
                   px, py, width, height);
            fflush(stdout);
        }

        // Hide the OS cursor. MC2 renders its own in-game cursor sprite, so
        // the default arrow would otherwise double up on top of it.
        // MC2_OS_CURSOR=1: keep the native OS cursor visible instead. Use when the
        // software cursor doesn't render (e.g. old-era MC2X/MCO campaigns at the
        // 800-logical GUI tier, where the cursorsa sprite draws invisibly). Paired
        // with a software-cursor-draw skip in UserInput::render so there's no double.
        if (!gos_UnattendedRunRequested()) {
            if (mc2::gate::get<"MC2_OS_CURSOR">())
                SDL_ShowCursor(SDL_ENABLE);
            else
                SDL_ShowCursor(SDL_DISABLE);
        }
    }

    RenderWindow* rw = new RenderWindow();
    rw->window_ = window;
    rw->width_ = width;
    rw->height_ = height;

    g_sdl_window = window;
    g_window_thread = SDL_ThreadID();
    note_window_pumped();
    static const bool watchdogStarted = (start_main_stall_watchdog(), true);
    (void)watchdogStarted;
    // CURSOR-GRAB-FIX-1: still requested unconditionally -- this records the
    // engine's INTENT to grab, which is correct for an ordinary interactive
    // launch. apply_mouse_grab() decides whether it is ALLOWED, and refuses when
    // the window is not user-visible. The decision deliberately does not live
    // here: a per-call-site guard would have to be repeated at the FOCUS_GAINED
    // re-arm and at all four refresh_mouse_grab() sites, and the next one added
    // would be missed. See apply_mouse_grab().
    set_mouse_grab(true);

    return rw;
}

//==============================================================================


//==============================================================================


//==============================================================================




//==============================================================================


//==============================================================================


//==============================================================================
bool resize_window(RenderWindowHandle rw_handle, int width, int height)
{
    RenderWindow* rw = static_cast<RenderWindow*>(rw_handle);
    assert(rw);

    // A logical/video canvas change must not resize the desktop fullscreen surface.
    if (!(SDL_GetWindowFlags(rw->window_) & SDL_WINDOW_FULLSCREEN))
        SDL_SetWindowSize(rw->window_, width, height);
    rw->width_ = width;
    rw->height_ = height;
    // NOFOCUS-HARDEN-1: SDL re-centres on resize. Re-assert SYNCHRONOUSLY here
    // (not from a 250ms external poller) so the window is never observable at a
    // visible position. This is the path prefs.applyPrefs() -> gos_SetScreenMode
    // takes during boot, which is what used to undo a one-shot park.
    apply_offscreen_position(rw->window_);
    if (rw->window_ == g_sdl_window) {
        refresh_mouse_grab();
        apply_os_cursor_policy("window_mode", (SDL_GetWindowFlags(g_sdl_window) & SDL_WINDOW_INPUT_FOCUS) != 0);
    }

    return true;
}

//==============================================================================
bool set_window_fullscreen(RenderWindowHandle rw_handle, bool fullscreen)
{
    RenderWindow* rw = static_cast<RenderWindow*>(rw_handle);
    assert(rw);

    // NOFOCUS-HARDEN-1: a fullscreen window covers the display by definition --
    // there is no position that hides it. Under MC2_WINDOW_OFFSCREEN a later
    // fullscreen request (options.cfg FullScreen=TRUE replayed by
    // prefs.applyPrefs()) is refused outright rather than granted-then-parked.
    if (fullscreen && window_offscreen_requested()) {
        fullscreen = false;
    }

    Uint32 flags = fullscreen ? /*SDL_WINDOW_FULLSCREEN*/ SDL_WINDOW_FULLSCREEN_DESKTOP : 0;

    const Uint32 current = SDL_GetWindowFlags(rw->window_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (current != flags && SDL_SetWindowFullscreen(rw->window_, flags) != 0) {
        MC2_LOG(Render, Error, "SDL_SetWindowFullscreen: {}", mc2::fmt::safe(SDL_GetError()));
        return false;
    }

    // Leaving fullscreen restores the pre-fullscreen (visible) origin; re-assert.
    apply_offscreen_position(rw->window_);
    window_checkpoint("mode_after", fullscreen);

    if (rw->window_ == g_sdl_window) {
        refresh_mouse_grab();
        apply_os_cursor_policy("window_mode", (SDL_GetWindowFlags(g_sdl_window) & SDL_WINDOW_INPUT_FOCUS) != 0);
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
	desired.driverdata = nullptr;

    SDL_DisplayMode returned;
    
    if(nullptr == SDL_GetClosestDisplayMode(displayIndex, &desired, &returned)) {
        MC2_LOG(Render, Error, "resize_window: {}", mc2::fmt::safe(SDL_GetError()));
        return false;
    }

    //const char* df = SDL_GetPixelFormatName(desired.format);
    //const char* rf = SDL_GetPixelFormatName(returned.format);

    if(returned.w == desired.w && returned.h == desired.h && returned.format == desired.format)
        return true;

    return false;
}

//==============================================================================
// get_window_display_index / get_num_display_modes / get_display_mode_by_index moved to Platform/WindowInput.cpp (R1-window).

//==============================================================================
bool get_desktop_display_mode(int display_index, int* width, int* height, int* bpp)
{
    assert(width && height && bpp);

    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(display_index, &dm) != 0) {
        MC2_LOG(Render, Error, "SDL_GetDesktopDisplayMode failed: {}", mc2::fmt::safe(SDL_GetError()));
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
    RenderWindow* rw = static_cast<RenderWindow*>(rw_handle);
    assert(rw && width && height);
    *width = rw->width_;
    *height = rw->height_;
}

//==============================================================================
void get_drawable_size(RenderWindowHandle rw_handle, int* width, int* height)
{
    RenderWindow* rw = static_cast<RenderWindow*>(rw_handle);
    assert(rw && width && height);
	// TOD: does it make sense to cahce this value? probably not
	SDL_GetWindowSizeInPixels(rw->window_, width, height);
}

//==============================================================================
void destroy_window(RenderWindowHandle rw_handle)
{
    RenderWindow* rw = static_cast<RenderWindow*>(rw_handle);
    if (rw->window_ == g_sdl_window) {
        set_mouse_grab(false);
    }
    SDL_ShowCursor(SDL_ENABLE);
    SDL_DestroyWindow(rw->window_);
    delete rw;

    g_sdl_window = nullptr;
}

//==============================================================================
static void PrintRendererFlag(Uint32 flag)
{
	switch (flag) {
	case SDL_RENDERER_PRESENTVSYNC:
		mc2::fmt::print_err("PresentVSync");
		break;
	case SDL_RENDERER_ACCELERATED:
		mc2::fmt::print_err("Accelerated");
		break;
	default:
		mc2::fmt::print_err("0x{:08x}", mc2::fmt::as_u32(flag));
		break;
	}
}

//==============================================================================
static void PrintPixelFormat(Uint32 format)
{
	switch (format) {
	case SDL_PIXELFORMAT_UNKNOWN:
		mc2::fmt::print_err("Unknwon");
		break;
	case SDL_PIXELFORMAT_INDEX1LSB:
		mc2::fmt::print_err("Index1LSB");
		break;
	case SDL_PIXELFORMAT_INDEX1MSB:
		mc2::fmt::print_err("Index1MSB");
		break;
	case SDL_PIXELFORMAT_INDEX4LSB:
		mc2::fmt::print_err("Index4LSB");
		break;
	case SDL_PIXELFORMAT_INDEX4MSB:
		mc2::fmt::print_err("Index4MSB");
		break;
	case SDL_PIXELFORMAT_INDEX8:
		mc2::fmt::print_err("Index8");
		break;
	case SDL_PIXELFORMAT_RGB332:
		mc2::fmt::print_err("RGB332");
		break;
	case SDL_PIXELFORMAT_RGB444:
		mc2::fmt::print_err("RGB444");
		break;
	case SDL_PIXELFORMAT_RGB555:
		mc2::fmt::print_err("RGB555");
		break;
	case SDL_PIXELFORMAT_BGR555:
		mc2::fmt::print_err("BGR555");
		break;
	case SDL_PIXELFORMAT_ARGB4444:
		mc2::fmt::print_err("ARGB4444");
		break;
	case SDL_PIXELFORMAT_ABGR4444:
		mc2::fmt::print_err("ABGR4444");
		break;
	case SDL_PIXELFORMAT_ARGB1555:
		mc2::fmt::print_err("ARGB1555");
		break;
	case SDL_PIXELFORMAT_ABGR1555:
		mc2::fmt::print_err("ABGR1555");
		break;
	case SDL_PIXELFORMAT_RGB565:
		mc2::fmt::print_err("RGB565");
		break;
	case SDL_PIXELFORMAT_BGR565:
		mc2::fmt::print_err("BGR565");
		break;
	case SDL_PIXELFORMAT_RGB24:
		mc2::fmt::print_err("RGB24");
		break;
	case SDL_PIXELFORMAT_BGR24:
		mc2::fmt::print_err("BGR24");
		break;
	case SDL_PIXELFORMAT_RGB888:
		mc2::fmt::print_err("RGB888");
		break;
	case SDL_PIXELFORMAT_BGR888:
		mc2::fmt::print_err("BGR888");
		break;
	case SDL_PIXELFORMAT_ARGB8888:
		mc2::fmt::print_err("ARGB8888");
		break;
	case SDL_PIXELFORMAT_RGBA8888:
		mc2::fmt::print_err("RGBA8888");
		break;
	case SDL_PIXELFORMAT_ABGR8888:
		mc2::fmt::print_err("ABGR8888");
		break;
	case SDL_PIXELFORMAT_BGRA8888:
		mc2::fmt::print_err("BGRA8888");
		break;
	case SDL_PIXELFORMAT_ARGB2101010:
		mc2::fmt::print_err("ARGB2101010");
		break;
	case SDL_PIXELFORMAT_YV12:
		mc2::fmt::print_err("YV12");
		break;
	case SDL_PIXELFORMAT_IYUV:
		mc2::fmt::print_err("IYUV");
		break;
	case SDL_PIXELFORMAT_YUY2:
		mc2::fmt::print_err("YUY2");
		break;
	case SDL_PIXELFORMAT_UYVY:
		mc2::fmt::print_err("UYVY");
		break;
	case SDL_PIXELFORMAT_YVYU:
		mc2::fmt::print_err("YVYU");
		break;
	default:
		mc2::fmt::print_err("0x{:08x}", mc2::fmt::as_u32(format));
		break;
	}
}

//==============================================================================
static void PrintRenderer(SDL_RendererInfo * info)
{
    size_t i, count;

    mc2::fmt::print_err("  Renderer {}:\n", mc2::fmt::safe(info->name));

    mc2::fmt::print_err("    Flags: 0x{:08X}", mc2::fmt::as_u32(info->flags));
    mc2::fmt::print_err(" (");
    count = 0;
    for (i = 0; i < sizeof(info->flags) * 8; ++i) {
        Uint32 flag = (1 << i);
        if (info->flags & flag) {
            if (count > 0) {
                mc2::fmt::print_err(" | ");
            }
            PrintRendererFlag(flag);
            ++count;
        }
    }
    mc2::fmt::print_err(")\n");

    mc2::fmt::print_err("    Texture formats ({:d}): ", mc2::fmt::as_i32(info->num_texture_formats));
    for (i = 0; i < info->num_texture_formats; ++i) {
        if (i > 0) {
			mc2::fmt::print_err(", ");
		}
		PrintPixelFormat(info->texture_formats[i]);
	}
	mc2::fmt::print_err("\n");

	if (info->max_texture_width || info->max_texture_height) {
		mc2::fmt::print_err("    Max Texture Size: {:d}x{:d}\n",
				mc2::fmt::as_i32(info->max_texture_width), mc2::fmt::as_i32(info->max_texture_height));
	}
}



//==============================================================================
SDL_Window* getSDLWindow() noexcept
{
    return g_sdl_window;
}

//==============================================================================


}; // namespace graphics
