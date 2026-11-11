#include "gameos.hpp"

#include <SDL2/SDL.h> // can avoid inluding this here if move MouseInfo struct to separate file
#include "gos_input.h"
#include "WindowInput.hpp"
// CURSOR-GRAB-FIX-1: the cursor-capture policy declared there is DEFINED here,
// so include it -- a declaration this TU cannot see is a signature mismatch the
// compiler would never catch.
#include "gos_render.h"
#include "utils/vec.h"

#include<string.h>
#include<stdlib.h>
#include<stdio.h>
#include<cstdlib>
#include "Gates/gate.h"
#include "mc2fmt.h"

// The one SDL window (owned by gos_render). Used to normalize the raw mouse by
// the SAME coordinate space SDL delivers mouse-motion events in (window points),
// which under HiDPI diverges from the physical drawable. See gos_GetMouseInfo.
extern SDL_Window* g_sdl_window;

// SMOKE-NO-CURSOR-STEAL-1 -----------------------------------------------------
// Automated launches (smoke / visual_verify / capture harness) used to steal the
// user's mouse: the window-creation mouse-grab clamps the pointer into the game
// window via Win32 ClipCursor, and the engine also warps the pointer outright
// (gos_SetMousePosition -> SDL_WarpMouseInWindow; e.g. the tutorial guiFrozen
// path recenters every frame). Both are pointless for an unattended run and
// interrupt whatever the user is doing in another window.
//
// Policy (evaluated once, cached):
//   MC2_NO_CURSOR_GRAB=1  -> suppressed (explicit, works interactively too)
//   MC2_NO_CURSOR_GRAB=0  -> NOT suppressed (escape hatch, even under smoke)
//   otherwise             -> suppressed iff MC2_SMOKE_MODE is set
// Interactive launches (neither var set) keep exactly today's behavior.
bool gos_CursorControlSuppressed()
{
    static int s_state = -1;
    if (s_state < 0) {
        if (mc2::gate::isNonEmpty<"MC2_NO_CURSOR_GRAB">()) {
            s_state = mc2::gate::get<"MC2_NO_CURSOR_GRAB">() ? 1 : 0;   // Lead0Def row: leading 0 -> 0, anything else -> 1
        } else {
            s_state = mc2::gate::get<"MC2_SMOKE_MODE">() ? 1 : 0;
        }
    }
    return s_state == 1;
}

// CURSOR-GRAB-FIX-1 -----------------------------------------------------------
// See the block comment in gos_render.h for the rule and the field bug.
//
// WHY THESE LIVE HERE, next to gos_CursorControlSuppressed(): this TU is in both
// the game and the editor build (the editor drops gos_render.cpp only), and none
// of this needs the render context -- just env and SDL window state. Keeping the
// whole policy in one place is deliberate: the previous shape had the offscreen
// decision in gos_render.cpp and the cursor decision here, and nothing made the
// two agree.

bool gos_WindowOffscreenRequested()
{
    // Cached: the variable is read many times per second on the grab path and
    // cannot change within a process. Same "set and not '0'" polarity as the
    // original graphics::window_offscreen_requested(), which now delegates here
    // so there is exactly ONE parse of MC2_WINDOW_OFFSCREEN in the engine.
    static int s_state = -1;
    if (s_state < 0) {
        s_state = mc2::gate::get<"MC2_WINDOW_OFFSCREEN">();
    }
    return s_state == 1;
}

bool gos_UnattendedRunRequested()
{
    return gos_CursorControlSuppressed() || gos_WindowOffscreenRequested();
}

