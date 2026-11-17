#include <optional>
#include <utility>
#include <SDL2/SDL.h>
#include <string.h> // memset
#include <cstdio>
#include <deque>
#include <string>
#include <initializer_list>
#include <assert.h>
#include "gos_input.h"
#include "input_capture_policy.h"
#include "mc2fmt.h"
#include "Gates/gate.h"
#include "Gates/gate_cold.h"
#include "gos_screenshot.h"

#ifdef PLATFORM_WINDOWS
#include <windows.h>   // GetAsyncKeyState, GetSystemMetrics, SM_SWAPBUTTON
#endif

// I3: the pure edge kernel in input_capture_policy.h stores its result in the
// same byte slots as input::KeyState; the two enums must stay value-identical.
static_assert(static_cast<int>(input::capture::Edge::Free)     == std::to_underlying(input::KS_FREE),     "capture::Edge must mirror input::KeyState");
static_assert(static_cast<int>(input::capture::Edge::Pressed)  == std::to_underlying(input::KS_PRESSED),  "capture::Edge must mirror input::KeyState");
static_assert(static_cast<int>(input::capture::Edge::Held)     == std::to_underlying(input::KS_HELD),     "capture::Edge must mirror input::KeyState");
static_assert(static_cast<int>(input::capture::Edge::Released) == std::to_underlying(input::KS_RELEASED), "capture::Edge must mirror input::KeyState");

static input::MouseInfo g_mouse_info;
static input::KeyboardInfo g_keyboard_info;
static input::capture::PointerCapture g_pointer_capture;

