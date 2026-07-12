#include <SDL2/SDL.h>
#include <string.h> // memset
#include <assert.h>
#include "gos_input.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>   // GetAsyncKeyState, GetSystemMetrics, SM_SWAPBUTTON
#endif

static input::MouseInfo g_mouse_info;
static input::KeyboardInfo g_keyboard_info;

namespace input {

MouseInfo::MouseInfo()
    : x_(0), y_(0), rel_x_(0), rel_y_(0),
     wheel_vert_(0), wheel_hor_(0)
{
    memset(button_state_, 0 ,sizeof(button_state_));
}

KeyboardInfo::KeyboardInfo():
    key_pressed_(false),
    key_released_(false),
    first_pressed_(-1)
{
    memset(last_state_, 0, sizeof(last_state_));
}

const MouseInfo* getMouseInfo() {
    return &g_mouse_info;
}

// MC2_VK_PICK_PROBE test hook: inject a synthetic raw SDL mouse point (window-
// points space, i.e. the space event->motion.x lives in). Lets the pick probe
// drive the exact production normalize path (gos_GetMouseInfo) deterministically.
void setMouseInfoRaw(float x, float y) {
    g_mouse_info.x_ = x;
    g_mouse_info.y_ = y;
}

const KeyboardInfo* getKeyboardInfo() {
    return &g_keyboard_info;
}

void resetKeypress() {
    g_keyboard_info.key_pressed_ = g_keyboard_info.key_released_ = false;
    g_keyboard_info.first_pressed_ = -1;
}

////////////////////////////////////////////////////////////////////////////////
static int sdl2idx(int button) {
    switch(button) {
        case SDL_BUTTON_LEFT: return 0;
        case SDL_BUTTON_MIDDLE: return 1;
        case SDL_BUTTON_RIGHT: return 2;
        case SDL_BUTTON_X1: return 3;
        case SDL_BUTTON_X2: return 4;
        default: return -1;
    }
}

void handleMouseMotion(const SDL_Event* event) {
    assert(event);

    MouseInfo* mi = &g_mouse_info;
    mi->x_ = (float)event->motion.x;
    mi->y_ = (float)event->motion.y;
    mi->rel_x_ = (float)event->motion.xrel;
    mi->rel_y_ = (float)event->motion.yrel;
}

void handleMouseButton(const SDL_Event* event) {
    assert(event);

    MouseInfo* mi = &g_mouse_info;
    int idx = sdl2idx(event->button.button);
    if(idx != -1 && idx < MouseInfo::NUM_BUTTONS) {
        mi->button_state_[idx] = event->type == SDL_MOUSEBUTTONUP ? KS_PRESSED : KS_RELEASED;
    }
}

void handleMouseWheel(const SDL_Event* event) {
    assert(event);
    
    MouseInfo* mi = &g_mouse_info;
    mi->wheel_vert_ = (float)event->wheel.y;
    mi->wheel_hor_ = (float)event->wheel.x;

    /* not in my SDL, apparenty >= SDL 2.0.4
    if(event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        mi->wheel_vert_ *= -1;
        mi->wheel_hor_ *= -1;
    }
    */
}

void beginUpdateMouseState() {
    MouseInfo* mi = &g_mouse_info;
    mi->rel_x_ = mi->rel_y_ = 0.0f;
    mi->wheel_hor_ = mi->wheel_vert_ = 0.0f;
}

static bool s_imguiWantsMouse = false;
void setImguiWantsMouse(bool v) { s_imguiWantsMouse = v; }

// IMGUI-PAUSE-INPUT-FIX-1: when a game MODAL (the pause menu) is up, the game must
// win mouse input even if an ImGui window (e.g. Graphics Options, Ctrl+Shift+G) is
// open and overlapping it. Without this, ImGui's WantCaptureMouse zeroes the button
// state every frame, the pause menu's DOWN->UP click transition never fires, and the
// RETURN/ABORT/EXIT buttons are dead (the "can't exit via menu" hang). Set per-frame
// by MissionInterfaceManager::update from isPaused() && !isPausedWithoutMenu().
static bool s_gameModalActive = false;
void setGameModalActive(bool v) { s_gameModalActive = v; }

void updateMouseState() {
    MouseInfo* mi = &g_mouse_info;

#ifdef PLATFORM_WINDOWS
    // The editor's GL child HWND uses a custom WndProc (MC2EditorGLChild) that
    // SDL does not subclass, so SDL_GetMouseState() never reflects clicks on
    // the GL viewport.  Poll Win32 hardware state directly instead.
    // This is safe for the game too — GetAsyncKeyState reflects the physical
    // button state just as SDL_GetMouseState would under normal focus.
    Uint32 button_state = 0;
    {
        // Respect OS left/right button swap setting.
        const bool swapped = ::GetSystemMetrics(SM_SWAPBUTTON) != 0;
        const DWORD lBtn = swapped ? VK_RBUTTON : VK_LBUTTON;
        const DWORD rBtn = swapped ? VK_LBUTTON : VK_RBUTTON;
        if (::GetAsyncKeyState(lBtn)        & 0x8000) button_state |= SDL_BUTTON(SDL_BUTTON_LEFT);
        if (::GetAsyncKeyState(VK_MBUTTON)  & 0x8000) button_state |= SDL_BUTTON(SDL_BUTTON_MIDDLE);
        if (::GetAsyncKeyState(rBtn)        & 0x8000) button_state |= SDL_BUTTON(SDL_BUTTON_RIGHT);
        if (::GetAsyncKeyState(VK_XBUTTON1) & 0x8000) button_state |= SDL_BUTTON(SDL_BUTTON_X1);
        if (::GetAsyncKeyState(VK_XBUTTON2) & 0x8000) button_state |= SDL_BUTTON(SDL_BUTTON_X2);
    }
#else
    Uint32 button_state = SDL_GetMouseState(NULL, NULL);
#endif

    // Gate ImGui-captured clicks out of game input — EXCEPT when a game modal (pause
    // menu) is active, where the game must remain clickable through/over ImGui windows.
    if (s_imguiWantsMouse && !s_gameModalActive) button_state = 0;

    int buttons[] = {SDL_BUTTON_LEFT, SDL_BUTTON_MIDDLE, SDL_BUTTON_RIGHT, SDL_BUTTON_X1, SDL_BUTTON_X2 };

    for(unsigned int b=0; b < sizeof(buttons)/sizeof(buttons[0]); ++b) {

        int idx = sdl2idx(buttons[b]);
        if(idx == -1 || idx >= MouseInfo::NUM_BUTTONS)
            continue;

        KeyState prev_ks = mi->button_state_[idx];

        if(button_state & SDL_BUTTON(buttons[b])) {
            mi->button_state_[idx] = 
                (prev_ks==KS_FREE||prev_ks==KS_RELEASED) ? KS_PRESSED : KS_HELD;
        } else {
            mi->button_state_[idx] = 
                (prev_ks==KS_HELD||prev_ks==KS_PRESSED) ? KS_RELEASED : KS_FREE;
        }
#if 0
        if(mi->button_state_[idx] == KS_PRESSED)
            printf("%d pressed\n", idx);
        if(mi->button_state_[idx] == KS_RELEASED)
            printf("%d released\n", idx);
#endif
    }
}

////////////////////////////////////////////////////////////////////////////////
// Keyboard
//

void handleKeyEvent(const SDL_Event* event) {
    KeyboardInfo* ki = &g_keyboard_info;
    if(event->key.state == SDL_PRESSED) {
        ki->key_pressed_ = true;
    } else {
        ki->key_released_ = true;
    }

    ki->pressed_keysym_ = event->key.keysym;
}

void updateKeyboardState() {
    KeyboardInfo* ki = &g_keyboard_info;

    int array_len;
    const Uint8* state = SDL_GetKeyboardState(&array_len);
    assert(array_len <= sizeof(ki->last_state_)/sizeof(ki->last_state_[0]));

    ki->first_pressed_ = -1;

    for(int i=0; i<array_len; ++i) {

        uint8_t ls = ki->last_state_[i];

        if(state[i]) {
           ls = (ls==KS_FREE||ls==KS_RELEASED) ? KS_PRESSED : KS_HELD;
        } else {
           ls = (ls==KS_HELD||ls==KS_PRESSED) ? KS_RELEASED : KS_FREE;
        }
        ki->last_state_[i] = ls;

#if 0
        if(ls == KS_PRESSED || ls==KS_RELEASED) {
            printf("key: %d %s\n", i, ls==KS_PRESSED ? "PRESSED" : "RELEASED");
        }
#endif

        if(ki->first_pressed_==-1 && ls==KS_PRESSED) {
            ki->first_pressed_ = i;
        }
    }

#ifdef PLATFORM_WINDOWS
    // SDL_GetKeyboardState only reflects keys for SDL-owned windows. In the
    // editor, keyboard focus stays on the MFC parent window, so SDL never
    // processes its WM_KEYDOWN/UP and modifier key states read as KS_FREE.
    //
    // Fix: override the modifier key slots with Win32 hardware state.
    // This runs AFTER the SDL scan so SDL state cannot re-zero them.
    // We only inject the keys that the GameOS input system queries for the
    // inspector shortcut (Ctrl, Shift, Alt); the rest remain SDL-sourced.
    {
        // Helper: apply one Win32 key state into last_state_[scancode].
        const int kStateLen = (int)(sizeof(ki->last_state_)/sizeof(ki->last_state_[0]));
        auto injectModifier = [&](SDL_Scancode sc, DWORD vk) {
            int idx = (int)sc;
            if (idx < 0 || idx >= kStateLen) return;
            uint8_t ls = ki->last_state_[idx];
            bool down = (::GetAsyncKeyState(vk) & 0x8000) != 0;
            if (down)
                ls = (ls==KS_FREE||ls==KS_RELEASED) ? KS_PRESSED : KS_HELD;
            else
                ls = (ls==KS_HELD||ls==KS_PRESSED) ? KS_RELEASED : KS_FREE;
            ki->last_state_[idx] = ls;
        };
        injectModifier(SDL_SCANCODE_LCTRL,  VK_LCONTROL);
        injectModifier(SDL_SCANCODE_RCTRL,  VK_RCONTROL);
        injectModifier(SDL_SCANCODE_LSHIFT, VK_LSHIFT);
        injectModifier(SDL_SCANCODE_RSHIFT, VK_RSHIFT);
        injectModifier(SDL_SCANCODE_LALT,   VK_LMENU);
        injectModifier(SDL_SCANCODE_RALT,   VK_RMENU);
    }
#endif
}

} // namespace