// META4-CLOSE-1 ---------------------------------------------------------------
// "Do not touch my cursor" and "no human is at this machine" are DIFFERENT
// claims, and gos_UnattendedRunRequested() conflates them because
// gos_CursorControlSuppressed() is true for a bare MC2_NO_CURSOR_GRAB=1.
//
// That flag is documented in three places as a user-facing INTERACTIVE option
// (docs/tier1_env_vars.md, docs/gameplay-driver.md's windowed recipe, and
// RendererFeatureRegistry.h: "=1 forces it on for an interactive launch").
// Keying window styles and the crash dialog on the union made that
// documentation false: a player who set it to play without a cursor clamp got
// WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW -- no taskbar button, no Alt-Tab entry,
// a window that refuses keyboard focus -- plus a silently suppressed crash
// dialog and a forced fast-exit.
//
// So: only the two intents that mean "there is no window a user could be
// looking at" answer this one.
//
//   MC2_WINDOW_OFFSCREEN -- the window is parked at (-32000,-32000), on no
//     monitor. Giving THAT an Alt-Tab entry is the trap CURSOR-GRAB-FIX-1
//     closed: the user Alt-Tabs, and his keystrokes disappear into a window he
//     cannot see. Still covered, so that fix is not reopened.
//   MC2_SMOKE_MODE -- a harness run by definition.
//
// Dropping the bare MC2_NO_CURSOR_GRAB arm costs NO coverage, and that is a
// property of the harness, not a hope: scripts/smoke_lib/nofocus.py:230-253 is
// the single launch chokepoint (enforced by scripts/check-background-launch.py)
// and it either sets MC2_WINDOW_OFFSCREEN=1 alongside MC2_NO_CURSOR_GRAB=1, or
// RAISES DesktopOccupancyRefused, or has already returned early because
// MC2_HARNESS_ALLOW_FOREGROUND=1 declared a user-present session. There is no
// path through it that reaches the engine with MC2_NO_CURSOR_GRAB alone.
//
// KNOWN AND DELIBERATE: MC2_SMOKE_MODE is taken at face value, so a
// user-present session that also asks for smoke mode still gets the unattended
// styles. That is the caller stating two things at once, not a gap.
bool gos_NoUserPresentRequested()
{
    static int s_state = -1;
    if (s_state < 0) {
        s_state = (gos_WindowOffscreenRequested() ||
                   mc2::gate::get<"MC2_SMOKE_MODE">()) ? 1 : 0;
    }
    return s_state == 1;
}

bool gos_WindowUserVisible(SDL_Window* window)
{
    if (!window)
        return false;

    const Uint32 flags = SDL_GetWindowFlags(window);
    if (flags & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED))
        return false;

    int wx = 0, wy = 0, ww = 0, wh = 0;
    SDL_GetWindowPosition(window, &wx, &wy);
    SDL_GetWindowSize(window, &ww, &wh);
    if (ww <= 0 || wh <= 0)
        return false;

    const int numDisplays = SDL_GetNumVideoDisplays();
    if (numDisplays <= 0) {
        // Cannot enumerate displays -> cannot prove invisibility. Answer
        // "visible" so an SDL hiccup never silently disables the grab for a
        // player who IS looking at the window. The intent gates in
        // gos_CursorCaptureForbidden() and the on-no-monitor ClipCursor
        // failsafe in gos_render.cpp both still hold in that case.
        return true;
    }

    const SDL_Rect winRect = { wx, wy, ww, wh };
    for (int i = 0; i < numDisplays; ++i) {
        SDL_Rect bounds;
        if (SDL_GetDisplayBounds(i, &bounds) != 0)
            continue;
        if (SDL_HasIntersection(&winRect, &bounds) == SDL_TRUE)
            return true;
    }
    // The window is mapped but lands on no display -- e.g. parked at
    // (-32000,-32000) by MC2_WINDOW_OFFSCREEN. Nobody can see it, so nobody
    // consented to it owning the pointer.
    return false;
}

bool gos_CursorCaptureForbidden(SDL_Window* window, const char** reason)
{
    const char* why = nullptr;
    if (gos_CursorControlSuppressed())
        why = "MC2_NO_CURSOR_GRAB / MC2_SMOKE_MODE";
    else if (gos_WindowOffscreenRequested())
        why = "MC2_WINDOW_OFFSCREEN";
    else if (!window)
        why = "no window";
    else if (!gos_WindowUserVisible(window))
        why = "window is hidden, minimized or on no display";
    if (reason)
        *reason = why;
    return why != nullptr;
}

// MC2_VK_PICK_PROBE test hook: inject a synthetic raw SDL mouse point (window-
// points space) so the pick probe can drive the exact production normalize path.
namespace input { void setMouseInfoRaw(float x, float y); }
void gos_TestSetRawMouse(float winPtX, float winPtY)
{
    input::setMouseInfoRaw(winPtX, winPtY);
}

