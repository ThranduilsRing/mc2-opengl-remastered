#include "gameos.hpp"

#include <SDL2/SDL.h> // can avoid inluding this here if move MouseInfo struct to separate file
#include "gos_input.h"
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

// loks like KEY_* enum contains everything, from scancodes to localized keys, e.g. it has GRAVE and APOSTROPHE but those are usually on the same physical button and valuse depends on localization
// let's keep it like this for now and see later
gosEnum_KeyIndex remap_sdl_to_gos(const SDL_Scancode scancode) {

    if(scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
        return (gosEnum_KeyIndex)(scancode - SDL_SCANCODE_1 + KEY_1);
    if(scancode == SDL_SCANCODE_0) return KEY_0;

    if(scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
        return (gosEnum_KeyIndex)(scancode - SDL_SCANCODE_A + KEY_A);

    switch(scancode) {
        case SDL_SCANCODE_ESCAPE: return KEY_ESCAPE;
        case SDL_SCANCODE_MINUS: return KEY_MINUS;
        case SDL_SCANCODE_EQUALS: return KEY_EQUALS;
        case SDL_SCANCODE_BACKSPACE: return KEY_BACK;
        case SDL_SCANCODE_TAB: return KEY_TAB;
        case SDL_SCANCODE_LEFTBRACKET: return KEY_LBRACKET;
        case SDL_SCANCODE_RIGHTBRACKET: return KEY_RBRACKET;
        case SDL_SCANCODE_RETURN: return KEY_RETURN;
        case SDL_SCANCODE_SEMICOLON: return KEY_SEMICOLON;
        case SDL_SCANCODE_APOSTROPHE: return KEY_APOSTROPHE;
        case SDL_SCANCODE_GRAVE: return KEY_GRAVE;
        case SDL_SCANCODE_BACKSLASH: return KEY_BACKSLASH;
        case SDL_SCANCODE_COMMA: return KEY_COMMA;
        case SDL_SCANCODE_PERIOD: return KEY_PERIOD;
        case SDL_SCANCODE_SLASH: return KEY_SLASH;
        case SDL_SCANCODE_KP_MULTIPLY: return KEY_MULTIPLY;
        case SDL_SCANCODE_MENU: return KEY_LMENU;
        case SDL_SCANCODE_SPACE: return KEY_SPACE;
        case SDL_SCANCODE_CAPSLOCK: return KEY_CAPITAL;
        case SDL_SCANCODE_F1: return KEY_F1;
        case SDL_SCANCODE_F2: return KEY_F2;
        case SDL_SCANCODE_F3: return KEY_F3;
        case SDL_SCANCODE_F4: return KEY_F4;
        case SDL_SCANCODE_F5: return KEY_F5;
        case SDL_SCANCODE_F6: return KEY_F6;
        case SDL_SCANCODE_F7: return KEY_F7;
        case SDL_SCANCODE_F8: return KEY_F8;
        case SDL_SCANCODE_F9: return KEY_F9;
        case SDL_SCANCODE_F10: return KEY_F10;
        case SDL_SCANCODE_F11: return KEY_F11;
        case SDL_SCANCODE_F12: return KEY_F12;
        case SDL_SCANCODE_F13: return KEY_F13;
        case SDL_SCANCODE_F14: return KEY_F14;
        case SDL_SCANCODE_F15: return KEY_F15;
        case SDL_SCANCODE_KP_0: return KEY_NUMPAD0;
        case SDL_SCANCODE_KP_1: return KEY_NUMPAD1;
        case SDL_SCANCODE_KP_2: return KEY_NUMPAD2;
        case SDL_SCANCODE_KP_3: return KEY_NUMPAD3;
        case SDL_SCANCODE_KP_4: return KEY_NUMPAD4;
        case SDL_SCANCODE_KP_5: return KEY_NUMPAD5;
        case SDL_SCANCODE_KP_6: return KEY_NUMPAD6;
        case SDL_SCANCODE_KP_7: return KEY_NUMPAD7;
        case SDL_SCANCODE_KP_8: return KEY_NUMPAD8;
        case SDL_SCANCODE_KP_9: return KEY_NUMPAD9;
        case SDL_SCANCODE_KP_MINUS: return KEY_SUBTRACT;
        case SDL_SCANCODE_KP_PLUS: return KEY_ADD;
        case SDL_SCANCODE_KP_PERIOD: return KEY_DECIMAL;
        case SDL_SCANCODE_LANG3: return KEY_KANA;

        case SDL_SCANCODE_INTERNATIONAL3: return KEY_YEN;
        //case SDL_SCANCODE_GRAVE: return KEY_CIRCUMFLEX;
        case SDL_SCANCODE_KP_AT: return KEY_AT;
        case SDL_SCANCODE_KP_COLON: return KEY_COLON;
        case SDL_SCANCODE_STOP: return KEY_STOP;
        case SDL_SCANCODE_KP_EQUALS: return KEY_NUMPADEQUALS;
        case SDL_SCANCODE_LANG2: return KEY_KANJI; // is it correct?
        case SDL_SCANCODE_KP_ENTER: return KEY_NUMPADENTER;
        case SDL_SCANCODE_KP_COMMA: return KEY_NUMPADCOMMA;
        case SDL_SCANCODE_KP_DIVIDE: return KEY_DIVIDE;
        case SDL_SCANCODE_SYSREQ: return KEY_SYSRQ;
        case SDL_SCANCODE_HOME: return KEY_HOME;
        case SDL_SCANCODE_UP: return KEY_UP;
        case SDL_SCANCODE_PAGEUP: return KEY_PRIOR;
        case SDL_SCANCODE_LEFT: return KEY_LEFT;
        case SDL_SCANCODE_RIGHT: return KEY_RIGHT;
        case SDL_SCANCODE_END: return KEY_END;
        case SDL_SCANCODE_DOWN: return KEY_DOWN;
        case SDL_SCANCODE_PAGEDOWN: return KEY_NEXT;
        case SDL_SCANCODE_INSERT: return KEY_INSERT;
        case SDL_SCANCODE_DELETE: return KEY_DELETE;
        case SDL_SCANCODE_LGUI: return KEY_LWIN;
        case SDL_SCANCODE_RGUI: return KEY_RWIN;
        case SDL_SCANCODE_APPLICATION: return KEY_APPS;
        case SDL_SCANCODE_NUMLOCKCLEAR: return KEY_NUMLOCK;
        case SDL_SCANCODE_PAUSE: return KEY_PAUSE;

        case SDL_SCANCODE_LCTRL: return KEY_LCONTROL;
        case SDL_SCANCODE_RCTRL: return KEY_RCONTROL;
        case SDL_SCANCODE_LSHIFT: return KEY_LSHIFT;
        case SDL_SCANCODE_RSHIFT: return KEY_RSHIFT;
        case SDL_SCANCODE_LALT: return KEY_LALT;
        case SDL_SCANCODE_RALT: return KEY_RALT;
        default:
            PAUSE(("Unsupported scancode: %d\n", scancode));
    };

    return KEY_ESCAPE;
}

SDL_Scancode remap_gos_to_sdl(const gosEnum_KeyIndex key_index) {

    if(key_index >= KEY_1 && key_index <= KEY_9)
        return static_cast<SDL_Scancode>((key_index - KEY_1 + SDL_SCANCODE_1));
    if(key_index == KEY_0) return SDL_SCANCODE_0;

    if(key_index >= KEY_A && key_index <= KEY_Z)
        return static_cast<SDL_Scancode>((key_index - KEY_A + SDL_SCANCODE_A));

    switch(key_index) {
        case KEY_ESCAPE: return SDL_SCANCODE_ESCAPE;
        case KEY_MINUS: return SDL_SCANCODE_MINUS;
        case KEY_EQUALS: return SDL_SCANCODE_EQUALS;
        case KEY_BACK: return SDL_SCANCODE_BACKSPACE;
        case KEY_TAB: return SDL_SCANCODE_TAB;
        case KEY_LBRACKET: return SDL_SCANCODE_LEFTBRACKET;
        case KEY_RBRACKET: return SDL_SCANCODE_RIGHTBRACKET;

        // KEY_COLON: 
        case KEY_SEMICOLON: return SDL_SCANCODE_SEMICOLON;

        //case KEY_APOSTROPHE: return SDL_SCANCODE_BACKSLASH;
        // KEY_AT: 
        case KEY_GRAVE: return SDL_SCANCODE_GRAVE;

        //KEY_YEN: //return SDL_SCANCODE_INTERNATIONAL3;
        case KEY_BACKSLASH: return SDL_SCANCODE_BACKSLASH;

        case KEY_COMMA: return SDL_SCANCODE_COMMA;
        case KEY_PERIOD: return SDL_SCANCODE_PERIOD;
        case KEY_SLASH: return SDL_SCANCODE_SLASH;
        case KEY_MULTIPLY: return SDL_SCANCODE_KP_MULTIPLY;
        case KEY_LMENU: return SDL_SCANCODE_MENU;
        case KEY_SPACE: return SDL_SCANCODE_SPACE;
        case KEY_CAPITAL: return SDL_SCANCODE_CAPSLOCK;
        case KEY_F1: return SDL_SCANCODE_F1;
        case KEY_F2: return SDL_SCANCODE_F2;
        case KEY_F3: return SDL_SCANCODE_F3;
        case KEY_F4: return SDL_SCANCODE_F4;
        case KEY_F5: return SDL_SCANCODE_F5;
        case KEY_F6: return SDL_SCANCODE_F6;
        case KEY_F7: return SDL_SCANCODE_F7;
        case KEY_F8: return SDL_SCANCODE_F8;
        case KEY_F9: return SDL_SCANCODE_F9;
        case KEY_F10: return SDL_SCANCODE_F10;
        case KEY_F11: return SDL_SCANCODE_F11;
        case KEY_F12: return SDL_SCANCODE_F12;
        case KEY_F13: return SDL_SCANCODE_F13;
        case KEY_F14: return SDL_SCANCODE_F14;
        case KEY_F15: return SDL_SCANCODE_F15;
        case KEY_NUMPAD0: return SDL_SCANCODE_KP_0;
        case KEY_NUMPAD1: return SDL_SCANCODE_KP_1;
        case KEY_NUMPAD2: return SDL_SCANCODE_KP_2;
        case KEY_NUMPAD3: return SDL_SCANCODE_KP_3;
        case KEY_NUMPAD4: return SDL_SCANCODE_KP_4;
        case KEY_NUMPAD5: return SDL_SCANCODE_KP_5;
        case KEY_NUMPAD6: return SDL_SCANCODE_KP_6;
        case KEY_NUMPAD7: return SDL_SCANCODE_KP_7;
        case KEY_NUMPAD8: return SDL_SCANCODE_KP_8;
        case KEY_NUMPAD9: return SDL_SCANCODE_KP_9;
        case KEY_SUBTRACT: return SDL_SCANCODE_KP_MINUS;
        case KEY_ADD: return SDL_SCANCODE_KP_PLUS;
        case KEY_DECIMAL: return SDL_SCANCODE_KP_PERIOD;
        case KEY_KANA: return SDL_SCANCODE_LANG3;

        //case KEY_CIRCUMFLEX: return SDL_SCANCODE_GRAVE;
        case KEY_NUMPADEQUALS: return SDL_SCANCODE_KP_EQUALS;
        case KEY_KANJI: return SDL_SCANCODE_LANG2;; // is it correct?

        // KEY_NUMPADENTER: 
        case KEY_RETURN: return SDL_SCANCODE_RETURN;

        case KEY_NUMPADCOMMA: return SDL_SCANCODE_KP_COMMA;
        case KEY_DIVIDE: return SDL_SCANCODE_KP_DIVIDE;
        case KEY_SYSRQ: return SDL_SCANCODE_SYSREQ;
        case KEY_HOME: return SDL_SCANCODE_HOME;
        case KEY_UP: return SDL_SCANCODE_UP;
        case KEY_PRIOR: return SDL_SCANCODE_PAGEUP;
        case KEY_LEFT: return SDL_SCANCODE_LEFT;
        case KEY_RIGHT: return SDL_SCANCODE_RIGHT;
        case KEY_END: return SDL_SCANCODE_END;
        case KEY_DOWN: return SDL_SCANCODE_DOWN;
        case KEY_NEXT: return SDL_SCANCODE_PAGEDOWN;
        case KEY_INSERT: return SDL_SCANCODE_INSERT;
        case KEY_DELETE: return SDL_SCANCODE_DELETE;
        case KEY_LWIN: return SDL_SCANCODE_LGUI;
        case KEY_RWIN: return SDL_SCANCODE_RGUI;
        case KEY_APPS: return SDL_SCANCODE_APPLICATION;
        case KEY_NUMLOCK: return SDL_SCANCODE_NUMLOCKCLEAR;

        //KEY_PAUSE: 
        case KEY_STOP:return SDL_SCANCODE_PAUSE;

                        // gos does not differentiate
        //KEY_LCONTROL: 
        case KEY_RCONTROL: return SDL_SCANCODE_LCTRL;

        // KEY_LSHIFT
        case KEY_RSHIFT: return SDL_SCANCODE_LSHIFT;

        case KEY_LALT: return SDL_SCANCODE_LALT;
        case KEY_RALT: return SDL_SCANCODE_RALT;
        default:
            PAUSE(("Unsupported KEY_*: %d\n", key_index));
    };

    return SDL_SCANCODE_UNKNOWN;
}

////////////////////////////////////////////////////////////////////////////////
DWORD __stdcall gos_GetKey()
{
    const input::KeyboardInfo* ki = input::getKeyboardInfo();

    if(ki->first_pressed_!=-1) {
        SDL_Scancode sc = static_cast<SDL_Scancode>(ki->first_pressed_);
        SDL_Keycode kc = SDL_GetKeyFromScancode(sc);
        unsigned char c = 0;
        // check if it represents a character:
        if(!(kc & SDLK_SCANCODE_MASK) && isascii(kc)) {
            c = static_cast<unsigned char>(kc);

            // this is all hardcodede crap, mayeb will fix it later

			// FIXME: it is incorrect to do so because caps lock is not held constantly but either turned on or off
            //const bool caps_pressed = !!ki->last_state_[SDL_SCANCODE_CAPSLOCK];

            const bool shift_pressed = !!(ki->last_state_[SDL_SCANCODE_LSHIFT] || ki->last_state_[SDL_SCANCODE_RSHIFT]);
            const bool register_mod = /*caps_pressed ^ */shift_pressed; 
            if(c >= SDLK_a && c<=SDLK_z && register_mod)
            {
                c-=32;
            }
            else if(register_mod)
            {
                switch(c)
                {
                    case SDLK_0:        c = SDLK_RIGHTPAREN; break;
                    case SDLK_1:        c = SDLK_EXCLAIM; break;
                    case SDLK_2:        c = SDLK_AT; break;
                    case SDLK_3:        c = SDLK_HASH; break;
                    case SDLK_4:        c = SDLK_DOLLAR; break;
                    case SDLK_5:        c = 37; break; // %
                    case SDLK_6:        c = SDLK_COLON; break;
                    case SDLK_7:        c = SDLK_AMPERSAND; break;
                    case SDLK_8:        c = SDLK_ASTERISK; break;
                    case SDLK_9:        c = SDLK_LEFTPAREN; break;
                    case SDLK_EQUALS:   c = SDLK_PLUS; break;
                    case SDLK_MINUS:    c = SDLK_UNDERSCORE; break;
                    case SDLK_COMMA:    c = SDLK_LESS; break;
                    case SDLK_PERIOD:   c = SDLK_GREATER; break;
                    case SDLK_SLASH:    c = SDLK_QUESTION; break;
                    case SDLK_BACKSLASH:c = '|'; break;
                    default: break;
                }
            }
        }

        gosEnum_KeyIndex ki = remap_sdl_to_gos(sc);

        return (ki<<8) | c;
    }

    if(Environment.ButtonsAsKeys) {
        // TODO: check buttons
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
const char* __stdcall gos_DescribeKey( DWORD Key )
{
    const DWORD k = (Key>>8)&0xFF;
    DWORD ascii = Key&0xFF; (void)ascii; // may be 0 if extended key
    bool is_extended = (Key&0x100)!=0; (void)is_extended;

    SDL_Scancode scancode = remap_gos_to_sdl((gosEnum_KeyIndex)k);
    return SDL_GetScancodeName(scancode);
}

////////////////////////////////////////////////////////////////////////////////
void __stdcall gos_KeyboardFlush()
{
    input::resetKeypress();
}

////////////////////////////////////////////////////////////////////////////////
gosEnum_KeyStatus __stdcall gos_GetKeyStatus(gosEnum_KeyIndex index)
{
    const input::MouseInfo* mi = input::getMouseInfo();
    const input::KeyboardInfo* ki = input::getKeyboardInfo();

    if(index >= KEY_LMOUSE && index <= KEY_MOUSEX2) {
        gosASSERT(index > 0 && index <= input::MouseInfo::NUM_BUTTONS);
        return (gosEnum_KeyStatus)mi->button_state_[index-1];
    }

    // if keyboard
    if((index & 0x01000000) == 0) {
        if(index > 255)
        {
            int asfa=0;
            (void)asfa;
        }
        SDL_Scancode sc = remap_gos_to_sdl((gosEnum_KeyIndex)(index & 0xFF));
        gosASSERT(sc>=0 && sc<sizeof(ki->last_state_)/sizeof(ki->last_state_[0]));

        return (gosEnum_KeyStatus)ki->last_state_[sc];
    }

    return KEY_FREE;
}