namespace input {

static bool s_imguiWantsMouse = false;
static bool s_imguiWantsKeyboard = false;
static bool s_gameModalActive = false;
static bool s_blockedKeys[512] = {};

// Legacy edit-box dialogs confirm/cancel on release even while ImGui owns text.
static bool isDialogKey(int scancode) {
    return scancode == SDL_SCANCODE_RETURN || scancode == SDL_SCANCODE_KP_ENTER || scancode == SDL_SCANCODE_ESCAPE;
}

static void applyInputFocus(bool focused) {
    g_pointer_capture.setFocus(focused);
    if (!focused) {
        memset(g_keyboard_info.last_state_, 0, sizeof(g_keyboard_info.last_state_));
        resetKeypress();
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// MC2_INPUT_SCRIPT=<file>: a scripted player for the player-path harness (scripts/run_player_path.py).
// It feeds the SAME state the engine reads: the mouse position/button state behind gos_GetMouseInfo / gos_GetKeyStatus
// (GUI hit-testing and button handlers run for real) and the keyboard state behind gos_GetKey. It never touches the OS:
// no SendInput, no cursor, no focus, so it works with the window unfocused, minimised or parked off-screen.
// The file is tailed every frame, so the harness can append commands as it sees log markers. One command per line:
//   wait N | move X Y | down [l|r] | up [l|r] | click X Y [l|r] | wheel Y | focus lost|gained | keydown KEY | keyup KEY | key KEY | shot PATH.tga | mark TEXT
// X Y are logical 800x600 canvas pixels (normalised x/800, y/600 the way gos_GetMouseInfo reports them); KEY is an SDL
// key name (Return, Escape, Space, F1, A). Every executed command logs `[INPUT_SCRIPT v1] frame=N cmd=...`.
// Inert (one cached string test per frame) when the variable is unset.
// ---------------------------------------------------------------------------------------------------------------------
namespace {
struct ScriptPlayer {
    std::string path;
    bool inited = false, active = false;
    long readOff = 0;
    std::string partial;
    std::deque<std::string> q;
    int waitFrames = 0;
    long frame = 0;
    bool mouseSet = false;
    float nx = 0.f, ny = 0.f;
    bool btn[2] = {false, false};      // left, right
    bool keys[512] = {};
    char lineBuf[256];

    void init() {
        inited = true;
        path = mc2::gate::str<"MC2_INPUT_SCRIPT">();
        active = !path.empty();
        if (active) {
            applyInputFocus(true); // a parked window may have lost focus before script initialization
            mc2::fmt::print_out("[INPUT_SCRIPT v1] frame=0 script={}\n", mc2::fmt::safe(path.c_str()));
        }
    }
    void pull() {
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return;
        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        if (size > readOff) {
            fseek(f, readOff, SEEK_SET);
            std::string chunk(static_cast<size_t>(size - readOff), '\0');
            const size_t got = fread(chunk.data(), 1, chunk.size(), f);
            chunk.resize(got);
            readOff += static_cast<long>(got);
            partial += chunk;
            size_t nl;
            while ((nl = partial.find('\n')) != std::string::npos) {
                std::string line = partial.substr(0, nl);
                partial.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (!line.empty() && line[0] != '#') q.push_back(line);
            }
        }
        fclose(f);
    }
    static bool key_of(std::string_view name, int* sc) {
        const std::string z(name);
        const int v = static_cast<int>(SDL_GetScancodeFromName(z.c_str()));
        if (v <= 0 || v >= 512) return false;
        *sc = v;
        return true;
    }
    void exec(std::string_view lineView) {
        const std::string line(lineView);
        char verb[32] = {}, a[96] = {}, b[96] = {}, c[32] = {};
        const int n = sscanf(line.c_str(), "%31s %95s %95s %31s", verb, a, b, c);
        const std::string v = verb;
        auto push_front = [&](std::initializer_list<std::string> l) { q.insert(q.begin(), l.begin(), l.end()); };
        auto btn_idx = [](char c0) { return (c0 == 'r' || c0 == 'R') ? 1 : 0; };
        mc2::fmt::print_out("[INPUT_SCRIPT v1] frame={:d} cmd={}\n", mc2::fmt::as_i32(static_cast<int>(frame)), mc2::fmt::safe(line.c_str()));
        if (v == "wait" && n >= 2) waitFrames = atoi(a);
        else if (v == "move" && n >= 3) { nx = static_cast<float>(atof(a)) / 800.f; ny = static_cast<float>(atof(b)) / 600.f; mouseSet = true; }
        else if (v == "down") btn[n >= 2 ? btn_idx(a[0]) : 0] = true;
        else if (v == "up") btn[n >= 2 ? btn_idx(a[0]) : 0] = false;
        else if (v == "focus" && n >= 2) applyInputFocus(std::string_view(a) == "gained");
        else if (v == "wheel" && n >= 2) {
            SDL_Event event = {};
            event.type = SDL_MOUSEWHEEL;
            event.wheel.y = atoi(a);
            handleMouseWheel(&event);
        }
        else if (v == "click" && n >= 3) {
            const std::string bn = n >= 4 ? c : "l";
            push_front({std::string("move ") + a + " " + b, "wait 4", "down " + bn, "wait 4", "up " + bn, "wait 4"});
        }
        else if (v == "keydown" && n >= 2) { int sc; if (key_of(std::string_view(line).substr(line.find(' ') + 1), &sc)) keys[sc] = true; }
        else if (v == "keyup" && n >= 2) { int sc; if (key_of(std::string_view(line).substr(line.find(' ') + 1), &sc)) keys[sc] = false; }
        else if (v == "key" && n >= 2) {
            const std::string name = line.substr(line.find(' ') + 1);
            push_front({"keydown " + name, "wait 4", "keyup " + name, "wait 4"});
        }
        else if (v == "shot" && n >= 2) {   // a TGA of the next presented frame (the Vulkan readback the capture harness trusts)
            const std::string p = line.substr(line.find(' ') + 1);
            gos::screenshot::request(p.c_str());
        }
        else if (v != "mark")
            mc2::fmt::print_out("[INPUT_SCRIPT v1] frame={:d} unknown command\n", mc2::fmt::as_i32(static_cast<int>(frame)));
    }
    void tick() {
        if (!inited) init();
        if (!active) return;
        ++frame;
        if (waitFrames > 0) { --waitFrames; return; }
        if (q.empty()) pull();
        while (waitFrames == 0 && !q.empty()) {
            const std::string line = q.front();
            q.pop_front();
            exec(line);
        }
    }
};
ScriptPlayer g_script;
} // namespace

bool scriptMouseNorm(float* nx, float* ny, bool* buttons) {
    if (!g_script.active || !g_script.mouseSet) return false;
    *nx = g_script.nx;
    *ny = g_script.ny;
    if (buttons) { buttons[0] = g_script.btn[0]; buttons[1] = g_script.btn[1]; }
    return true;
}

bool scriptActive() { return g_script.active; }

bool inputFocused() { return g_pointer_capture.focused; }

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

void setInputFocus(bool focused) {
    // Off-screen scripts supply their own focus events, independent of the desktop.
    if (!g_script.active) applyInputFocus(focused);
}

////////////////////////////////////////////////////////////////////////////////
static std::optional<int> sdl2idx(int button) {
    switch(button) {
        case SDL_BUTTON_LEFT: return 0;
        case SDL_BUTTON_MIDDLE: return 1;
        case SDL_BUTTON_RIGHT: return 2;
        case SDL_BUTTON_X1: return 3;
        case SDL_BUTTON_X2: return 4;
        default: return std::nullopt;
    }
}

void handleMouseMotion(const SDL_Event* event) {
    assert(event);

    MouseInfo* mi = &g_mouse_info;
    mi->x_ = static_cast<float>(event->motion.x);
    mi->y_ = static_cast<float>(event->motion.y);
    mi->rel_x_ = static_cast<float>(event->motion.xrel);
    mi->rel_y_ = static_cast<float>(event->motion.yrel);
}

void handleMouseButton(const SDL_Event* event) {
    assert(event);

    MouseInfo* mi = &g_mouse_info;
    const auto idxOpt = sdl2idx(event->button.button);
    if(idxOpt && *idxOpt < MouseInfo::NUM_BUTTONS) {
        const int idx = *idxOpt;
        mi->button_state_[idx] = event->type == SDL_MOUSEBUTTONUP ? KS_PRESSED : KS_RELEASED;
    }
}

static bool s_imguiHoversMouse = false;
void setImguiHoversMouse(bool v) { s_imguiHoversMouse = v; }

// (hoisted above handleMouseWheel for the wheel guard; see comment at the
// original site below for the IMGUI-PAUSE-INPUT-FIX-1 rationale)
void setGameModalActive(bool v) { s_gameModalActive = v; }

void handleMouseWheel(const SDL_Event* event) {
    assert(event);

    // Captured or hovered ImGui windows own the wheel, including over pause.
    if (!g_pointer_capture.focused || !capture::wheelReachesGame({ s_imguiWantsMouse, s_imguiHoversMouse, s_gameModalActive }))
        return;

    MouseInfo* mi = &g_mouse_info;
    mi->wheel_vert_ = static_cast<float>(event->wheel.y);
    mi->wheel_hor_ = static_cast<float>(event->wheel.x);

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

void setImguiWantsMouse(bool v) {
    s_imguiWantsMouse = v;
    if (v && !s_gameModalActive) {
        g_pointer_capture.claim();
        // NewFrame claims after sampling, before game logic consumes the edges.
        for (int i = 0; i < MouseInfo::NUM_BUTTONS; ++i)
            if (g_pointer_capture.blocked & SDL_BUTTON(i + 1)) g_mouse_info.button_state_[i] = KS_FREE;
    }
}
void setImguiWantsKeyboard(bool v) {
    s_imguiWantsKeyboard = v;
    if (v) {
        for (int i = 0; i < 512; ++i) {
            if (isDialogKey(i)) continue;
            s_blockedKeys[i] = s_blockedKeys[i] || g_keyboard_info.last_state_[i] == 1 || g_keyboard_info.last_state_[i] == 2;
            g_keyboard_info.last_state_[i] = 0;
        }
        resetKeypress();
    }
}

void updateMouseState() {
    MouseInfo* mi = &g_mouse_info;
    g_script.tick();   // MC2_INPUT_SCRIPT: once per frame, before this frame's state is read

#ifdef PLATFORM_WINDOWS
    // The editor's GL child HWND uses a custom WndProc (MC2EditorGLChild) that
    // SDL does not subclass, so SDL_GetMouseState() never reflects clicks on
    // the GL viewport.  Poll Win32 hardware state directly instead.
    // The focus/gesture filter below blocks background and activation clicks.
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
    Uint32 button_state = SDL_GetMouseState(nullptr, nullptr);
#endif

    // Route each gesture at its press; keep that ownership through release.
    if (g_script.active) {   // Desktop input must not leak into off-screen proofs.
        button_state = 0;
        if (g_script.btn[0]) button_state |= SDL_BUTTON(SDL_BUTTON_LEFT);
        if (g_script.btn[1]) button_state |= SDL_BUTTON(SDL_BUTTON_RIGHT);
    }

    button_state = g_pointer_capture.sample(button_state,
        capture::mouseButtonsReachGame({ s_imguiWantsMouse, s_imguiHoversMouse, s_gameModalActive }));

    int buttons[] = {SDL_BUTTON_LEFT, SDL_BUTTON_MIDDLE, SDL_BUTTON_RIGHT, SDL_BUTTON_X1, SDL_BUTTON_X2 };

    for(unsigned int b=0; b < sizeof(buttons)/sizeof(buttons[0]); ++b) {

        const auto idxOpt = sdl2idx(buttons[b]);
        if(!idxOpt || *idxOpt >= MouseInfo::NUM_BUTTONS)
            continue;
        const int idx = *idxOpt;

        const capture::Edge prev = static_cast<capture::Edge>(mi->button_state_[idx]);
        const capture::Edge next = g_pointer_capture.edge(prev, SDL_BUTTON(buttons[b]), button_state);
        mi->button_state_[idx] = static_cast<KeyState>(next);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Keyboard
//

void handleKeyEvent(const SDL_Event* event) {
    const int scancode = static_cast<int>(event->key.keysym.scancode);
    if (!isDialogKey(scancode) && (s_imguiWantsKeyboard || (scancode >= 0 && scancode < 512 && s_blockedKeys[scancode]))) return;
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

    if (!g_pointer_capture.focused) {
        memset(ki->last_state_, 0, sizeof(ki->last_state_));
        resetKeypress();
        return;
    }

    int array_len;
    const Uint8* state = SDL_GetKeyboardState(&array_len);
    assert(array_len <= sizeof(ki->last_state_)/sizeof(ki->last_state_[0]));

    ki->first_pressed_ = -1;

    for(int i=0; i<array_len; ++i) {

        const capture::Edge prev = static_cast<capture::Edge>(ki->last_state_[i]);
        const bool down = g_script.active ? (i < 512 && g_script.keys[i]) : state[i] != 0;
        const capture::Edge next = capture::keyboardEdge(prev, down, s_imguiWantsKeyboard, s_blockedKeys[i], isDialogKey(i));
        ki->last_state_[i] = static_cast<uint8_t>(next);


        if(ki->first_pressed_==-1 && next==capture::Edge::Pressed) {
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
        const int kStateLen = static_cast<int>((sizeof(ki->last_state_)/sizeof(ki->last_state_[0])));
        auto injectModifier = [&](SDL_Scancode sc, DWORD vk) {
            int idx = static_cast<int>(sc);
            if (idx < 0 || idx >= kStateLen) return;
            // A script owns all keys; the SDL scan advanced their edges once.
            if (g_script.active) return;
            const capture::Edge prev = static_cast<capture::Edge>(ki->last_state_[idx]);
            const bool down = (::GetAsyncKeyState(vk) & 0x8000) != 0;
            ki->last_state_[idx] = static_cast<uint8_t>(capture::keyboardEdge(prev, down, s_imguiWantsKeyboard, s_blockedKeys[idx]));
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
