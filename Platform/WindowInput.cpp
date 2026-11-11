// Platform R0 (design: .claude/cleanup/platform-module-design.md): the keyboard/key-status seam moved out of
// GameOS/gameos/gameos_input.cpp unchanged. The bodies below are byte-for-byte the former gos_GetKey /
// gos_DescribeKey / gos_KeyboardFlush / gos_GetKeyStatus and their SDL scancode remap tables; only the entry
// names changed (the gos_* wrappers in gameos_input.cpp forward here). No behaviour change.
#include "WindowInput.hpp"

#include "gameos.hpp"

#include "mc2_czstring.h"
#include "Gates/gate.h"
#include "mc2fmt.h"
#include "utils/mc2log.h"

#include <tracy/Tracy.hpp>

#include <cassert>
#include <SDL2/SDL.h>
#include "gos_input.h"

// The one SDL window, owned by gos_render (gos_render.cpp / EditorGosRender.cpp). Platform reads it through
// window() below and never stores a copy.
extern SDL_Window* g_sdl_window;

static bool g_gos_exit_game_os = false;

// sebi 2026-04-22: when ExitGameOS/gos_TerminateApplication fires, the game
// quietly stops at the top of the next frame with no visible error. This
// shim logs the caller's stack so silent exits are debuggable. Gated on
// env var MC2_TRACE_EXIT so release builds stay quiet by default.
// (R1-window: moved unchanged from GameOS/gameos/gameos.cpp.)
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
static void mc2_log_exit_caller(mc2::czstring fn)
{
    static const bool s_trace = mc2::gate::get<"MC2_TRACE_EXIT">();
    static bool s_once = false;
    if (!s_trace) {
        if (!s_once) mc2::fmt::print_err("[EXIT] {} called\n", mc2::fmt::safe(fn));
        s_once = true;
        return;
    }
    s_once = true;

    ZoneScopedN("Shutdown.ExitStack");
    mc2::fmt::print_err("[EXIT] {} called. Stack:\n", mc2::fmt::safe(fn));
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    static bool s_sym_initialized = false;
    if (!s_sym_initialized) { SymInitialize(proc, nullptr, TRUE); s_sym_initialized = true; }

    void* frames[32];
    USHORT n = CaptureStackBackTrace(1, 32, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        char symBuf[sizeof(SYMBOL_INFO) + 512];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(symBuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 512;
        DWORD64 disp64 = 0;
        const char* symName = "?";
        IMAGEHLP_LINE64 line{}; line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD dispL = 0;
        const char* fileName = "?"; DWORD lineNum = 0;
        if (SymFromAddr(proc, (DWORD64)frames[i], &disp64, sym))
            symName = sym->Name;
        if (SymGetLineFromAddr64(proc, (DWORD64)frames[i], &dispL, &line)) {
            fileName = line.FileName; lineNum = line.LineNumber;
        }
        mc2::fmt::print_err("  #{:02d}  {}  ({}:{:d})\n", mc2::fmt::as_i32(i), mc2::fmt::safe(symName), mc2::fmt::safe(fileName), mc2::fmt::as_u32(lineNum));
    }
    fflush(stderr);
}
#else
static void mc2_log_exit_caller(const char*) {}
#endif

namespace mc2::platform {
namespace {

SDL_Window* window() { return g_sdl_window; }

// loks like KEY_* enum contains everything, from scancodes to localized keys, e.g. it has GRAVE and APOSTROPHE but those are usually on the same physical button and valuse depends on localization
// let's keep it like this for now and see later
gosEnum_KeyIndex remap_sdl_to_gos(const SDL_Scancode scancode) {

    if(scancode >= SDL_SCANCODE_1 && scancode <= SDL_SCANCODE_9)
        return static_cast<gosEnum_KeyIndex>(scancode - SDL_SCANCODE_1 + KEY_1);
    if(scancode == SDL_SCANCODE_0) return KEY_0;

    if(scancode >= SDL_SCANCODE_A && scancode <= SDL_SCANCODE_Z)
        return static_cast<gosEnum_KeyIndex>(scancode - SDL_SCANCODE_A + KEY_A);

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

} // namespace

////////////////////////////////////////////////////////////////////////////////
uint32_t GetKey()
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
const char* DescribeKey(uint32_t Key)
{
    const DWORD k = (Key>>8)&0xFF;
    DWORD ascii = Key&0xFF; (void)ascii; // may be 0 if extended key
    bool is_extended = (Key&0x100)!=0; (void)is_extended;

    SDL_Scancode scancode = remap_gos_to_sdl(static_cast<gosEnum_KeyIndex>(k));
    return SDL_GetScancodeName(scancode);
}

////////////////////////////////////////////////////////////////////////////////
void KeyboardFlush()
{
    input::resetKeypress();
}

////////////////////////////////////////////////////////////////////////////////
int GetKeyStatus(int keyIndex)
{
    const gosEnum_KeyIndex index = static_cast<gosEnum_KeyIndex>(keyIndex);
    const input::MouseInfo* mi = input::getMouseInfo();
    const input::KeyboardInfo* ki = input::getKeyboardInfo();

    if(index >= KEY_LMOUSE && index <= KEY_MOUSEX2) {
        gosASSERT(index > 0 && index <= input::MouseInfo::NUM_BUTTONS);
        return static_cast<int>(mi->button_state_[index-1]);
    }

    // if keyboard
    if((index & 0x01000000) == 0) {
        if(index > 255)
        {
            int asfa=0;
            (void)asfa;
        }
        SDL_Scancode sc = remap_gos_to_sdl(static_cast<gosEnum_KeyIndex>(index & 0xFF));
        gosASSERT(sc>=0 && sc<sizeof(ki->last_state_)/sizeof(ki->last_state_[0]));

        return static_cast<int>(ki->last_state_[sc]);
    }

    return static_cast<int>(KEY_FREE);
}

////////////////////////////////////////////////////////////////////////////////
// R1-window: termination flag (was the static in gameos.cpp shared by ExitGameOS / gos_TerminateApplication /
// gosExitGameOS).
void RequestExit(const char* caller)
{
    g_gos_exit_game_os = true;
    mc2_log_exit_caller(caller);
}

bool ExitRequested()
{
    return g_gos_exit_game_os;
}

void TerminateApplication()
{
    RequestExit("gos_TerminateApplication");
}

////////////////////////////////////////////////////////////////////////////////
// R1-window: display queries (bodies unchanged from gos_render.cpp graphics::get_*; the window comes from g_sdl_window,
// which is the window the renderer's handle wraps).
int GetWindowDisplayIndex()
{
    SDL_Window* w = window();
    assert(w);
    return SDL_GetWindowDisplayIndex(w);
}

int GetNumDisplayModes(int displayIndex)
{
    return SDL_GetNumDisplayModes(displayIndex);
}

bool GetDisplayModeByIndex(int displayIndex, int modeIndex, int* width, int* height, int* bpp)
{
    assert(width && height && bpp);

    SDL_DisplayMode dm;
    if (SDL_GetDisplayMode(displayIndex, modeIndex, &dm) != 0) {
        MC2_LOG(Render, Error, "SDL_GetDisplayMode failed: {}", mc2::fmt::safe(SDL_GetError()));
        return false;
    }

    *width = dm.w;
    *height = dm.h;
    *bpp = SDL_BITSPERPIXEL(dm.format);
    return true;
}

// gos_ProbeDrawableSize body (was renderer probeDrawableSize -> graphics::get_drawable_size): fresh SDL drawable size.
bool ProbeDrawableSize(int* w, int* h)
{
    SDL_Window* win = window();
    if (!win)
        return false;
    assert(w && h);
    SDL_GetWindowSizeInPixels(win, w, h);
    return true;
}

////////////////////////////////////////////////////////////////////////////////
// R1-window: IME. The engine never implemented these; they are empty by design (the appearance struct is a GameOS
// type, so it crosses this header as an opaque pointer).
void EnableIME(bool) {}
void ToggleIME(bool) {}
void GetIMEAppearance(void*) {}
void SetIMEAppearance(const void*) {}
void PositionIME(std::uint32_t, std::uint32_t) {}
void SetIMELevel(std::uint32_t) {}
void FinalizeStringIME() {}


} // namespace mc2::platform