//
// The mouse position and buttons are read once a frame.
//
// Position returns values converted into position on window. Range is 0.0 to 1.0  (0 to 639 on a 640 wide screen)
// When running in a window this may not match the sum of the deltas, as the windows mouse cursor can have acceleration
//
//
// Deltas returns values as a percentage of screen size. Range is -1.0 to 1.0
//
// ie: 0.5 means half a screen to the right or down
//
// The wheel position is only available as a delta value, Range is -1.0 to 1.0 (999.0 means no wheel)
//
//
// ButtonsPressed are bitflags, 0=Left Button, 2=Right, 1=Middle, 3=X1, 4=X2 (X1 and X2 are on 5 button mice)
//
// If you want to make sure you do not miss mouse clicks, check for the mouse buttons with
//
// You may pass zero as any parameter you do not wish to query
//
////////////////////////////////////////////////////////////////////////////////
void __stdcall gos_GetMouseInfo( float* pXPosition, float* pYPosition, int* pXDelta, int* pYDelta, int* pWheelDelta, DWORD* pButtonsPressed )
{
    // MOUSE-DPI-COORD (BUG-A fix): normalize the raw mouse by the SDL WINDOW size
    // (logical points), NOT the physical drawable. SDL guarantees mouse-motion
    // event coords (event->motion.x, stored in mi->x_) are in the same coordinate
    // space as SDL_GetWindowSize (window points). Under HiDPI (4K @225%: drawable
    // 3840 vs window 1707) those diverge, and the previous drawable divisor
    // compressed the mouse into the top-left 44% of the pick canvas -> the whole
    // world/object pick chain (findObjectByMouse compares against object screen
    // bboxes authored in the LOGICAL viewMulX==window space) resolved far from the
    // cursor. Dividing by the window size makes getMouseX an identity on the SDL
    // window point (norm*viewMulX == mi->x_ when window==viewMul canvas), so the
    // pick is DPI/resolution independent. When window==drawable (scale 1.0, the
    // old fullscreen/1920 config) this reduces to the previous behaviour exactly.
    int winW = Environment.drawableWidth, winH = Environment.drawableHeight;
    if (g_sdl_window) SDL_GetWindowSize(g_sdl_window, &winW, &winH);
    if (winW <= 0) winW = Environment.drawableWidth;
    if (winH <= 0) winH = Environment.drawableHeight;
    const float w = static_cast<float>(winW);
    const float h = static_cast<float>(winH);

    const input::MouseInfo* mi = input::getMouseInfo();

    // [FORCE-43 v1] When the frame is pillarboxed, the visible 0..1 span is the
    // centered 4:3 box, not the full drawable. Normalize relative to the box so
    // the existing viewMul transform (and thus world pick) lands on the same
    // pixels the scene was composited into. Outside the box -> <0 or >1 (no hit).
    int bx, by, bw, bh;
    // UI-ASPECT-ANCHOR-1: on front-end frames the UI draws into the centered
    // 16:9 canvas, so normalize mouse relative to that box (same convention as
    // the FORCE-43 branch below). Inactive in mission (never asserted) so the
    // world-pick viewMul transform is untouched.
    if (gos_ComputeUiCanvasBox(winW, winH,
                               &bx, &by, &bw, &bh)) {
        if(pXPosition)
            *pXPosition = (mi->x_ - static_cast<float>(bx)) / static_cast<float>(bw);
        if(pYPosition)
            *pYPosition = (mi->y_ - static_cast<float>(by)) / static_cast<float>(bh);
    } else if (gos_Compute43Box(winW, winH,
                         &bx, &by, &bw, &bh)) {
        if(pXPosition)
            *pXPosition = (mi->x_ - static_cast<float>(bx)) / static_cast<float>(bw);
        if(pYPosition)
            *pYPosition = (mi->y_ - static_cast<float>(by)) / static_cast<float>(bh);
    } else {
        if(pXPosition)
            *pXPosition = mi->x_ / w;
        if(pYPosition)
            *pYPosition = mi->y_ / h;
    }

    // MC2_MOUSE_RECON: env-gated one-shot-ish confirm log. Prints the raw mouse,
    // both candidate denominators, and the resulting normalized coord so the
    // logical-vs-drawable divergence (the dead-zone root cause) is visible in a
    // pasted console log. Throttled to "moved > 8px" to avoid flooding.
    if (false)
    {
        static float s_lastX = -1.0f, s_lastY = -1.0f;
        const float dx = mi->x_ - s_lastX, dy = mi->y_ - s_lastY;
        if (dx*dx + dy*dy > 64.0f)
        {
            s_lastX = mi->x_; s_lastY = mi->y_;
            mc2::fmt::print_out("[MOUSE_RECON v1] raw=({:.0f},{:.0f}) logical=({:d}x{:d}) drawable=({:d}x{:d}) norm_logical=({:.3f},{:.3f}) norm_drawable=({:.3f},{:.3f})\n", mi->x_, mi->y_, mc2::fmt::as_i32(Environment.screenWidth), mc2::fmt::as_i32(Environment.screenHeight), mc2::fmt::as_i32(Environment.drawableWidth), mc2::fmt::as_i32(Environment.drawableHeight), mi->x_ / w, mi->y_ / h, mi->x_ / static_cast<float>(Environment.drawableWidth), mi->y_ / static_cast<float>(Environment.drawableHeight));
        }
    }

    // MC2_INPUT_SCRIPT: the scripted player owns the pointer once it has moved it (normalised space, as reported below)
    {
        float snx, sny;
        if (input::scriptMouseNorm(&snx, &sny)) {
            if (pXPosition) *pXPosition = snx;
            if (pYPosition) *pYPosition = sny;
        }
    }

	// as percentage of screen size
    
    // sebi: I do not like this percentage style and it does not work good, so
    /*
    if(pXDelta)
        *pXDelta = (int)(100.0f * clamp(mi->rel_x_ / w, -1.0f, 1.0f));
    if(pYDelta)
        *pYDelta = (int)(100.0f * clamp(mi->rel_y_ / h, -1.0f, 1.0f));
        */
    if(pXDelta)
        *pXDelta = static_cast<int>((clamp(mi->rel_x_, -w, w)));
    if(pYDelta)
        *pYDelta = static_cast<int>((clamp(mi->rel_y_, -h, h)));

    //if(pWheelDelta)
    //  *pWheelDelta = (int)(100.0f * (mi->wheel_vert_ / h));
    if(pWheelDelta)
        *pWheelDelta = static_cast<int>((mi->wheel_vert_));

    if(pButtonsPressed) {
        DWORD bs = 0;
        for(int i=0; i<input::MouseInfo::NUM_BUTTONS;++i)
        {
            bool down = mi->button_state_[i] == input::KS_PRESSED;
            down |= mi->button_state_[i] == input::KS_HELD;
            bs |= down ? (1<<i) : 0;
        }
        *pButtonsPressed = bs;
    }
}

////////////////////////////////////////////////////////////////////////////////
void __stdcall gos_SetMousePosition( float XPosition, float YPosition )
{
    // SMOKE-NO-CURSOR-STEAL-1 + CURSOR-GRAB-FIX-1: never warp the OS pointer
    // under automation OR when the window is not user-visible. A warp is the
    // same theft as a clip -- the tutorial guiFrozen path recenters the pointer
    // EVERY FRAME -- and the old gos_CursorControlSuppressed()-only test let it
    // through for an MC2_WINDOW_OFFSCREEN run.
    if(gos_CursorCaptureForbidden(g_sdl_window, nullptr))
        return;
    if(g_sdl_window) {
        // XPosition/YPosition arrive as normalized 0..1 coordinates — the
        // write side of the contract established by gos_GetMouseInfo. Must use
        // the SAME denominator as the read side: the SDL WINDOW (points) size,
        // because SDL_WarpMouseInWindow consumes window-point coordinates, the
        // same space mouse-motion events are delivered in (see BUG-A fix in
        // gos_GetMouseInfo). Under HiDPI the drawable is larger than the window;
        // using the drawable here would warp off-window by the DPI factor.
        int winW = Environment.drawableWidth, winH = Environment.drawableHeight;
        SDL_GetWindowSize(g_sdl_window, &winW, &winH);
        if (winW <= 0) winW = Environment.drawableWidth;
        if (winH <= 0) winH = Environment.drawableHeight;
        const float w = static_cast<float>(winW);
        const float h = static_cast<float>(winH);
        // [FORCE-43 v1] Inverse of the box-relative normalize in gos_GetMouseInfo:
        // map the 0..1 box coordinate back into window-point pixels.
        int bx, by, bw, bh;
        // UI-ASPECT-ANCHOR-1: inverse of the canvas-relative normalize above.
        if (gos_ComputeUiCanvasBox(winW, winH,
                                   &bx, &by, &bw, &bh)) {
            SDL_WarpMouseInWindow(g_sdl_window,
                                  static_cast<int>((static_cast<float>(bx) + XPosition * static_cast<float>(bw))),
                                  static_cast<int>((static_cast<float>(by) + YPosition * static_cast<float>(bh))));
        } else if (gos_Compute43Box(winW, winH,
                             &bx, &by, &bw, &bh)) {
            SDL_WarpMouseInWindow(g_sdl_window,
                                  static_cast<int>((static_cast<float>(bx) + XPosition * static_cast<float>(bw))),
                                  static_cast<int>((static_cast<float>(by) + YPosition * static_cast<float>(bh))));
        } else {
            SDL_WarpMouseInWindow(g_sdl_window,
                                  static_cast<int>((XPosition * w)),
                                  static_cast<int>((YPosition * h)));
        }
    }
}

// Platform R0: the key seam lives in Platform/WindowInput.cpp (mc2::platform); these are the unchanged
// gameos.hpp entry points forwarding to it.
////////////////////////////////////////////////////////////////////////////////
DWORD __stdcall gos_GetKey()
{
    return mc2::platform::GetKey();
}

////////////////////////////////////////////////////////////////////////////////
const char* __stdcall gos_DescribeKey( DWORD Key )
{
    return mc2::platform::DescribeKey(Key);
}

////////////////////////////////////////////////////////////////////////////////
void __stdcall gos_KeyboardFlush()
{
    mc2::platform::KeyboardFlush();
}

////////////////////////////////////////////////////////////////////////////////
gosEnum_KeyStatus __stdcall gos_GetKeyStatus(gosEnum_KeyIndex index)
{
    return static_cast<gosEnum_KeyStatus>(mc2::platform::GetKeyStatus(static_cast<int>(index)));
}
